#include "runtime/container_lifecycle.hpp"
#include "runtime/pod_memory_pool.h"
#include <cstdint>
#include "runtime/pod_log.hpp"
#if defined(ESP_PLATFORM)
#include "esp_memory_utils.h"
#endif
#include "runtime/host_functions.hpp"
#include "runtime/app_pod.hpp"
#include "osal/system.hpp"
#include "osal/socket.hpp"

#include "wasm3.h"
#include "m3_config.h"
#include "m3_env.h"

#include <iostream>
#include <algorithm>
#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace kube32::runtime {

static osal::RecursiveMutex s_wasm3_global_mutex;

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
    cleanup_runtime();
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
        m3_FreeRuntime(reinterpret_cast<IM3Runtime>(m3_runtime_));
        m3_runtime_ = nullptr;
        m3_module_ = nullptr; // Freed by m3_FreeRuntime
    }
    if (m3_env_) {
        m3_FreeEnvironment(reinterpret_cast<IM3Environment>(m3_env_));
        m3_env_ = nullptr;
    }
}

bool ContainerLifecycleController::deploy(const uint8_t* wasm_bytes, size_t len) {
    osal::LockGuard guard(mutex_);
    osal::LockGuard<osal::RecursiveMutex> global_guard(s_wasm3_global_mutex);

    cleanup_runtime();

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

    m3_env_ = m3_NewEnvironment();
    if (!m3_env_) {
        status_.phase = ContainerPhase::Failed;
        status_.exit_message = "Failed to create Wasm3 environment";
        return false;
    }

    m3_runtime_ = m3_NewRuntime(reinterpret_cast<IM3Environment>(m3_env_), config_.stack_size_bytes, nullptr);
    // wasm3 compiles functions lazily and only emits gas checks into code
    // compiled while a gas limit is set. Arm the runtime now so every function
    // of every pod is metered; that is what lets stop() interrupt a guest loop
    // (gasRemaining = -1) and what the per-request budget relies on.
    if (m3_runtime_) m3_SetResourceLimit(reinterpret_cast<IM3Runtime>(m3_runtime_), c_m3Limit_GasUnits, static_cast<uint64_t>(INT64_MAX));
    if (!m3_runtime_) {
        status_.phase = ContainerPhase::Failed;
        status_.exit_message = "Failed to create Wasm3 runtime";
        cleanup_runtime();
        return false;
    }

    // Enforce memory limits in bytes
    if (config_.memory_limit_bytes > 0) {
        m3_SetResourceLimit(reinterpret_cast<IM3Runtime>(m3_runtime_), c_m3Limit_MemoryBytes, static_cast<uint64_t>(config_.memory_limit_bytes));
    }

#if d_m3HasGasMetering
    // Arm execution budget so loops can be asynchronously aborted via gas trap
    m3_SetResourceLimit(reinterpret_cast<IM3Runtime>(m3_runtime_), c_m3Limit_GasUnits, 0x7FFFFFFFFFFFFFFFULL);
#endif

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

    if (!runtime) return;

    // Look for standard entrypoints: _start, app_main, main
    if (m3_FindFunction(&entry_fn, runtime, "_start") != m3Err_none) {
        if (m3_FindFunction(&entry_fn, runtime, "app_main") != m3Err_none) {
            m3_FindFunction(&entry_fn, runtime, "main");
        }
    }

    if (entry_fn) {
#if defined(ESP_PLATFORM)
        printf("[STACK] pod thread before entry: free=%u bytes\n", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
#endif
        M3Result r = m3_CallV(entry_fn);

        if (r == m3Err_none) {
            PodLogStore::runtime().append(config_.pod_id, "[kube32] entrypoint returned, container completed");
            osal::LockGuard guard(mutex_);
            status_.phase = ContainerPhase::Terminated;
            status_.exit_code = 0;
            status_.termination_reason = "Completed";
            status_.exit_message = "Completed";
            status_.ready = false;
        } else {
            int32_t exit_code = 137;
            std::string reason = "RuntimeTrap";

            if (r == m3Err_trapOutOfBoundsMemoryAccess || r == m3Err_memoryLimitExceeded || r == m3Err_wasmMemoryOverflow) {
                exit_code = 137;
                reason = "OOMKilled";
            } else if (r == m3Err_trapDivisionByZero || r == m3Err_trapIntegerOverflow) {
                exit_code = 136;
                reason = "IntegerDivisionByZero";
            } else if (r == m3Err_trapUnreachable || r == m3Err_trapAbort) {
                exit_code = 134;
                reason = "UnreachableExecuted";
            } else if (r == m3Err_trapStackOverflow) {
                exit_code = 137;
                reason = "StackOverflow";
            } else if (r == m3Err_trapOutOfGas) {
                exit_code = 137;
                reason = "ExecutionTimeout";
            }

            std::string trap_msg = (r == m3Err_trapOutOfGas)
                ? "ExecutionTimeout: Workload did not terminate within grace period"
                : std::string(r ? r : "UnknownTrap");
            on_trap_caught(trap_msg, exit_code, reason);
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
    status_.restart_count++;

    calculate_and_apply_backoff();
}

void ContainerLifecycleController::on_trap_caught(const std::string& trap_message, int32_t exit_code, const std::string& reason) {
    PodLogStore::runtime().append(config_.pod_id, "[kube32] container trapped: " + trap_message + " (" + reason +
                                                      ", exit code " + std::to_string(exit_code) + ")");
    osal::LockGuard guard(mutex_);
    on_trap_caught_locked(trap_message, exit_code, reason);
}

void ContainerLifecycleController::calculate_and_apply_backoff() {
    uint32_t count = (status_.restart_count > 0) ? status_.restart_count : 1;
    uint64_t delay_sec = 10ULL * (1ULL << std::min<uint32_t>(count - 1, 5));
    if (delay_sec > 300) delay_sec = 300;
    next_restart_allowed_time_ms_ = osal::System::get_uptime_ms() + (delay_sec * 1000ULL);
}

bool ContainerLifecycleController::start() {
    osal::LockGuard guard(mutex_);

    if (!m3_runtime_) return false;

    stop_requested_.store(false);
    pause_requested_.store(false);
    thread_detached_.store(false);
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
            status_.exit_message = "no pod thread stack available";
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
    stop_requested_.store(true);

    if (worker_thread_) {
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

            // Phase 3: Fail-safe detach only if worker truly refused to trap after full timeout
            if (worker_thread_->is_joinable() && still_running) {
                std::cout << "[RUNTIME] " << config_.pod_id << ": guest ignored stop, detaching (memory stays reserved)" << std::endl;
                PodLogStore::runtime().append(config_.pod_id, "[kube32] container ignored stop within grace period, detached (ExecutionTimeout)");
                worker_thread_->detach();
                worker_thread_.reset();
                thread_detached_.store(true);

                osal::LockGuard guard(mutex_);
                status_.phase = ContainerPhase::Failed;
                status_.exit_code = 137;
                status_.termination_reason = "ExecutionTimeout";
                status_.exit_message = "ExecutionTimeout: Workload did not terminate within grace period";
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

bool ContainerLifecycleController::restart() {
    stop(1000);
    if (wasm_view_ && wasm_view_len_) {
        if (!deploy(wasm_view_, wasm_view_len_)) {
            return false;
        }
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
    osal::LockGuard guard(mutex_);
    if (!m3_runtime_) return -1;

    IM3Function fn = nullptr;
    M3Result r = m3_FindFunction(&fn, reinterpret_cast<IM3Runtime>(m3_runtime_), fn_name.c_str());
    if (r != m3Err_none || !fn) {
        return -1;
    }

    // Call based on args count (0 or 1..4 arguments supported)
    M3Result call_r = m3Err_none;
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

    if (call_r != m3Err_none) {
        // Trap occurred during function call
        std::cout << "[RUNTIME] " << config_.pod_id << ": trap in " << fn_name << ": " << call_r << std::endl;
        int32_t exit_code = 137;
        std::string reason = "RuntimeTrap";
        if (call_r == m3Err_trapOutOfBoundsMemoryAccess || call_r == m3Err_memoryLimitExceeded) {
            exit_code = 137; reason = "OOMKilled";
        } else if (call_r == m3Err_trapDivisionByZero) {
            exit_code = 136; reason = "IntegerDivisionByZero";
        } else if (call_r == m3Err_trapUnreachable) {
            exit_code = 134; reason = "UnreachableExecuted";
        }
        on_trap_caught_locked(call_r, exit_code, reason);
        return -exit_code;
    }

    // Read return value (i32)
    int32_t ret_val = 0;
    r = m3_GetResultsV(fn, &ret_val);
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
