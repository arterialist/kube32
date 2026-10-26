#include "runtime/wasm_runtime.hpp"
#include "runtime/container_lifecycle.hpp"
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
