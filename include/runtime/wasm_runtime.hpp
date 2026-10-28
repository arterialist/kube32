#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <memory>
#include <vector>
#include <map>

namespace kube32::runtime {

enum class ContainerPhase {
    Pending,
    Running,
    Paused,
    Terminated,
    Failed
};

struct ContainerStatus {
    ContainerPhase phase{ContainerPhase::Pending};
    int restart_count{0};
    uint32_t memory_used_bytes{0};   // Wasm linear memory currently mapped
    uint64_t bytecode_bytes{0};      // module bytes the controller keeps for restarts
    uint64_t stack_bytes{0};         // interpreter operand stack reserved for the module
    uint64_t runtime_bytes{0};       // backing bytes of the module's memories as wasm3 accounts them (c_m3Limit_MemoryBytes)
    std::string exit_message;
    bool ready{false};
    uint64_t start_time_ms{0};
    int32_t exit_code{0};
    std::string termination_reason;
};

// Full deployment spec used by the kubelet: memory limit, env, stack.
struct DeploySpec {
    uint32_t memory_limit_bytes{64 * 1024};
    uint32_t stack_size_bytes{16 * 1024};
    std::vector<std::pair<std::string, std::string>> env;
};

// Abstract Runtime Interface conforming to PROJECT.md § 4.3
class Runtime {
public:
    virtual ~Runtime() = default;
    virtual bool deploy_workload(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, uint32_t memory_limit_bytes) = 0;
    // Default forwards to deploy_workload; WasmRuntime overrides with env and app registration.
    virtual bool deploy_workload_ex(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, const DeploySpec& spec, std::string& out_error) {
        bool ok = deploy_workload(pod_id, wasm_bytes, len, spec.memory_limit_bytes);
        if (!ok) out_error = "runtime rejected module";
        return ok;
    }
    virtual bool start(const std::string& pod_id) = 0;
    virtual bool pause(const std::string& pod_id) = 0;
    virtual bool terminate(const std::string& pod_id) = 0;
    virtual ContainerStatus get_status(const std::string& pod_id) = 0;
};

class WasmRuntime : public Runtime {
public:
    WasmRuntime();
    ~WasmRuntime() override;

    // Runtime interface implementation
    bool deploy_workload(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, uint32_t memory_limit_bytes) override;
    bool start(const std::string& pod_id) override;
    bool pause(const std::string& pod_id) override;
    bool terminate(const std::string& pod_id) override;
    ContainerStatus get_status(const std::string& pod_id) override;

    // Extended runtime management operations
    bool resume(const std::string& pod_id);
    bool restart(const std::string& pod_id);
    bool is_deployed(const std::string& pod_id) const;
    std::vector<std::string> list_containers() const;

    // Direct execution helper for unit testing and synchronous execution
    int32_t execute_function(const std::string& pod_id, const std::string& fn_name, const std::vector<int32_t>& args = {});

    // Full deployment used by the kubelet: memory limit, env, and registration
    // of application pods (modules exporting k32_* entry points). On success
    // `out_error` is empty; otherwise it carries a human readable reason.
    bool deploy_workload_ex(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, const DeploySpec& spec, std::string& out_error) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Factory function
std::unique_ptr<WasmRuntime> create_wasm_runtime();

} // namespace kube32::runtime
