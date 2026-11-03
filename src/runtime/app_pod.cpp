#include "runtime/app_pod.hpp"
#include "runtime/pod_memory_pool.h"
#include "runtime/container_lifecycle.hpp"
#include "runtime/pod_cpu.hpp"
#include "runtime/pod_log.hpp"
#include "runtime/pod_fs.hpp"
#include "runtime/pod_linalg.hpp"
#include "runtime/pod_blobs.hpp"
#include "runtime/k32_app_abi.h"
#include "hal/capability_firewall.hpp"
#include "ipc/channel.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"
#include "runtime/pod_fs.hpp"

#include "wasm3.h"
#include "m3_env.h"
#include "m3_host.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <map>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

bool BufferResponseSink::write(const uint8_t* data, size_t len) {
    if (len_ < cap_) {
        size_t take = (cap_ - len_ < len) ? cap_ - len_ : len;
        std::memcpy(buf_ + len_, data, take);
        len_ += take;
    }
    total_ += len;
    return true;   // the rest is counted (overflow), the guest goes on writing
}

// True when `pod` is one of the comma separated entries of `chain`
static bool chain_contains(std::string_view chain, std::string_view pod) {
    size_t start = 0;
    while (start <= chain.size()) {
        size_t end = chain.find(',', start);
        if (end == std::string_view::npos) end = chain.size();
        if (chain.substr(start, end - start) == pod) return true;
        start = end + 1;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Platform hooks
// -----------------------------------------------------------------------------
static AppPlatformHooks s_hooks;
static bool s_verbose_requests = false;

// The pod worker this thread is (AppPodHost::start_workers), or -1
static thread_local int t_worker = -1;

// What each thread that calls pods is calling now (diagnostics); a nested
// call (a service call) restores its caller's entry
struct InFlight {
    uintptr_t thread{0};
    std::string what;
    uint64_t since{0};
};
static std::mutex s_inflight_mutex;
static std::vector<InFlight> s_inflight;

static InFlight& inflight_locked() {
    const uintptr_t id = osal::Thread::current_id();
    for (auto& f : s_inflight) if (f.thread == id) return f;
    if (s_inflight.size() >= 32) {
        // Threads that ended leave idle entries behind
        s_inflight.erase(std::remove_if(s_inflight.begin(), s_inflight.end(), [](const InFlight& f) { return f.what.empty(); }),
                         s_inflight.end());
    }
    s_inflight.push_back(InFlight{id, std::string(), 0});
    return s_inflight.back();
}

void set_app_request_logging(bool verbose) { s_verbose_requests = verbose; }

// Every host-initiated entry (k32_init, k32_alloc, k32_handle, k32_tick,
// k32_event) gets a fresh budget: a runaway call traps instead of pinning the
// pod, and no call runs on what an earlier call left over.
// wasm3 charges 45-120 units per ordinary instruction and 10,000 per
// indirect call; the budget is per pod (ContainerConfig::gas_budget_per_call).
static void arm_call_budget(ContainerLifecycleController* ctrl) {
#if d_m3HasGasMetering
    IM3Runtime rt = ctrl ? static_cast<IM3Runtime>(ctrl->m3_runtime_handle()) : nullptr;
    if (!rt || ctrl->config().gas_budget_per_call == 0) return;   // unmetered pod
    uint64_t budget = ctrl->config().gas_budget_per_call;
    m3_SetResourceLimit(rt, c_m3Limit_GasUnits, budget);
#else
    (void)ctrl;
#endif
}

// Gas spent by the call that just returned; logs new per-pod peaks so the
// budget can be sized from measurements (bounded output).
static void note_call_cost(ContainerLifecycleController* ctrl, const char* fn) {
#if d_m3HasGasMetering
    IM3Runtime rt = ctrl ? static_cast<IM3Runtime>(ctrl->m3_runtime_handle()) : nullptr;
    if (!rt || ctrl->config().gas_budget_per_call == 0) return;
    uint64_t used = m3_GetResourceUsage(rt, c_m3Limit_GasUnits);
    static std::mutex s_peak_mutex;
    static std::map<std::string, uint64_t> s_peak;
    static int s_logged = 0;
    std::lock_guard<std::mutex> lock(s_peak_mutex);
    uint64_t& peak = s_peak[ctrl->config().pod_id];
    if (used > peak + peak / 4 && s_logged < 24) {
        s_logged++;
        printf("[POD] %s: new peak call cost %llu gas units in %s (budget %llu)\n", ctrl->config().pod_id.c_str(),
               (unsigned long long)used, fn, (unsigned long long)m3_GetResourceLimit(rt, c_m3Limit_GasUnits));
    }
    if (used > peak) peak = used;
#else
    (void)ctrl; (void)fn;
#endif
}

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
    bool accepts_gzip{false};
    std::string chain;    // pods serving this request, this one last (service calls)
    std::vector<std::pair<std::string, std::string>> pending_headers;   // set before the response began
};

static constexpr size_t kMaxPendingHeaders = 8;

// Starts the response: status line, then the headers the guest set earlier
static void begin_response(CallContext* c, int status, std::string_view ctype) {
    c->began = true;
    c->status = status;
    c->sink->begin(status, ctype);
    for (const auto& h : c->pending_headers) c->sink->header(h.first, h.second);
    c->pending_headers.clear();
}

static std::mutex s_ctx_mutex;
static std::map<const void*, CallContext*> s_ctx_by_runtime;
static std::map<const void*, std::string> s_pod_by_runtime;
static std::map<const void*, int32_t> s_http_status_by_runtime;   // k32_http_status

static void set_http_status(IM3Runtime rt, int32_t status) {
    std::lock_guard<std::mutex> lock(s_ctx_mutex);
    s_http_status_by_runtime[rt] = status;
}

static CallContext* ctx_for(IM3Runtime rt) {
    std::lock_guard<std::mutex> lock(s_ctx_mutex);
    auto it = s_ctx_by_runtime.find(rt);
    return (it == s_ctx_by_runtime.end()) ? nullptr : it->second;
}

// The pod a host call comes from, by reference: the entry lives as long as
// the pod's runtime, so a host call copies no string.
static const std::string& pod_for(IM3Runtime rt, IM3ImportContext) {
    static const std::string kUnknown("default-pod");
    std::lock_guard<std::mutex> lock(s_ctx_mutex);
    auto it = s_pod_by_runtime.find(rt);
    return it != s_pod_by_runtime.end() ? it->second : kUnknown;
}

// Clocks of restored pods, by wasm3 runtime. Empty unless a pod was
// restored here, and then a few entries: a pod that started here reads the
// node's clock without taking the lock.
static std::mutex s_clock_mutex;
static std::vector<std::pair<const void*, int64_t>> s_clock_offsets;
static std::atomic<size_t> s_clock_count{0};

int64_t pod_clock_offset_ms(const void* rt) {
    if (s_clock_count.load(std::memory_order_acquire) == 0) return 0;
    std::lock_guard<std::mutex> lock(s_clock_mutex);
    for (const auto& c : s_clock_offsets) if (c.first == rt) return c.second;
    return 0;
}

void set_pod_clock_offset(const void* rt, int64_t offset) {
    std::lock_guard<std::mutex> lock(s_clock_mutex);
    auto it = std::find_if(s_clock_offsets.begin(), s_clock_offsets.end(), [rt](const std::pair<const void*, int64_t>& c) { return c.first == rt; });
    if (it != s_clock_offsets.end()) s_clock_offsets.erase(it);
    if (offset != 0) s_clock_offsets.emplace_back(rt, offset);
    s_clock_count.store(s_clock_offsets.size(), std::memory_order_release);
}

const std::string& app_pod_id(IM3Runtime runtime) { return pod_for(runtime, nullptr); }

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
    begin_response(c, status, ctype);
    return m3Err_none;
}

static const void* raw_resp_header(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
    uint32_t noff = static_cast<uint32_t>(sp[0]);
    uint32_t nlen = static_cast<uint32_t>(sp[1]);
    uint32_t voff = static_cast<uint32_t>(sp[2]);
    uint32_t vlen = static_cast<uint32_t>(sp[3]);
    CallContext* c = ctx_for(rt);
    if (!c || !c->sink || c->ended) return m3Err_none;
    if (nlen == 0 || nlen > 64 || vlen > K32_MAX_HEADER_BYTES) return m3Err_none;
    if (!guest_range_ok(mem, noff, nlen) || !guest_range_ok(mem, voff, vlen)) return m3Err_none;
    std::string_view name(static_cast<const char*>(mem) + noff, nlen);
    std::string_view value(static_cast<const char*>(mem) + voff, vlen);
    if (name.find_first_of("\r\n:") != std::string_view::npos || value.find_first_of("\r\n") != std::string_view::npos) return m3Err_none;
    if (c->began) {
        if (!value.empty()) c->sink->header(name, value);
        return m3Err_none;
    }
    // Kept for k32_resp_begin or k32_resp_asset; an empty value withdraws
    // the headers of that name set earlier (names compare without case)
    auto same = [&](const std::string& n) {
        if (n.size() != name.size()) return false;
        for (size_t i = 0; i < n.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(n[i])) != std::tolower(static_cast<unsigned char>(name[i]))) return false;
        }
        return true;
    };
    if (value.empty()) {
        c->pending_headers.erase(std::remove_if(c->pending_headers.begin(), c->pending_headers.end(),
                                                [&](const std::pair<std::string, std::string>& h) { return same(h.first); }),
                                 c->pending_headers.end());
    } else if (c->pending_headers.size() < kMaxPendingHeaders) {
        c->pending_headers.emplace_back(std::string(name), std::string(value));
    }
    return m3Err_none;
}

static const void* raw_resp_write(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
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
    const std::string& pod_id = pod_for(rt, ictx);
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

static const void* raw_time_ms(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void*) {
    *reinterpret_cast<int64_t*>(sp) = static_cast<int64_t>(osal::System::get_uptime_ms()) + pod_clock_offset_ms(rt);
    return m3Err_none;
}

static const void* raw_unix_time_ms(IM3Runtime, IM3ImportContext, uint64_t* sp, void*) {
    *reinterpret_cast<int64_t*>(sp) = static_cast<int64_t>(osal::System::get_unix_time_ms());
    return m3Err_none;
}

static const void* raw_log(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void* mem) {
    uint32_t off = static_cast<uint32_t>(sp[0]);
    uint32_t len = static_cast<uint32_t>(sp[1]);
    if (len > 512 || !guest_range_ok(mem, off, len)) return m3Err_none;
    const std::string& pod_id = pod_for(rt, ictx);
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
    if (klen == 0 || klen > 64 || !guest_range_ok(mem, koff, klen) || !guest_range_ok(mem, ooff, ocap)) {
        *reinterpret_cast<int32_t*>(sp) = hal::HAL_ERR_FAULT;
        return m3Err_none;
    }
    const std::string& pod_id = pod_for(rt, ictx);
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

// k32_resp_asset(path, path_len, status) -> 0 or K32_ERR_*
static const void* raw_resp_asset(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void* mem) {
    uint32_t off = static_cast<uint32_t>(sp[1]);
    uint32_t len = static_cast<uint32_t>(sp[2]);
    uint32_t status = static_cast<uint32_t>(sp[3]);
    int32_t& ret = *reinterpret_cast<int32_t*>(sp);
    CallContext* c = ctx_for(rt);
    if (!c || !c->sink || c->began) { ret = K32_ERR_INVAL; return m3Err_none; }   // only as the whole response
    if (len == 0 || len > K32_FS_MAX_PATH || !guest_range_ok(mem, off, len)) {
        ret = (len > K32_FS_MAX_PATH) ? K32_ERR_NAMETOOLONG : K32_ERR_INVAL;
        return m3Err_none;
    }
    if (status == 0) status = 200;
    if (status < 100 || status > 599) { ret = K32_ERR_INVAL; return m3Err_none; }
    AppAssetSource src = AppPodHost::instance().assets(pod_for(rt, ictx));
    if (src.empty()) { ret = K32_ERR_NOENT; return m3Err_none; }
    // The sink is driven directly: the CallContext only records that the
    // response is complete
    ret = send_bundle_file(src, std::string_view(static_cast<const char*>(mem) + off, len), static_cast<int>(status), c->accepts_gzip,
                           c->pending_headers, *c->sink);
    if (ret == 0) {
        c->pending_headers.clear();
        c->began = true;
        c->ended = true;
        c->status = static_cast<int>(status);
    }
    return m3Err_none;
}

// k32_http_call(method, method_len, url, url_len, headers, headers_len, body,
//               body_len, resp, resp_cap, resp_len_ptr, timeout_ms) -> status or K32_ERR_*
static const void* raw_http_call(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void* mem) {
    uint32_t a[12];
    for (int i = 0; i < 12; ++i) a[i] = static_cast<uint32_t>(sp[i + 1]);
    int32_t& ret = *reinterpret_cast<int32_t*>(sp);
    const std::string& pod_id = pod_for(rt, ictx);
    set_http_status(rt, 0);
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::ServiceCall, 0, hal::AccessMode::ReadWrite)) {
        ret = K32_ERR_PERM;
        return m3Err_none;
    }
    const uint32_t mlen = a[1], ulen = a[3], hlen = a[5], blen = a[7], rcap = a[9];
    if (mlen == 0 || mlen > 16 || ulen == 0 || ulen > 512 || hlen > 2048 || blen > K32_MAX_REQUEST_BYTES ||
        !guest_range_ok(mem, a[0], mlen) || !guest_range_ok(mem, a[2], ulen) || !guest_range_ok(mem, a[4], hlen) ||
        !guest_range_ok(mem, a[6], blen) || !guest_range_ok(mem, a[8], rcap) || !guest_range_ok(mem, a[10], 4)) {
        ret = K32_ERR_FAULT;
        return m3Err_none;
    }
    if (!s_hooks.service_call) { ret = K32_ERR_IO; return m3Err_none; }
    const char* base = static_cast<const char*>(mem);
    CallContext* c = ctx_for(rt);
    // The caller is suspended in this call, so its memory cannot move or grow
    BufferResponseSink sink(static_cast<uint8_t*>(mem) + a[8], rcap);
    // An exception must not unwind through the interpreter's C frames (that
    // ends the node in std::terminate): a heap that runs out fails the call
    int32_t rc;
    try {
        std::string chain = c ? c->chain : pod_id;   // k32_init / k32_tick: the pod alone
        ServiceCallRequest req;
        req.caller_pod = pod_id;
        req.method = std::string_view(base + a[0], mlen);
        req.url = std::string_view(base + a[2], ulen);
        req.headers = std::string_view(base + a[4], hlen);
        req.body = std::string_view(base + a[6], blen);
        req.call_chain = chain;
        req.timeout_ms = a[11] ? (a[11] > 60000 ? 60000 : a[11]) : 5000;
        req.caller_runtime = rt;
        rc = s_hooks.service_call(req, sink);
    } catch (const std::bad_alloc&) {
        rc = K32_ERR_NOMEM;
    } catch (const std::exception&) {
        rc = K32_ERR_IO;
    }
    if (rc >= 0 && sink.overflow()) rc = K32_ERR_TOOBIG;
    set_http_status(rt, sink.status());
    uint32_t copied = static_cast<uint32_t>(sink.copied());
    std::memcpy(static_cast<uint8_t*>(mem) + a[10], &copied, 4);
    ret = rc;
    if (runtime_stop_requested(rt)) return m3Err_trapOutOfGas;   // being stopped: end the guest here
    return m3Err_none;
}

// k32_http_status() -> the status of the answer to the pod's last
// k32_http_call (also when its body did not fit), 0 when none arrived
static const void* raw_http_status(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void*) {
    std::lock_guard<std::mutex> lock(s_ctx_mutex);
    auto it = s_http_status_by_runtime.find(rt);
    *reinterpret_cast<int32_t*>(sp) = it != s_http_status_by_runtime.end() ? it->second : 0;
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
    const std::string& pod_id = pod_for(rt, ictx);
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
    const std::string& pod_id = pod_for(rt, ictx);
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
    const std::string& pod_id = pod_for(rt, ictx);
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
    const std::string& pod_id = pod_for(rt, ictx);
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::Keyboard, 0, hal::AccessMode::Read)) {
        *reinterpret_cast<int32_t*>(sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }
    int32_t key = s_hooks.key_read ? s_hooks.key_read() : hal::HAL_ERR_NODEV;
    *reinterpret_cast<int32_t*>(sp) = key;
    return m3Err_none;
}

static const void* raw_tone(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void*) {
    const std::string& pod_id = pod_for(rt, ictx);
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::Speaker, 0, hal::AccessMode::Write)) {
        *reinterpret_cast<int32_t*>(sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }
    uint32_t freq = static_cast<uint32_t>(sp[1]);
    uint32_t ms = static_cast<uint32_t>(sp[2]);
    uint32_t vol = static_cast<uint32_t>(sp[3]);
    if (runtime_stop_requested(rt)) return m3Err_trapOutOfGas;   // being stopped: end here, no new sound
    if (ms > 2000) ms = 2000;
    if (vol > 100) vol = 100;
    *reinterpret_cast<int32_t*>(sp) = s_hooks.tone ? s_hooks.tone(freq, ms, vol) : hal::HAL_ERR_NODEV;
    return m3Err_none;
}

// k32_audio_play(samples, count, sample_rate, volume_pct) -> count or K32_ERR_*
static const void* raw_audio_play(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void* mem) {
    const std::string& pod_id = pod_for(rt, ictx);
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::Speaker, 0, hal::AccessMode::Write)) {
        *reinterpret_cast<int32_t*>(sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }
    const uint32_t off = static_cast<uint32_t>(sp[1]), count = static_cast<uint32_t>(sp[2]);
    const uint32_t rate = static_cast<uint32_t>(sp[3]), vol = static_cast<uint32_t>(sp[4]);
    if (count > 48000 || !guest_range_ok(mem, off, count * 2)) {
        *reinterpret_cast<int32_t*>(sp) = K32_ERR_FAULT;
        return m3Err_none;
    }
    if (runtime_stop_requested(rt)) return m3Err_trapOutOfGas;   // being stopped: end here, no new sound
    *reinterpret_cast<int32_t*>(sp) = s_hooks.audio_play
        ? s_hooks.audio_play(reinterpret_cast<const int16_t*>(static_cast<uint8_t*>(mem) + off), count, rate, vol)
        : hal::HAL_ERR_NODEV;
    return m3Err_none;
}

// k32_mic_read(out, max_samples, timeout_ms) -> samples or K32_ERR_*
static const void* raw_mic_read(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void* mem) {
    const std::string& pod_id = pod_for(rt, ictx);
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::Microphone, 0, hal::AccessMode::Read)) {
        *reinterpret_cast<int32_t*>(sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }
    const uint32_t off = static_cast<uint32_t>(sp[1]);
    uint32_t max = static_cast<uint32_t>(sp[2]);
    const uint32_t timeout = static_cast<uint32_t>(sp[3]);
    if (max > 16000) max = 16000;
    if (!guest_range_ok(mem, off, max * 2)) {
        *reinterpret_cast<int32_t*>(sp) = K32_ERR_FAULT;
        return m3Err_none;
    }
    if (runtime_stop_requested(rt)) return m3Err_trapOutOfGas;
    *reinterpret_cast<int32_t*>(sp) = s_hooks.mic_read
        ? s_hooks.mic_read(reinterpret_cast<int16_t*>(static_cast<uint8_t*>(mem) + off), max, timeout)
        : hal::HAL_ERR_NODEV;
    return m3Err_none;
}

// Pod storage imports ----------------------------------------------------------
// Permission is checked inside PodFs on every call; these only marshal guest
// memory. Guest pointers are validated against linear memory before use.
static const void* raw_fs_open(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void* mem) {
    uint32_t off = static_cast<uint32_t>(sp[1]);
    uint32_t len = static_cast<uint32_t>(sp[2]);
    uint32_t flags = static_cast<uint32_t>(sp[3]);
    if (len > K32_FS_MAX_PATH || !guest_range_ok(mem, off, len)) {
        *reinterpret_cast<int32_t*>(sp) = (len > K32_FS_MAX_PATH) ? K32_ERR_NAMETOOLONG : K32_ERR_FAULT;
        return m3Err_none;
    }
    const std::string& pod_id = pod_for(rt, ictx);
    *reinterpret_cast<int32_t*>(sp) = PodFs::instance().open(pod_id, std::string_view(static_cast<const char*>(mem) + off, len), flags);
    return m3Err_none;
}

static const void* raw_fs_read(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void* mem) {
    int32_t fd = static_cast<int32_t>(sp[1]);
    uint32_t off = static_cast<uint32_t>(sp[2]);
    uint32_t len = static_cast<uint32_t>(sp[3]);
    if (len > K32_FS_MAX_IO) len = K32_FS_MAX_IO;
    if (!guest_range_ok(mem, off, len)) {
        *reinterpret_cast<int32_t*>(sp) = K32_ERR_FAULT;
        return m3Err_none;
    }
    const std::string& pod_id = pod_for(rt, ictx);
    *reinterpret_cast<int32_t*>(sp) = PodFs::instance().read(pod_id, fd, static_cast<uint8_t*>(mem) + off, len);
    return m3Err_none;
}

static const void* raw_fs_write(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void* mem) {
    int32_t fd = static_cast<int32_t>(sp[1]);
    uint32_t off = static_cast<uint32_t>(sp[2]);
    uint32_t len = static_cast<uint32_t>(sp[3]);
    if (len > K32_FS_MAX_IO) len = K32_FS_MAX_IO;
    if (!guest_range_ok(mem, off, len)) {
        *reinterpret_cast<int32_t*>(sp) = K32_ERR_FAULT;
        return m3Err_none;
    }
    const std::string& pod_id = pod_for(rt, ictx);
    *reinterpret_cast<int32_t*>(sp) = PodFs::instance().write(pod_id, fd, static_cast<const uint8_t*>(mem) + off, len);
    return m3Err_none;
}

static const void* raw_fs_close(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void*) {
    int32_t fd = static_cast<int32_t>(sp[1]);
    const std::string& pod_id = pod_for(rt, ictx);
    *reinterpret_cast<int32_t*>(sp) = PodFs::instance().close(pod_id, fd);
    return m3Err_none;
}

static const void* raw_fs_size(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void*) {
    int32_t fd = static_cast<int32_t>(sp[1]);
    const std::string& pod_id = pod_for(rt, ictx);
    *reinterpret_cast<int32_t*>(sp) = PodFs::instance().size(pod_id, fd);
    return m3Err_none;
}

static const void* raw_fs_list(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void* mem) {
    uint32_t doff = static_cast<uint32_t>(sp[1]);
    uint32_t dlen = static_cast<uint32_t>(sp[2]);
    uint32_t ooff = static_cast<uint32_t>(sp[3]);
    uint32_t ocap = static_cast<uint32_t>(sp[4]);
    if (ocap > K32_FS_MAX_IO) ocap = K32_FS_MAX_IO;
    if (dlen > K32_FS_MAX_PATH || !guest_range_ok(mem, doff, dlen) || !guest_range_ok(mem, ooff, ocap)) {
        *reinterpret_cast<int32_t*>(sp) = (dlen > K32_FS_MAX_PATH) ? K32_ERR_NAMETOOLONG : K32_ERR_FAULT;
        return m3Err_none;
    }
    const std::string& pod_id = pod_for(rt, ictx);
    *reinterpret_cast<int32_t*>(sp) = PodFs::instance().list(pod_id, std::string_view(static_cast<const char*>(mem) + doff, dlen),
                                                             static_cast<char*>(mem) + ooff, ocap);
    return m3Err_none;
}

static const void* raw_fs_remove(IM3Runtime rt, IM3ImportContext ictx, uint64_t* sp, void* mem) {
    uint32_t off = static_cast<uint32_t>(sp[1]);
    uint32_t len = static_cast<uint32_t>(sp[2]);
    if (len > K32_FS_MAX_PATH || !guest_range_ok(mem, off, len)) {
        *reinterpret_cast<int32_t*>(sp) = (len > K32_FS_MAX_PATH) ? K32_ERR_NAMETOOLONG : K32_ERR_FAULT;
        return m3Err_none;
    }
    const std::string& pod_id = pod_for(rt, ictx);
    *reinterpret_cast<int32_t*>(sp) = PodFs::instance().remove(pod_id, std::string_view(static_cast<const char*>(mem) + off, len));
    return m3Err_none;
}

void unlink_app_host_functions(IM3Runtime runtime) {
    if (s_clock_count.load(std::memory_order_acquire)) set_pod_clock_offset(runtime, 0);
    std::lock_guard<std::mutex> lock(s_ctx_mutex);
    s_pod_by_runtime.erase(runtime);
    s_http_status_by_runtime.erase(runtime);
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
    m3_LinkRawFunction(module, "env", "k32_unix_time_ms", "I()",        &raw_unix_time_ms);
    m3_LinkRawFunction(module, "env", "k32_log",          "v(ii)",      &raw_log);
    m3_LinkRawFunction(module, "env", "k32_random",       "i()",        &raw_random);
    m3_LinkRawFunction(module, "env", "k32_env_get",      "i(iiii)",    &raw_env_get);
    m3_LinkRawFunction(module, "env", "k32_mem_stats",    "i(ii)",      &raw_mem_stats);
    m3_LinkRawFunction(module, "env", "k32_resp_asset",   "i(iii)",     &raw_resp_asset);
    m3_LinkRawFunction(module, "env", "k32_http_call",    "i(iiiiiiiiiiii)", &raw_http_call);
    m3_LinkRawFunction(module, "env", "k32_http_status",  "i()",        &raw_http_status);
    m3_LinkRawFunction(module, "env", "k32_display_text", "i(iiiiii)",  &raw_display_text);
    m3_LinkRawFunction(module, "env", "k32_display_rect", "i(iiiii)",   &raw_display_rect);
    m3_LinkRawFunction(module, "env", "k32_display_bar",  "i(iiiiiii)", &raw_display_bar);
    m3_LinkRawFunction(module, "env", "k32_key_read",     "i()",        &raw_key_read);
    m3_LinkRawFunction(module, "env", "k32_tone",         "i(iii)",     &raw_tone);
    m3_LinkRawFunction(module, "env", "k32_audio_play",   "i(iiii)",    &raw_audio_play);
    m3_LinkRawFunction(module, "env", "k32_mic_read",     "i(iii)",     &raw_mic_read);
    m3_LinkRawFunction(module, "env", "k32_fs_open",      "i(iii)",     &raw_fs_open);
    m3_LinkRawFunction(module, "env", "k32_fs_read",      "i(iii)",     &raw_fs_read);
    m3_LinkRawFunction(module, "env", "k32_fs_write",     "i(iii)",     &raw_fs_write);
    m3_LinkRawFunction(module, "env", "k32_fs_close",     "i(i)",       &raw_fs_close);
    m3_LinkRawFunction(module, "env", "k32_fs_size",      "i(i)",       &raw_fs_size);
    m3_LinkRawFunction(module, "env", "k32_fs_list",      "i(iiii)",    &raw_fs_list);
    m3_LinkRawFunction(module, "env", "k32_fs_remove",    "i(ii)",      &raw_fs_remove);
    link_linalg_host_functions(module);
    link_blob_host_functions(module);
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

void AppPodHost::set_event_topics(const std::string& pod_id, const std::vector<std::string>& topics) {
    osal::LockGuard guard(mutex_);
    if (topics.empty()) event_topics_.erase(pod_id);
    else event_topics_[pod_id] = topics;
    sync_topic_subscriptions_locked();
}

void AppPodHost::sync_topic_subscriptions_locked() {
    std::vector<std::string> wanted;
    for (const auto& kv : event_topics_) {
        auto pod = pods_.find(kv.first);
        if (pod == pods_.end() || !pod->second.has_event) continue;
        for (const auto& t : kv.second) {
            if (std::find(wanted.begin(), wanted.end(), t) == wanted.end()) wanted.push_back(t);
        }
    }
    for (auto it = topic_subs_.begin(); it != topic_subs_.end();) {
        if (std::find(wanted.begin(), wanted.end(), it->first) == wanted.end()) {
            ipc::ChannelManager::instance().unsubscribe(it->second);
            it = topic_subs_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& t : wanted) {
        if (topic_subs_.count(t)) continue;
        // Runs on the channel dispatcher thread
        topic_subs_[t] = ipc::ChannelManager::instance().subscribe(t, [this](std::string_view topic, uint32_t, const uint8_t* data, size_t len) {
            deliver_topic(topic, data, len);
        });
    }
    {
        osal::LockGuard guard(event_mutex_);
        event_thread_wanted_ = !wanted.empty();
    }
    if (wanted.empty()) event_cv_.notify_all();
    else start_event_thread();
}

// Runs on the channel dispatcher thread, whose stack is too small for the
// interpreter: the message is queued for the event thread.
void AppPodHost::deliver_topic(std::string_view topic, const uint8_t* data, size_t len) {
    if (len > K32_MAX_EVENT_BYTES) return;
    // Each message is built while the pods' table is locked, and queued
    // without a lock on it
    const std::string_view body(reinterpret_cast<const char*>(data), len);
    std::vector<PendingEvent> ready;
    {
        osal::LockGuard guard(mutex_);
        for (const auto& kv : event_topics_) {
            auto pod = pods_.find(kv.first);
            if (pod == pods_.end() || !pod->second.has_event) continue;
            for (const auto& t : kv.second) {
                if (t == topic) { ready.emplace_back(kv.first, topic, body, pod->second.worker); break; }
            }
        }
    }
    if (ready.empty()) return;
    const size_t cap = (osal::System::get_total_psram_bytes() > 0 ? kMaxQueuedEvents : kMaxQueuedEventsSmall) * std::max<size_t>(1, worker_n_.load());
    {
        osal::LockGuard guard(event_mutex_);
        for (auto& ev : ready) {
            if (event_queue_.size() >= cap) {
                event_queue_.pop_front();   // a slow handler loses the oldest message, not the newest
                if (events_dropped_++ < 4) printf("[POD] event queue full, dropping the oldest message\n");
            }
            event_queue_.push_back(std::move(ev));
        }
    }
    start_event_thread();   // normally running already; retried here if it could not start
    // The workers wait on the one condition, each for its own pods' events
    if (worker_n_.load()) event_cv_.notify_all();
    else event_cv_.notify_one();
}

// The event thread runs while some pod subscribes to a topic: it is started
// with the first subscription (so a pod that cannot get one fails visibly at
// start, not message by message) and ends when the last one goes.
void AppPodHost::start_event_thread() {
    {
        osal::LockGuard guard(event_mutex_);
        if (event_thread_running_ || !event_thread_wanted_ || worker_n_.load()) return;   // workers deliver events
        event_thread_running_ = true;
    }
    osal::Thread::Config cfg;
    cfg.name = "k32_events";
#if defined(ESP_PLATFORM)
    const bool psram = osal::System::get_total_psram_bytes() > 0;
    cfg.stack_size = event_stack_bytes_ ? event_stack_bytes_ : (psram ? 32768 : 16384);
    cfg.stack_in_psram = event_stack_bytes_ ? event_stack_in_psram_ : psram;
#else
    cfg.stack_size = event_stack_bytes_ ? event_stack_bytes_ : 262144;
#endif
    cfg.priority = 4;
    cfg.core_affinity = 1;
    if (!osal::Thread::create([](void* self) { static_cast<AppPodHost*>(self)->event_thread_main(); }, this, cfg)) {
        printf("[POD] cannot start the event thread (%u byte stack); queued messages wait for the next attempt\n", (unsigned)cfg.stack_size);
        osal::LockGuard guard(event_mutex_);
        event_thread_running_ = false;
    }
}

void AppPodHost::event_thread_main() {
    for (;;) {
        PendingEvent ev;
        {
            osal::UniqueLock<osal::Mutex> lock(event_mutex_);
            event_cv_.wait_for(lock, 1000, [this]() { return !event_queue_.empty() || !event_thread_wanted_ || worker_n_.load(); });
            if (event_queue_.empty() || worker_n_.load()) {
                if (!event_thread_wanted_ || worker_n_.load()) {
                    event_thread_running_ = false;
                    return;
                }
                continue;
            }
            ev = std::move(event_queue_.front());
            event_queue_.pop_front();
        }
        try {
            event(std::string(ev.pod_id()), ev.topic(), reinterpret_cast<const uint8_t*>(ev.data().data()), ev.data().size());
        } catch (const std::exception& e) {
            // out of memory: this event is dropped, the next one is delivered
            printf("[POD] event delivery skipped: %s\n", e.what());
        }
        osal::System::feed_watchdog();
    }
}

void AppPodHost::set_event_thread_stack(size_t bytes, bool in_psram) {
    osal::LockGuard guard(event_mutex_);
    event_stack_bytes_ = bytes;
    event_stack_in_psram_ = in_psram;
}

uint64_t AppPodHost::events_dropped() const {
    osal::LockGuard guard(event_mutex_);
    return events_dropped_;
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
    // The tick thread may look at the pod between this and init_pod: its
    // first tick waits for k32_init to return
    e.ticking = !e.has_init;
    e.call_mutex = std::make_unique<osal::Mutex>();
    osal::LockGuard guard(mutex_);
    auto interval = tick_intervals_.find(pod_id);
    if (interval != tick_intervals_.end()) e.tick_interval_ms = interval->second;
    auto core = core_requests_.find(pod_id);
    if (core != core_requests_.end()) e.core_request = core->second;
    e.group = ++group_seq_;
    e.worker = place_locked(e.core_request);
    if (worker_n_.load() > 1 && (e.has_tick || e.has_event)) {
        printf("[POD] %s: core %d (%s)\n", pod_id.c_str(), workers_[e.worker]->opts.core, e.core_request >= 0 ? "kube32.io/core" : "least busy");
    }
    pods_[pod_id] = std::move(e);
    sync_topic_subscriptions_locked();
    return true;
}

void AppPodHost::set_assets(const std::string& pod_id, AppAssetSource src) {
    osal::LockGuard guard(mutex_);
    if (src.empty()) assets_.erase(pod_id);
    else assets_[pod_id] = std::move(src);
}

AppAssetSource AppPodHost::assets(const std::string& pod_id) const {
    osal::LockGuard guard(mutex_);
    auto it = assets_.find(pod_id);
    return it == assets_.end() ? AppAssetSource{} : it->second;
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
        assets_.erase(pod_id);
        event_topics_.erase(pod_id);
        tick_intervals_.erase(pod_id);
        core_requests_.erase(pod_id);
        migration_.erase(pod_id);
        sync_topic_subscriptions_locked();
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
    std::vector<std::string> out;
    list_pods(out);
    return out;
}

void AppPodHost::list_pods(std::vector<std::string>& out) const {
    osal::LockGuard guard(mutex_);
    // Strings past the current count keep their buffers for later lists
    size_t n = 0;
    for (const auto& kv : pods_) {
        if (n < out.size()) out[n].assign(kv.first);
        else out.push_back(kv.first);
        ++n;
    }
    out.resize(n);
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
    // The call that has run longest
    std::lock_guard<std::mutex> lk(s_inflight_mutex);
    const InFlight* oldest = nullptr;
    for (const auto& f : s_inflight) {
        if (!f.what.empty() && (!oldest || f.since < oldest->since)) oldest = &f;
    }
    out_age_ms = oldest ? static_cast<uint32_t>(osal::System::get_uptime_ms() - oldest->since) : 0;
    return oldest ? oldest->what : std::string();
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
    int32_t init_rc = call_export(pod_id, ctrl, K32_EXPORT_INIT);
    if (init_rc == 0) {
        osal::LockGuard guard(mutex_);
        if (Entry* e = find_locked(pod_id)) {
            e->ticking = true;
            e->next_tick_ms = 0;
        }
    }
    if (init_rc == 0) wake_workers();   // its first tick is due now
    return init_rc;
}

// Copies a block into guest memory via k32_alloc and calls `fn(ptr,len)` or
// `fn(topic_ptr, topic_len, ptr, len)` for events.
int32_t AppPodHost::call_with_block(Entry& e, const std::string& pod_id, const char* fn, std::string_view topic,
                                    const uint8_t* data, size_t len, AppResponseSink* sink, const AppRequest* req) {
    ContainerLifecycleController* ctrl = e.ctrl;
    if (!e.has_alloc) return -1;

    // The call's CPU time goes to the pod; a pod it calls on this thread is
    // charged its own
    struct Charge {
        AppPodHost* host;
        const std::string& id;
        PodCpuRegion cpu;
        ~Charge() { host->add_cpu(id, cpu.finish()); }
    } charge{this, pod_id};
    uint32_t total = static_cast<uint32_t>(len + topic.size());
    set_busy(pod_id, fn);
    struct Idle { AppPodHost* h; const std::string& id; ~Idle() { h->set_busy(id, nullptr); } } idle{this, pod_id};
    arm_call_budget(ctrl);
    int32_t ptr = ctrl->call_function(K32_EXPORT_ALLOC, {static_cast<int32_t>(total + 1)});
    arm_call_budget(ctrl);   // the entry point itself gets a full budget too
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
    if (req) {
        ctx.accepts_gzip = req->accepts_gzip;
        ctx.chain.reserve(req->call_chain.size() + pod_id.size() + 1);
        ctx.chain.assign(req->call_chain.data(), req->call_chain.size());
        if (!ctx.chain.empty()) ctx.chain.push_back(',');
    }
    ctx.chain.append(pod_id);
    IM3Runtime rt = static_cast<IM3Runtime>(ctrl->m3_runtime_handle());
    {
        std::lock_guard<std::mutex> lock(s_ctx_mutex);
        s_ctx_by_runtime[rt] = &ctx;
    }
    uint64_t t_call = osal::Thread::get_time_us();
    std::string outer_inflight;   // a nested call (service call) restores its caller's entry
    uint64_t outer_since = 0;
    {
        std::lock_guard<std::mutex> lk(s_inflight_mutex);
        InFlight& f = inflight_locked();
        outer_inflight.swap(f.what);
        outer_since = f.since;
        f.what.assign(pod_id);
        f.what.append(" ");
        f.what.append(fn);
        f.what.append(" ");
        f.what.append(reinterpret_cast<const char*>(data), len > 48 ? 48 : len);
        f.since = osal::System::get_uptime_ms();
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
    note_call_cost(ctrl, fn);

    {
        std::lock_guard<std::mutex> lock(s_ctx_mutex);
        s_ctx_by_runtime.erase(rt);
    }
    uint32_t call_us = static_cast<uint32_t>(osal::Thread::get_time_us() - t_call);
    {
        std::lock_guard<std::mutex> lk(s_inflight_mutex);
        InFlight& f = inflight_locked();
        f.what.swap(outer_inflight);
        f.since = outer_since;
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
    return dispatch(pod_id, req, sink, 0, false);
}

int32_t AppPodHost::call_nested(const std::string& pod_id, const AppRequest& req, AppResponseSink& sink, uint32_t timeout_ms) {
    return dispatch(pod_id, req, sink, timeout_ms ? timeout_ms : 1, true);
}

static void send_error(AppResponseSink& sink, int status, const char* body) {
    sink.begin(status, "application/json");
    sink.write(reinterpret_cast<const uint8_t*>(body), std::strlen(body));
    sink.end();
}

// A frozen pod (a checkpoint or a move in progress) asks the client to retry
static int32_t send_frozen(AppResponseSink& sink) {
    static const char kBody[] = "{\"error\":\"unavailable\",\"message\":\"the pod is frozen for a checkpoint\"}";
    sink.begin(503, "application/json");
    sink.header("Retry-After", "1");
    sink.write(reinterpret_cast<const uint8_t*>(kBody), sizeof(kBody) - 1);
    sink.end();
    return 503;
}

int32_t AppPodHost::dispatch(const std::string& pod_id, const AppRequest& req, AppResponseSink& sink, uint32_t wait_ms, bool nested) {
    Entry* e = nullptr;
    osal::Mutex* cm = nullptr;
    bool stack_held_by_chain = false;
    {
        osal::LockGuard guard(mutex_);
        e = find_locked(pod_id);
        if (!e || !e->has_handle) return nested ? K32_ERR_NOENT : -2;
        if (e->frozen) return send_frozen(sink);
        cm = e->call_mutex.get();
        // The caller, last in the chain, and this pod go to one worker
        if (nested && worker_n_.load() > 1 && !req.call_chain.empty()) {
            const size_t comma = req.call_chain.rfind(',');
            const std::string caller(req.call_chain.substr(comma == std::string_view::npos ? 0 : comma + 1));
            link_locked(caller, pod_id, osal::System::get_uptime_ms());
        }
        // A pod of this node earlier in the chain waits in a service call on
        // the node's one interpreter stack: this call could only run once
        // that one had timed out
        if (!nested && kube32_interp_stack_active()) {
            for (size_t start = 0; start < req.call_chain.size() && !stack_held_by_chain;) {
                size_t end = req.call_chain.find(',', start);
                if (end == std::string_view::npos) end = req.call_chain.size();
                const Entry* c = find_locked(std::string(req.call_chain.substr(start, end - start)));
                stack_held_by_chain = c && c->has_handle;
                start = end + 1;
            }
        }
    }
    if (stack_held_by_chain && !chain_contains(req.call_chain, pod_id)) {
        send_error(sink, 508, "{\"error\":\"loop_detected\",\"message\":\"a pod of this node earlier in the call chain holds its interpreter\"}");
        return 508;
    }
    if (chain_contains(req.call_chain, pod_id)) {
        // The pod is already serving this request further up the chain:
        // entering it again would wait on itself
        if (nested) return K32_ERR_LOOP;
        send_error(sink, 508, "{\"error\":\"loop_detected\",\"message\":\"the pod is already serving this call chain\"}");
        return 508;
    }
    if (nested) {
        // The callee interprets on this thread's stack, below the caller
        const uint8_t* base = static_cast<const uint8_t*>(m3_HostStackBase());
        const uint8_t* here = static_cast<const uint8_t*>(__builtin_frame_address(0));
        if (base && base < here && static_cast<size_t>(here - base) < nested_stack_reserve_) {
            printf("[APP] %s: in-process call refused, %u bytes of stack left (%u needed)\n", pod_id.c_str(),
                   (unsigned)(here - base), (unsigned)nested_stack_reserve_);
            return K32_ERR_NOMEM;
        }
    }
    std::string block = encode_app_request(req);
    if (block.size() > K32_MAX_REQUEST_BYTES) {
        send_error(sink, 413, "{\"error\":\"too_large\",\"message\":\"request exceeds 16 KiB\"}");
        return 413;
    }

    uint64_t t0 = osal::Thread::get_time_us();
    int32_t status;
    {
        // A nested call waits for a busy pod at most its timeout: two pods
        // calling each other from two threads time out instead of deadlocking
        if (wait_ms == 0) cm->lock();
        else if (!cm->try_lock(wait_ms)) return K32_ERR_TIMEOUT;
        struct Unlock { osal::Mutex* m; ~Unlock() { m->unlock(); } } call_guard{cm};
        // Entry may have been unregistered (or frozen) while we waited
        {
            osal::LockGuard guard(mutex_);
            e = find_locked(pod_id);
            if (!e) return nested ? K32_ERR_NOENT : -2;
            if (e->frozen) return send_frozen(sink);
        }
        status = call_with_block(*e, pod_id, K32_EXPORT_HANDLE, std::string_view(), reinterpret_cast<const uint8_t*>(block.data()), block.size(), &sink, &req);
    }
    if (nested && status < 0) status = 500;   // the callee trapped or gave no response
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
    int32_t rc;
    uint64_t cpu_us = 0;
    {
        osal::LockGuard call_guard(*cm);
        int64_t offset = 0;
        {
            osal::LockGuard guard(mutex_);
            Entry* e = find_locked(pod_id);
            if (!e || e->frozen) return 0;
            offset = e->clock_offset_ms;
            e->busy_fn = K32_EXPORT_TICK;
            e->busy_since_ms = osal::System::get_uptime_ms();
        }
        PodCpuRegion cpu;
        arm_call_budget(ctrl);
        rc = ctrl->call_function(K32_EXPORT_TICK, {static_cast<int32_t>(static_cast<int64_t>(uptime_ms) + offset)});
        note_call_cost(ctrl, K32_EXPORT_TICK);
        cpu_us = cpu.finish();
        set_busy(pod_id, nullptr);
    }
    add_cpu(pod_id, cpu_us);
    return rc;
}

void AppPodHost::set_tick_interval(const std::string& pod_id, uint32_t ms) {
    osal::LockGuard guard(mutex_);
    tick_intervals_[pod_id] = ms ? ms : 1000;
    Entry* e = find_locked(pod_id);
    if (e) e->tick_interval_ms = tick_intervals_[pod_id];
}

uint32_t AppPodHost::tick_due(uint64_t now_ms, uint32_t max_wait_ms) {
    // Called from one thread; strings keep their buffers, so a round allocates nothing
    std::vector<std::string>& due = tick_scratch_;
    size_t n = 0;
    {
        osal::LockGuard guard(mutex_);
        for (const auto& kv : pods_) {
            if (!kv.second.has_tick || !kv.second.ticking || kv.second.frozen || kv.second.next_tick_ms > now_ms) continue;
            if (n < due.size()) due[n].assign(kv.first);
            else due.push_back(kv.first);
            ++n;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const std::string& id = due[i];
        const int32_t rc = tick(id, static_cast<uint32_t>(now_ms));
        osal::LockGuard guard(mutex_);
        Entry* e = find_locked(id);
        if (e) e->next_tick_ms = now_ms + (rc > 0 ? 10 : e->tick_interval_ms);
    }
    uint64_t wait = max_wait_ms;
    osal::LockGuard guard(mutex_);
    for (const auto& kv : pods_) {
        if (!kv.second.has_tick || !kv.second.ticking || kv.second.frozen) continue;
        const uint64_t next = kv.second.next_tick_ms;
        wait = std::min<uint64_t>(wait, next > now_ms ? next - now_ms : 0);
    }
    return static_cast<uint32_t>(wait);
}

int32_t AppPodHost::event(const std::string& pod_id, std::string_view topic, const uint8_t* data, size_t len) {
    Entry* e = nullptr;
    osal::Mutex* cm = nullptr;
    if (topic.empty() || len > K32_MAX_EVENT_BYTES) return K32_ERR_INVAL;
    // A frozen pod holds the event; it is delivered after a thaw or a restore
    auto hold = [&](Entry& f) {
        if (f.held.size() >= kMaxHeldEvents) {
            if (f.held_dropped++ < 4) printf("[POD] %s: frozen with %u events held, dropping one\n", pod_id.c_str(), (unsigned)f.held.size());
            return 0;
        }
        f.held.push_back(HeldEvent{std::string(topic), std::string(reinterpret_cast<const char*>(data), len)});
        return 0;
    };
    {
        osal::LockGuard guard(mutex_);
        e = find_locked(pod_id);
        if (!e || !e->has_event) return -2;
        if (e->frozen) return hold(*e);
        cm = e->call_mutex.get();
    }
    int32_t rc;
    {
        osal::LockGuard call_guard(*cm);
        {
            osal::LockGuard guard(mutex_);
            e = find_locked(pod_id);
            if (!e) return -2;
            if (e->frozen) return hold(*e);
        }
        rc = call_with_block(*e, pod_id, K32_EXPORT_EVENT, topic, data, len, nullptr);
    }
    return rc;
}

// -----------------------------------------------------------------------------
// Migration
// -----------------------------------------------------------------------------
int32_t AppPodHost::call_export(const std::string& pod_id, ContainerLifecycleController* ctrl, const char* fn) {
    set_busy(pod_id, fn);
    PodCpuRegion cpu;
    arm_call_budget(ctrl);
    const int32_t rc = ctrl->call_function(fn);
    note_call_cost(ctrl, fn);
    add_cpu(pod_id, cpu.finish());
    set_busy(pod_id, nullptr);
    return rc;
}

void AppPodHost::add_cpu(const std::string& pod_id, uint64_t us) {
    if (us == 0) return;
    osal::LockGuard guard(mutex_);
    if (Entry* e = find_locked(pod_id)) {
        e->stats.cpu_us += us;
        if (t_worker >= 0) e->worker_cpu_us += us;   // what the balance weighs
    }
}

void AppPodHost::set_busy(const std::string& pod_id, const char* fn) {
    osal::LockGuard guard(mutex_);
    if (Entry* e = find_locked(pod_id)) {
        e->busy_fn = fn;
        if (fn) e->busy_since_ms = osal::System::get_uptime_ms();
    }
}

// -----------------------------------------------------------------------------
// Pod workers
// -----------------------------------------------------------------------------
size_t AppPodHost::start_workers(const std::vector<PodWorkerOptions>& workers) {
    if (worker_n_.load() || workers.empty()) return worker_n_.load();
    {
        osal::LockGuard guard(event_mutex_);
        workers_stop_ = false;
    }
    // No worker reads the list before worker_n_ counts it
    workers_.reserve(workers.size());
    for (const auto& o : workers) {
        auto w = std::make_unique<Worker>();
        w->host = this;
        w->index = workers_.size();
        w->opts = o;
        osal::Thread::Config cfg;
        cfg.name = "k32_pods";
#if defined(ESP_PLATFORM)
        cfg.stack_size = o.stack_bytes ? o.stack_bytes : 8192;
#else
        cfg.stack_size = o.stack_bytes ? o.stack_bytes : 262144;
#endif
        cfg.stack_in_psram = o.stack_in_psram;
        cfg.stack_buffer = o.stack_buffer;
        cfg.priority = o.priority;
        cfg.core_affinity = o.pinned ? o.core : -1;
        w->opts.stack_bytes = cfg.stack_size;
        Worker* raw = w.get();
        workers_.push_back(std::move(w));
        if (!raw->thread.start([](void* p) { auto* wk = static_cast<Worker*>(p); wk->host->worker_main(*wk); }, raw, cfg)) {
            printf("[POD] cannot start the pod worker for core %d (%u byte stack)\n", o.core, (unsigned)cfg.stack_size);
            workers_.pop_back();
        }
    }
    {
        osal::LockGuard guard(mutex_);
        worker_n_.store(workers_.size());
        // Pods registered before the workers are placed now, their queued
        // events with them
        for (auto& kv : pods_) kv.second.worker = place_locked(kv.second.core_request);
        osal::LockGuard events(event_mutex_);
        for (auto& ev : event_queue_) {
            auto it = pods_.find(std::string(ev.pod_id()));
            if (it != pods_.end()) ev.worker = static_cast<uint16_t>(it->second.worker);
        }
    }
    wake_workers();
    return worker_n_.load();
}

void AppPodHost::stop_workers() {
    {
        osal::LockGuard guard(event_mutex_);
        workers_stop_ = true;
    }
    event_cv_.notify_all();
    for (auto& w : workers_) w->thread.join();
    osal::LockGuard guard(mutex_);
    worker_n_.store(0);
    workers_.clear();
    for (auto& kv : pods_) kv.second.worker = 0;
}

void AppPodHost::set_core(const std::string& pod_id, int core) {
    osal::LockGuard guard(mutex_);
    if (core < 0) core_requests_.erase(pod_id);
    else core_requests_[pod_id] = core;
}

int AppPodHost::pod_core(const std::string& pod_id) const {
    osal::LockGuard guard(mutex_);
    auto it = pods_.find(pod_id);
    if (it == pods_.end() || it->second.worker >= worker_n_.load()) return -1;
    return workers_[it->second.worker]->opts.core;
}

size_t AppPodHost::worker_hint(const std::string& pod_id) const {
    osal::LockGuard guard(mutex_);
    auto it = pods_.find(pod_id);
    return it == pods_.end() ? 0 : it->second.worker;
}

void AppPodHost::wake_workers() {
    if (!worker_n_.load()) return;
    {
        osal::LockGuard guard(event_mutex_);
        for (size_t i = 0; i < worker_n_.load(); ++i) workers_[i]->kick = true;
    }
    event_cv_.notify_all();
}

// The worker of the core asked for; else the one whose pods used the least
// CPU in the last window, then the one with the fewest pods
size_t AppPodHost::place_locked(int core_request) const {
    const size_t n = worker_n_.load();
    if (n < 2) return 0;
    if (core_request >= 0) {
        for (size_t i = 0; i < n; ++i) {
            if (workers_[i]->opts.core == core_request) return i;
        }
    }
    std::vector<size_t> pods(n, 0);
    for (const auto& kv : pods_) {
        if (kv.second.worker < n) pods[kv.second.worker]++;
    }
    size_t best = 0;
    for (size_t i = 1; i < n; ++i) {
        const uint64_t a = workers_[i]->load_us, b = workers_[best]->load_us;
        if (a < b || (a == b && pods[i] < pods[best])) best = i;
    }
    return best;
}

void AppPodHost::move_group_locked(uint32_t group, size_t to, uint64_t now_ms, const char* why) {
    std::vector<std::string> moved;
    for (auto& kv : pods_) {
        Entry& e = kv.second;
        if (e.group != group || e.worker == to) continue;
        e.worker = to;
        e.moved_ms = now_ms;
        moved.push_back(kv.first);
        printf("[POD] %s: core %d (%s)\n", kv.first.c_str(), workers_[to]->opts.core, why);
    }
    if (moved.empty()) return;
    {
        osal::LockGuard guard(event_mutex_);
        for (auto& ev : event_queue_) {
            for (const auto& id : moved) {
                if (ev.pod_id() == id) {
                    ev.worker = static_cast<uint16_t>(to);
                    break;
                }
            }
        }
        workers_[to]->kick = true;
    }
    event_cv_.notify_all();
}

void AppPodHost::link_locked(const std::string& caller, const std::string& callee, uint64_t now_ms) {
    Entry* a = find_locked(caller);
    Entry* b = find_locked(callee);
    // Pods placed by kube32.io/core stay where they are
    if (!a || !b || a->group == b->group || a->core_request >= 0 || b->core_request >= 0) return;
    const uint32_t from = b->group, into = a->group;
    for (auto& kv : pods_) {
        if (kv.second.group == from) kv.second.group = into;
    }
    move_group_locked(into, a->worker, now_ms, "calls in-process");
}

void AppPodHost::balance(uint64_t now_ms) {
    const size_t n = worker_n_.load();
    if (n < 2) return;
    osal::LockGuard guard(mutex_);
    if (balance_at_ms_ != 0 && now_ms - balance_at_ms_ < kBalanceWindowMs) return;
    const uint64_t window_us = balance_at_ms_ ? (now_ms - balance_at_ms_) * 1000 : 0;
    balance_at_ms_ = now_ms;
    struct Group {
        uint32_t id;
        size_t worker;
        uint64_t load;
        bool movable;
    };
    std::vector<Group> groups;
    std::vector<uint64_t> load(n, 0);
    for (auto& kv : pods_) {
        Entry& e = kv.second;
        const uint64_t d = e.worker_cpu_us - e.balance_mark_us;
        e.balance_mark_us = e.worker_cpu_us;
        if (e.worker >= n) continue;
        load[e.worker] += d;
        auto g = std::find_if(groups.begin(), groups.end(), [&](const Group& x) { return x.id == e.group; });
        if (g == groups.end()) {
            groups.push_back(Group{e.group, e.worker, 0, true});
            g = groups.end() - 1;
        }
        g->load += d;
        g->movable = g->movable && e.core_request < 0 && (e.moved_ms == 0 || now_ms - e.moved_ms >= kMoveHoldMs);
    }
    for (size_t i = 0; i < n; ++i) workers_[i]->load_us = load[i];
    if (window_us == 0) return;
    size_t busy = 0, idle = 0;
    for (size_t i = 1; i < n; ++i) {
        if (load[i] > load[busy]) busy = i;
        if (load[i] < load[idle]) idle = i;
    }
    if (load[busy] - load[idle] < window_us / 4) return;
    const Group* pick = nullptr;
    uint64_t best = load[busy];
    for (const auto& g : groups) {
        if (g.worker != busy || !g.movable || g.load == 0) continue;
        const uint64_t after = std::max(load[busy] - g.load, load[idle] + g.load);
        if (after + window_us / 10 < best) {
            best = after;
            pick = &g;
        }
    }
    if (!pick) return;
    char why[64];
    snprintf(why, sizeof(why), "balance: cores busy %u%% and %u%%", (unsigned)(load[busy] * 100 / window_us),
             (unsigned)(load[idle] * 100 / window_us));
    move_group_locked(pick->id, idle, now_ms, why);
}

uint32_t AppPodHost::tick_worker(Worker& w, uint64_t now_ms, uint32_t max_wait_ms) {
    std::vector<std::string>& due = w.due;
    size_t n = 0;
    {
        osal::LockGuard guard(mutex_);
        for (const auto& kv : pods_) {
            const Entry& e = kv.second;
            if (e.worker != w.index || !e.has_tick || !e.ticking || e.frozen || e.in_worker || e.next_tick_ms > now_ms) continue;
            if (n < due.size()) due[n].assign(kv.first);
            else due.push_back(kv.first);
            ++n;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const std::string& id = due[i];
        {
            // Moved to another worker since, or ticked there now
            osal::LockGuard guard(mutex_);
            Entry* e = find_locked(id);
            if (!e || e->worker != w.index || e->in_worker) continue;
            e->in_worker = true;
        }
        int32_t rc = 0;
        try {
            rc = tick(id, static_cast<uint32_t>(now_ms));
        } catch (...) {
            osal::LockGuard guard(mutex_);
            if (Entry* e = find_locked(id)) e->in_worker = false;
            throw;
        }
        osal::LockGuard guard(mutex_);
        if (Entry* e = find_locked(id)) {
            e->in_worker = false;
            e->next_tick_ms = now_ms + (rc > 0 ? 10 : e->tick_interval_ms);
        }
    }
    uint64_t wait = max_wait_ms;
    osal::LockGuard guard(mutex_);
    for (const auto& kv : pods_) {
        const Entry& e = kv.second;
        if (e.worker != w.index || !e.has_tick || !e.ticking || e.frozen) continue;
        wait = std::min<uint64_t>(wait, e.next_tick_ms > now_ms ? e.next_tick_ms - now_ms : 0);
    }
    return static_cast<uint32_t>(wait);
}

// The oldest queued event of the worker's pods, unless another worker still
// delivers one to that pod (it moved): a pod gets its events in order
bool AppPodHost::deliver_worker_event(Worker& w) {
    PendingEvent ev;
    {
        osal::LockGuard guard(event_mutex_);
        auto it = event_queue_.begin();
        for (; it != event_queue_.end(); ++it) {
            if (it->worker != w.index) continue;
            bool elsewhere = false;
            for (size_t i = 0; i < worker_n_.load() && !elsewhere; ++i) {
                elsewhere = i != w.index && workers_[i]->event_pod == it->pod_id();
            }
            if (!elsewhere) break;
        }
        if (it == event_queue_.end()) return false;
        ev = std::move(*it);
        event_queue_.erase(it);
        w.event_pod.assign(ev.pod_id());
    }
    try {
        event(std::string(ev.pod_id()), ev.topic(), reinterpret_cast<const uint8_t*>(ev.data().data()), ev.data().size());
    } catch (const std::exception& e) {
        printf("[POD] event delivery skipped: %s\n", e.what());   // out of memory: the next one is delivered
    }
    osal::LockGuard guard(event_mutex_);
    w.event_pod.clear();
    return true;
}

void AppPodHost::worker_main(Worker& w) {
    t_worker = static_cast<int>(w.index);
#if defined(ESP_PLATFORM)
    // Calls into pods run here, on this stack
    if (w.opts.own_interpreter_stack) {
        kube32_interp_stack_add(w.opts.stack_buffer ? w.opts.stack_buffer : pxTaskGetStackStart(nullptr), w.opts.stack_bytes);
    }
#endif
    for (;;) {
        uint32_t wait = 1000;
        try {
            // A few events, then the ticks that are due
            for (int i = 0; i < 8 && deliver_worker_event(w); ++i) {
            }
            wait = tick_worker(w, osal::System::get_uptime_ms(), 1000);
            balance(osal::System::get_uptime_ms());
        } catch (const std::exception& e) {
            printf("[POD] core %d: pass skipped: %s\n", w.opts.core, e.what());   // out of memory: the next pass is a second away
        }
        osal::System::feed_watchdog();
        osal::UniqueLock<osal::Mutex> lock(event_mutex_);
        auto ready = [&] {
            if (workers_stop_ || w.kick) return true;
            for (const auto& ev : event_queue_) {
                if (ev.worker == w.index) return true;
            }
            return false;
        };
        event_cv_.wait_for(lock, std::max<uint32_t>(wait, 10), ready);
        w.kick = false;
        if (workers_stop_) return;
    }
}

} // namespace kube32::runtime
