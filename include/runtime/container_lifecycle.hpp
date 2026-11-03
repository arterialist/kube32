#pragma once

#include "runtime/wasm_runtime.hpp"
#include "runtime/pod_memory_pool.h"
#include "osal/thread.hpp"
#include "osal/mutex.hpp"
#include "osal/timer.hpp"
#include "mem/function_ref.hpp"

#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <initializer_list>
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
    // The operand stack is sized from the compiled module (its deepest call
    // chain, module_stack_need_bytes), stack_size_bytes at most
    bool stack_from_module{false};
    RestartPolicy restart_policy{RestartPolicy::Always};
    ProbeSpec liveness_probe;
    ProbeSpec readiness_probe;
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
    uint64_t native_stack_left{0};           // native stack below the call when it began; 0 = not known
    int64_t native_used{0};                  // native stack the call had used at a native stack trap
    int64_t native_limit{0};                 // the trap limit, below the start of the call
    bool native_on_interpreter{false};       // ... on the shared interpreter stack
};

TrapReport describe_trap(const char* trap, const TrapContext& ctx);

// Runs `fn` on the shared interpreter stack when one is set
// (kube32_interp_stack_set) and the caller is on no interpreter stack (the
// shared one, or a pod worker's own, kube32_interp_stack_add); otherwise on
// the caller's stack. `fn` is invoked in place: nothing is taken
// from the heap.
void run_on_interpreter_stack(mem::FunctionRef<void()> fn);

// Whether a module loading now gets the compiler's scratch lent from the
// shared interpreter stack (boards without PSRAM) instead of allocating it.
bool compile_scratch_lent();

// True once the container owning this wasm3 runtime is being stopped. Host
// calls that wait (sleeps, channel receives, tones) check it and return early,
// so the guest gets back to metered code and takes the stop trap.
bool runtime_stop_requested(const void* m3_runtime);

// While alive, what the runtime allocates on this thread is charged to a
// pod's memory account (runtime/pod_memory_pool.h)
class PodMemoryScope {
public:
    explicit PodMemoryScope(int account) : previous_(kube32_pod_account_enter(account)) {}
    ~PodMemoryScope() { kube32_pod_account_leave(previous_); }
    PodMemoryScope(const PodMemoryScope&) = delete;
    PodMemoryScope& operator=(const PodMemoryScope&) = delete;

private:
    int previous_;
};

class ContainerLifecycleController {
public:
    explicit ContainerLifecycleController(const ContainerConfig& config);
    ~ContainerLifecycleController();

    // Lifecycle transitions
    bool deploy(const uint8_t* wasm_bytes, size_t len);
    // Takes the bytes of a module read into pod memory (from the card or the
    // API server) and charges them to the pod: no second copy is made, and
    // restarts reuse them.
    bool deploy(ModuleBytes&& wasm_bytes);
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
    // Everything the container holds (ContainerStatus::held_bytes); 0 when
    // it has no memory account
    uint64_t held_bytes() const;
    // The account the runtime's allocations for this container are charged to
    int memory_account() const { return memory_account_; }
    bool is_running() const;

    // Health probe execution hooks
    bool run_liveness_probe();
    bool run_readiness_probe();

    // Internal trap callback (invoked by worker thread on interpreter trap)
    void on_trap_caught(const std::string& trap_message, int32_t exit_code, const std::string& reason);

    // Synchronous function invocation helper. Arguments are passed as an
    // array (a braced list or a vector): a call takes nothing from the heap.
    int32_t call_function(const std::string& fn_name, std::initializer_list<int32_t> args = {}) {
        return call_function(fn_name, args.begin(), args.size());
    }
    int32_t call_function(const std::string& fn_name, const std::vector<int32_t>& args) {
        return call_function(fn_name, args.data(), args.size());
    }
    int32_t call_function(const std::string& fn_name, const int32_t* args, size_t argc);

    // Introspection used by the application ABI host
    bool has_export(const char* fn_name) const;
    bool find_export_locked(const char* fn_name) const;   // mutex_ held by caller
    uint8_t* guest_memory(size_t& out_size) const;
    void* m3_runtime_handle() const { return m3_runtime_; }
    void* m3_module_handle() const { return m3_module_; }
    const ContainerConfig& config() const { return config_; }
    // SHA-256 of the module's bytes, computed on first use
    void image_sha256(uint8_t out[32]);

private:
    static void worker_thread_entry(void* arg);
    void worker_thread_main();
    void run_entry_point();
    void on_trap_caught_locked(const std::string& trap_message, int32_t exit_code, const std::string& reason);
    uint64_t held_bytes_locked() const;   // mutex_ held by caller
    void cleanup_runtime();

    ContainerConfig config_;
    mutable osal::Mutex mutex_;          // status and runtime pointers
    osal::Mutex call_mutex_;             // one call into the module at a time (call_function, deploy)
    ContainerStatus status_;
    
    ModuleBytes wasm_bytecode_;               // the module's bytes when the image is not memory mapped
    const uint8_t* wasm_view_{nullptr};       // bytes the module was parsed from
    size_t wasm_view_len_{0};
    std::atomic<bool> stop_requested_{false};
    void* pod_stack_{nullptr};               // boot-reserved stack (boards without PSRAM)
    std::atomic<bool> pause_requested_{false};

    // Low-level Wasm3 opaque pointers
    void* m3_env_{nullptr};
    void* m3_runtime_{nullptr};
    void* m3_module_{nullptr};
    uint32_t wasm_stack_bytes_{0};          // the operand stack the module has (set by deploy)
    uint8_t image_sha_[32]{};
    bool image_sha_known_{false};

    // A stack sized from the module: what it has while the module loads
    // (constant expressions), and what it gets above its computed need
    static constexpr uint32_t kLoadStackBytes = 256;
    static constexpr uint32_t kStackMarginBytes = 64;

    std::string thread_name_;
    std::unique_ptr<osal::Thread> worker_thread_;
    std::atomic<bool> thread_detached_{false};
    std::atomic<bool> worker_done_{false};     // worker_thread_main returned
    std::atomic<bool> stop_gave_up_{false};    // stop() returned while the worker was still running
    std::atomic<uint32_t> worker_stack_bytes_{0};   // the worker thread's stack while it exists
    int memory_account_{0};                    // kube32_pod_account_open
    // CPU time of the worker thread: read from the thread while it runs
    // (mutex_ keeps it from ending meanwhile), kept once it ended
    osal::Thread* worker_cpu_thread_{nullptr};      // mutex_
    uint64_t worker_cpu_us_{0};                     // mutex_
    bool worker_cpu_measured_{false};               // mutex_
    std::atomic<uint64_t> worker_nested_cpu_us_{0}; // calls into other pods made on the worker

    uint32_t consecutive_liveness_failures_{0};
    uint32_t consecutive_readiness_failures_{0};
};

} // namespace kube32::runtime
