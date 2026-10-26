#pragma once

#include "runtime/wasm_runtime.hpp"
#include "osal/thread.hpp"
#include "osal/mutex.hpp"
#include "osal/timer.hpp"

#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <atomic>

namespace kube32::runtime {

enum class RestartPolicy {
    Always,
    OnFailure,
    Never
};

enum class ProbeType {
    ExecWasm,
    HttpGet,
    TcpSocket
};

struct ProbeSpec {
    bool enabled{false};
    ProbeType type{ProbeType::ExecWasm};
    std::string handler; // Function name (e.g. "probe_live") or HTTP path ("/healthz")
    uint16_t port{0};
    uint32_t initial_delay_seconds{0};
    uint32_t period_seconds{10};
    uint32_t timeout_seconds{1};
    uint32_t failure_threshold{3};
    uint32_t success_threshold{1};
};

struct ContainerConfig {
    std::string pod_id;
    std::string container_name;
    uint32_t memory_limit_bytes{64 * 1024};
    uint32_t stack_size_bytes{8 * 1024};
    RestartPolicy restart_policy{RestartPolicy::Always};
    ProbeSpec liveness_probe;
    ProbeSpec readiness_probe;
};

class ContainerLifecycleController {
public:
    explicit ContainerLifecycleController(const ContainerConfig& config);
    ~ContainerLifecycleController();

    // Lifecycle transitions
    bool deploy(const uint8_t* wasm_bytes, size_t len);
    bool start();
    bool pause();
    bool resume();
    bool stop(uint32_t grace_period_ms = 5000);
    bool restart();

    // Telemetry and status
    ContainerStatus get_status();
    uint32_t get_memory_usage_bytes() const;
    bool is_running() const;

    // Health probe execution hooks
    bool run_liveness_probe();
    bool run_readiness_probe();

    // Internal trap callback (invoked by worker thread on interpreter trap)
    void on_trap_caught(const std::string& trap_message, int32_t exit_code, const std::string& reason);

    // Synchronous function invocation helper
    int32_t call_function(const std::string& fn_name, const std::vector<int32_t>& args = {});

private:
    static void worker_thread_entry(void* arg);
    void worker_thread_main();
    void on_trap_caught_locked(const std::string& trap_message, int32_t exit_code, const std::string& reason);
    void calculate_and_apply_backoff();
    void cleanup_runtime();

    ContainerConfig config_;
    mutable osal::Mutex mutex_;
    ContainerStatus status_;
    
    std::vector<uint8_t> wasm_bytecode_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> pause_requested_{false};

    // Low-level Wasm3 opaque pointers
    void* m3_env_{nullptr};
    void* m3_runtime_{nullptr};
    void* m3_module_{nullptr};

    std::string thread_name_;
    std::unique_ptr<osal::Thread> worker_thread_;
    std::atomic<bool> thread_detached_{false};

    uint32_t consecutive_liveness_failures_{0};
    uint32_t consecutive_readiness_failures_{0};
    uint64_t next_restart_allowed_time_ms_{0};
};

} // namespace kube32::runtime
