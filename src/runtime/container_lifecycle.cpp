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
#include "runtime/pod_cpu.hpp"
#include "osal/system.hpp"
#include "osal/socket.hpp"
#include "osal/sha256.hpp"

#include "wasm3.h"
#include "m3_config.h"
#include "m3_env.h"
#include "m3_host.h"

#include <iostream>
#include <cstring>
#include <algorithm>
#include <vector>
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

// Per thread: the innermost PodCpuRegion and the pod thread's sink. A table
// rather than thread-local storage: nested regions run on the shared
// interpreter stack, where ESP-IDF rules thread-local storage out
// (esp_execute_shared_stack_function). Holds the threads inside a region and
// the pod threads, a few entries.
namespace {
struct CpuThread {
    uintptr_t thread;
    PodCpuRegion* region;
    std::atomic<uint64_t>* sink;
};
std::mutex s_cpu_mutex;
std::vector<CpuThread> s_cpu_threads;

CpuThread& cpu_thread_locked(uintptr_t thread) {
    for (auto& t : s_cpu_threads) {
        if (t.thread == thread) return t;
    }
    s_cpu_threads.push_back(CpuThread{thread, nullptr, nullptr});
    return s_cpu_threads.back();
}

void drop_idle_locked(uintptr_t thread) {
    s_cpu_threads.erase(std::remove_if(s_cpu_threads.begin(), s_cpu_threads.end(), [thread](const CpuThread& t) {
        return t.thread == thread && !t.region && !t.sink;
    }), s_cpu_threads.end());
}
} // namespace

PodCpuRegion::PodCpuRegion() {
    measured_ = osal::Thread::current_cpu_time_us(start_us_);
    std::lock_guard<std::mutex> lock(s_cpu_mutex);
    CpuThread& t = cpu_thread_locked(osal::Thread::current_id());
    parent_ = t.region;
    t.region = this;
}

uint64_t PodCpuRegion::finish() {
    if (done_) return 0;
    done_ = true;
    uint64_t now = 0;
    const bool measured = measured_ && osal::Thread::current_cpu_time_us(now) && now >= start_us_;
    std::atomic<uint64_t>* sink = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_cpu_mutex);
        const uintptr_t id = osal::Thread::current_id();
        CpuThread& t = cpu_thread_locked(id);
        t.region = parent_;
        sink = t.sink;
        drop_idle_locked(id);
    }
    if (!measured) return 0;
    const uint64_t total = now - start_us_;
    if (parent_) parent_->nested_us_ += total;   // the parent is on this thread
    else if (sink) sink->fetch_add(total);
    return total > nested_us_ ? total - nested_us_ : 0;
}

void set_thread_nested_cpu_sink(std::atomic<uint64_t>* sink) {
    std::lock_guard<std::mutex> lock(s_cpu_mutex);
    const uintptr_t id = osal::Thread::current_id();
    cpu_thread_locked(id).sink = sink;
    drop_idle_locked(id);
}

// Held while a call runs on the shared interpreter stack, and while a module
// loads with its compile scratch lent from it (CompileScratchLoan)
static std::mutex s_interp_mutex;

#if defined(ESP_PLATFORM)
static SemaphoreHandle_t s_interp_lock = nullptr;
static const mem::FunctionRef<void()>* s_interp_fn = nullptr;
static void interp_trampoline() { if (s_interp_fn) (*s_interp_fn)(); }
#endif

void run_on_interpreter_stack(mem::FunctionRef<void()> fn) {
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

// While a module loads on a board without PSRAM, the parser, validator and
// compiler share one scratch (m3_CompileScratchSize, about 6 KB) lent from
// the shared interpreter stack, so the pod pool needs no room for it. No pod
// call runs while the loan is held. Taken before a pod's call lock, as
// call_function takes the stack. Not taken by a caller on the stack itself.
bool compile_scratch_lent() {
    int external = 0;
    if (kube32_pool_block_size() == 0 || (kube32_pool_available(&external), external)) return false;
    size_t size = 0;
    return kube32_interp_stack_get(&size) && size >= m3_CompileScratchSize();
}

class CompileScratchLoan {
public:
    CompileScratchLoan() {
        if (!compile_scratch_lent() || kube32_interp_stack_base_for(__builtin_frame_address(0))) return;
        lock_ = std::unique_lock<std::mutex>(s_interp_mutex);
        memory_ = kube32_interp_stack_get(nullptr);
    }
    void* memory() const { return memory_; }

private:
    std::unique_lock<std::mutex> lock_;
    void* memory_{nullptr};
};

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
    if (trap == m3Err_trapStackOverflow && ctx.native_stack_left) {
        // The native stack the interpreter runs on (an operation frame per
        // instruction where the compiler makes no tail calls), not the
        // module's operand stack: say what the call had
        t.message += " (native stack: " + std::to_string(ctx.native_stack_left) + " bytes when the call began, on " +
                     (ctx.native_on_interpreter ? "the shared interpreter stack" : "the calling thread's stack") +
                     (ctx.native_limit ? "; " + std::to_string(ctx.native_used) + " used at the trap, limit " + std::to_string(ctx.native_limit) + " below the start)" : ")");
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
    : config_(config), memory_account_(kube32_pod_account_open()) {
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
    kube32_pod_account_close(memory_account_);
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
        unlink_app_host_functions(reinterpret_cast<IM3Runtime>(m3_runtime_));
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
    CompileScratchLoan scratch;
    osal::LockGuard call_guard(call_mutex_);
    osal::LockGuard guard(mutex_);
    osal::LockGuard<osal::RecursiveMutex> global_guard(s_wasm3_global_mutex);
    PodMemoryScope charged(memory_account_);

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
    // are used in place; anything else (manifest-inline images) is copied. The
    // S3 maps flash and PSRAM into one data address range: a buffer in PSRAM
    // is not flash, and the pod's own copy is kept wherever it lives.
    const bool own_copy = !wasm_bytecode_.empty() && wasm_bytes == wasm_bytecode_.data();
#if defined(ESP_PLATFORM)
    if (!own_copy && esp_ptr_in_drom(wasm_bytes) && !esp_ptr_external_ram(wasm_bytes)) {
        wasm_bytecode_.clear();
        wasm_bytecode_.shrink_to_fit();
        wasm_view_ = wasm_bytes;
        wasm_view_len_ = len;
    } else
#endif
    {
        if (!own_copy) wasm_bytecode_.assign(wasm_bytes, wasm_bytes + len);   // restart reuses the copy
        wasm_view_ = wasm_bytecode_.data();
        wasm_view_len_ = wasm_bytecode_.size();
    }

    // A pod whose linear memory wants the pool's block whole keeps its own
    // runtime structures in the general heap, so the block stays free for it
    struct PreferGeneral {
        bool on;
        explicit PreferGeneral(bool o) : on(o) { if (on) kube32_pool_prefer_general(1); }
        ~PreferGeneral() { if (on) kube32_pool_prefer_general(0); }
    } prefer_general(kube32_pool_wants_block(config_.memory_limit_bytes) != 0);
    m3_env_ = m3_NewEnvironment();
    if (!m3_env_) {
        status_.phase = ContainerPhase::Failed;
        status_.exit_message = "Failed to create Wasm3 environment";
        return false;
    }
    // Lent for the parse, load and compile; taken back before anything runs
    struct LentScratch {
        void*& env_ref;      // m3_env_: a failed load frees the environment first
        void* env;
        LentScratch(void*& ref, void* scratch) : env_ref(ref), env(ref) { static_cast<IM3Environment>(env)->compileScratch = scratch; }
        ~LentScratch() { if (env_ref == env) static_cast<IM3Environment>(env)->compileScratch = nullptr; }
    } lent(m3_env_, scratch.memory());

    // A stack sized from the module starts with room for the constant
    // expressions evaluated while the module loads, and gets its size once
    // every function is compiled
    const uint32_t first_stack = config_.stack_from_module ? std::min<uint32_t>(config_.stack_size_bytes, kLoadStackBytes)
                                                           : config_.stack_size_bytes;
    m3_runtime_ = m3_NewRuntime(reinterpret_cast<IM3Environment>(m3_env_), first_stack, nullptr);
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
    image_sha_known_ = false;

    // Link standard Kube32 host functions and the application ABI imports
    link_host_functions(module, config_.pod_id);
    link_app_host_functions(module, config_.pod_id);

    // Every function is compiled now, so the pod's code is in place before it
    // runs: a module that does not fit fails here, cleanly, instead of in a
    // later call. A function that does not compile for another reason fails
    // when it is called, as before.
    r = m3_CompileModule(module);
    if (r == m3Err_mallocFailed || r == m3Err_mallocFailedCodePage) {
        status_.phase = ContainerPhase::Failed;
        status_.exit_message = std::string("Wasm compile error: ") + r;
#if defined(ESP_PLATFORM)
        status_.exit_message += " (free heap " + std::to_string(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) +
                                ", largest block " + std::to_string(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) +
                                ", pod pool " + std::to_string(kube32_pool_available(nullptr)) +
                                " in pieces of at most " + std::to_string(kube32_pool_largest_free()) +
                                ", refused " + std::to_string(kube32_pool_last_refused()) + " bytes)";
#endif
        cleanup_runtime();
        return false;
    }
    // The last code page is cut to what the module needs
    m3_TrimCodePages(reinterpret_cast<IM3Runtime>(m3_runtime_));

    // The operand stack: what the module's deepest call chain needs (a
    // little more), at most the pod's stack size; that size when the
    // module recurses
    uint32_t stack_bytes = config_.stack_size_bytes;
    if (config_.stack_from_module) {
        const uint32_t need = module_stack_need_bytes(module);
        if (need > 0) stack_bytes = std::min(stack_bytes, (need + kStackMarginBytes + 15) & ~15u);
        r = m3_ResizeStack(reinterpret_cast<IM3Runtime>(m3_runtime_), stack_bytes);
        if (r != m3Err_none) {
            status_.phase = ContainerPhase::Failed;
            status_.exit_message = std::string("Wasm load error: ") + r + " (operand stack of " + std::to_string(stack_bytes) + " bytes)";
            cleanup_runtime();
            return false;
        }
    }
    wasm_stack_bytes_ = stack_bytes;

    size_t mem_size = m3_GetMemorySize(module, 0);
    status_.memory_used_bytes = static_cast<uint32_t>(mem_size);
    status_.bytecode_bytes = wasm_bytecode_.size();
    status_.stack_bytes = stack_bytes;
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
    set_thread_nested_cpu_sink(&worker_nested_cpu_us_);
    run_entry_point();
    set_thread_nested_cpu_sink(nullptr);
    {
        // The thread ends: its CPU time is kept and the thread not read again
        uint64_t cpu = 0;
        const bool measured = osal::Thread::current_cpu_time_us(cpu);
        osal::LockGuard guard(mutex_);
        worker_cpu_thread_ = nullptr;
        worker_cpu_us_ = cpu;
        worker_cpu_measured_ = measured;
    }
    worker_done_.store(true);
}

void ContainerLifecycleController::run_entry_point() {
    IM3Function entry_fn = nullptr;
    IM3Runtime runtime = reinterpret_cast<IM3Runtime>(m3_runtime_);

    if (!runtime) { worker_done_.store(true); return; }
    PodMemoryScope charged(memory_account_);

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
            ctx.wasm_stack_bytes = wasm_stack_bytes_;
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

    worker_cpu_us_ = 0;
    worker_cpu_measured_ = false;
    worker_nested_cpu_us_.store(0);
    worker_thread_ = std::make_unique<osal::Thread>();
    bool ok = worker_thread_->start(worker_thread_entry, this, t_cfg);
    if (ok) {
        worker_stack_bytes_.store(static_cast<uint32_t>(t_cfg.stack_size));
        worker_cpu_thread_ = worker_thread_.get();   // the worker clears it under mutex_, which this function holds
    }
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
            worker_stack_bytes_.store(0);
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
    status_.held_bytes = held_bytes_locked();
    uint64_t cpu = worker_cpu_us_;
    status_.has_thread_cpu = worker_cpu_thread_ ? worker_cpu_thread_->cpu_time_us(cpu) : worker_cpu_measured_;
    if (status_.has_thread_cpu) {
        const uint64_t nested = worker_nested_cpu_us_.load();
        status_.thread_cpu_us = cpu > nested ? cpu - nested : 0;
    }
    return status_;
}

uint64_t ContainerLifecycleController::held_bytes() const {
    osal::LockGuard guard(mutex_);
    return held_bytes_locked();
}

uint64_t ContainerLifecycleController::held_bytes_locked() const {
    if (!memory_account_) return 0;
    // What the runtime allocated for the container (kube32_rt_*, the pool
    // block), each allocation as the allocator holds it
    uint64_t held = kube32_pod_account_bytes(memory_account_);
#if d_m3GuardedMemory
    // The host maps linear memory outside the pool: the committed pages of
    // each memory's slot, its header page included
    if (const IM3Module module = reinterpret_cast<IM3Module>(m3_module_)) {
        const size_t page = m3_HostPageSize();
        for (uint32_t i = 0; i < module->numMemories; ++i) {
            const IM3Memory mem = module->memories[i];
            if (!mem || mem->owner != module || !mem->guardSlot || !mem->mallocated) continue;
            held += page + (mem->mallocated->length + page - 1) / page * page;
        }
    }
#endif
    // The module's bytes when they are a copy (not mapped from flash)
    if (!wasm_bytecode_.empty()) held += kube32_heap_allocated_size(wasm_bytecode_.data());
    return held + worker_stack_bytes_.load();
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

int32_t ContainerLifecycleController::call_function(const std::string& fn_name, const int32_t* args, size_t argc) {
#if defined(ESP_PLATFORM)
    // The shared interpreter stack is taken before this pod's call lock, as
    // in a nested in-process call (pod A on the stack calling pod B): taking
    // them in the other order could leave A waiting for B's call lock while
    // B's caller waits for the stack.
    if (kube32_interp_stack_active() && !kube32_interp_stack_base_for(__builtin_frame_address(0))) {
        int32_t rc = -1;
        run_on_interpreter_stack([&]() { rc = call_function(fn_name, args, argc); });
        return rc;
    }
#endif
    // Calls into the module are serialized by call_mutex_; mutex_ (status)
    // is not held while the guest runs, so status queries do not wait for a
    // call that is blocked in a host function (a service call, for example).
    osal::LockGuard call_guard(call_mutex_);
    PodMemoryScope charged(memory_account_);   // memory.grow, tables
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
    uint64_t native_left = 0;
    bool on_interpreter = false;
    uintptr_t call_sp = 0;
    rt->stackTrapAt = rt->stackTrapLimit = nullptr;
    run_on_interpreter_stack([&]() {
        // What a native stack overflow trap reports: the stack the call runs on
        const uint8_t* here = static_cast<const uint8_t*>(__builtin_frame_address(0));
        call_sp = reinterpret_cast<uintptr_t>(here);
        const uint8_t* base = static_cast<const uint8_t*>(m3_HostStackBase());
        native_left = (base && base < here) ? static_cast<uint64_t>(here - base) : 0;
        on_interpreter = kube32_interp_stack_base_for(here) != nullptr;
        if (argc == 0) {
            call_r = m3_CallV(fn);
        } else if (argc == 1) {
            call_r = m3_CallV(fn, args[0]);
        } else if (argc == 2) {
            call_r = m3_CallV(fn, args[0], args[1]);
        } else if (argc == 3) {
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
        ctx.wasm_stack_bytes = wasm_stack_bytes_;
        ctx.native_stack_left = native_left;
        ctx.native_on_interpreter = on_interpreter;
        if (rt->stackTrapAt && call_sp) {
            ctx.native_used = static_cast<int64_t>(call_sp) - static_cast<int64_t>(reinterpret_cast<uintptr_t>(rt->stackTrapAt));
            ctx.native_limit = static_cast<int64_t>(call_sp) - static_cast<int64_t>(reinterpret_cast<uintptr_t>(rt->stackTrapLimit));
        }
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

void ContainerLifecycleController::image_sha256(uint8_t out[32]) {
    osal::LockGuard guard(mutex_);
    if (!image_sha_known_) {
        const osal::Sha256::Digest d = osal::Sha256::of(wasm_view_, wasm_view_len_);
        std::memcpy(image_sha_, d.data(), 32);
        image_sha_known_ = true;
    }
    std::memcpy(out, image_sha_, 32);
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
