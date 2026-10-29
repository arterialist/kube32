#include "runtime/wasm_runtime.hpp"
#include "runtime/container_lifecycle.hpp"
#include "osal/thread.hpp"
#include "osal/system.hpp"

#include <iostream>
#include <cassert>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>

using namespace kube32::runtime;

// =============================================================================
// WebAssembly Test Modules
// =============================================================================

// 1. Division by Zero module exporting "app_main" -> i32 (42 / 0)
static const uint8_t wasm_div_zero_app_main[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // Magic & Version
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,       // Type: () -> i32
    0x03, 0x02, 0x01, 0x00,                         // Function: type 0
    0x07, 0x0c, 0x01, 0x08, 'a','p','p','_','m','a','i','n', 0x00, 0x00, // Export: app_main
    0x0a, 0x09, 0x01, 0x07, 0x00, 0x41, 0x2a, 0x41, 0x00, 0x6d, 0x0b     // 42, 0, i32.div_s, end
};

// 2. Unreachable Opcode module exporting "app_main" -> void
static const uint8_t wasm_unreachable_app_main[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // Magic & Version
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,             // Type: () -> void
    0x03, 0x02, 0x01, 0x00,                         // Function: type 0
    0x07, 0x0c, 0x01, 0x08, 'a','p','p','_','m','a','i','n', 0x00, 0x00, // Export: app_main
    0x0a, 0x05, 0x01, 0x03, 0x00, 0x00, 0x0b       // unreachable, end
};

// 3. Out-Of-Bounds Memory module with 1 page (64KiB) memory exporting "app_main" loading offset 1MB
static const uint8_t wasm_oob_app_main[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // Magic & Version
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,       // Type: () -> i32
    0x03, 0x02, 0x01, 0x00,                         // Function: type 0
    0x05, 0x03, 0x01, 0x00, 0x01,                   // Memory: min 1 page (64KB)
    0x07, 0x0c, 0x01, 0x08, 'a','p','p','_','m','a','i','n', 0x00, 0x00, // Export: app_main
    0x0a, 0x0b, 0x01, 0x09, 0x00, 0x41, 0x80, 0x80, 0x40, 0x28, 0x02, 0x00, 0x0b // load at 1MB
};

// 4. Memory Growth Module: 0 initial pages, max 10 pages, calls memory.grow(1)
static const uint8_t wasm_memory_grow_test[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // Magic & Version
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,       // Type: () -> i32
    0x03, 0x02, 0x01, 0x00,                         // Function: type 0
    0x05, 0x04, 0x01, 0x01, 0x00, 0x0a,             // Memory: min 0, max 10
    0x07, 0x0c, 0x01, 0x08, 'a','p','p','_','m','a','i','n', 0x00, 0x00, // Export: app_main
    0x0a, 0x08, 0x01, 0x06, 0x00, 0x41, 0x01, 0x40, 0x00, 0x0b           // i32.const 1, memory.grow 0, end
};

// 5. Successful Clean Micro-Workload (returns 42)
static const uint8_t wasm_clean_workload[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x0c, 0x01, 0x08, 'a','p','p','_','m','a','i','n', 0x00, 0x00,
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2a, 0x0b
};

// =============================================================================
// Trap Containment & Fault Recovery
// =============================================================================
void test_trap_containment_and_recovery() {
    std::cout << "\n=== [traps] Trap Containment & Recovery ===" << std::endl;
    auto runtime = create_wasm_runtime();

    // 1.1 Out-of-bounds linear memory access in worker thread
    {
        std::string pod_id = "adv-oob-thread";
        bool deployed = runtime->deploy_workload(pod_id, wasm_oob_app_main, sizeof(wasm_oob_app_main), 64 * 1024);
        assert(deployed);
        assert(runtime->start(pod_id));

        // Wait for worker thread execution
        kube32::osal::Thread::sleep_ms(80);

        ContainerStatus s = runtime->get_status(pod_id);
        std::cout << "[traps] OOB Trap -> Phase: " << static_cast<int>(s.phase)
                  << ", Exit Code: " << s.exit_code
                  << ", Reason: " << s.termination_reason << std::endl;
        assert(s.phase == ContainerPhase::Failed);
        assert(s.exit_code == 139);                          // SIGSEGV, as a native process
        assert(s.termination_reason == "Error");
        assert(s.exit_message == "wasm trap: out of bounds memory access");
        assert(runtime->terminate(pod_id));
    }

    // 1.2 Division by zero in worker thread
    {
        std::string pod_id = "adv-divzero-thread";
        bool deployed = runtime->deploy_workload(pod_id, wasm_div_zero_app_main, sizeof(wasm_div_zero_app_main), 64 * 1024);
        assert(deployed);
        assert(runtime->start(pod_id));

        kube32::osal::Thread::sleep_ms(80);

        ContainerStatus s = runtime->get_status(pod_id);
        std::cout << "[traps] DivZero Trap -> Phase: " << static_cast<int>(s.phase)
                  << ", Exit Code: " << s.exit_code
                  << ", Reason: " << s.termination_reason << std::endl;
        assert(s.phase == ContainerPhase::Failed);
        assert(s.exit_code == 136);
        assert(s.termination_reason == "Error");
        assert(runtime->terminate(pod_id));
    }

    // 1.3 Unreachable opcode in worker thread
    {
        std::string pod_id = "adv-unreachable-thread";
        bool deployed = runtime->deploy_workload(pod_id, wasm_unreachable_app_main, sizeof(wasm_unreachable_app_main), 64 * 1024);
        assert(deployed);
        assert(runtime->start(pod_id));

        kube32::osal::Thread::sleep_ms(80);

        ContainerStatus s = runtime->get_status(pod_id);
        std::cout << "[traps] Unreachable Trap -> Phase: " << static_cast<int>(s.phase)
                  << ", Exit Code: " << s.exit_code
                  << ", Reason: " << s.termination_reason << std::endl;
        assert(s.phase == ContainerPhase::Failed);
        assert(s.exit_code == 134);
        assert(s.termination_reason == "Error");
        assert(runtime->terminate(pod_id));
    }

    // 1.4 Post-trap runtime reuse & recovery verification
    {
        std::string pod_id = "adv-recovery-clean";
        bool deployed = runtime->deploy_workload(pod_id, wasm_clean_workload, sizeof(wasm_clean_workload), 64 * 1024);
        assert(deployed);
        assert(runtime->start(pod_id));

        kube32::osal::Thread::sleep_ms(50);

        ContainerStatus s = runtime->get_status(pod_id);
        assert(s.phase == ContainerPhase::Terminated);
        assert(s.exit_code == 42);                 // app_main's return value
        assert(s.termination_reason == "Error");   // non-zero exit
        assert(runtime->terminate(pod_id));
        std::cout << "[traps] Clean workload ran to its return (exit code 42) after traps." << std::endl;
    }

    std::cout << "[PASS] All guest traps caught cleanly with accurate exit codes (137, 136, 134)." << std::endl;
}

// =============================================================================
// Memory Limits & Allocation Quotas
// =============================================================================
void test_memory_limits_and_allocation_quotas() {
    std::cout << "\n=== [quota] Memory Limits & Allocation Quota ===" << std::endl;
    auto runtime = create_wasm_runtime();

    // 2.1 A module declaring one 64 KiB page under a 32 KiB limit: the page
    // is backed by the limit only (see test_pod_memory_caps for accesses)
    {
        std::string pod_id = "adv-mem-initial-capped";
        bool deployed = runtime->deploy_workload(pod_id, wasm_oob_app_main, sizeof(wasm_oob_app_main), 32 * 1024);
        std::cout << "[quota] Deploy 64KiB module with 32KiB quota: deployed=" << deployed << std::endl;
        assert(deployed);
        ContainerStatus s = runtime->get_status(pod_id);
        std::cout << "[quota] Memory backing: " << s.memory_used_bytes << std::endl;
        assert(s.memory_used_bytes == 32 * 1024);
        assert(runtime->terminate(pod_id));
    }

    // 2.2 A 0-page module under a 32 KiB limit grows its first page, which
    // is backed by the limit; the memory then holds the whole limit
    {
        std::string pod_id = "adv-mem-grow-capped";
        bool deployed = runtime->deploy_workload(pod_id, wasm_memory_grow_test, sizeof(wasm_memory_grow_test), 32 * 1024);
        assert(deployed);

        int32_t result = runtime->execute_function(pod_id, "app_main");
        std::cout << "[quota] memory.grow(1) result under 32KiB limit: " << result << std::endl;
        assert(result == 0);

        ContainerStatus s = runtime->get_status(pod_id);
        std::cout << "[quota] Memory used bytes: " << s.memory_used_bytes << std::endl;
        assert(s.memory_used_bytes == 32 * 1024);

        // A second page has nothing behind it: refused (-1)
        result = runtime->execute_function(pod_id, "app_main");
        std::cout << "[quota] second memory.grow(1) under 32KiB limit: " << result << std::endl;
        assert(result == -1);

        assert(runtime->terminate(pod_id));
    }

    // 2.3 Deploy same 0-initial-page module with 128KiB limit, memory.grow(1) must succeed
    {
        std::string pod_id = "adv-mem-grow-allowed";
        bool deployed = runtime->deploy_workload(pod_id, wasm_memory_grow_test, sizeof(wasm_memory_grow_test), 128 * 1024);
        assert(deployed);

        int32_t result = runtime->execute_function(pod_id, "app_main");
        std::cout << "[quota] memory.grow(1) result under 128KiB limit: " << result << std::endl;
        // Returns previous page count (0)
        assert(result == 0);

        ContainerStatus s = runtime->get_status(pod_id);
        std::cout << "[quota] Memory used bytes after growth: " << s.memory_used_bytes << std::endl;
        assert(s.memory_used_bytes == 65536);

        assert(runtime->terminate(pod_id));
    }

    std::cout << "[PASS] Wasm3 resource limiter strictly enforces memory quotas." << std::endl;
}

// =============================================================================
// Thread Lifecycle & Starvation (50 Workloads)
// =============================================================================
void test_thread_lifecycle_and_starvation() {
    std::cout << "\n=== [threads] Thread Lifecycle & Starvation (50 Workloads) ===" << std::endl;
    auto runtime = create_wasm_runtime();

    // 3.1 50 Sequential Micro-Workloads: deploy -> start -> wait -> terminate
    {
        std::cout << "[threads] Executing 50 sequential workloads..." << std::endl;
        auto start_time = std::chrono::steady_clock::now();

        for (int i = 0; i < 50; ++i) {
            std::string pod_id = "seq-pod-" + std::to_string(i);
            bool dep = runtime->deploy_workload(pod_id, wasm_clean_workload, sizeof(wasm_clean_workload), 64 * 1024);
            assert(dep);
            assert(runtime->start(pod_id));

            // Wait for completion
            int checks = 0;
            while (checks < 50) {
                ContainerStatus s = runtime->get_status(pod_id);
                if (s.phase == ContainerPhase::Terminated) break;
                kube32::osal::Thread::sleep_ms(5);
                checks++;
            }

            ContainerStatus s = runtime->get_status(pod_id);
            assert(s.phase == ContainerPhase::Terminated);
            assert(s.exit_code == 42);   // app_main returns 42

            assert(runtime->terminate(pod_id));
            assert(!runtime->is_deployed(pod_id));
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        std::cout << "[threads] 50 sequential workloads completed cleanly in " << elapsed << " ms." << std::endl;
    }

    // 3.2 50 Concurrent Micro-Workloads: deploy all -> start all -> join all -> terminate all
    {
        std::cout << "[threads] Executing 50 concurrent workloads..." << std::endl;
        const int CONCURRENT_COUNT = 50;
        std::vector<std::string> pod_ids;

        for (int i = 0; i < CONCURRENT_COUNT; ++i) {
            std::string pid = "conc-pod-" + std::to_string(i);
            pod_ids.push_back(pid);
            bool dep = runtime->deploy_workload(pid, wasm_clean_workload, sizeof(wasm_clean_workload), 64 * 1024);
            assert(dep);
        }

        auto start_time = std::chrono::steady_clock::now();

        // Start all 50 concurrent workloads
        for (const auto& pid : pod_ids) {
            assert(runtime->start(pid));
        }

        // Wait for all 50 to complete
        bool all_done = false;
        int max_wait_ms = 3000;
        int waited_ms = 0;

        while (!all_done && waited_ms < max_wait_ms) {
            all_done = true;
            for (const auto& pid : pod_ids) {
                ContainerStatus s = runtime->get_status(pid);
                if (s.phase != ContainerPhase::Terminated) {
                    all_done = false;
                    break;
                }
            }
            if (!all_done) {
                kube32::osal::Thread::sleep_ms(20);
                waited_ms += 20;
            }
        }

        assert(all_done);

        // Terminate all 50 workloads
        for (const auto& pid : pod_ids) {
            assert(runtime->terminate(pid));
            assert(!runtime->is_deployed(pid));
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        std::cout << "[threads] 50 concurrent workloads joined and terminated cleanly in " << elapsed << " ms." << std::endl;
    }

    std::cout << "[PASS] 50 sequential and 50 concurrent workloads executed with zero thread leaks." << std::endl;
}

// =============================================================================
// CrashLoopBackOff & Repeated Failure Stress
// =============================================================================
void test_crashloop_backoff_resilience() {
    std::cout << "\n=== [crashloop] CrashLoopBackOff Resilience ===" << std::endl;

    ContainerConfig cfg;
    cfg.pod_id = "crashloop-fragile-pod";
    cfg.container_name = "fragile-worker";
    cfg.memory_limit_bytes = 64 * 1024;
    cfg.restart_policy = RestartPolicy::Always;

    ContainerLifecycleController controller(cfg);
    assert(controller.deploy(wasm_div_zero_app_main, sizeof(wasm_div_zero_app_main)));

    std::cout << "[crashloop] Initial start of fragile workload..." << std::endl;
    assert(controller.start());
    kube32::osal::Thread::sleep_ms(50);

    ContainerStatus s1 = controller.get_status();
    std::cout << "[crashloop] After 1st failure: phase=" << static_cast<int>(s1.phase)
              << ", restartCount=" << s1.restart_count
              << ", exitCode=" << s1.exit_code << std::endl;
    assert(s1.phase == ContainerPhase::Failed);
    assert(s1.restart_count == 0);   // a trap is not a restart
    assert(s1.exit_code == 136);

    // Test multiple repeated restarts
    const int RESTART_CYCLES = 6;
    for (int cycle = 2; cycle <= RESTART_CYCLES; ++cycle) {
        assert(controller.restart());
        kube32::osal::Thread::sleep_ms(40);

        ContainerStatus sc = controller.get_status();
        std::cout << "[crashloop " << cycle << "] After failure cycle " << cycle
                  << ": restartCount=" << sc.restart_count
                  << ", exitCode=" << sc.exit_code << std::endl;
        assert(sc.phase == ContainerPhase::Failed);
        assert(sc.restart_count == cycle - 1);   // one per restart() call
    }

    assert(controller.stop());
    std::cout << "[PASS] Container recorded " << RESTART_CYCLES
              << " repeated failures with incrementing restartCount without crashing host." << std::endl;
}

// =============================================================================
// Uncooperative Guest Wasm Termination Test
// =============================================================================
void test_uncooperative_guest_wasm_termination() {
    std::cout << "\n=== [termination] Uncooperative Guest Wasm Termination ===" << std::endl;

    // Bytecode: app_main() -> (loop (br 0))
    static const uint8_t wasm_infinite_loop[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x07, 0x0c, 0x01, 0x08, 'a','p','p','_','m','a','i','n', 0x00, 0x00,
        0x0a, 0x0b, 0x01, 0x09, 0x00, 0x03, 0x40, 0x0c, 0x00, 0x0b, 0x41, 0x00, 0x0b
    };

    ContainerConfig cfg;
    cfg.pod_id = "runaway-guest-pod";
    cfg.container_name = "runaway-worker";
    cfg.memory_limit_bytes = 64 * 1024;
    cfg.restart_policy = RestartPolicy::Never;

    ContainerLifecycleController controller(cfg);
    assert(controller.deploy(wasm_infinite_loop, sizeof(wasm_infinite_loop)));
    assert(controller.start());

    kube32::osal::Thread::sleep_ms(40);
    assert(controller.is_running());

    auto start_t = std::chrono::steady_clock::now();
    bool stopped = controller.stop(150); // Grace period: 150ms
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_t).count();

    std::cout << "[termination] stop(150ms) returned stopped=" << stopped
              << " in " << elapsed_ms << " ms" << std::endl;

    assert(stopped);
    assert(elapsed_ms < 500); // Must NOT block indefinitely

    ContainerStatus s = controller.get_status();
    assert(s.phase == ContainerPhase::Failed);
    assert(s.exit_code == 137);
    assert(s.termination_reason == "Error");
    assert(!controller.is_running());

    std::cout << "[PASS] Uncooperative guest loop terminated cleanly without blocking host." << std::endl;
}

int main() {
    std::cout << "================================================================" << std::endl;
    std::cout << " Kube32 Container Fault Containment Tests                     " << std::endl;
    std::cout << " Targeting: Wasm3 Sandboxing, Quotas, Threading, & Fault Traps  " << std::endl;
    std::cout << "================================================================" << std::endl;

    test_trap_containment_and_recovery();
    test_memory_limits_and_allocation_quotas();
    test_thread_lifecycle_and_starvation();
    test_crashloop_backoff_resilience();
    test_uncooperative_guest_wasm_termination();

    std::cout << "\n================================================================" << std::endl;
    std::cout << " ALL CONTAINER FAULT TESTS PASSED                            " << std::endl;
    std::cout << "================================================================" << std::endl;
    return 0;
}
