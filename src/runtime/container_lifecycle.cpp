#include "runtime/container_lifecycle.hpp"
#include "runtime/pod_memory_pool.h"
#include <cstdint>
#include "runtime/pod_log.hpp"
#if defined(ESP_PLATFORM)
#include "esp_memory_utils.h"
#include "esp_heap_caps.h"
#endif
#include "runtime/host_functions.hpp"
#include "runtime/app_pod.hpp"
#include "runtime/pod_fs.hpp"
#include "osal/system.hpp"
#include "osal/socket.hpp"

#include "wasm3.h"
#include "m3_config.h"
#include "m3_env.h"

#include <iostream>
#include <algorithm>
#include <mutex>
#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#if defined(ESP_PLATFORM)
#include "esp_expression_with_stack.h"
#include "freertos/semphr.h"
#endif

namespace kube32::runtime {

#if defined(ESP_PLATFORM)
static std::mutex s_interp_mutex;
static SemaphoreHandle_t s_interp_lock = nullptr;
static const std::function<void()>* s_interp_fn = nullptr;
static void interp_trampoline() { if (s_interp_fn) (*s_interp_fn)(); }
#endif

void run_on_interpreter_stack(const std::function<void()>& fn) {
#if defined(ESP_PLATFORM)
    if (kube32_interp_stack_active() && !kube32_interp_stack_base_for(__builtin_frame_address(0))) {
        size_t size = 0;
        void* stack = kube32_interp_stack_get(&size);
        std::lock_guard<std::mutex> lock(s_interp_mutex);
        if (!s_interp_lock) s_interp_lock = xSemaphoreCreateMutex();
        s_interp_fn = &fn;
        esp_execute_shared_stack_function(s_interp_lock, stack, size, interp_trampoline);
        s_interp_fn = nullptr;
        return;
    }
#endif
    fn();
}

static osal::RecursiveMutex s_wasm3_global_mutex;

// Runtimes of containers being stopped (see runtime_stop_requested)
static std::mutex s_stopping_mutex;
static std::vector<const void*> s_stopping;

static void set_stopping(const void* rt, bool stopping) {
    if (!rt) return;
    std::lock_guard<std::mutex> lock(s_stopping_mutex);
    auto it = std::find(s_stopping.begin(), s_stopping.end(), rt);
    if (stopping && it == s_stopping.end()) s_stopping.push_back(rt);
    if (!stopping && it != s_stopping.end()) s_stopping.erase(it);
}

bool runtime_stop_requested(const void* m3_runtime) {
    std::lock_guard<std::mutex> lock(s_stopping_mutex);
    return std::find(s_stopping.begin(), s_stopping.end(), m3_runtime) != s_stopping.end();
}

TrapReport describe_trap(const char* trap, const TrapContext& ctx) {
    std::string what = trap ? trap : "unknown trap";
    if (what.rfind("[trap] ", 0) == 0) what.erase(0, 7);
    const std::string limit = std::to_string(ctx.memory_limit_bytes / 1024) + " KiB";
    TrapReport t;
    t.message = "wasm trap: " + what;

    if (trap == m3Err_mallocFailed || trap == m3Err_mallocFailedCodePage || trap == m3Err_memoryLimitExceeded ||
        trap == m3Err_wasmMemoryOverflow) {
        t.exit_code = 137;
        t.reason = "OOMKilled";
        t.message = "runtime out of memory: " + what + " (pod memory limit " + limit + ")";
        return t;
    }
    if (trap == m3Err_trapOutOfGas) {
        if (ctx.stop_requested || ctx.gas_budget == 0) {
            t.exit_code = 137;   // SIGKILL
            t.message = "stopped by the kubelet (" + t.message + ")";
        } else {
            t.exit_code = 152;   // SIGXCPU
            t.message += ": a call used its whole execution budget of " + std::to_string(ctx.gas_budget) +
                         " gas units (kube32.io/execution-budget)";
        }
        return t;
    }
    if (trap == m3Err_trapWasmStackOverflow) {
        t.exit_code = 139;   // SIGSEGV, as a native stack overflow
        t.message = "wasm trap: the module's stack of " + std::to_string(ctx.wasm_stack_bytes) +
                    " bytes is exhausted (raise it with the annotation kube32.io/wasm-stack)";
        return t;
    }
    if (trap == m3Err_trapOutOfBoundsMemoryAccess || trap == m3Err_trapStackOverflow || trap == m3Err_trapTableOutOfBounds ||
        trap == m3Err_trapTableIndexOutOfRange || trap == m3Err_trapTableElementIsNull ||
        trap == m3Err_trapIndirectCallTypeMismatch || trap == m3Err_trapNullFunctionRef || trap == m3Err_trapNullReference) {
        t.exit_code = 139;   // SIGSEGV
    } else if (trap == m3Err_trapDivisionByZero || trap == m3Err_trapIntegerOverflow || trap == m3Err_trapIntegerConversion) {
        t.exit_code = 136;   // SIGFPE
    } else if (trap == m3Err_trapUnsupportedInstruction) {
        t.exit_code = 132;   // SIGILL
    } else if (trap == m3Err_trapUnalignedAtomic) {
        t.exit_code = 135;   // SIGBUS
    } else {
        t.exit_code = 134;   // SIGABRT: unreachable, abort, uncaught exception, anything else
    }

    // memory.grow fails with -1 rather than trapping; allocators turn that into
    // an abort. Refused at the pod's limit (or for lack of node memory), the
    // abort is the program running out of memory.
    const bool grow_refused = ctx.memory_grow_error == m3Err_memoryLimitExceeded || ctx.memory_grow_error == m3Err_mallocFailed;
    if (grow_refused) {
        if (t.exit_code == 134) {
            t.exit_code = 137;
            t.reason = "OOMKilled";
            t.message = "memory.grow refused at the pod memory limit of " + limit + ", then " + t.message;
        } else {
            t.message += " (after a memory.grow was refused at the pod memory limit of " + limit + ")";
        }
    }
    return t;
}

ContainerLifecycleController::ContainerLifecycleController(const ContainerConfig& config)
    : config_(config) {
    status_.phase = ContainerPhase::Pending;
    status_.restart_count = 0;
    status_.memory_used_bytes = 0;
    status_.ready = false;
    status_.start_time_ms = 0;
    status_.exit_code = 0;
}

ContainerLifecycleController::~ContainerLifecycleController() {
    stop(100);
    if (lingering()) {
        // Still inside a host call: the thread keeps using the runtime, its
        // stack and the thread object, so all of them are left to it.
        worker_thread_->detach();
        (void)worker_thread_.release();
        pod_stack_ = nullptr;
        thread_detached_.store(true);
    }
    cleanup_runtime();
}

bool ContainerLifecycleController::lingering() const {
    return worker_thread_ && worker_thread_->is_joinable() && !worker_done_.load();
}

void ContainerLifecycleController::request_stop() {
    stop_requested_.store(true);
    set_stopping(m3_runtime_, true);
}

void ContainerLifecycleController::cleanup_runtime() {
    osal::LockGuard<osal::RecursiveMutex> global_guard(s_wasm3_global_mutex);
    if (thread_detached_.load()) {
        // Runaway guest thread was detached; preserve runtime allocation to prevent Use-After-Free
        m3_runtime_ = nullptr;
        m3_module_ = nullptr;
        m3_env_ = nullptr;
        return;
    }
    if (m3_runtime_) {
        set_stopping(m3_runtime_, false);
        m3_FreeRuntime(reinterpret_cast<IM3Runtime>(m3_runtime_));
        m3_runtime_ = nullptr;
        m3_module_ = nullptr; // Freed by m3_FreeRuntime
    }
    if (m3_env_) {
        m3_FreeEnvironment(reinterpret_cast<IM3Environment>(m3_env_));
        m3_env_ = nullptr;
    }
}

bool ContainerLifecycleController::deploy(std::vector<uint8_t>&& wasm_bytes) {
    {
        osal::LockGuard guard(mutex_);
        wasm_bytecode_ = std::move(wasm_bytes);
    }
    return deploy(wasm_bytecode_.data(), wasm_bytecode_.size());
}

bool ContainerLifecycleController::deploy(const uint8_t* wasm_bytes, size_t len) {
    osal::LockGuard call_guard(call_mutex_);
    osal::LockGuard guard(mutex_);
    osal::LockGuard<osal::RecursiveMutex> global_guard(s_wasm3_global_mutex);

    cleanup_runtime();
    // A fresh instance starts with no SD files open (also on restart)
    PodFs::instance().release_pod(config_.pod_id);

    if (!wasm_bytes || len == 0) {
        status_.phase = ContainerPhase::Failed;
        status_.exit_message = "Empty Wasm payload";
        return false;
    }

    // wasm3 keeps pointers into the binary for the lifetime of the module, so
    // the bytes must stay valid. Images embedded in flash are memory mapped and
    // are used in place; anything else (manifest-inline images) is copied.
#if defined(ESP_PLATFORM)
    if (esp_ptr_in_drom(wasm_bytes)) {
        wasm_bytecode_.clear();
        wasm_bytecode_.shrink_to_fit();
        wasm_view_ = wasm_bytes;
        wasm_view_len_ = len;
    } else
#endif
    {
        if (wasm_bytes != wasm_bytecode_.data()) wasm_bytecode_.assign(wasm_bytes, wasm_bytes + len);   // restart reuses the copy
        wasm_view_ = wasm_bytecode_.data();
        wasm_view_len_ = wasm_bytecode_.size();
    }

    // A pod whose linear memory wants the pool's block whole keeps its own
    // runtime structures in the general heap, so the block stays free for it
    struct PreferGeneral {
        bool on;
        explicit PreferGeneral(bool o) : on(o) { if (on) kube32_pool_prefer_general(1); }
        ~PreferGeneral() { if (on) kube32_pool_prefer_general(0); }
    } prefer_general(config_.memory_limit_bytes >= kube32_pool_threshold());
    m3_env_ = m3_NewEnvironment();
    if (!m3_env_) {
        status_.phase = ContainerPhase::Failed;
        status_.exit_message = "Failed to create Wasm3 environment";
        return false;
    }

    m3_runtime_ = m3_NewRuntime(reinterpret_cast<IM3Environment>(m3_env_), config_.stack_size_bytes, nullptr);
    // wasm3 compiles functions lazily and only emits gas checks into code
    // compiled while a gas limit is set. A metered pod is armed now so every
    // function is metered: that lets stop() interrupt a guest loop
    // (gasRemaining = -1) and bounds each call. Unmetered pods never get a
    // limit and run without the checks.
    if (m3_runtime_ && config_.gas_budget_per_call > 0) m3_SetResourceLimit(reinterpret_cast<IM3Runtime>(m3_runtime_), c_m3Limit_GasUnits, static_cast<uint64_t>(INT64_MAX));
    if (!m3_runtime_) {
        status_.phase = ContainerPhase::Failed;
        status_.exit_message = "Failed to create Wasm3 runtime";
        cleanup_runtime();
        return false;
    }

    // Enforce memory limits in bytes. A limit below one 64 KiB page caps the
    // backing of the first page instead: the module sees a page, only the
    // limit is allocated, accesses past it trap and memory.grow fails.
    if (config_.memory_limit_bytes > 0) {
        IM3Runtime rt = reinterpret_cast<IM3Runtime>(m3_runtime_);
        m3_SetResourceLimit(rt, c_m3Limit_MemoryBytes, static_cast<uint64_t>(config_.memory_limit_bytes));
        if (config_.memory_limit_bytes < d_m3DefaultMemPageSize) rt->memoryLimit = config_.memory_limit_bytes;
    }


    IM3Module module = nullptr;
    M3Result r = m3_ParseModule(reinterpret_cast<IM3Environment>(m3_env_), &module, wasm_view_, wasm_view_len_);
    if (r != m3Err_none) {
        status_.phase = ContainerPhase::Failed;
        status_.exit_message = std::string("Wasm parse error: ") + r;
        cleanup_runtime();
        return false;
    }

    r = m3_LoadModule(reinterpret_cast<IM3Runtime>(m3_runtime_), module);
    if (r != m3Err_none) {
        status_.phase = ContainerPhase::Failed;
        status_.exit_message = std::string("Wasm load error: ") + r;
#if defined(ESP_PLATFORM)
        if (r == m3Err_mallocFailed) {
            size_t pool_ext = 0;
            (void)pool_ext;
            status_.exit_message += " (free heap " + std::to_string(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) +
                                    ", largest block " + std::to_string(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) +
                                    ", pod pool " + std::to_string(kube32_pool_available(nullptr)) + ")";
        }
#endif
        cleanup_runtime();
        return false;
    }

    m3_module_ = module;

    // Link standard Kube32 host functions and the application ABI imports
    link_host_functions(module, config_.pod_id);
    link_app_host_functions(module, config_.pod_id);

    // Optional pre-compile
    m3_CompileModule(module);

    size_t mem_size = m3_GetMemorySize(module, 0);
    status_.memory_used_bytes = static_cast<uint32_t>(mem_size);
    status_.bytecode_bytes = wasm_bytecode_.size();
    status_.stack_bytes = config_.stack_size_bytes;
    status_.runtime_bytes = m3_GetResourceUsage(reinterpret_cast<IM3Runtime>(m3_runtime_), c_m3Limit_MemoryBytes);
    status_.phase = ContainerPhase::Pending;
    status_.ready = false;
    status_.exit_message.clear();
    status_.termination_reason.clear();
    status_.exit_code = 0;

    return true;
}

void ContainerLifecycleController::worker_thread_entry(void* arg) {
    auto* self = static_cast<ContainerLifecycleController*>(arg);
    if (self) {
        self->worker_thread_main();
    }
}

void ContainerLifecycleController::worker_thread_main() {
    IM3Function entry_fn = nullptr;
    IM3Runtime runtime = reinterpret_cast<IM3Runtime>(m3_runtime_);

    if (!runtime) { worker_done_.store(true); return; }

    // Look for standard entrypoints: _start, app_main, main
    if (m3_FindFunction(&entry_fn, runtime, "_start") != m3Err_none) {
        if (m3_FindFunction(&entry_fn, runtime, "app_main") != m3Err_none) {
            m3_FindFunction(&entry_fn, runtime, "main");
        }
    }

    if (entry_fn) {
        runtime->lastMemoryGrowError = m3Err_none;
        M3Result r = m3_CallV(entry_fn);

        if (r == m3Err_none) {
            // An i32 result (app_main, main) is the exit code; _start returns nothing.
            int32_t ret = 0;
            if (m3_GetRetCount(entry_fn) == 1 && m3_GetRetType(entry_fn, 0) == c_m3Type_i32) {
                if (m3_GetResultsV(entry_fn, &ret) != m3Err_none) ret = 0;
            }
            PodLogStore::runtime().append(config_.pod_id, "[kube32] entry point returned " + std::to_string(ret) +
                                                              (ret == 0 ? ", container completed" : ", container failed"));
            osal::LockGuard guard(mutex_);
            status_.phase = ContainerPhase::Terminated;
            status_.exit_code = ret;
            status_.termination_reason = (ret == 0) ? "Completed" : "Error";
            status_.exit_message = "entry point returned " + std::to_string(ret);
            status_.ready = false;
        } else {
            // The entry point runs under an unbounded gas limit, so running out
            // of gas here means stop() interrupted it.
            TrapContext ctx;
            ctx.stop_requested = stop_requested_.load();
            ctx.memory_grow_error = runtime->lastMemoryGrowError;
            ctx.memory_limit_bytes = config_.memory_limit_bytes;
            ctx.wasm_stack_bytes = config_.stack_size_bytes;
            TrapReport t = describe_trap(r, ctx);
            on_trap_caught(t.message, t.exit_code, t.reason);
        }
    } else {
        // Event loop or library workload without main entrypoint
        while (!stop_requested_.load()) {
            osal::Thread::sleep_ms(100);
            osal::System::feed_watchdog();
        }
        osal::LockGuard guard(mutex_);
        status_.phase = ContainerPhase::Terminated;
        status_.exit_code = 0;
        status_.ready = false;
    }
    worker_done_.store(true);
}

void ContainerLifecycleController::on_trap_caught_locked(const std::string& trap_message, int32_t exit_code, const std::string& reason) {
    status_.phase = ContainerPhase::Failed;
    status_.exit_code = exit_code;
    status_.termination_reason = reason;
    status_.exit_message = trap_message;
    status_.ready = false;
}

void ContainerLifecycleController::on_trap_caught(const std::string& trap_message, int32_t exit_code, const std::string& reason) {
    PodLogStore::runtime().append(config_.pod_id, "[kube32] container trapped: " + trap_message + " (" + reason +
                                                      ", exit code " + std::to_string(exit_code) + ")");
    osal::LockGuard guard(mutex_);
    on_trap_caught_locked(trap_message, exit_code, reason);
}

bool ContainerLifecycleController::start() {
    osal::LockGuard guard(mutex_);

    if (!m3_runtime_) return false;

    stop_requested_.store(false);
    set_stopping(m3_runtime_, false);
    pause_requested_.store(false);
    thread_detached_.store(false);
    worker_done_.store(false);
    stop_gave_up_.store(false);
    status_.phase = ContainerPhase::Running;
    status_.ready = true;
    status_.start_time_ms = osal::System::get_uptime_ms();
    PodLogStore::runtime().append(config_.pod_id, "[kube32] container started (linear memory " +
                                                      std::to_string(status_.memory_used_bytes / 1024) + " KiB, limit " +
                                                      std::to_string(config_.memory_limit_bytes / 1024) + " KiB)");

    // Request-handling pods (k32_handle/k32_tick/k32_event, no _start or
    // app_main) are driven by the host's own threads; a worker thread would
    // only sleep, so none is created and no stack is reserved for it.
    if (!find_export_locked("_start") && !find_export_locked("app_main") && !find_export_locked("main")) {
        return true;
    }

    osal::Thread::Config t_cfg;
    thread_name_ = "pod_" + config_.pod_id;
    t_cfg.name = thread_name_.c_str();
#if defined(ESP_PLATFORM)
    // app_main style pods interpret inside this task and call into drivers
    t_cfg.stack_size = (osal::System::get_total_psram_bytes() > 0) ? 65536 : 49152;
#else
    t_cfg.stack_size = 65536;
#endif
    t_cfg.priority = 5;
    t_cfg.core_affinity = 1;
    t_cfg.stack_in_psram = true;   // falls back to internal RAM on boards without PSRAM
#if defined(ESP_PLATFORM)
    if (osal::System::get_total_psram_bytes() == 0 && kube32_pool_stack_size() > 0) {
        // Internal RAM fragments quickly once Wi-Fi is up; the stack reserved
        // at boot is the only reliable source of a 48 KB block.
        pod_stack_ = kube32_pool_stack_alloc(kube32_pool_stack_size());
        if (!pod_stack_) {
            status_.phase = ContainerPhase::Failed;
            status_.ready = false;
            status_.exit_message = kube32_interp_stack_active()
                ? "no stack for an app_main thread: on this board pods run on the shared interpreter stack (export k32_handle, k32_tick or k32_event instead)"
                : "no pod thread stack available";
            return false;
        }
        t_cfg.stack_buffer = pod_stack_;
        t_cfg.stack_size = kube32_pool_stack_size();
    }
#endif

    worker_thread_ = std::make_unique<osal::Thread>();
    bool ok = worker_thread_->start(worker_thread_entry, this, t_cfg);
    if (!ok) {
        worker_thread_.reset();
        if (pod_stack_) { kube32_pool_stack_free(pod_stack_); pod_stack_ = nullptr; }
        status_.phase = ContainerPhase::Failed;
        status_.ready = false;
        status_.exit_message = "pod thread could not be created";
    }
    return ok;
}

bool ContainerLifecycleController::pause() {
    osal::LockGuard guard(mutex_);
    if (status_.phase != ContainerPhase::Running) return false;
    pause_requested_.store(true);
    status_.phase = ContainerPhase::Paused;
    return true;
}

bool ContainerLifecycleController::resume() {
    osal::LockGuard guard(mutex_);
    if (status_.phase != ContainerPhase::Paused) return false;
    pause_requested_.store(false);
    status_.phase = ContainerPhase::Running;
    return true;
}

bool ContainerLifecycleController::stop(uint32_t grace_period_ms) {
    request_stop();

    if (worker_thread_) {
        // Already given up on: the thread is still in its host call
        if (stop_gave_up_.load() && !worker_done_.load()) return true;
        uint64_t start_t = osal::System::get_uptime_ms();

        // Phase 1: Wait up to grace_period_ms for worker to finish cooperatively
        while (worker_thread_->is_joinable() && (osal::System::get_uptime_ms() - start_t < grace_period_ms)) {
            {
                osal::LockGuard guard(mutex_);
                if (status_.phase != ContainerPhase::Running) {
                    break;
                }
            }
            osal::Thread::sleep_ms(10);
        }

        // Phase 2: If still running, abort guest loop via gas metering trap
        bool still_running = false;
        {
            osal::LockGuard guard(mutex_);
            still_running = (status_.phase == ContainerPhase::Running);
        }

        if (worker_thread_->is_joinable() && still_running) {
#if d_m3HasGasMetering
            uint64_t trap_t = osal::System::get_uptime_ms();
            // Continuously enforce gas exhaustion until worker thread traps or timeout.
            // A guest parked in a blocking host call (channel recv, sleep) only
            // sees the trap when it returns, so allow the full grace period.
            const uint32_t trap_window_ms = grace_period_ms > 250 ? grace_period_ms : 250;
            while (worker_thread_->is_joinable() && (osal::System::get_uptime_ms() - trap_t < trap_window_ms)) {
                if (m3_runtime_) {
                    __atomic_store_n(&(reinterpret_cast<IM3Runtime>(m3_runtime_)->gasRemaining), -1, __ATOMIC_SEQ_CST);
                }
                {
                    osal::LockGuard guard(mutex_);
                    if (status_.phase != ContainerPhase::Running) {
                        break;
                    }
                }
                osal::Thread::sleep_ms(2);
            }
#endif
            {
                osal::LockGuard guard(mutex_);
                still_running = (status_.phase == ContainerPhase::Running);
            }

            // Phase 3: the guest is still inside a host call (or runs
            // unmetered). Report the container stopped; the thread keeps its
            // stack and the module until it returns and traps (lingering()).
            if (worker_thread_->is_joinable() && still_running && !worker_done_.load()) {
                std::cout << "[RUNTIME] " << config_.pod_id << ": guest did not stop within " << grace_period_ms
                          << " ms; its memory is released when the host call it is in returns" << std::endl;
                PodLogStore::runtime().append(config_.pod_id, "[kube32] container did not stop within the grace period (Error, exit code 137)");
                stop_gave_up_.store(true);
                osal::LockGuard guard(mutex_);
                status_.phase = ContainerPhase::Failed;
                status_.exit_code = 137;
                status_.termination_reason = "Error";
                status_.exit_message = "did not stop within the grace period";
                status_.ready = false;
                return true;
            }
        }

        // Clean completion or gas trap caught: synchronously join worker thread to prevent UAF
        if (worker_thread_ && worker_thread_->is_joinable()) {
            worker_thread_->join();
            worker_thread_.reset();   // waits for the kernel to retire a static task
            if (pod_stack_) { kube32_pool_stack_free(pod_stack_); pod_stack_ = nullptr; }
            std::cout << "[RUNTIME] " << config_.pod_id << ": stopped (" << status_.termination_reason << ")" << std::endl;
            PodLogStore::runtime().append(config_.pod_id, "[kube32] container stopped (" +
                                                              (status_.termination_reason.empty() ? std::string("stop requested by kubelet") : status_.termination_reason) + ")");
        }
    }

    osal::LockGuard guard(mutex_);
    if (status_.phase == ContainerPhase::Running) {
        status_.phase = ContainerPhase::Terminated;
        status_.ready = false;
    }
    return true;
}

// Restarts the container from the image it was deployed with. restart_count
// counts these restarts; the kubelet decides when one happens (restartPolicy
// and back-off live there).
bool ContainerLifecycleController::restart() {
    stop(1000);
    if (lingering()) return false;   // the old instance still holds the module
    if (wasm_view_ && wasm_view_len_) {
        if (!deploy(wasm_view_, wasm_view_len_)) {
            return false;
        }
    }
    {
        osal::LockGuard guard(mutex_);
        status_.restart_count++;
    }
    return start();
}

ContainerStatus ContainerLifecycleController::get_status() {
    osal::LockGuard guard(mutex_);
    if (m3_module_) {
        size_t mem_size = m3_GetMemorySize(reinterpret_cast<IM3Module>(m3_module_), 0);
        status_.memory_used_bytes = static_cast<uint32_t>(mem_size);
    }
    if (m3_runtime_) {
        status_.runtime_bytes = m3_GetResourceUsage(reinterpret_cast<IM3Runtime>(m3_runtime_), c_m3Limit_MemoryBytes);
    }
    return status_;
}

uint32_t ContainerLifecycleController::get_memory_usage_bytes() const {
    osal::LockGuard guard(mutex_);
    if (m3_module_) {
        return static_cast<uint32_t>(m3_GetMemorySize(reinterpret_cast<IM3Module>(m3_module_), 0));
    }
    return status_.memory_used_bytes;
}

bool ContainerLifecycleController::is_running() const {
    osal::LockGuard guard(mutex_);
    return status_.phase == ContainerPhase::Running;
}

int32_t ContainerLifecycleController::call_function(const std::string& fn_name, const std::vector<int32_t>& args) {
#if defined(ESP_PLATFORM)
    // The shared interpreter stack is taken before this pod's call lock, as
    // in a nested in-process call (pod A on the stack calling pod B): taking
    // them in the other order could leave A waiting for B's call lock while
    // B's caller waits for the stack.
    if (kube32_interp_stack_active() && !kube32_interp_stack_base_for(__builtin_frame_address(0))) {
        int32_t rc = -1;
        run_on_interpreter_stack([&]() { rc = call_function(fn_name, args); });
        return rc;
    }
#endif
    // Calls into the module are serialized by call_mutex_; mutex_ (status)
    // is not held while the guest runs, so status queries do not wait for a
    // call that is blocked in a host function (a service call, for example).
    osal::LockGuard call_guard(call_mutex_);
    IM3Function fn = nullptr;
    IM3Runtime rt = nullptr;
    {
        osal::LockGuard guard(mutex_);
        if (!m3_runtime_) return -1;
        M3Result r = m3_FindFunction(&fn, reinterpret_cast<IM3Runtime>(m3_runtime_), fn_name.c_str());
        if (r != m3Err_none || !fn) {
            return -1;
        }
        rt = reinterpret_cast<IM3Runtime>(m3_runtime_);
    }

    // Call based on args count (0 or 1..4 arguments supported)
    rt->lastMemoryGrowError = m3Err_none;
    M3Result call_r = m3Err_none;
    run_on_interpreter_stack([&]() {
        if (args.empty()) {
            call_r = m3_CallV(fn);
        } else if (args.size() == 1) {
            call_r = m3_CallV(fn, args[0]);
        } else if (args.size() == 2) {
            call_r = m3_CallV(fn, args[0], args[1]);
        } else if (args.size() == 3) {
            call_r = m3_CallV(fn, args[0], args[1], args[2]);
        } else {
            call_r = m3_CallV(fn, args[0], args[1], args[2], args[3]);
        }
    });

    if (call_r != m3Err_none) {
        // Trap occurred during function call
        std::cout << "[RUNTIME] " << config_.pod_id << ": trap in " << fn_name << ": " << call_r << std::endl;
        TrapContext ctx;
        ctx.stop_requested = stop_requested_.load();
        ctx.memory_grow_error = rt->lastMemoryGrowError;
        ctx.memory_limit_bytes = config_.memory_limit_bytes;
        ctx.wasm_stack_bytes = config_.stack_size_bytes;
#if d_m3HasGasMetering
        // AppPodHost arms the per-call budget; an unbounded limit means none
        uint64_t gas_limit = m3_GetResourceLimit(rt, c_m3Limit_GasUnits);
        if (gas_limit < static_cast<uint64_t>(INT64_MAX)) ctx.gas_budget = gas_limit;
#endif
        TrapReport t = describe_trap(call_r, ctx);
        PodLogStore::runtime().append(config_.pod_id, "[kube32] container trapped in " + fn_name + ": " + t.message + " (" + t.reason +
                                                          ", exit code " + std::to_string(t.exit_code) + ")");
        osal::LockGuard guard(mutex_);
        on_trap_caught_locked(t.message, t.exit_code, t.reason);
        const int32_t exit_code = t.exit_code;
        return -exit_code;
    }

    // Read return value (i32)
    int32_t ret_val = 0;
    M3Result r = m3_GetResultsV(fn, &ret_val);
    if (r == m3Err_none) {
        return ret_val;
    }
    return 0;
}

bool ContainerLifecycleController::has_export(const char* fn_name) const {
    osal::LockGuard guard(mutex_);
    return find_export_locked(fn_name);
}

// Caller holds mutex_.
bool ContainerLifecycleController::find_export_locked(const char* fn_name) const {
    if (!m3_runtime_ || !fn_name) return false;
    IM3Function fn = nullptr;
    M3Result r = m3_FindFunction(&fn, reinterpret_cast<IM3Runtime>(m3_runtime_), fn_name);
    return (r == m3Err_none && fn != nullptr);
}

uint8_t* ContainerLifecycleController::guest_memory(size_t& out_size) const {
    out_size = 0;
    if (!m3_module_) return nullptr;
    size_t size = 0;
    uint8_t* mem = m3_GetMemory(reinterpret_cast<IM3Module>(m3_module_), &size, 0);
    out_size = size;
    return mem;
}

bool ContainerLifecycleController::run_liveness_probe() {
    if (!config_.liveness_probe.enabled) return true;

    bool healthy = false;
    if (config_.liveness_probe.type == ProbeType::ExecWasm) {
        int32_t res = call_function(config_.liveness_probe.handler);
        healthy = (res == 0);
    } else if (config_.liveness_probe.type == ProbeType::HttpGet) {
        osal::Socket sock;
        if (sock.create(osal::SocketType::Tcp)) {
            sock.set_timeout(1000, 1000);
            if (sock.connect("127.0.0.1", config_.liveness_probe.port)) {
                std::string req = "GET " + config_.liveness_probe.handler + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
                sock.send(req.data(), req.size());
                char buf[128];
                int n = sock.recv(buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    healthy = (strstr(buf, "200 OK") != nullptr || strstr(buf, "HTTP/1.1 2") != nullptr);
                }
            }
        }
    } else if (config_.liveness_probe.type == ProbeType::TcpSocket) {
        osal::Socket sock;
        if (sock.create(osal::SocketType::Tcp)) {
            sock.set_timeout(1000, 1000);
            healthy = sock.connect("127.0.0.1", config_.liveness_probe.port);
        }
    }

    if (healthy) {
        consecutive_liveness_failures_ = 0;
    } else {
        consecutive_liveness_failures_++;
        if (consecutive_liveness_failures_ >= config_.liveness_probe.failure_threshold) {
            restart();
            return false;
        }
    }
    return healthy;
}

bool ContainerLifecycleController::run_readiness_probe() {
    if (!config_.readiness_probe.enabled) return true;

    bool healthy = false;
    if (config_.readiness_probe.type == ProbeType::ExecWasm) {
        int32_t res = call_function(config_.readiness_probe.handler);
        healthy = (res == 0);
    }

    osal::LockGuard guard(mutex_);
    if (healthy) {
        consecutive_readiness_failures_ = 0;
        status_.ready = true;
    } else {
        consecutive_readiness_failures_++;
        if (consecutive_readiness_failures_ >= config_.readiness_probe.failure_threshold) {
            status_.ready = false;
        }
    }
    return healthy;
}

} // namespace kube32::runtime
