#include "runtime/app_pod.hpp"
#include "runtime/pod_log.hpp"
#include "runtime/k32_app_abi.h"
#include "hal/capability_firewall.hpp"
#include "osal/system.hpp"

#include "wasm3.h"
#include "m3_env.h"

#include <cstring>
#include <cstdio>
#include <mutex>
#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static void k32_stack_probe(const char* where) {
    static uint32_t s_calls = 0;
    if (s_calls < 6 || (++s_calls % 40) == 1) {
        ++s_calls;
        printf("[STACK] %s call#%u free=%u bytes task=%s\n", where, (unsigned)s_calls,
               (unsigned)uxTaskGetStackHighWaterMark(nullptr), pcTaskGetName(nullptr));
    }
}
#else
static void k32_stack_probe(const char*) {}
#endif

namespace kube32::runtime {

// -----------------------------------------------------------------------------
// Request encoding
// -----------------------------------------------------------------------------
std::string encode_app_request(const AppRequest& req) {
    std::string out;
    out.reserve(req.method.size() + req.path_and_query.size() + req.body.size() + 160);
    out.append(req.method.data(), req.method.size());
    out.push_back(' ');
    out.append(req.path_and_query.data(), req.path_and_query.size());
    out.push_back('\n');
    auto add = [&](const char* name, std::string_view v) {
        if (v.empty()) return;
        out.append(name);
        out.append(": ");
        for (char c : v) out.push_back((c == '\n' || c == '\r') ? ' ' : c);
        out.push_back('\n');
    };
    add("Cookie", req.cookie);
    add("Content-Type", req.content_type);
    add("X-Request-Id", req.request_id);
    add("X-Forwarded-For", req.forwarded_for);
    out.push_back('\n');
    out.append(req.body.data(), req.body.size());
    return out;
}

// -----------------------------------------------------------------------------
// Platform hooks
// -----------------------------------------------------------------------------
static AppPlatformHooks s_hooks;
static bool s_verbose_requests = false;
static std::mutex s_inflight_mutex;
static std::string s_inflight;
static uint64_t s_inflight_since = 0;
static uint64_t s_gas_budget_per_call = 400ull * 1000ull * 1000ull;

void set_app_request_logging(bool verbose) { s_verbose_requests = verbose; }
void set_app_gas_budget(uint64_t units) { s_gas_budget_per_call = units; }

void set_app_platform_hooks(AppPlatformHooks hooks) {
    s_hooks = std::move(hooks);
}

const AppPlatformHooks& app_platform_hooks() {
    return s_hooks;
}

// -----------------------------------------------------------------------------
// Per-call context, keyed by the wasm3 runtime pointer the raw imports receive
// -----------------------------------------------------------------------------
struct CallContext {
    std::string pod_id;
    AppResponseSink* sink{nullptr};
    bool began{false};
    bool ended{false};
    int status{0};
    bool write_failed{false};
};

static std::mutex s_ctx_mutex;
static std::map<const void*, CallContext*> s_ctx_by_runtime;
static std::map<const void*, std::string> s_pod_by_runtime;

static CallContext* ctx_for(IM3Runtime rt) {
    std::lock_guard<std::mutex> lock(s_ctx_mutex);
    auto it = s_ctx_by_runtime.find(rt);
    return (it == s_ctx_by_runtime.end()) ? nullptr : it->second;
}

static std::string pod_for(IM3Runtime rt, IM3ImportContext ctx) {
    if (ctx && ctx->userdata) return static_cast<const char*>(ctx->userdata);
    std::lock_guard<std::mutex> lock(s_ctx_mutex);
    auto it = s_pod_by_runtime.find(rt);
    if (it != s_pod_by_runtime.end()) return it->second;
    return "default-pod";
}

static bool guest_range_ok(void* mem, uint32_t off, uint32_t len) {
    size_t size = m3_GetMemorySizeAt(mem);
    return static_cast<uint64_t>(off) + len <= size;
}

// -----------------------------------------------------------------------------
// Raw imports
// -----------------------------------------------------------------------------
static const void* raw_resp_begin(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
    int32_t status = static_cast<int32_t>(sp[0]);
    uint32_t off = static_cast<uint32_t>(sp[1]);
    uint32_t len = static_cast<uint32_t>(sp[2]);
    CallContext* c = ctx_for(rt);
    if (!c || !c->sink || c->began) return m3Err_none;
    std::string_view ctype("application/json");
    if (len > 0 && len < 128 && guest_range_ok(mem, off, len)) {
        ctype = std::string_view(static_cast<const char*>(mem) + off, len);
    }
    c->began = true;
    c->status = status;
    c->sink->begin(status, ctype);
    return m3Err_none;
}

static const void* raw_resp_header(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
    uint32_t noff = static_cast<uint32_t>(sp[0]);
    uint32_t nlen = static_cast<uint32_t>(sp[1]);
    uint32_t voff = static_cast<uint32_t>(sp[2]);
    uint32_t vlen = static_cast<uint32_t>(sp[3]);
    CallContext* c = ctx_for(rt);
    if (!c || !c->sink || !c->began) return m3Err_none;
    if (nlen == 0 || nlen > 64 || vlen > K32_MAX_HEADER_BYTES) return m3Err_none;
    if (!guest_range_ok(mem, noff, nlen) || !guest_range_ok(mem, voff, vlen)) return m3Err_none;
    c->sink->header(std::string_view(static_cast<const char*>(mem) + noff, nlen),
                    std::string_view(static_cast<const char*>(mem) + voff, vlen));
    return m3Err_none;
}

static const void* raw_resp_write(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
    k32_stack_probe("resp_write");
    uint32_t off = static_cast<uint32_t>(sp[1]);
    uint32_t len = static_cast<uint32_t>(sp[2]);
    CallContext* c = ctx_for(rt);
    if (!c || !c->sink || !c->began || c->ended) {
        *reinterpret_cast<int32_t*>(sp) = K32_ERR_INVAL;
        return m3Err_none;
    }
    if (!guest_range_ok(mem, off, len)) {
        *reinterpret_cast<int32_t*>(sp) = hal::HAL_ERR_FAULT;
        return m3Err_none;
    }
    if (len == 0) {
        *reinterpret_cast<int32_t*>(sp) = 0;
        return m3Err_none;
    }
    bool ok = c->sink->write(static_cast<const uint8_t*>(mem) + off, len);
    if (!ok) c->write_failed = true;
    *reinterpret_cast<int32_t*>(sp) = ok ? static_cast<int32_t>(len) : hal::HAL_ERR_IO;
    return m3Err_none;
}

static const void* raw_resp_end(IM3Runtime rt, IM3ImportContext, uint64_t*, void*) {
    CallContext* c = ctx_for(rt);
    if (!c || !c->sink || !c->began || c->ended) return m3Err_none;
    c->ended = true;
    c->sink->end();
    return m3Err_none;
}

static const void* raw_event_emit(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void* mem) {
    uint32_t toff = static_cast<uint32_t>(sp[1]);
    uint32_t tlen = static_cast<uint32_t>(sp[2]);
    uint32_t doff = static_cast<uint32_t>(sp[3]);
    uint32_t dlen = static_cast<uint32_t>(sp[4]);
    std::string pod_id = pod_for(rt, ictx);
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::IPC, 0, hal::AccessMode::Write)) {
        *reinterpret_cast<int32_t*>(sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }
    if (tlen == 0 || tlen > 64 || dlen > K32_MAX_EVENT_BYTES || !guest_range_ok(mem, toff, tlen) || !guest_range_ok(mem, doff, dlen)) {
        *reinterpret_cast<int32_t*>(sp) = K32_ERR_INVAL;
        return m3Err_none;
    }
    std::string_view topic(static_cast<const char*>(mem) + toff, tlen);
    int32_t res = hal::HAL_ERR_IO;
    if (s_hooks.event_emit) {
        res = s_hooks.event_emit(pod_id, topic, static_cast<const uint8_t*>(mem) + doff, dlen);
    }
    *reinterpret_cast<int32_t*>(sp) = res;
    return m3Err_none;
}

static const void* raw_time_ms(IM3Runtime, IM3ImportContext, uint64_t* sp, void*) {
    *reinterpret_cast<int64_t*>(sp) = static_cast<int64_t>(osal::System::get_uptime_ms());
    return m3Err_none;
}

static const void* raw_log(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void* mem) {
    uint32_t off = static_cast<uint32_t>(sp[0]);
    uint32_t len = static_cast<uint32_t>(sp[1]);
    if (len > 512 || !guest_range_ok(mem, off, len)) return m3Err_none;
    std::string pod_id = pod_for(rt, ictx);
    std::string_view text(static_cast<const char*>(mem) + off, len);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
    printf("[POD %s] %.*s\n", pod_id.c_str(), static_cast<int>(text.size()), text.data());
    // The pod's log ring is what `kubectl logs` serves
    PodLogStore::runtime().append(pod_id, text);
    return m3Err_none;
}

static const void* raw_random(IM3Runtime, IM3ImportContext, uint64_t* sp, void*) {
    uint32_t v = s_hooks.random ? s_hooks.random() : static_cast<uint32_t>(osal::System::get_uptime_ms() * 2654435761u);
    *reinterpret_cast<uint32_t*>(sp) = v;
    return m3Err_none;
}

static const void* raw_env_get(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void* mem) {
    uint32_t koff = static_cast<uint32_t>(sp[1]);
    uint32_t klen = static_cast<uint32_t>(sp[2]);
    uint32_t ooff = static_cast<uint32_t>(sp[3]);
    uint32_t ocap = static_cast<uint32_t>(sp[4]);
    k32_stack_probe("env_get");
    if (klen == 0 || klen > 64 || !guest_range_ok(mem, koff, klen) || !guest_range_ok(mem, ooff, ocap)) {
        *reinterpret_cast<int32_t*>(sp) = hal::HAL_ERR_FAULT;
        return m3Err_none;
    }
    std::string pod_id = pod_for(rt, ictx);
    std::string value;
    int32_t n = AppPodHost::instance().env_get(pod_id, std::string_view(static_cast<const char*>(mem) + koff, klen), value);
    if (n < 0) {
        *reinterpret_cast<int32_t*>(sp) = -1;
        return m3Err_none;
    }
    size_t copy = value.size() < ocap ? value.size() : ocap;
    if (copy > 0) std::memcpy(static_cast<uint8_t*>(mem) + ooff, value.data(), copy);
    if (copy < ocap) static_cast<uint8_t*>(mem)[ooff + copy] = 0;
    *reinterpret_cast<int32_t*>(sp) = static_cast<int32_t>(value.size());
    return m3Err_none;
}

static const void* raw_mem_stats(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
    uint32_t uoff = static_cast<uint32_t>(sp[1]);
    uint32_t loff = static_cast<uint32_t>(sp[2]);
    if (!guest_range_ok(mem, uoff, 4) || !guest_range_ok(mem, loff, 4)) {
        *reinterpret_cast<int32_t*>(sp) = hal::HAL_ERR_FAULT;
        return m3Err_none;
    }
    uint32_t used = static_cast<uint32_t>(m3_GetMemorySizeAt(mem));
    uint64_t limit = rt ? rt->memoryBytesLimit : 0;
    std::memcpy(static_cast<uint8_t*>(mem) + uoff, &used, 4);
    uint32_t lim32 = static_cast<uint32_t>(limit);
    std::memcpy(static_cast<uint8_t*>(mem) + loff, &lim32, 4);
    *reinterpret_cast<int32_t*>(sp) = 0;
    return m3Err_none;
}

// Hardware imports -------------------------------------------------------------
static const void* raw_display_text(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void* mem) {
    int32_t x = static_cast<int32_t>(sp[1]);
    int32_t y = static_cast<int32_t>(sp[2]);
    uint32_t off = static_cast<uint32_t>(sp[3]);
    uint32_t len = static_cast<uint32_t>(sp[4]);
    uint32_t fg = static_cast<uint32_t>(sp[5]);
    uint32_t bg = static_cast<uint32_t>(sp[6]);
    std::string pod_id = pod_for(rt, ictx);
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::Display, 0, hal::AccessMode::Write)) {
        *reinterpret_cast<int32_t*>(sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }
    if (len > 64 || !guest_range_ok(mem, off, len)) {
        *reinterpret_cast<int32_t*>(sp) = hal::HAL_ERR_FAULT;
        return m3Err_none;
    }
    int32_t r = s_hooks.display_text ? s_hooks.display_text(x, y, static_cast<const char*>(mem) + off, len, fg, bg) : hal::HAL_ERR_NODEV;
    *reinterpret_cast<int32_t*>(sp) = r;
    return m3Err_none;
}

static const void* raw_display_rect(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void*) {
    k32_stack_probe("display_rect");
    std::string pod_id = pod_for(rt, ictx);
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::Display, 0, hal::AccessMode::Write)) {
        *reinterpret_cast<int32_t*>(sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }
    int32_t r = s_hooks.display_rect
        ? s_hooks.display_rect(static_cast<int32_t>(sp[1]), static_cast<int32_t>(sp[2]), static_cast<int32_t>(sp[3]), static_cast<int32_t>(sp[4]), static_cast<uint32_t>(sp[5]))
        : hal::HAL_ERR_NODEV;
    *reinterpret_cast<int32_t*>(sp) = r;
    return m3Err_none;
}

static const void* raw_display_bar(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void*) {
    std::string pod_id = pod_for(rt, ictx);
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::Display, 0, hal::AccessMode::Write)) {
        *reinterpret_cast<int32_t*>(sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }
    int32_t r = s_hooks.display_bar
        ? s_hooks.display_bar(static_cast<int32_t>(sp[1]), static_cast<int32_t>(sp[2]), static_cast<int32_t>(sp[3]), static_cast<int32_t>(sp[4]),
                              static_cast<uint32_t>(sp[5]), static_cast<uint32_t>(sp[6]), static_cast<uint32_t>(sp[7]))
        : hal::HAL_ERR_NODEV;
    *reinterpret_cast<int32_t*>(sp) = r;
    return m3Err_none;
}

static const void* raw_key_read(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void*) {
    k32_stack_probe("key_read");
    std::string pod_id = pod_for(rt, ictx);
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::Keyboard, 0, hal::AccessMode::Read)) {
        *reinterpret_cast<int32_t*>(sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }
    *reinterpret_cast<int32_t*>(sp) = s_hooks.key_read ? s_hooks.key_read() : hal::HAL_ERR_NODEV;
    return m3Err_none;
}

static const void* raw_tone(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void*) {
    std::string pod_id = pod_for(rt, ictx);
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::Speaker, 0, hal::AccessMode::Write)) {
        *reinterpret_cast<int32_t*>(sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }
    uint32_t freq = static_cast<uint32_t>(sp[1]);
    uint32_t ms = static_cast<uint32_t>(sp[2]);
    uint32_t vol = static_cast<uint32_t>(sp[3]);
    if (ms > 2000) ms = 2000;
    if (vol > 100) vol = 100;
    *reinterpret_cast<int32_t*>(sp) = s_hooks.tone ? s_hooks.tone(freq, ms, vol) : hal::HAL_ERR_NODEV;
    return m3Err_none;
}

void link_app_host_functions(IM3Module module, const std::string& pod_id) {
    if (!module) return;
    IM3Runtime rt = m3_GetModuleRuntime(module);
    if (rt) {
        std::lock_guard<std::mutex> lock(s_ctx_mutex);
        s_pod_by_runtime[rt] = pod_id;
    }
    m3_LinkRawFunction(module, "env", "k32_resp_begin",   "v(iii)",     &raw_resp_begin);
    m3_LinkRawFunction(module, "env", "k32_resp_header",  "v(iiii)",    &raw_resp_header);
    m3_LinkRawFunction(module, "env", "k32_resp_write",   "i(ii)",      &raw_resp_write);
    m3_LinkRawFunction(module, "env", "k32_resp_end",     "v()",        &raw_resp_end);
    m3_LinkRawFunction(module, "env", "k32_event_emit",   "i(iiii)",    &raw_event_emit);
    m3_LinkRawFunction(module, "env", "k32_time_ms",      "I()",        &raw_time_ms);
    m3_LinkRawFunction(module, "env", "k32_log",          "v(ii)",      &raw_log);
    m3_LinkRawFunction(module, "env", "k32_random",       "i()",        &raw_random);
    m3_LinkRawFunction(module, "env", "k32_env_get",      "i(iiii)",    &raw_env_get);
    m3_LinkRawFunction(module, "env", "k32_mem_stats",    "i(ii)",      &raw_mem_stats);
    m3_LinkRawFunction(module, "env", "k32_display_text", "i(iiiiii)",  &raw_display_text);
    m3_LinkRawFunction(module, "env", "k32_display_rect", "i(iiiii)",   &raw_display_rect);
    m3_LinkRawFunction(module, "env", "k32_display_bar",  "i(iiiiiii)", &raw_display_bar);
    m3_LinkRawFunction(module, "env", "k32_key_read",     "i()",        &raw_key_read);
    m3_LinkRawFunction(module, "env", "k32_tone",         "i(iii)",     &raw_tone);
}

// -----------------------------------------------------------------------------
// AppPodHost
// -----------------------------------------------------------------------------
AppPodHost& AppPodHost::instance() {
    static AppPodHost s_host;
    return s_host;
}

void AppPodHost::set_env(const std::string& pod_id, const std::vector<std::pair<std::string, std::string>>& env) {
    osal::LockGuard guard(mutex_);
    env_[pod_id] = env;
}

int32_t AppPodHost::env_get(const std::string& pod_id, std::string_view key, std::string& out) const {
    osal::LockGuard guard(mutex_);
    auto it = env_.find(pod_id);
    if (it == env_.end()) return -1;
    for (const auto& kv : it->second) {
        if (kv.first.size() == key.size() && std::memcmp(kv.first.data(), key.data(), key.size()) == 0) {
            out = kv.second;
            return static_cast<int32_t>(out.size());
        }
    }
    return -1;
}

bool AppPodHost::register_pod(const std::string& pod_id, ContainerLifecycleController* ctrl) {
    if (!ctrl) return false;
    Entry e;
    e.ctrl = ctrl;
    e.has_handle = ctrl->has_export(K32_EXPORT_HANDLE);
    e.has_event = ctrl->has_export(K32_EXPORT_EVENT);
    e.has_tick = ctrl->has_export(K32_EXPORT_TICK);
    e.has_alloc = ctrl->has_export(K32_EXPORT_ALLOC);
    e.has_init = ctrl->has_export(K32_EXPORT_INIT);
    if (!e.has_handle && !e.has_event && !e.has_tick && !e.has_init) return false;
    e.call_mutex = std::make_unique<osal::Mutex>();
    osal::LockGuard guard(mutex_);
    pods_[pod_id] = std::move(e);
    return true;
}

void AppPodHost::unregister_pod(const std::string& pod_id) {
    std::unique_ptr<osal::Mutex> hold;
    {
        osal::LockGuard guard(mutex_);
        auto it = pods_.find(pod_id);
        if (it != pods_.end()) {
            hold = std::move(it->second.call_mutex);
            pods_.erase(it);
        }
        env_.erase(pod_id);
    }
    if (hold) {
        // Wait for any in-flight call on this pod to finish before the mutex dies
        hold->lock();
        hold->unlock();
    }
}

bool AppPodHost::has_pod(const std::string& pod_id) const {
    osal::LockGuard guard(mutex_);
    return pods_.find(pod_id) != pods_.end();
}

bool AppPodHost::pod_handles_requests(const std::string& pod_id) const {
    osal::LockGuard guard(mutex_);
    auto it = pods_.find(pod_id);
    return it != pods_.end() && it->second.has_handle;
}

std::vector<std::string> AppPodHost::list_pods() const {
    osal::LockGuard guard(mutex_);
    std::vector<std::string> out;
    for (const auto& kv : pods_) out.push_back(kv.first);
    return out;
}

std::string AppPodHost::default_request_pod() const {
    osal::LockGuard guard(mutex_);
    for (const auto& kv : pods_) {
        if (kv.second.has_handle) return kv.first;
    }
    return "";
}

AppPodHost::Entry* AppPodHost::find_locked(const std::string& pod_id) {
    auto it = pods_.find(pod_id);
    return (it == pods_.end()) ? nullptr : &it->second;
}

std::string AppPodHost::in_flight(uint32_t& out_age_ms) const {
    std::lock_guard<std::mutex> lk(s_inflight_mutex);
    out_age_ms = s_inflight.empty() ? 0 : static_cast<uint32_t>(osal::System::get_uptime_ms() - s_inflight_since);
    return s_inflight;
}

AppPodStats AppPodHost::stats(const std::string& pod_id) const {
    osal::LockGuard guard(mutex_);
    auto it = pods_.find(pod_id);
    return (it == pods_.end()) ? AppPodStats{} : it->second.stats;
}

int32_t AppPodHost::init_pod(const std::string& pod_id) {
    ContainerLifecycleController* ctrl = nullptr;
    osal::Mutex* cm = nullptr;
    bool has_init = false;
    {
        osal::LockGuard guard(mutex_);
        Entry* e = find_locked(pod_id);
        if (!e) return -2;
        ctrl = e->ctrl;
        cm = e->call_mutex.get();
        has_init = e->has_init;
    }
    if (!has_init) return 0;
    osal::LockGuard call_guard(*cm);
    return ctrl->call_function(K32_EXPORT_INIT);
}

// Copies a block into guest memory via k32_alloc and calls `fn(ptr,len)` or
// `fn(topic_ptr, topic_len, ptr, len)` for events.
int32_t AppPodHost::call_with_block(Entry& e, const std::string& pod_id, const char* fn, std::string_view topic,
                                    const uint8_t* data, size_t len, AppResponseSink* sink) {
    ContainerLifecycleController* ctrl = e.ctrl;
    if (!e.has_alloc) return -1;

    uint32_t total = static_cast<uint32_t>(len + topic.size());
    int32_t ptr = ctrl->call_function(K32_EXPORT_ALLOC, {static_cast<int32_t>(total + 1)});
    if (ptr <= 0) return -1;

    size_t mem_size = 0;
    uint8_t* mem = ctrl->guest_memory(mem_size);
    if (!mem || static_cast<uint64_t>(static_cast<uint32_t>(ptr)) + total + 1 > mem_size) return -1;

    uint8_t* dst = mem + static_cast<uint32_t>(ptr);
    if (!topic.empty()) std::memcpy(dst, topic.data(), topic.size());
    if (len > 0) std::memcpy(dst + topic.size(), data, len);
    dst[total] = 0;

    CallContext ctx;
    ctx.pod_id = pod_id;
    ctx.sink = sink;
    IM3Runtime rt = static_cast<IM3Runtime>(ctrl->m3_runtime_handle());
    {
        std::lock_guard<std::mutex> lock(s_ctx_mutex);
        s_ctx_by_runtime[rt] = &ctx;
    }
#if d_m3HasGasMetering
    // A runaway handler must not pin the pod forever: budget one call's work.
    // 400M gas units is roughly 2-4 s of interpretation on a 240 MHz core.
    if (rt) m3_SetResourceLimit(rt, c_m3Limit_GasUnits, s_gas_budget_per_call);
#endif
    uint64_t t_call = osal::Thread::get_time_us();
    {
        std::lock_guard<std::mutex> lk(s_inflight_mutex);
        s_inflight.assign(fn);
        s_inflight.append(" ");
        s_inflight.append(reinterpret_cast<const char*>(data), len > 48 ? 48 : len);
        s_inflight_since = osal::System::get_uptime_ms();
    }
    if (s_verbose_requests) {
        printf("[APP] > %s %s%.*s\n", pod_id.c_str(), fn, (int)(len > 48 ? 48 : len), (const char*)data);
    }

    int32_t result;
    if (topic.empty()) {
        result = ctrl->call_function(fn, {ptr, static_cast<int32_t>(len)});
    } else {
        result = ctrl->call_function(fn, {ptr, static_cast<int32_t>(topic.size()),
                                          ptr + static_cast<int32_t>(topic.size()), static_cast<int32_t>(len)});
    }

    {
        std::lock_guard<std::mutex> lock(s_ctx_mutex);
        s_ctx_by_runtime.erase(rt);
    }
    uint32_t call_us = static_cast<uint32_t>(osal::Thread::get_time_us() - t_call);
    {
        std::lock_guard<std::mutex> lk(s_inflight_mutex);
        s_inflight.clear();
    }
    if (s_verbose_requests || call_us > 2000000u) {
        printf("[APP] < %s %s rc=%d %u us\n", pod_id.c_str(), fn, (int)result, (unsigned)call_us);
    }

    if (sink) {
        if (!ctx.began) {
            // Guest trapped or forgot to answer: synthesize a 500
            sink->begin(500, "application/json");
            const char* body = "{\"error\":\"pod_failure\",\"message\":\"workload produced no response\"}";
            sink->write(reinterpret_cast<const uint8_t*>(body), std::strlen(body));
            sink->end();
            return -1;
        }
        if (!ctx.ended) sink->end();
        return ctx.status > 0 ? ctx.status : result;
    }
    return result;
}

int32_t AppPodHost::handle(const std::string& pod_id, const AppRequest& req, AppResponseSink& sink) {
    Entry* e = nullptr;
    osal::Mutex* cm = nullptr;
    {
        osal::LockGuard guard(mutex_);
        e = find_locked(pod_id);
        if (!e || !e->has_handle) return -2;
        cm = e->call_mutex.get();
    }
    std::string block = encode_app_request(req);
    if (block.size() > K32_MAX_REQUEST_BYTES) {
        sink.begin(413, "application/json");
        const char* body = "{\"error\":\"too_large\",\"message\":\"request exceeds 16 KiB\"}";
        sink.write(reinterpret_cast<const uint8_t*>(body), std::strlen(body));
        sink.end();
        return 413;
    }

    uint64_t t0 = osal::Thread::get_time_us();
    int32_t status;
    k32_stack_probe("handle");
    {
        osal::LockGuard call_guard(*cm);
        // Entry may have been unregistered while we waited
        {
            osal::LockGuard guard(mutex_);
            e = find_locked(pod_id);
            if (!e) return -2;
        }
        status = call_with_block(*e, pod_id, K32_EXPORT_HANDLE, std::string_view(), reinterpret_cast<const uint8_t*>(block.data()), block.size(), &sink);
    }
    uint32_t us = static_cast<uint32_t>(osal::Thread::get_time_us() - t0);
    if (status < 0 || status >= 500) {
        printf("[APP] %s %.*s %.*s -> %d (%u us)\n", pod_id.c_str(), (int)req.method.size(), req.method.data(),
               (int)req.path_and_query.size(), req.path_and_query.data(), (int)status, (unsigned)us);
    }
    {
        osal::LockGuard guard(mutex_);
        Entry* e2 = find_locked(pod_id);
        if (e2) {
            e2->stats.requests++;
            e2->stats.total_us += us;
            e2->stats.last_us = us;
            if (us > e2->stats.max_us) e2->stats.max_us = us;
            if (status < 0 || status >= 500) e2->stats.errors++;
        }
    }
    return status;
}

int32_t AppPodHost::tick(const std::string& pod_id, uint32_t uptime_ms) {
    ContainerLifecycleController* ctrl = nullptr;
    osal::Mutex* cm = nullptr;
    {
        osal::LockGuard guard(mutex_);
        Entry* e = find_locked(pod_id);
        if (!e || !e->has_tick) return -2;
        ctrl = e->ctrl;
        cm = e->call_mutex.get();
    }
    uint64_t t0 = osal::Thread::get_time_us();
    int32_t rc;
    {
        osal::LockGuard call_guard(*cm);
        rc = ctrl->call_function(K32_EXPORT_TICK, {static_cast<int32_t>(uptime_ms)});
    }
    uint32_t us = static_cast<uint32_t>(osal::Thread::get_time_us() - t0);
    {
        osal::LockGuard guard(mutex_);
        Entry* e2 = find_locked(pod_id);
        if (e2) e2->stats.total_us += us;   // interpreter time feeds pod CPU metrics
    }
    return rc;
}

int32_t AppPodHost::event(const std::string& pod_id, std::string_view topic, const uint8_t* data, size_t len) {
    Entry* e = nullptr;
    osal::Mutex* cm = nullptr;
    {
        osal::LockGuard guard(mutex_);
        e = find_locked(pod_id);
        if (!e || !e->has_event) return -2;
        cm = e->call_mutex.get();
    }
    if (topic.empty() || len > K32_MAX_EVENT_BYTES) return K32_ERR_INVAL;
    uint64_t t0 = osal::Thread::get_time_us();
    int32_t rc;
    {
        osal::LockGuard call_guard(*cm);
        {
            osal::LockGuard guard(mutex_);
            e = find_locked(pod_id);
            if (!e) return -2;
        }
        rc = call_with_block(*e, pod_id, K32_EXPORT_EVENT, topic, data, len, nullptr);
    }
    uint32_t us = static_cast<uint32_t>(osal::Thread::get_time_us() - t0);
    {
        osal::LockGuard guard(mutex_);
        Entry* e2 = find_locked(pod_id);
        if (e2) e2->stats.total_us += us;
    }
    return rc;
}

void AppPodHost::broadcast_event(std::string_view topic, const uint8_t* data, size_t len) {
    std::vector<std::string> targets;
    {
        osal::LockGuard guard(mutex_);
        for (const auto& kv : pods_) {
            if (kv.second.has_event) targets.push_back(kv.first);
        }
    }
    for (const auto& id : targets) {
        event(id, topic, data, len);
    }
}

} // namespace kube32::runtime
