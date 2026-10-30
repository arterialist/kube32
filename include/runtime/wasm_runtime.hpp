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
    bool stack_size_set{false};                     // stack_size_bytes given by the pod (kube32.io/wasm-stack): used as is
    std::vector<std::pair<std::string, std::string>> env;
    uint64_t gas_budget_per_call{4000000000ULL};   // 0: no metering (see ContainerConfig)
    std::vector<std::string> event_topics;          // channel topics delivered to k32_event
    uint32_t tick_interval_ms{1000};                // k32_tick period (kube32.io/tick-interval)
    // Bundle files the pod may send with k32_resp_asset: an embedded bundle
    // (images/web/<name>/) or a directory on node storage
    std::string asset_bundle;
    std::string asset_directory;
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
    // Deploys an image the caller owns on the heap; the runtime keeps the
    // buffer for the module's lifetime instead of copying it (the only way a
    // 64 KiB image fits a board with 90 KB of free RAM).
    virtual bool deploy_workload_owned(const std::string& pod_id, std::vector<uint8_t>&& wasm_bytes, const DeploySpec& spec, std::string& out_error) {
        std::vector<uint8_t> bytes = std::move(wasm_bytes);
        return deploy_workload_ex(pod_id, bytes.data(), bytes.size(), spec, out_error);
    }
    virtual bool start(const std::string& pod_id) = 0;
    virtual bool pause(const std::string& pod_id) = 0;
    virtual bool terminate(const std::string& pod_id) = 0;
    virtual ContainerStatus get_status(const std::string& pod_id) = 0;
    // Frees containers whose stop had to wait for a host call to return, once
    // it has (called periodically by the kubelet).
    virtual void reap_stopped() {}
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
    void reap_stopped() override;
    // Stopped containers whose thread has not ended yet
    size_t lingering_count() const;

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
    bool deploy_workload_owned(const std::string& pod_id, std::vector<uint8_t>&& wasm_bytes, const DeploySpec& spec, std::string& out_error) override;

private:
    struct Impl;
    bool deploy_impl(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, std::vector<uint8_t>* owned, const DeploySpec& spec, std::string& out_error);
    std::unique_ptr<Impl> impl_;
};

// Factory function
std::unique_ptr<WasmRuntime> create_wasm_runtime();

// Memory a pod needs to start on the board: its runtime, value stack, linear
// memory (memory_limit_bytes below one page, else the module's initial
// pages), compiled code and module structures, its host-side state, and the
// compiler's scratch while it loads. Estimated from the module's sections
// with the factors measured on a 32-bit board; `wasm` may be null when only
// the size is known.
size_t estimate_pod_memory(const uint8_t* wasm, size_t len, uint32_t memory_limit_bytes, uint32_t stack_bytes);

// Whether a deploy error is the node running out of memory while the module
// loaded, rather than a fault of the module.
bool deploy_error_is_out_of_memory(const std::string& error);

} // namespace kube32::runtime
