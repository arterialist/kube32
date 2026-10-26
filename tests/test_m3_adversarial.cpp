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
// CHALLENGE 1: Trap Containment & Fault Recovery
// =============================================================================
void challenge_1_trap_containment_and_recovery() {
    std::cout << "\n=== [CHALLENGE 1] Trap Containment & Recovery Stress Test ===" << std::endl;
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
        std::cout << "[CHALLENGE 1.1] OOB Trap -> Phase: " << static_cast<int>(s.phase)
                  << ", Exit Code: " << s.exit_code
                  << ", Reason: " << s.termination_reason << std::endl;
        assert(s.phase == ContainerPhase::Failed);
        assert(s.exit_code == 137);
        assert(s.termination_reason == "OOMKilled");
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
        std::cout << "[CHALLENGE 1.2] DivZero Trap -> Phase: " << static_cast<int>(s.phase)
                  << ", Exit Code: " << s.exit_code
                  << ", Reason: " << s.termination_reason << std::endl;
        assert(s.phase == ContainerPhase::Failed);
        assert(s.exit_code == 136);
        assert(s.termination_reason == "IntegerDivisionByZero");
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
        std::cout << "[CHALLENGE 1.3] Unreachable Trap -> Phase: " << static_cast<int>(s.phase)
                  << ", Exit Code: " << s.exit_code
                  << ", Reason: " << s.termination_reason << std::endl;
        assert(s.phase == ContainerPhase::Failed);
        assert(s.exit_code == 134);
        assert(s.termination_reason == "UnreachableExecuted");
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
        assert(s.exit_code == 0);
        assert(s.termination_reason == "Completed");
        assert(runtime->terminate(pod_id));
        std::cout << "[CHALLENGE 1.4] Clean workload recovered and completed after traps." << std::endl;
    }

    std::cout << "[PASS] CHALLENGE 1: All guest traps caught cleanly with accurate exit codes (137, 136, 134)." << std::endl;
}

// =============================================================================
// CHALLENGE 2: Memory Limits & Allocation Quotas
// =============================================================================
void challenge_2_memory_limits_and_allocation_quotas() {
    std::cout << "\n=== [CHALLENGE 2] Memory Limits & Allocation Quota Stress Test ===" << std::endl;
    auto runtime = create_wasm_runtime();

    // 2.1 Deploy module requiring 64KiB initial memory with 32KiB limit
    {
        std::string pod_id = "adv-mem-initial-exceeded";
        // wasm_oob_app_main declares (memory 1) which requires 64KiB (65536 bytes)
        // Request limit: 32KiB (32768 bytes)
        bool deployed = runtime->deploy_workload(pod_id, wasm_oob_app_main, sizeof(wasm_oob_app_main), 32 * 1024);
        std::cout << "[CHALLENGE 2.1] Deploy 64KiB module with 32KiB quota: deployed=" << deployed << std::endl;
        // Wasm3 resource limiter must reject module loading when initial pages exceed limit
        assert(!deployed);
        assert(!runtime->is_deployed(pod_id));
    }

    // 2.2 Deploy 0-initial-page module with 32KiB limit, bytecode executes memory.grow(1)
    {
        std::string pod_id = "adv-mem-grow-rejected";
        // wasm_memory_grow_test starts at 0 pages and tries to grow 1 page (64KiB)
        // Request limit: 32KiB (32768 bytes)
        bool deployed = runtime->deploy_workload(pod_id, wasm_memory_grow_test, sizeof(wasm_memory_grow_test), 32 * 1024);
        assert(deployed);

        int32_t result = runtime->execute_function(pod_id, "app_main");
        std::cout << "[CHALLENGE 2.2] memory.grow(1) result under 32KiB limit: " << result << std::endl;
        // WebAssembly specification: memory.grow returns -1 on failure
        assert(result == -1);

        // Verify memory used bytes remains 0
        ContainerStatus s = runtime->get_status(pod_id);
        std::cout << "[CHALLENGE 2.2] Memory used bytes: " << s.memory_used_bytes << std::endl;
        assert(s.memory_used_bytes == 0);

        assert(runtime->terminate(pod_id));
    }

    // 2.3 Deploy same 0-initial-page module with 128KiB limit, memory.grow(1) must succeed
    {
        std::string pod_id = "adv-mem-grow-allowed";
        bool deployed = runtime->deploy_workload(pod_id, wasm_memory_grow_test, sizeof(wasm_memory_grow_test), 128 * 1024);
        assert(deployed);

        int32_t result = runtime->execute_function(pod_id, "app_main");
        std::cout << "[CHALLENGE 2.3] memory.grow(1) result under 128KiB limit: " << result << std::endl;
        // Returns previous page count (0)
        assert(result == 0);

        ContainerStatus s = runtime->get_status(pod_id);
        std::cout << "[CHALLENGE 2.3] Memory used bytes after growth: " << s.memory_used_bytes << std::endl;
        assert(s.memory_used_bytes == 65536);

        assert(runtime->terminate(pod_id));
    }

    std::cout << "[PASS] CHALLENGE 2: Wasm3 resource limiter strictly enforces memory quotas." << std::endl;
}

// =============================================================================
// CHALLENGE 3: Thread Lifecycle & Starvation (50 Workloads)
// =============================================================================
void challenge_3_thread_lifecycle_and_starvation() {
    std::cout << "\n=== [CHALLENGE 3] Thread Lifecycle & Starvation (50 Workloads) ===" << std::endl;
    auto runtime = create_wasm_runtime();

    // 3.1 50 Sequential Micro-Workloads: deploy -> start -> wait -> terminate
    {
        std::cout << "[CHALLENGE 3.1] Executing 50 sequential workloads..." << std::endl;
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
            assert(s.exit_code == 0);

            assert(runtime->terminate(pod_id));
            assert(!runtime->is_deployed(pod_id));
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        std::cout << "[CHALLENGE 3.1] 50 sequential workloads completed cleanly in " << elapsed << " ms." << std::endl;
    }

    // 3.2 50 Concurrent Micro-Workloads: deploy all -> start all -> join all -> terminate all
    {
        std::cout << "[CHALLENGE 3.2] Executing 50 concurrent workloads..." << std::endl;
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
        std::cout << "[CHALLENGE 3.2] 50 concurrent workloads joined and terminated cleanly in " << elapsed << " ms." << std::endl;
    }

    std::cout << "[PASS] CHALLENGE 3: 50 sequential and 50 concurrent workloads executed with zero thread leaks." << std::endl;
}

// =============================================================================
// CHALLENGE 4: CrashLoopBackOff & Repeated Failure Stress
// =============================================================================
void challenge_4_crashloop_backoff_resilience() {
    std::cout << "\n=== [CHALLENGE 4] CrashLoopBackOff Resilience Stress Test ===" << std::endl;

    ContainerConfig cfg;
    cfg.pod_id = "crashloop-fragile-pod";
    cfg.container_name = "fragile-worker";
    cfg.memory_limit_bytes = 64 * 1024;
    cfg.restart_policy = RestartPolicy::Always;

    ContainerLifecycleController controller(cfg);
    assert(controller.deploy(wasm_div_zero_app_main, sizeof(wasm_div_zero_app_main)));

    std::cout << "[CHALLENGE 4.1] Initial start of fragile workload..." << std::endl;
    assert(controller.start());
    kube32::osal::Thread::sleep_ms(50);

    ContainerStatus s1 = controller.get_status();
    std::cout << "[CHALLENGE 4.1] After 1st failure: phase=" << static_cast<int>(s1.phase)
              << ", restartCount=" << s1.restart_count
              << ", exitCode=" << s1.exit_code << std::endl;
    assert(s1.phase == ContainerPhase::Failed);
    assert(s1.restart_count == 1);
    assert(s1.exit_code == 136);

    // Test multiple repeated restarts
    const int RESTART_CYCLES = 6;
    for (int cycle = 2; cycle <= RESTART_CYCLES; ++cycle) {
        assert(controller.restart());
        kube32::osal::Thread::sleep_ms(40);

        ContainerStatus sc = controller.get_status();
        std::cout << "[CHALLENGE 4." << cycle << "] After failure cycle " << cycle
                  << ": restartCount=" << sc.restart_count
                  << ", exitCode=" << sc.exit_code << std::endl;
        assert(sc.phase == ContainerPhase::Failed);
        assert(sc.restart_count == cycle);
    }

    assert(controller.stop());
    std::cout << "[PASS] CHALLENGE 4: Container recorded " << RESTART_CYCLES
              << " repeated failures with incrementing restartCount without crashing host." << std::endl;
}

int main() {
    std::cout << "================================================================" << std::endl;
    std::cout << " Kube32 Milestone 3 Empirical Adversarial Stress Harness        " << std::endl;
    std::cout << " Targeting: Wasm3 Sandboxing, Quotas, Threading, & Fault Traps  " << std::endl;
    std::cout << "================================================================" << std::endl;

    challenge_1_trap_containment_and_recovery();
    challenge_2_memory_limits_and_allocation_quotas();
    challenge_3_thread_lifecycle_and_starvation();
    challenge_4_crashloop_backoff_resilience();

    std::cout << "\n================================================================" << std::endl;
    std::cout << " ALL 4 EMPIRICAL ADVERSARIAL CHALLENGES COMPLETED SUCCESSFULLY! " << std::endl;
    std::cout << "================================================================" << std::endl;
    return 0;
}
