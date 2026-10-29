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
    std::vector<std::pair<std::string, std::string>> env; // exposed through k32_env_get
    // wasm3 gas units each host-initiated call may spend (k32_init, k32_handle,
    // k32_tick, k32_event, k32_alloc). 0 turns metering off for this pod: its
    // code compiles without gas checks (faster) and a call cannot be
    // interrupted, so deleting the pod waits for the running call to return.
    uint64_t gas_budget_per_call{kDefaultGasBudgetPerCall};
    static constexpr uint64_t kDefaultGasBudgetPerCall = 4000000000ULL;
};

// How a trapped container is reported. A trap stands for the signal a native
// process would die of, and the status follows what a Linux container runtime
// reports for it: exit code 128 + signal with reason "Error" (139 SIGSEGV for
// out-of-bounds access, bad indirect calls and stack overflow, 136 SIGFPE for
// integer traps, 134 SIGABRT for unreachable/abort, 132 SIGILL, 135 SIGBUS,
// 152 SIGXCPU for a call that spends its execution budget, 137 SIGKILL when
// the kubelet stops the container). "OOMKilled" (137) is kept for memory
// exhaustion: the runtime failing to allocate, or the program aborting after
// a memory.grow was refused at the pod's memory limit.
struct TrapReport {
    int32_t exit_code{134};
    std::string reason{"Error"};
    std::string message;
};

struct TrapContext {
    bool stop_requested{false};              // the kubelet was stopping the container
    const char* memory_grow_error{nullptr};  // why the last memory.grow of the call was refused, if one was
    uint64_t memory_limit_bytes{0};
    uint64_t gas_budget{0};                  // per-call budget; 0 = unmetered or unbounded
    uint64_t wasm_stack_bytes{0};            // the module's operand stack (kube32.io/wasm-stack)
};

TrapReport describe_trap(const char* trap, const TrapContext& ctx);

// Runs `fn` on the shared interpreter stack when one is set (boards without
// PSRAM, kube32_interp_stack_set) and the caller is not on it already;
// otherwise on the caller's stack.
void run_on_interpreter_stack(const std::function<void()>& fn);

// True once the container owning this wasm3 runtime is being stopped. Host
// calls that wait (sleeps, channel receives, tones) check it and return early,
// so the guest gets back to metered code and takes the stop trap.
bool runtime_stop_requested(const void* m3_runtime);

class ContainerLifecycleController {
public:
    explicit ContainerLifecycleController(const ContainerConfig& config);
    ~ContainerLifecycleController();

    // Lifecycle transitions
    bool deploy(const uint8_t* wasm_bytes, size_t len);
    // Takes ownership of a heap image (pulled from the card or decoded from a
    // manifest) so no second copy is ever made; kept for restarts.
    bool deploy(std::vector<uint8_t>&& wasm_bytes);
    bool start();
    bool pause();
    bool resume();
    bool stop(uint32_t grace_period_ms = 5000);
    // Marks the container as stopping without waiting: blocking host calls
    // return early from here on.
    void request_stop();
    // stop() gave up waiting while the guest was inside a host call: the
    // thread, its stack and the module's memory are still in use. Becomes
    // false once the thread has ended; destroying the controller then frees
    // them (WasmRuntime keeps such controllers until that happens).
    bool lingering() const;
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

    // Introspection used by the application ABI host
    bool has_export(const char* fn_name) const;
    bool find_export_locked(const char* fn_name) const;   // mutex_ held by caller
    uint8_t* guest_memory(size_t& out_size) const;
    void* m3_runtime_handle() const { return m3_runtime_; }
    const ContainerConfig& config() const { return config_; }

private:
    static void worker_thread_entry(void* arg);
    void worker_thread_main();
    void on_trap_caught_locked(const std::string& trap_message, int32_t exit_code, const std::string& reason);
    void cleanup_runtime();

    ContainerConfig config_;
    mutable osal::Mutex mutex_;          // status and runtime pointers
    osal::Mutex call_mutex_;             // one call into the module at a time (call_function, deploy)
    ContainerStatus status_;
    
    std::vector<uint8_t> wasm_bytecode_;      // owned copy when the image is not memory mapped
    const uint8_t* wasm_view_{nullptr};       // bytes the module was parsed from
    size_t wasm_view_len_{0};
    std::atomic<bool> stop_requested_{false};
    void* pod_stack_{nullptr};               // boot-reserved stack (boards without PSRAM)
    std::atomic<bool> pause_requested_{false};

    // Low-level Wasm3 opaque pointers
    void* m3_env_{nullptr};
    void* m3_runtime_{nullptr};
    void* m3_module_{nullptr};

    std::string thread_name_;
    std::unique_ptr<osal::Thread> worker_thread_;
    std::atomic<bool> thread_detached_{false};
    std::atomic<bool> worker_done_{false};     // worker_thread_main returned
    std::atomic<bool> stop_gave_up_{false};    // stop() returned while the worker was still running

    uint32_t consecutive_liveness_failures_{0};
    uint32_t consecutive_readiness_failures_{0};
};

} // namespace kube32::runtime
