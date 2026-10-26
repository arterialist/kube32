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
    uint32_t memory_used_bytes{0};
    std::string exit_message;
    bool ready{false};
    uint64_t start_time_ms{0};
    int32_t exit_code{0};
    std::string termination_reason;
};

// Abstract Runtime Interface conforming to PROJECT.md § 4.3
class Runtime {
public:
    virtual ~Runtime() = default;
    virtual bool deploy_workload(const std::string& pod_id, const uint8_t* wasm_bytes, size_t len, uint32_t memory_limit_bytes) = 0;
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Factory function
std::unique_ptr<WasmRuntime> create_wasm_runtime();

} // namespace kube32::runtime
