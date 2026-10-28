#include "runtime/wasm_runtime.hpp"
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif
#include "runtime/container_lifecycle.hpp"
#include "runtime/app_pod.hpp"
#include "runtime/k32_app_abi.h"
#include "osal/mutex.hpp"

#include <map>
#include <vector>

namespace kube32::runtime {

struct WasmRuntime::Impl {
    mutable osal::Mutex mutex;
    std::map<std::string, std::unique_ptr<ContainerLifecycleController>> containers;
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
    out_error.clear();
    osal::LockGuard guard(impl_->mutex);

    ContainerConfig config;
    config.pod_id = pod_id;
    config.container_name = pod_id;
    config.memory_limit_bytes = (spec.memory_limit_bytes > 0) ? spec.memory_limit_bytes : (64 * 1024);
    config.stack_size_bytes = (spec.stack_size_bytes > 0) ? spec.stack_size_bytes : (8 * 1024);
    if (config.memory_limit_bytes <= 64 * 1024 && config.stack_size_bytes > 4 * 1024) {
        config.stack_size_bytes = 4 * 1024; // one-page pods are small programs
    }
    config.restart_policy = RestartPolicy::Always;
    config.env = spec.env;

    AppPodHost::instance().set_env(pod_id, spec.env);

    const uint64_t t_load = osal::Thread::get_time_us();
#if defined(ESP_PLATFORM)
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif
    auto controller = std::make_unique<ContainerLifecycleController>(config);
    if (!controller->deploy(wasm_bytes, len)) {
        out_error = controller->get_status().exit_message;
        AppPodHost::instance().unregister_pod(pod_id);
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
    AppPodHost::instance().unregister_pod(pod_id);
    {
        osal::LockGuard guard(impl_->mutex);
        auto it = impl_->containers.find(pod_id);
        if (it == impl_->containers.end()) return false;
        target = std::move(it->second);
        impl_->containers.erase(it);
    }
    return target->stop(1000);
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

} // namespace kube32::runtime
