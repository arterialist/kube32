#include "runtime/container_lifecycle.hpp"
#include "hal/capability_firewall.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>
#include <random>

using namespace kube32;
using namespace kube32::runtime;
using namespace kube32::hal;

// =============================================================================
// Bytecode: app_main() -> (loop (br 0))
// CPU-bound infinite loop without any host calls or yields
// =============================================================================
static const uint8_t wasm_infinite_loop[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x0c, 0x01, 0x08, 'a','p','p','_','m','a','i','n', 0x00, 0x00,
    0x0a, 0x0b, 0x01, 0x09, 0x00, 0x03, 0x40, 0x0c, 0x00, 0x0b, 0x41, 0x00, 0x0b
};

// =============================================================================
// SECTION 1: CONCURRENT STOP AND DESTRUCTION CYCLES WITH VARYING GRACE PERIODS
// Grace periods: 10ms, 50ms, 150ms
// =============================================================================

void test_varying_grace_periods_sequential(uint32_t grace_period_ms, int cycles) {
    std::cout << "\n------------------------------------------------------------------" << std::endl;
    std::cout << " [TEST 1.1] Sequential Stop & Destruction: Grace " << grace_period_ms
              << "ms (" << cycles << " cycles)" << std::endl;
    std::cout << "------------------------------------------------------------------" << std::endl;

    for (int cycle = 1; cycle <= cycles; ++cycle) {
        ContainerConfig cfg;
        cfg.pod_id = "seq-grace-" + std::to_string(grace_period_ms) + "-" + std::to_string(cycle);
        cfg.container_name = "worker";
        cfg.memory_limit_bytes = 64 * 1024;
        cfg.restart_policy = RestartPolicy::Never;

        auto ctrl = std::make_unique<ContainerLifecycleController>(cfg);
        assert(ctrl->deploy(wasm_infinite_loop, sizeof(wasm_infinite_loop)));
        assert(ctrl->start());

        // Let the worker enter the infinite loop
        osal::Thread::sleep_ms(20);
        assert(ctrl->is_running());

        auto t0 = std::chrono::steady_clock::now();
        bool stopped = ctrl->stop(grace_period_ms);
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        assert(stopped);
        assert(elapsed_ms < 600);
        assert(!ctrl->is_running());

        ContainerStatus st = ctrl->get_status();
        assert(st.phase == ContainerPhase::Failed);
        assert(st.exit_code == 137);
        assert(st.termination_reason == "ExecutionTimeout");

        // Immediate destruction — tests clean thread join and zero use-after-free
        ctrl.reset();
    }

    std::cout << " -> Passed " << cycles << " sequential cycles with grace=" << grace_period_ms << "ms" << std::endl;
}

void test_varying_grace_periods_concurrent(const std::vector<uint32_t>& grace_periods, int threads_per_grace) {
    std::cout << "\n------------------------------------------------------------------" << std::endl;
    std::cout << " [TEST 1.2] Concurrent Stop & Destruction across Grace Periods    " << std::endl;
    std::cout << " Grace periods: 10ms, 50ms, 150ms | Threads per grace: " << threads_per_grace << std::endl;
    std::cout << " Total concurrent worker threads: " << (grace_periods.size() * threads_per_grace) << std::endl;
    std::cout << "------------------------------------------------------------------" << std::endl;

    std::vector<std::thread> workers;
    std::atomic<int> pass_count{0};
    std::atomic<bool> start_gate{false};

    int total_tasks = static_cast<int>(grace_periods.size()) * threads_per_grace;

    for (size_t g_idx = 0; g_idx < grace_periods.size(); ++g_idx) {
        uint32_t grace = grace_periods[g_idx];
        for (int t = 0; t < threads_per_grace; ++t) {
            workers.emplace_back([grace, t, &pass_count, &start_gate]() {
                while (!start_gate.load()) {
                    std::this_thread::yield();
                }

                ContainerConfig cfg;
                cfg.pod_id = "conc-grace-" + std::to_string(grace) + "-t" + std::to_string(t);
                cfg.container_name = "conc-worker";
                cfg.memory_limit_bytes = 64 * 1024;
                cfg.restart_policy = RestartPolicy::Never;

                auto ctrl = std::make_unique<ContainerLifecycleController>(cfg);
                if (!ctrl->deploy(wasm_infinite_loop, sizeof(wasm_infinite_loop))) return;
                if (!ctrl->start()) return;

                osal::Thread::sleep_ms(15 + (t % 5) * 5);

                auto t0 = std::chrono::steady_clock::now();
                bool ok = ctrl->stop(grace);
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();

                if (!ok) return;
                if (elapsed_ms >= 600) return; // Must terminate within grace + 250ms trap + margin

                ContainerStatus st = ctrl->get_status();
                if (st.phase != ContainerPhase::Failed || st.exit_code != 137 || st.termination_reason != "ExecutionTimeout") {
                    return;
                }

                // Immediate controller destruction to stress-test concurrent teardown
                ctrl.reset();
                pass_count++;
            });
        }
    }

    // Release all threads simultaneously
    start_gate.store(true);

    for (auto& th : workers) {
        if (th.joinable()) th.join();
    }

    std::cout << " -> Completed: " << pass_count.load() << "/" << total_tasks << " concurrent tasks passed." << std::endl;
    assert(pass_count.load() == total_tasks);
    std::cout << " -> All concurrent stop & destruction cycles verified with 0 crashes!" << std::endl;
}

void test_direct_destruction_without_explicit_stop(int cycles) {
    std::cout << "\n------------------------------------------------------------------" << std::endl;
    std::cout << " [TEST 1.3] Direct Destructor Invocation while Loop Running        " << std::endl;
    std::cout << " Cycles: " << cycles << " (tests ~ContainerLifecycleController() join safety)" << std::endl;
    std::cout << "------------------------------------------------------------------" << std::endl;

    for (int cycle = 1; cycle <= cycles; ++cycle) {
        ContainerConfig cfg;
        cfg.pod_id = "dtor-stress-" + std::to_string(cycle);
        cfg.container_name = "dtor-worker";
        cfg.memory_limit_bytes = 64 * 1024;
        cfg.restart_policy = RestartPolicy::Never;

        auto ctrl = std::make_unique<ContainerLifecycleController>(cfg);
        assert(ctrl->deploy(wasm_infinite_loop, sizeof(wasm_infinite_loop)));
        assert(ctrl->start());

        osal::Thread::sleep_ms(15);
        assert(ctrl->is_running());

        // Directly destroy WITHOUT prior stop() call!
        // Destructor must execute stop(100) and cleanup_runtime() cleanly!
        ctrl.reset();
    }

    std::cout << " -> Passed " << cycles << " direct destructor cycles while thread was actively spinning." << std::endl;
}

// =============================================================================
// SECTION 2: GENERIC PIN ROLLBACK DEEP ADVERSARIAL STRESS
// =============================================================================

void test_generic_pin_rollback_exhaustive() {
    std::cout << "\n------------------------------------------------------------------" << std::endl;
    std::cout << " [TEST 2.1] Exhaustive Generic Pin Rollback Under Failure Modes   " << std::endl;
    std::cout << "------------------------------------------------------------------" << std::endl;

    CapabilityFirewall& fw = CapabilityFirewall::instance();
    fw.reset();

    // 1. Generic GPIO Exhaustion Overflow:
    // ESP32-S3 safe pins = 37 total. Requesting 50 generic GPIOs must fail and leak 0 pins.
    {
        std::string manifest = R"({
            "spec": {
                "containers": [{
                    "name": "c",
                    "resources": { "requests": { "esp.io/gpio": "50" } }
                }]
            }
        })";
        std::string err;
        bool ok = fw.register_pod("gpio-overflow-pod", manifest, err);
        assert(!ok);
        assert(err.find("Insufficient allocatable GPIO pins") != std::string::npos);
        assert(fw.get_allocated_gpio_mask() == 0);
    }

    // 2. Generic ADC Exhaustion Overflow:
    // Request 5 generic GPIOs + 15 generic ADC channels (ESP32-S3 max 10 channels)
    {
        std::string manifest = R"({
            "spec": {
                "containers": [{
                    "name": "c",
                    "resources": {
                        "requests": {
                            "esp32.io/gpio": "5",
                            "esp32.io/adc": "15"
                        }
                    }
                }]
            }
        })";
        std::string err;
        bool ok = fw.register_pod("adc-overflow-pod", manifest, err);
        assert(!ok);
        assert(err.find("Insufficient allocatable ADC1 channels") != std::string::npos);
        // Assert: 0 GPIO pins leaked!
        assert(fw.get_allocated_gpio_mask() == 0);
        assert(fw.get_allocated_adc1_mask() == 0);
    }

    // 3. Generic I2C Exhaustion Overflow:
    // Request 8 generic GPIOs + 4 generic I2C buses (ESP32-S3 max 2 buses)
    {
        std::string manifest = R"({
            "spec": {
                "containers": [{
                    "name": "c",
                    "resources": {
                        "requests": {
                            "esp.io/gpio": "8",
                            "esp.io/i2c": "4"
                        }
                    }
                }]
            }
        })";
        std::string err;
        bool ok = fw.register_pod("i2c-overflow-pod", manifest, err);
        assert(!ok);
        assert(err.find("Insufficient allocatable I2C buses") != std::string::npos);
        assert(fw.get_allocated_gpio_mask() == 0);
        assert(fw.get_allocated_i2c_mask() == 0);
    }

    // 4. Specific Strapping Pin GPIO 0 Collision + Generic GPIOs:
    // Request 6 generic GPIOs and strapping pin GPIO 0
    {
        std::string manifest = R"({
            "spec": {
                "containers": [{
                    "name": "c",
                    "resources": {
                        "requests": {
                            "esp32.io/gpio": "6",
                            "esp32.io/gpio-0": "1"
                        }
                    }
                }]
            }
        })";
        std::string err;
        bool ok = fw.register_pod("strap-collision-pod", manifest, err);
        assert(!ok);
        assert(err.find("Security Violation") != std::string::npos);
        assert(fw.get_allocated_gpio_mask() == 0);
    }

    // 5. Specific Reserved Flash Pin GPIO 28 + Generic GPIOs:
    {
        std::string manifest = R"({
            "spec": {
                "containers": [{
                    "name": "c",
                    "resources": {
                        "requests": {
                            "esp32.io/gpio": "10",
                            "esp32.io/gpio-28": "1"
                        }
                    }
                }]
            }
        })";
        std::string err;
        bool ok = fw.register_pod("flash-collision-pod", manifest, err);
        assert(!ok);
        assert(err.find("Security Violation") != std::string::npos);
        assert(fw.get_allocated_gpio_mask() == 0);
    }

    // 6. Generic UART Console Collision:
    {
        std::string manifest = R"({
            "spec": {
                "containers": [{
                    "name": "c",
                    "resources": {
                        "requests": {
                            "esp32.io/gpio": "4",
                            "esp32.io/uart-0": "1"
                        }
                    }
                }]
            }
        })";
        std::string err;
        bool ok = fw.register_pod("uart0-collision-pod", manifest, err);
        assert(!ok);
        assert(err.find("UART0 is console") != std::string::npos);
        assert(fw.get_allocated_gpio_mask() == 0);
    }

    std::cout << " -> All 6 failure permutations successfully rolled back with 0 leaked pins/resources." << std::endl;
}

void test_generic_pin_concurrent_contention_stress(int thread_count, int ops_per_thread) {
    std::cout << "\n------------------------------------------------------------------" << std::endl;
    std::cout << " [TEST 2.2] High-Concurrency Generic Pin Contention & Rollback    " << std::endl;
    std::cout << " Threads: " << thread_count << " | Operations per thread: " << ops_per_thread << std::endl;
    std::cout << "------------------------------------------------------------------" << std::endl;

    CapabilityFirewall& fw = CapabilityFirewall::instance();
    fw.reset();

    std::vector<std::thread> workers;
    std::atomic<bool> start_gate{false};
    std::atomic<int> completed_threads{0};

    for (int t = 0; t < thread_count; ++t) {
        workers.emplace_back([t, ops_per_thread, &fw, &start_gate, &completed_threads]() {
            while (!start_gate.load()) std::this_thread::yield();

            for (int op = 0; op < ops_per_thread; ++op) {
                // Interleave doomed registrations (causing rollback) and valid registrations
                if (op % 2 == 0) {
                    // Doomed: 3 generic GPIOs + 7 generic I2C buses (overflow)
                    std::string doomed_id = "doomed-" + std::to_string(t) + "-" + std::to_string(op);
                    std::string doomed_manifest = R"({
                        "spec": {
                            "containers": [{
                                "name": "c",
                                "resources": {
                                    "requests": {
                                        "esp.io/gpio": "3",
                                        "esp.io/i2c": "7"
                                    }
                                }
                            }]
                        }
                    })";
                    std::string err;
                    bool ok = fw.register_pod(doomed_id, doomed_manifest, err);
                    assert(!ok);
                } else {
                    // Valid: 2 generic GPIOs
                    std::string valid_id = "valid-" + std::to_string(t) + "-" + std::to_string(op);
                    std::string valid_manifest = R"({
                        "spec": {
                            "containers": [{
                                "name": "c",
                                "resources": {
                                    "requests": {
                                        "esp.io/gpio": "2"
                                    }
                                }
                            }]
                        }
                    })";
                    std::string err;
                    bool ok = fw.register_pod(valid_id, valid_manifest, err);
                    if (ok) {
                        osal::Thread::sleep_ms(2);
                        fw.unregister_pod(valid_id);
                    }
                }
            }
            completed_threads++;
        });
    }

    start_gate.store(true);
    for (auto& th : workers) {
        if (th.joinable()) th.join();
    }

    assert(completed_threads.load() == thread_count);

    // Final clean state assertion: absolutely 0 leaked masks across all peripherals
    assert(fw.get_allocated_gpio_mask() == 0);
    assert(fw.get_allocated_i2c_mask() == 0);
    assert(fw.get_allocated_adc1_mask() == 0);
    assert(fw.get_allocated_uart_mask() == 0);

    std::cout << " -> Completed " << (thread_count * ops_per_thread)
              << " concurrent contention operations; 0 leaked pins verified." << std::endl;
}

// =============================================================================
// MAIN ENTRY POINT
// =============================================================================
int main() {
    std::cout << "==================================================================" << std::endl;
    std::cout << " Kube32 Milestone 3 Iteration 3 Challenger 6 Harness              " << std::endl;
    std::cout << " Concurrency Stress, Thread Lifecycle & Generic Pin Rollback      " << std::endl;
    std::cout << "==================================================================" << std::endl;

    // Task 2: Varying grace periods (10ms, 50ms, 150ms)
    test_varying_grace_periods_sequential(10, 10);
    test_varying_grace_periods_sequential(50, 10);
    test_varying_grace_periods_sequential(150, 10);

    // Concurrent stop & destruction across all three grace periods simultaneously
    test_varying_grace_periods_concurrent({10, 50, 150}, 4); // 3 * 4 = 12 concurrent workers

    // Direct destruction without explicit stop() call
    test_direct_destruction_without_explicit_stop(15);

    // Task 3: Generic pin rollback under allocation failures
    test_generic_pin_rollback_exhaustive();
    test_generic_pin_concurrent_contention_stress(10, 20);

    std::cout << "\n==================================================================" << std::endl;
    std::cout << " ALL CHALLENGER 6 EMPIRICAL STRESS TESTS PASSED! (100%)           " << std::endl;
    std::cout << "==================================================================" << std::endl;

    return 0;
}
