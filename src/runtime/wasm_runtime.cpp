#include "runtime/wasm_runtime.hpp"
#include "runtime/image_registry.hpp"
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif
#include "runtime/container_lifecycle.hpp"
#include "runtime/app_pod.hpp"
#include "runtime/pod_fs.hpp"
#include "runtime/pod_blobs.hpp"
#include "ipc/channel.hpp"
#include "runtime/k32_app_abi.h"
#include "runtime/pod_memory_pool.h"
#include "runtime/interp_kernel.hpp"
#include "osal/mutex.hpp"

#include "wasm3.h"
#include "m3_compile.h"
#include "m3_env.h"
#include "m3_function.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <vector>

namespace kube32::runtime {

struct WasmRuntime::Impl {
    mutable osal::Mutex mutex;
    std::map<std::string, std::unique_ptr<ContainerLifecycleController>> containers;
    // Stopped while the guest was inside a host call: kept until the thread
    // ends, then destroyed, which frees its stack and the module's memory.
    std::vector<std::unique_ptr<ContainerLifecycleController>> lingering;

    void reap_locked() {
        for (auto it = lingering.begin(); it != lingering.end();) {
            if ((*it)->lingering()) { ++it; continue; }
            const std::string pod_id = (*it)->config().pod_id;
            it = lingering.erase(it);
            std::cout << "[RUNTIME] " << pod_id << ": thread ended, memory released" << std::endl;
        }
    }
};

WasmRuntime::WasmRuntime() : impl_(std::make_unique<Impl>()) {}

WasmRuntime::~WasmRuntime() = default;

bool WasmRuntime::deploy_workload(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, uint32_t memory_limit_bytes) {
    osal::LockGuard guard(impl_->mutex);

    ContainerConfig config;
    config.pod_id = pod_id;
    config.container_name = pod_id;
    config.memory_limit_bytes = (memory_limit_bytes > 0) ? memory_limit_bytes : (64 * 1024);
    config.stack_size_bytes = 8 * 1024;
    config.restart_policy = RestartPolicy::Always;

    auto controller = std::make_unique<ContainerLifecycleController>(config);
    if (!controller->deploy(wasm_bytes, len)) {
        return false;
    }

    impl_->containers[pod_id] = std::move(controller);
    return true;
}

bool WasmRuntime::deploy_workload_ex(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, const DeploySpec& spec, std::string& out_error) {
    return deploy_impl(pod_id, wasm_bytes, len, nullptr, spec, out_error);
}

bool WasmRuntime::deploy_workload_owned(const std::string& pod_id, ModuleBytes&& wasm_bytes, const DeploySpec& spec, std::string& out_error) {
    ModuleBytes owned = std::move(wasm_bytes);
    return deploy_impl(pod_id, owned.data(), owned.size(), &owned, spec, out_error);
}

bool WasmRuntime::deploy_impl(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, ModuleBytes* owned, const DeploySpec& spec, std::string& out_error) {
    out_error.clear();
    osal::LockGuard guard(impl_->mutex);
    impl_->reap_locked();   // memory a stopped container held may be free now

    ContainerConfig config;
    config.pod_id = pod_id;
    config.container_name = pod_id;
    config.memory_limit_bytes = (spec.memory_limit_bytes > 0) ? spec.memory_limit_bytes : (64 * 1024);
    config.stack_size_bytes = (spec.stack_size_bytes > 0) ? spec.stack_size_bytes : (8 * 1024);
    if (!spec.stack_size_set && config.memory_limit_bytes <= 64 * 1024 && config.stack_size_bytes > 4 * 1024) {
        config.stack_size_bytes = 4 * 1024; // one-page pods are small programs
    }
    // Unless the pod sets it, the stack is what the module's calls can need,
    // the default at most
    config.stack_from_module = !spec.stack_size_set;
    config.restart_policy = RestartPolicy::Always;
    config.gas_budget_per_call = spec.gas_budget_per_call;

    // k32_env_get reads the pod's environment from here, its only copy
    AppPodHost::instance().set_env(pod_id, spec.env);
    AppPodHost::instance().set_migration_info(pod_id, spec);
    AppPodHost::instance().set_event_topics(pod_id, spec.event_topics);
    PodBlobs::instance().set_allowed(pod_id, spec.blobs);
    AppPodHost::instance().set_tick_interval(pod_id, spec.tick_interval_ms);
    AppPodHost::instance().set_core(pod_id, spec.core);
    AppPodHost::instance().set_assets(pod_id, AppAssetSource{spec.asset_bundle, spec.asset_directory});

    const uint64_t t_load = osal::Thread::get_time_us();
    const size_t pool_before = kube32_pool_available(nullptr);
#if defined(ESP_PLATFORM)
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif
    auto controller = std::make_unique<ContainerLifecycleController>(config);
    if (spec.module_source) controller->set_module_source(spec.module_source);
    bool deployed = owned ? controller->deploy(std::move(*owned)) : controller->deploy(wasm_bytes, len);
    if (!deployed) {
        out_error = controller->get_status().exit_message;
        AppPodHost::instance().unregister_pod(pod_id);
        PodFs::instance().release_pod(pod_id);
        PodBlobs::instance().forget_pod(pod_id);
        return false;
    }

    ContainerLifecycleController* raw = controller.get();
    impl_->containers[pod_id] = std::move(controller);

    const uint64_t t_init = osal::Thread::get_time_us();
    // An entry point runs on its own thread, which no snapshot captures
    AppPodHost::instance().note_entry_point(pod_id, *raw);
    const bool registered = AppPodHost::instance().register_pod(pod_id, raw);
    if (registered || spec.restore_from) {
        std::string why;
        const int32_t init_rc = spec.restore_from ? AppPodHost::instance().restore_pod(pod_id, spec.restore_from, why)
                                                  : AppPodHost::instance().init_pod(pod_id);
        const uint64_t t_done = osal::Thread::get_time_us();
        // An internal pool block holds the pods of a board without PSRAM:
        // what this one took of it, and what is left
        char pool[96] = "";
        int external = 0;
        if (kube32_pool_block_size() > 0 && (kube32_pool_available(&external), !external)) {
            const size_t pool_after = kube32_pool_available(nullptr);
            snprintf(pool, sizeof(pool), ", pod pool %ld bytes (%u free, largest %u)", (long)pool_before - (long)pool_after,
                     (unsigned)pool_after, (unsigned)kube32_pool_largest_free());
        }
        const char* start_step = spec.restore_from ? "restore" : "k32_init";
        const unsigned start_ms = (unsigned)((t_done - t_init) / 1000);
#if defined(ESP_PLATFORM)
        printf("[POD] %s: %u bytes wasm, load+compile %u ms, %s %u ms (rc=%d), memory internal %+d KiB psram %+d KiB%s\n",
               pod_id.c_str(), (unsigned)len, (unsigned)((t_init - t_load) / 1000), start_step, start_ms, (int)init_rc,
               (int)((long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) - (long)internal_before) / 1024,
               (int)((long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - (long)psram_before) / 1024, pool);
#else
        printf("[POD] %s: %u bytes wasm, load+compile %u ms, %s %u ms (rc=%d)%s\n",
               pod_id.c_str(), (unsigned)len, (unsigned)((t_init - t_load) / 1000), start_step, start_ms, (int)init_rc, pool);
#endif
        if (init_rc != 0) {
            out_error = spec.restore_from ? why : "k32_init returned " + std::to_string(init_rc);
            AppPodHost::instance().unregister_pod(pod_id);
            PodFs::instance().release_pod(pod_id);
            PodBlobs::instance().forget_pod(pod_id);
            ipc::ChannelManager::instance().close_all(pod_id);
            auto it = impl_->containers.find(pod_id);
            if (it != impl_->containers.end()) {
                std::unique_ptr<ContainerLifecycleController> dead = std::move(it->second);
                impl_->containers.erase(it);
                dead->stop(100);
            }
            return false;
        }
    }
    return true;
}

bool WasmRuntime::start(const std::string& pod_id) {
    osal::LockGuard guard(impl_->mutex);
    auto it = impl_->containers.find(pod_id);
    if (it == impl_->containers.end()) return false;
    return it->second->start();
}

bool WasmRuntime::pause(const std::string& pod_id) {
    osal::LockGuard guard(impl_->mutex);
    auto it = impl_->containers.find(pod_id);
    if (it == impl_->containers.end()) return false;
    return it->second->pause();
}

bool WasmRuntime::resume(const std::string& pod_id) {
    osal::LockGuard guard(impl_->mutex);
    auto it = impl_->containers.find(pod_id);
    if (it == impl_->containers.end()) return false;
    return it->second->resume();
}

bool WasmRuntime::restart(const std::string& pod_id) {
    osal::LockGuard guard(impl_->mutex);
    auto it = impl_->containers.find(pod_id);
    if (it == impl_->containers.end()) return false;
    return it->second->restart();
}

bool WasmRuntime::terminate(const std::string& pod_id) {
    std::unique_ptr<ContainerLifecycleController> target;
    {
        // Host calls the pod is blocked in return early from here on, also
        // the in-flight call unregister_pod waits for
        osal::LockGuard guard(impl_->mutex);
        auto it = impl_->containers.find(pod_id);
        if (it != impl_->containers.end()) it->second->request_stop();
    }
    AppPodHost::instance().unregister_pod(pod_id);
    PodFs::instance().release_pod(pod_id);
    PodBlobs::instance().forget_pod(pod_id);
    ipc::ChannelManager::instance().close_all(pod_id);
    {
        osal::LockGuard guard(impl_->mutex);
        auto it = impl_->containers.find(pod_id);
        if (it == impl_->containers.end()) return false;
        target = std::move(it->second);
        impl_->containers.erase(it);
    }
    bool ok = target->stop(1000);
    osal::LockGuard guard(impl_->mutex);
    if (target->lingering()) impl_->lingering.push_back(std::move(target));
    impl_->reap_locked();
    return ok;
}

void WasmRuntime::reap_stopped() {
    osal::LockGuard guard(impl_->mutex);
    impl_->reap_locked();
}

size_t WasmRuntime::lingering_count() const {
    osal::LockGuard guard(impl_->mutex);
    return impl_->lingering.size();
}

ContainerStatus WasmRuntime::get_status(const std::string& pod_id) {
    osal::LockGuard guard(impl_->mutex);
    auto it = impl_->containers.find(pod_id);
    if (it == impl_->containers.end()) {
        ContainerStatus s;
        s.phase = ContainerPhase::Terminated;
        s.exit_message = "Pod not found";
        return s;
    }
    return it->second->get_status();
}

bool WasmRuntime::is_deployed(const std::string& pod_id) const {
    osal::LockGuard guard(impl_->mutex);
    return impl_->containers.find(pod_id) != impl_->containers.end();
}

std::vector<std::string> WasmRuntime::list_containers() const {
    osal::LockGuard guard(impl_->mutex);
    std::vector<std::string> ids;
    ids.reserve(impl_->containers.size());
    for (const auto& kv : impl_->containers) {
        ids.push_back(kv.first);
    }
    return ids;
}

size_t WasmRuntime::container_count() const {
    osal::LockGuard guard(impl_->mutex);
    return impl_->containers.size();
}

int32_t WasmRuntime::execute_function(const std::string& pod_id, const std::string& fn_name, const std::vector<int32_t>& args) {
    osal::LockGuard guard(impl_->mutex);
    auto it = impl_->containers.find(pod_id);
    if (it == impl_->containers.end()) return -1;
    return it->second->call_function(fn_name, args);
}

std::unique_ptr<WasmRuntime> create_wasm_runtime() {
    return std::make_unique<WasmRuntime>();
}

namespace {

// Reads a module's bytes: LEB128 numbers, names, function bodies. A read past
// the end marks the reader failed instead of reading on.
struct BodyReader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok{true};

    uint8_t byte() {
        if (p >= end) { ok = false; return 0; }
        return *p++;
    }
    uint32_t u32() {
        uint32_t v = 0;
        for (int shift = 0; shift < 35 && ok; shift += 7) {
            const uint8_t b = byte();
            v |= static_cast<uint32_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    void leb() {   // any LEB128 up to 64 bits, signed or not
        for (int i = 0; i < 10 && ok; ++i) {
            if (!(byte() & 0x80)) return;
        }
        ok = false;
    }
    void skip(size_t n) {
        if (static_cast<size_t>(end - p) < n) ok = false;
        else p += n;
    }
    void memarg() {
        if (u32() & 0x40) u32();   // a memory index follows the alignment (multi-memory)
        leb();
    }
};

// A function body as far as its stack goes: the functions it calls, the types
// its indirect calls name, and (for the estimate) the frame slots of its
// locals and of the constants wasm3 reserves in its frame
struct BodyInfo {
    std::vector<uint32_t> direct;
    std::vector<uint32_t> indirect;
    uint32_t local_slots{0};
    uint32_t constant_slots{0};
};

// Reads a body (its size first, as in the code section). False for an
// instruction the call graph does not read (SIMD, exceptions, typed references).
bool scan_body(const uint8_t* wasm, const uint8_t* end, BodyInfo& out, uint32_t constant_table = d_m3MaxConstantTableSize) {
    BodyReader r{wasm, end};
    r.u32();
    const uint32_t groups = r.u32();
    for (uint32_t i = 0; i < groups && r.ok; ++i) {
        const uint32_t count = std::min<uint32_t>(r.u32(), 1u << 16);
        const uint8_t type = r.byte();
        out.local_slots += count * ((type == 0x7E || type == 0x7C) ? 2 : 1);   // i64, f64: two slots
    }
    // wasm3 reserves frame slots for what a blind scan of the body's bytes
    // takes for constants, up to its constant table (ReserveConstants)
    for (const uint8_t* q = r.p; q < end; ++q) {
        const uint32_t add = (*q == 0x41 || *q == 0x43) ? 1 : (*q == 0x42 || *q == 0x44) ? 2 : 0;
        if (out.constant_slots + add >= constant_table) break;
        out.constant_slots += add;
    }
    out.constant_slots = (out.constant_slots + 1) & ~1u;
    while (r.ok && r.p < r.end) {
        const uint8_t op = r.byte();
        if (op >= 0x45 && op <= 0xC4) continue;                  // numeric, conversions, sign extension
        switch (op) {
        case 0x00: case 0x01: case 0x05: case 0x0B: case 0x0F: case 0x1A: case 0x1B: case 0xD1:
            break;
        case 0x02: case 0x03: case 0x04:                         // block, loop, if: block type
            r.leb();
            break;
        case 0x0C: case 0x0D: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:
        case 0x3F: case 0x40: case 0xD2:
            r.u32();
            break;
        case 0x0E: {                                             // br_table: the labels and the default
            const uint32_t n = r.u32();
            for (uint32_t i = 0; i <= n && r.ok; ++i) r.u32();
            break;
        }
        case 0x10: case 0x12:                                    // call, return_call
            out.direct.push_back(r.u32());
            break;
        case 0x11: case 0x13:                                    // call_indirect, return_call_indirect
            out.indirect.push_back(r.u32());
            r.u32();
            break;
        case 0x1C:                                               // select with types
            r.skip(r.u32());
            break;
        case 0x41: case 0x42: r.leb(); break;
        case 0x43: r.skip(4); break;
        case 0x44: r.skip(8); break;
        case 0xD0: r.leb(); break;                               // ref.null: heap type
        case 0xFC: {
            const uint32_t sub = r.u32();
            if (sub <= 7) break;                                 // saturating truncations
            if (sub == 8 || sub == 10 || sub == 12 || sub == 14) { r.u32(); r.u32(); break; }
            if (sub == 9 || sub == 11 || sub == 13 || (sub >= 15 && sub <= 17)) { r.u32(); break; }
            return false;
        }
        case 0xFE:                                               // atomics
            if (r.u32() == 3) r.byte();                          // atomic.fence
            else r.memarg();
            break;
        default:
            if (op >= 0x28 && op <= 0x3E) { r.memarg(); break; } // loads and stores
            return false;
        }
    }
    for (auto* v : {&out.direct, &out.indirect}) {
        std::sort(v->begin(), v->end());
        v->erase(std::unique(v->begin(), v->end()), v->end());
    }
    return r.ok;
}

// The heaviest chain of calls: function i weighs weight(i) and calls
// calls[i].direct and, for each type of calls[i].indirect, every function j
// with same_type(type, j); a function's callees start inside its frame.
// Functions without a body (host functions) weigh nothing. 0 when a chain
// loops back (recursion): it has no bound.
template <class HasBody, class Weight, class SameType>
uint32_t heaviest_call_chain(const std::vector<BodyInfo>& calls, HasBody has_body, Weight weight, SameType same_type) {
    const uint32_t n = static_cast<uint32_t>(calls.size());
    std::vector<uint32_t> need(n, 0);
    std::vector<uint8_t> state(n, 0);                            // 0 new, 1 on the path, 2 done
    struct Frame { uint32_t fn; size_t next; };
    std::vector<Frame> path;
    uint32_t most = 0;
    for (uint32_t root = 0; root < n; ++root) {
        if (state[root] || !has_body(root)) continue;
        state[root] = 1;
        path.push_back({root, 0});
        while (!path.empty()) {
            const uint32_t fn = path.back().fn;
            const BodyInfo& c = calls[fn];
            const size_t direct = c.direct.size();
            const size_t k = path.back().next;
            if (k < direct + c.indirect.size() * n) {
                path.back().next++;
                const uint32_t callee = k < direct ? c.direct[k] : static_cast<uint32_t>((k - direct) % n);
                if (callee >= n) return 0;
                if (!has_body(callee) || (k >= direct && !same_type(c.indirect[(k - direct) / n], callee))) continue;
                if (state[callee] == 1) return 0;
                if (state[callee] == 0) {
                    state[callee] = 1;
                    path.push_back({callee, 0});
                } else {
                    need[fn] = std::max(need[fn], need[callee]);
                }
                continue;
            }
            need[fn] += weight(fn);
            state[fn] = 2;
            path.pop_back();
            if (!path.empty()) need[path.back().fn] = std::max(need[path.back().fn], need[fn]);
            most = std::max(most, need[fn]);
        }
    }
    return most;
}

// A pod allocation as the pool block holds it: 8-byte aligned, with a header
size_t pool_chunk(size_t bytes) { return ((bytes + 7) & ~static_cast<size_t>(7)) + 8; }

// What an estimate counts of wasm3's structures
struct Wasm3Sizes {
    size_t line;            // a code line (a pointer)
    size_t slot;            // an operand stack slot
    size_t function;        // M3Function
    size_t runtime;         // M3Runtime
    size_t environment;     // M3Environment
    size_t module;          // M3Module
    size_t memory;          // M3Memory
    size_t memory_header;   // M3MemoryHeader
    size_t func_type;       // M3FuncType, before its value types
    size_t page_header;     // M3CodePageHeader
    size_t export_entry;    // M3Export
    size_t scratch;         // m3_CompileScratchSize()
    uint32_t constants;     // d_m3MaxConstantTableSize
};

// The ESP32-S3 build's: 32-bit pointers and slots, PlatformIO's
// microcontroller configuration of wasm3 (m3_config_platforms.h)
constexpr Wasm3Sizes kBoardSizes{4, 4, 64, 176, 60, 176, 56, 16, 20, 20, 16, 6168, 64};

#if defined(ESP_PLATFORM)
static_assert(sizeof(code_t) == kBoardSizes.line && sizeof(m3slot_t) == kBoardSizes.slot && sizeof(M3Function) == kBoardSizes.function &&
                  sizeof(M3Runtime) == kBoardSizes.runtime && sizeof(M3Environment) == kBoardSizes.environment &&
                  sizeof(M3Module) == kBoardSizes.module && sizeof(M3Memory) == kBoardSizes.memory &&
                  sizeof(M3MemoryHeader) == kBoardSizes.memory_header && sizeof(M3FuncType) == kBoardSizes.func_type &&
                  sizeof(M3CodePageHeader) == kBoardSizes.page_header && sizeof(M3Export) == kBoardSizes.export_entry &&
                  d_m3MaxConstantTableSize == kBoardSizes.constants,
              "kBoardSizes no longer matches wasm3's structures on the board");
#endif

Wasm3Sizes this_build_sizes() {
    return {sizeof(code_t), sizeof(m3slot_t), sizeof(M3Function), sizeof(M3Runtime), sizeof(M3Environment), sizeof(M3Module),
            sizeof(M3Memory), sizeof(M3MemoryHeader), sizeof(M3FuncType), sizeof(M3CodePageHeader), sizeof(M3Export),
            m3_CompileScratchSize(), d_m3MaxConstantTableSize};
}

} // namespace

size_t estimate_pod_memory(const uint8_t* wasm, size_t len, uint32_t memory_limit_bytes, uint32_t stack_bytes, bool stack_from_module,
                           EstimateSizes which, size_t* out_linear, bool module_held) {
    const Wasm3Sizes z = which == EstimateSizes::ThisBuild ? this_build_sizes() : kBoardSizes;
    const bool held_only = which == EstimateSizes::BoardHeld;
    // What the module declares: its function types, imports, functions,
    // exports, the first memory's initial pages and the function bodies
    std::vector<std::string> types;                              // parameter and result types of each
    std::vector<uint32_t> function_types;                        // of every function, imports first
    std::vector<BodyInfo> bodies;                                // of every function; imports have none
    uint32_t imports = 0, exports = 0, initial_pages = 0;
    size_t names = 0, code = 0;
    bool bodies_read = true;
    if (wasm && len > 8) {
        BodyReader r{wasm + 8, wasm + len};
        while (r.ok && r.p < r.end) {
            const uint8_t id = r.byte();
            const uint32_t size = r.u32();
            if (!r.ok || size > static_cast<size_t>(r.end - r.p)) break;
            BodyReader s{r.p, r.p + size};
            r.p += size;
            if (id == 10) code = size;
            if (id != 1 && id != 2 && id != 3 && id != 5 && id != 7 && id != 10) continue;
            const uint32_t count = s.u32();
            for (uint32_t i = 0; i < count && s.ok; ++i) {
                if (id == 1) {                                   // (func params results)
                    s.byte();
                    const uint8_t* sig = s.p;
                    s.skip(s.u32());
                    s.skip(s.u32());
                    if (s.ok) types.emplace_back(reinterpret_cast<const char*>(sig), static_cast<size_t>(s.p - sig));
                } else if (id == 2) {                            // module and field names, kept
                    for (int name = 0; name < 2; ++name) {
                        const uint32_t n = s.u32();
                        s.skip(n);
                        names += pool_chunk(n + 1);
                    }
                    const uint8_t kind = s.byte();
                    if (kind == 0) {
                        function_types.push_back(s.u32());
                        ++imports;
                    } else if (kind == 1 || kind == 2) {         // table or memory: limits
                        if (kind == 1) s.byte();
                        const uint8_t flags = s.byte();
                        s.leb();
                        if (flags & 1) s.leb();
                    } else {                                     // global or tag
                        s.byte();
                        s.byte();
                    }
                } else if (id == 3) {
                    function_types.push_back(s.u32());
                } else if (id == 5) {
                    s.byte();
                    initial_pages = s.u32();
                    break;
                } else if (id == 7) {
                    const uint32_t n = s.u32();
                    s.skip(n);
                    const uint8_t kind = s.byte();
                    s.u32();
                    names += pool_chunk(n + 1) * (kind == 0 ? 2 : 1);   // the export's, and a function's own copy
                    ++exports;
                } else {
                    const uint8_t* body = s.p;
                    s.skip(s.u32());
                    bodies.resize(imports + i + 1);
                    if (!s.ok || !scan_body(body, s.p, bodies.back(), z.constants)) bodies_read = false;
                }
            }
        }
    }
    if (code == 0) code = len;
    const uint32_t functions = static_cast<uint32_t>(function_types.size());
    const uint32_t defined = functions - imports;
    bodies.resize(std::max<size_t>(functions, bodies.size()));

    // The operand stack: the pod's, or what its heaviest call chain needs,
    // each frame estimated as its arguments and results (two slots each),
    // locals, constants and a few slots for the values in flight
    if (stack_from_module && bodies_read && defined > 0) {
        const uint32_t slots = heaviest_call_chain(
            bodies, [&](uint32_t fn) { return fn >= imports && fn < functions; },
            [&](uint32_t fn) {
                const uint32_t t = function_types[fn];
                const uint32_t io = t < types.size() ? 2 * static_cast<uint32_t>(types[t].size()) : 0;
                return io + bodies[fn].local_slots + bodies[fn].constant_slots + 24;
            },
            [&](uint32_t type, uint32_t fn) {
                return type >= types.size() || function_types[fn] >= types.size() || types[type] == types[function_types[fn]];
            });
        if (slots > 0) stack_bytes = std::min<uint32_t>(stack_bytes, static_cast<uint32_t>(((slots + 1) * z.slot + 64 + 15) & ~15u));
    }

    const size_t page = 64 * 1024;
    const size_t linear = memory_limit_bytes < page ? memory_limit_bytes : (initial_pages ? initial_pages : 1) * page;
    if (out_linear) *out_linear = linear;
    // Function bodies compile to about 0.78 code lines a byte, metering
    // included, on pages of d_m3CodePageAlignSize bytes
    const size_t code_bytes = code * 39 / 50 * z.line;
    const size_t pages = code_bytes / (d_m3CodePageAlignSize - z.page_header) + 1;
    size_t need = pool_chunk(z.runtime) + pool_chunk(z.environment) + pool_chunk(z.module) + pool_chunk(z.memory) + pool_chunk(z.line);
    need += (c_m3Type_count + types.size()) * pool_chunk(z.func_type + 2) + pool_chunk(types.size() * z.line);
    need += pool_chunk(functions * z.function) + names + pool_chunk(exports * z.export_entry);
    need += defined * 12 * z.slot;                               // a function's constants (a dozen), after its code
    need += code_bytes + pages * pool_chunk(z.page_header);
    need += pool_chunk(linear + z.memory_header) + pool_chunk(stack_bytes + 4 * z.slot);
    need += 256;                                                 // data and element segments, globals, a table
    // The module's bytes, which wasm3 reads in place for the module's
    // lifetime, unless they are memory mapped from flash
    if (module_held && len > 0) need += pool_chunk(len);
    // The compiler's scratch while the module loads, unless it is lent
    if (!held_only && !compile_scratch_lent()) need += pool_chunk(z.scratch);
    // The pod's log ring, environment and registration, unless the pods of
    // this node live in an internal pool block (the general heap holds them)
    int external = 0;
    if (!held_only && (kube32_pool_block_size() == 0 || (kube32_pool_available(&external), external))) need += 2048;
    return need;
}

size_t set_pod_overhead(models::JsonValue& pod) {
    models::JsonValue* spec = pod.get_mut("spec");
    if (!spec || !spec->is_object()) return 0;
    const models::JsonValue* meta = pod.get("metadata");
    const models::JsonValue* ann = meta ? meta->get("annotations") : nullptr;
    auto annotation = [&](const char* key) {
        const models::JsonValue* v = ann && ann->is_object() ? ann->get(key) : nullptr;
        return v && v->is_string() ? v->str_val : std::string();
    };
    const models::JsonValue* containers = spec->get("containers");
    if (!containers || !containers->is_array()) return 0;
    size_t overhead = 0;
    for (const auto& c : containers->arr_val) {
        // The linear memory budget, as the kubelet takes it: esp32.io/psram
        // when larger, else limits.memory, then requests.memory, then a page
        uint64_t mem = 0, psram = 0;
        if (const models::JsonValue* res = c.get("resources")) {
            const models::JsonValue* lim = res->get("limits");
            const models::JsonValue* req = res->get("requests");
            if (lim && lim->is_object()) mem = models::parse_quantity(lim->get_string("memory", ""));
            if (!mem && req && req->is_object()) mem = models::parse_quantity(req->get_string("memory", ""));
            for (const models::JsonValue* r : {req, lim}) {
                if (r && r->is_object()) psram = std::max<uint64_t>(psram, models::parse_quantity(r->get_string("esp32.io/psram", "")));
            }
        }
        if (!mem) mem = 64 * 1024;
        if (psram > mem) mem = psram;
        uint32_t stack = mem <= 64 * 1024 ? 4 * 1024 : 16 * 1024;
        bool stack_set = false;
        if (const std::string v = annotation("kube32.io/wasm-stack"); !v.empty()) {
            const uint64_t n = models::parse_quantity(v);
            if (n >= 512 && n <= 64 * 1024) { stack = static_cast<uint32_t>(n); stack_set = true; }
        }
        // The image as the kubelet resolves it: an embedded one is mapped
        // from flash, the module of one in the image store is pod memory
        const uint8_t* module = nullptr;
        size_t module_len = 0;
        bool held = false;
        const std::string image = c.get_string("image", "");
        std::string sd_name, sd_tag;
        std::string blob_name;
        if (parse_sd_image_ref(image, sd_name, sd_tag)) {
            continue;   // on a node's card, which this node cannot see: the kubelet's admission counts it
        } else if (parse_blob_image_ref(image, blob_name)) {
            // in a node's flash blob store, read in place: its bytes are not held; the
            // code is estimated from kube32.io/wasm-size when the pod carries it
            const std::string size = annotation("kube32.io/wasm-size");
            if (size.empty()) continue;
            module_len = static_cast<size_t>(models::parse_quantity(size));
        } else if (const EmbeddedImage* img = ImageRegistry::instance().resolve(image, nullptr)) {
            module = img->data;
            module_len = img->len;
        } else if (const std::string size = annotation("kube32.io/wasm-size"); !size.empty()) {
            module_len = static_cast<size_t>(models::parse_quantity(size));
            held = true;
        } else {
            continue;   // an image this node cannot see: the kubelet's admission counts it
        }
        size_t linear = 0;
        const size_t need = estimate_pod_memory(module, module_len, static_cast<uint32_t>(std::min<uint64_t>(mem, UINT32_MAX)), stack, !stack_set,
                                                EstimateSizes::BoardHeld, &linear, held);
        if (need > linear) overhead += need - linear;
    }
    if (overhead == 0) {
        spec->remove("overhead");
        return 0;
    }
    // Rounded up to whole KiB, as a quantity a client reads back unchanged
    const size_t kib = (overhead + 1023) / 1024;
    models::JsonValue o;
    o.type = models::JsonType::Object;
    models::JsonValue q;
    q.type = models::JsonType::String;
    q.str_val = std::to_string(kib) + "Ki";
    o.set("memory", std::move(q));
    spec->set("overhead", std::move(o));
    return kib * 1024;
}

uint32_t module_stack_need_bytes(const M3Module* module) {
    if (!module) return 0;
    const uint32_t n = module->numFunctions;
    std::vector<BodyInfo> calls(n);
    for (uint32_t i = 0; i < n; ++i) {
        const M3Function& f = module->functions[i];
        if (!f.wasm) continue;                                   // a host function: only its arguments, in the caller's frame
        if (!f.compiled || !scan_body(f.wasm, f.wasmEnd, calls[i])) return 0;
    }
    // A function's frame is what wasm3 compiled it to; an indirect call may
    // reach any function of its type
    const uint32_t most = heaviest_call_chain(
        calls, [&](uint32_t fn) { return module->functions[fn].wasm != nullptr; },
        [&](uint32_t fn) { return static_cast<uint32_t>(module->functions[fn].maxStackSlots); },
        [&](uint32_t type, uint32_t fn) {
            if (type >= module->numFuncTypes) return true;
            const IM3FuncType want = module->funcTypes[type];
            const IM3FuncType have = module->functions[fn].funcType;
            return want && have && (want == have || AreFuncTypesEqual(want, have));
        });
    if (most == 0) return 0;
    // op_Entry requires the frame to end below the stack's end: one slot more
    return (most + 1) * static_cast<uint32_t>(sizeof(m3slot_t));
}

// measure_interp_mops: the runs of the kernel it times, the work of one run
// and the mean pause between two runs
static constexpr uint32_t kInterpRuns = 10;
static constexpr uint64_t kInterpRunUs = 10000;
static constexpr uint32_t kInterpGapMs = 50;

double measure_interp_mops() {
    IM3Environment env = m3_NewEnvironment();
    if (!env) return 0;
    IM3Runtime rt = m3_NewRuntime(env, 1024, nullptr);
    IM3Module mod = nullptr;
    IM3Function fn = nullptr;
    double mops = 0;
    if (rt && !m3_ParseModule(env, &mod, kInterpKernel, sizeof(kInterpKernel))) {
        if (m3_LoadModule(rt, mod)) {
            m3_FreeModule(mod);
        } else if (!m3_FindFunction(&fn, rt, "run")) {
            // Timed where pods run: on the shared interpreter stack when there
            // is one. The time is the CPU time the thread ran where the
            // platform counts it, so other processes of the host do not count.
            auto timed = [&](uint32_t n) {
                uint64_t us = 0;
                run_on_interpreter_stack([&]() {
                    uint64_t c0 = 0, c1 = 0;
                    const bool cpu = osal::Thread::current_cpu_time_us(c0);
                    const uint64_t t0 = osal::Thread::get_time_us();
                    m3_CallV(fn, n);
                    us = osal::Thread::get_time_us() - t0;
                    if (cpu && osal::Thread::current_cpu_time_us(c1) && c1 > c0) us = c1 - c0;
                });
                return us;
            };
            timed(100);   // compiles the kernel
            uint32_t n = 1000;
            uint64_t us = timed(n);
            // About kInterpRunUs of work, whatever the node's speed
            if (us < kInterpRunUs) n = static_cast<uint32_t>(std::min<uint64_t>(kInterpRunUs * n / (us ? us : 1), 100000000ull));
            // The fastest of several runs: a run that shares the core with
            // other work, or starts before the clock speed rises, takes up to
            // several times as long. The pauses between runs last a random
            // 0.5 to 1.5 kInterpGapMs, so a burst of other work passes and
            // two nodes that start together on one host do not run theirs
            // at the same moments.
            std::minstd_rand gap(static_cast<uint32_t>(osal::Thread::get_time_us()));
            us = 0;
            for (uint32_t i = 0; i < kInterpRuns; ++i) {
                if (i) osal::Thread::sleep_ms(kInterpGapMs / 2 + static_cast<uint32_t>(gap() % kInterpGapMs));
                const uint64_t t = timed(n);
                if (t > 0 && (us == 0 || t < us)) us = t;
            }
            if (us > 0) mops = static_cast<double>(n) * kInterpKernelOpsPerIteration / static_cast<double>(us);
        }
    }
    if (rt) m3_FreeRuntime(rt);
    m3_FreeEnvironment(env);
    return mops;
}

namespace {

// What k32_time_ms returns to the interpreter check: a value whose halves and
// bytes all differ
constexpr uint64_t kInterpCheckTime = 0x0123456789abcdefull;
// The bytes of the check's page that are backed, as for a pod below one page
constexpr uint32_t kInterpCheckMemory = 256;

const void* interp_check_time(IM3Runtime, IM3ImportContext, uint64_t* sp, void*) {
    *reinterpret_cast<int64_t*>(sp) = static_cast<int64_t>(kInterpCheckTime);
    return m3Err_none;
}

} // namespace

std::string check_interp_i64() {
    std::string failed = "the check did not run";
    IM3Environment env = m3_NewEnvironment();
    if (!env) return failed;
    IM3Runtime rt = m3_NewRuntime(env, 1024, nullptr);
    IM3Module mod = nullptr;
    IM3Function check = nullptr, lo = nullptr, hi = nullptr;
    if (rt) rt->memoryLimit = kInterpCheckMemory;
    if (rt && !m3_ParseModule(env, &mod, kInterpCheck, sizeof(kInterpCheck))) {
        if (m3_LoadModule(rt, mod)) {
            m3_FreeModule(mod);
        } else if (!m3_LinkRawFunction(mod, "env", "k32_time_ms", "I()", &interp_check_time) &&
                   !m3_FindFunction(&check, rt, "check") && !m3_FindFunction(&lo, rt, "value_lo") &&
                   !m3_FindFunction(&hi, rt, "value_hi")) {
            // Run where pods run: on the shared interpreter stack when there is one
            run_on_interpreter_stack([&]() {
                int32_t step = -1;
                uint32_t vlo = 0, vhi = 0;
                if (m3_CallV(check, 16) || m3_GetResultsV(check, &step)) return;
                if (step > 0 && step <= static_cast<int32_t>(sizeof(kInterpCheckSteps) / sizeof(kInterpCheckSteps[0]))) {
                    failed = kInterpCheckSteps[step - 1];
                    return;
                }
                if (step != 0 || m3_CallV(lo) || m3_GetResultsV(lo, &vlo) || m3_CallV(hi) || m3_GetResultsV(hi, &vhi)) return;
                failed = ((static_cast<uint64_t>(vhi) << 32) | vlo) == kInterpCheckTime ? "" : kInterpCheckSteps[5];
            });
        }
    }
    if (rt) m3_FreeRuntime(rt);
    m3_FreeEnvironment(env);
    return failed;
}

bool deploy_error_is_restore(const std::string& error) {
    return error.rfind(kRestoreErrorPrefix, 0) == 0;
}

const char* module_entry_point(const uint8_t* wasm, size_t len) {
    if (!wasm || len <= 8) return nullptr;
    BodyReader r{wasm + 8, wasm + len};
    while (r.ok && r.p < r.end) {
        const uint8_t id = r.byte();
        const uint32_t size = r.u32();
        if (!r.ok || size > static_cast<size_t>(r.end - r.p)) return nullptr;
        if (id != 7) { r.p += size; continue; }
        BodyReader s{r.p, r.p + size};
        const uint32_t count = s.u32();
        const char* found = nullptr;
        for (uint32_t i = 0; i < count && s.ok; ++i) {
            const uint32_t n = s.u32();
            const char* name = reinterpret_cast<const char*>(s.p);
            s.skip(n);
            const uint8_t kind = s.byte();
            s.u32();
            if (!s.ok || kind != 0) continue;
            for (const char* ep : {"_start", "app_main", "main"}) {
                if (std::strlen(ep) == n && std::memcmp(ep, name, n) == 0 && (!found || std::strcmp(ep, "_start") == 0)) found = ep;
            }
        }
        return found;
    }
    return nullptr;
}

bool deploy_error_is_out_of_memory(const std::string& error) {
    // m3Err_mallocFailedCodePage begins with m3Err_mallocFailed's text
    return error.find(m3Err_mallocFailed) != std::string::npos;
}

} // namespace kube32::runtime
