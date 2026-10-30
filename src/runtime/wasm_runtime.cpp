#include "runtime/wasm_runtime.hpp"
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif
#include "runtime/container_lifecycle.hpp"
#include "runtime/app_pod.hpp"
#include "runtime/pod_fs.hpp"
#include "runtime/k32_app_abi.h"
#include "osal/mutex.hpp"

#include "wasm3.h"
#include "m3_compile.h"

#include <iostream>
#include <map>
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

bool WasmRuntime::deploy_workload_owned(const std::string& pod_id, std::vector<uint8_t>&& wasm_bytes, const DeploySpec& spec, std::string& out_error) {
    std::vector<uint8_t> owned = std::move(wasm_bytes);
    return deploy_impl(pod_id, owned.data(), owned.size(), &owned, spec, out_error);
}

bool WasmRuntime::deploy_impl(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, std::vector<uint8_t>* owned, const DeploySpec& spec, std::string& out_error) {
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
    config.restart_policy = RestartPolicy::Always;
    config.env = spec.env;
    config.gas_budget_per_call = spec.gas_budget_per_call;

    AppPodHost::instance().set_env(pod_id, spec.env);
    AppPodHost::instance().set_event_topics(pod_id, spec.event_topics);
    AppPodHost::instance().set_tick_interval(pod_id, spec.tick_interval_ms);
    AppPodHost::instance().set_assets(pod_id, AppAssetSource{spec.asset_bundle, spec.asset_directory});

    const uint64_t t_load = osal::Thread::get_time_us();
#if defined(ESP_PLATFORM)
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif
    auto controller = std::make_unique<ContainerLifecycleController>(config);
    bool deployed = owned ? controller->deploy(std::move(*owned)) : controller->deploy(wasm_bytes, len);
    if (!deployed) {
        out_error = controller->get_status().exit_message;
        AppPodHost::instance().unregister_pod(pod_id);
        PodFs::instance().release_pod(pod_id);
        return false;
    }

    ContainerLifecycleController* raw = controller.get();
    impl_->containers[pod_id] = std::move(controller);

    const uint64_t t_init = osal::Thread::get_time_us();
    if (AppPodHost::instance().register_pod(pod_id, raw)) {
        int32_t init_rc = AppPodHost::instance().init_pod(pod_id);
        const uint64_t t_done = osal::Thread::get_time_us();
#if defined(ESP_PLATFORM)
        printf("[POD] %s: %u bytes wasm, load+compile %u ms, k32_init %u ms (rc=%d), memory internal %+d KB psram %+d KB\n",
               pod_id.c_str(), (unsigned)len, (unsigned)((t_init - t_load) / 1000), (unsigned)((t_done - t_init) / 1000), (int)init_rc,
               (int)((long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) - (long)internal_before) / 1024,
               (int)((long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - (long)psram_before) / 1024);
#else
        printf("[POD] %s: %u bytes wasm, load+compile %u ms, k32_init %u ms (rc=%d)\n",
               pod_id.c_str(), (unsigned)len, (unsigned)((t_init - t_load) / 1000), (unsigned)((t_done - t_init) / 1000), (int)init_rc);
#endif
        if (init_rc != 0) {
            out_error = "k32_init returned " + std::to_string(init_rc);
            AppPodHost::instance().unregister_pod(pod_id);
            PodFs::instance().release_pod(pod_id);
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

bool read_leb(const uint8_t*& p, const uint8_t* end, uint32_t& out) {
    out = 0;
    for (int shift = 0; p < end && shift < 35; shift += 7) {
        const uint8_t b = *p++;
        out |= static_cast<uint32_t>(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

} // namespace

size_t estimate_pod_memory(const uint8_t* wasm, size_t len, uint32_t memory_limit_bytes, uint32_t stack_bytes) {
    // What the module declares: imports and functions, the size of the code
    // section and the first memory's initial pages
    size_t code = len, functions = 0;
    uint32_t initial_pages = 0;
    if (wasm && len > 8) {
        const uint8_t* p = wasm + 8;
        const uint8_t* end = wasm + len;
        while (p < end) {
            const uint8_t id = *p++;
            uint32_t size = 0;
            if (!read_leb(p, end, size) || size > static_cast<size_t>(end - p)) break;
            const uint8_t* s = p;
            p += size;
            uint32_t count = 0;
            if ((id == 2 || id == 3) && read_leb(s, p, count)) functions += count;
            if (id == 5 && read_leb(s, p, count) && count > 0) {
                uint32_t flags = 0;
                if (read_leb(s, p, flags)) read_leb(s, p, initial_pages);
            }
            if (id == 10) code = size;
        }
    }
    const size_t page = 64 * 1024;
    const size_t linear = memory_limit_bytes < page ? memory_limit_bytes : (initial_pages ? initial_pages : 1) * page;
    // Measured on the board: function bodies compile to about 3.2 times their
    // size; each function keeps about 100 bytes of structures and the rest of
    // the module about half its size; the runtime and environment take under
    // 1 KiB, the pod's log ring, environment and registration about 2 KiB.
    return 768 + stack_bytes + 64 + linear + 16 + code * 13 / 4 + functions * 100 + (len - code) / 2 + 2048 +
           sizeof(M3Compilation);
}

bool deploy_error_is_out_of_memory(const std::string& error) {
    // m3Err_mallocFailedCodePage begins with m3Err_mallocFailed's text
    return error.find(m3Err_mallocFailed) != std::string::npos;
}

} // namespace kube32::runtime
