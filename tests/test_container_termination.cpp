#include "runtime/container_lifecycle.hpp"
#include "runtime/wasm_runtime.hpp"
#include "runtime/app_pod.hpp"
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

// app_main() { for (;;) esp32_delay_ms(60000); }
static const uint8_t wasm_long_sleep[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x09, 0x02, 0x60, 0x01, 0x7f, 0x00, 0x60, 0x00, 0x01, 0x7f,
    0x02, 0x16, 0x01, 0x03, 'e','n','v', 0x0e, 'e','s','p','3','2','_','d','e','l','a','y','_','m','s', 0x00, 0x00,
    0x03, 0x02, 0x01, 0x01,
    0x07, 0x0c, 0x01, 0x08, 'a','p','p','_','m','a','i','n', 0x00, 0x01,
    0x0a, 0x11, 0x01, 0x0f, 0x00, 0x03, 0x40, 0x41, 0xe0, 0xd4, 0x03, 0x10, 0x00, 0x0c, 0x00, 0x0b, 0x41, 0x00, 0x0b
};

// app_main() { for (;;) k32_tone(440, 2000, 0); }
static const uint8_t wasm_tone_loop[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x0c, 0x02, 0x60, 0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f,
    0x02, 0x10, 0x01, 0x03, 'e','n','v', 0x08, 'k','3','2','_','t','o','n','e', 0x00, 0x00,
    0x03, 0x02, 0x01, 0x01,
    0x05, 0x03, 0x01, 0x00, 0x01,
    0x07, 0x0c, 0x01, 0x08, 'a','p','p','_','m','a','i','n', 0x00, 0x01,
    0x0a, 0x16, 0x01, 0x14, 0x00, 0x03, 0x40, 0x41, 0xb8, 0x03, 0x41, 0xd0, 0x0f, 0x41, 0x00, 0x10, 0x00, 0x1a, 0x0c, 0x00, 0x0b, 0x41, 0x00, 0x0b
};

// A guest sleeping in esp32_delay_ms leaves the call when the container is
// stopped, so the stop completes and frees its thread at once.
void test_stop_interrupts_blocking_host_call() {
    auto rt = create_wasm_runtime();
    assert(rt->deploy_workload("sleeper", wasm_long_sleep, sizeof(wasm_long_sleep), 64 * 1024));
    assert(rt->start("sleeper"));
    osal::Thread::sleep_ms(50);
    assert(rt->get_status("sleeper").phase == ContainerPhase::Running);
    auto t0 = std::chrono::steady_clock::now();
    assert(rt->terminate("sleeper"));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "[stop] guest in a 60 s sleep stopped in " << ms << " ms" << std::endl;
    assert(ms < 300);                     // the sleep ends with the stop trap, no grace wait
    assert(rt->lingering_count() == 0);   // joined: stack and module freed
    std::cout << "[PASS] test_stop_interrupts_blocking_host_call" << std::endl;
}

// A host call that does not watch for the stop (a 4 s tone here) outlasts
// the grace period and the trap window: the container is reported stopped
// and its memory is released once the call returns.
void test_lingering_container_is_reaped() {
    AppPlatformHooks hooks;
    hooks.tone = [](uint32_t, uint32_t, uint32_t) -> int32_t { osal::Thread::sleep_ms(4000); return 0; };
    set_app_platform_hooks(hooks);
    CapabilityFirewall::instance().reset();
    DeviceGrant grant;
    grant.capabilities_mask = KUBE32_CAP_SPEAKER;
    std::string err;
    assert(CapabilityFirewall::instance().grant_capabilities("toner", grant, err));

    auto rt = create_wasm_runtime();
    assert(rt->deploy_workload("toner", wasm_tone_loop, sizeof(wasm_tone_loop), 64 * 1024));
    assert(rt->start("toner"));
    osal::Thread::sleep_ms(100);              // inside the first tone
    auto t0 = std::chrono::steady_clock::now();
    assert(rt->terminate("toner"));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    assert(ms < 3000);                        // did not wait for the tone
    assert(rt->lingering_count() == 1);
    rt->reap_stopped();
    assert(rt->lingering_count() == 1);       // still in the call
    for (int i = 0; i < 120 && rt->lingering_count() > 0; ++i) {
        osal::Thread::sleep_ms(50);
        rt->reap_stopped();
    }
    assert(rt->lingering_count() == 0);       // the call returned, the guest trapped, memory freed
    set_app_platform_hooks(AppPlatformHooks{});
    CapabilityFirewall::instance().reset();
    std::cout << "[PASS] test_lingering_container_is_reaped" << std::endl;
}

// =============================================================================
// Uncooperative Guest Wasm Termination & Teardown Verification
// =============================================================================
void test_uncooperative_wasm_loop_termination() {
    std::cout << "\n==================================================================" << std::endl;
    std::cout << " Single Uncooperative Guest Wasm Loop Termination" << std::endl;
    std::cout << "==================================================================" << std::endl;

    ContainerConfig cfg;
    cfg.pod_id = "infinite-loop-pod-1";
    cfg.container_name = "uncooperative-worker";
    cfg.memory_limit_bytes = 64 * 1024;
    cfg.restart_policy = RestartPolicy::Never;

    auto controller = std::make_unique<ContainerLifecycleController>(cfg);
    assert(controller->deploy(wasm_infinite_loop, sizeof(wasm_infinite_loop)));
    assert(controller->start());

    // Allow worker thread to spin up into the loop
    osal::Thread::sleep_ms(50);
    assert(controller->is_running());

    auto start_t = std::chrono::steady_clock::now();
    bool stopped = controller->stop(150); // Grace period: 150ms
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_t).count();

    std::cout << " -> stop(150ms) returned stopped=" << stopped
              << " in " << elapsed_ms << " ms" << std::endl;

    assert(stopped);
    assert(elapsed_ms < 500); // Must NOT block indefinitely; required < 500ms
    assert(!controller->is_running());

    ContainerStatus s = controller->get_status();
    std::cout << " -> Exit Code: " << s.exit_code
              << ", Reason: " << s.termination_reason
              << ", Phase: " << static_cast<int>(s.phase) << std::endl;

    assert(s.phase == ContainerPhase::Failed);
    assert(s.exit_code == 137);
    assert(s.termination_reason == "Error");

    // Destroy controller immediately and assert 0 segfaults or UAF crashes
    std::cout << " -> Destroying ContainerLifecycleController (heap reset)..." << std::endl;
    controller.reset();
    std::cout << " -> Controller destroyed successfully without crash." << std::endl;

    // Small delay to ensure any detached thread would have triggered UAF if memory was accessed incorrectly
    osal::Thread::sleep_ms(60);
    std::cout << "[PASS] Single Uncooperative Wasm Loop terminated and destroyed cleanly." << std::endl;
}

void test_stress_repeated_termination_and_destruction() {
    std::cout << "\n==================================================================" << std::endl;
    std::cout << " Repeated Uncooperative Loop Stress (15 Cycles)   " << std::endl;
    std::cout << "==================================================================" << std::endl;

    for (int cycle = 1; cycle <= 15; ++cycle) {
        ContainerConfig cfg;
        cfg.pod_id = "loop-stress-" + std::to_string(cycle);
        cfg.container_name = "stress-worker";
        cfg.memory_limit_bytes = 64 * 1024;
        cfg.restart_policy = RestartPolicy::Never;

        auto ctrl = std::make_unique<ContainerLifecycleController>(cfg);
        assert(ctrl->deploy(wasm_infinite_loop, sizeof(wasm_infinite_loop)));
        assert(ctrl->start());

        osal::Thread::sleep_ms(25);
        assert(ctrl->is_running());

        auto t0 = std::chrono::steady_clock::now();
        bool ok = ctrl->stop(150);
        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        assert(ok);
        assert(dt < 500);

        ContainerStatus st = ctrl->get_status();
        assert(st.phase == ContainerPhase::Failed);
        assert(st.exit_code == 137);
        assert(st.termination_reason == "Error");

        // Immediately deallocate
        ctrl.reset();
    }

    std::cout << "[PASS] 15 consecutive cycles of uncooperative loop stop(150) & destruction passed." << std::endl;
}

void test_concurrent_uncooperative_loops() {
    std::cout << "\n==================================================================" << std::endl;
    std::cout << " Concurrent Uncooperative Loops (5 Parallel Tasks)" << std::endl;
    std::cout << "==================================================================" << std::endl;

    constexpr int THREAD_COUNT = 5;
    std::vector<std::thread> threads;
    std::atomic<int> pass_count{0};

    for (int t = 0; t < THREAD_COUNT; ++t) {
        threads.emplace_back([t, &pass_count]() {
            ContainerConfig cfg;
            cfg.pod_id = "concurrent-loop-pod-" + std::to_string(t);
            cfg.container_name = "concurrent-worker";
            cfg.memory_limit_bytes = 64 * 1024;
            cfg.restart_policy = RestartPolicy::Never;

            auto ctrl = std::make_unique<ContainerLifecycleController>(cfg);
            if (!ctrl->deploy(wasm_infinite_loop, sizeof(wasm_infinite_loop))) return;
            if (!ctrl->start()) return;

            osal::Thread::sleep_ms(30 + t * 5);
            auto t0 = std::chrono::steady_clock::now();
            bool ok = ctrl->stop(150);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();

            if (!ok || elapsed >= 500) return;
            ContainerStatus st = ctrl->get_status();
            if (st.phase != ContainerPhase::Failed || st.exit_code != 137 || st.termination_reason != "Error") {
                return;
            }

            ctrl.reset(); // destroy controller
            pass_count++;
        });
    }

    for (auto& th : threads) {
        if (th.joinable()) th.join();
    }

    assert(pass_count == THREAD_COUNT);
    std::cout << "[PASS] All " << THREAD_COUNT
              << " concurrent uncooperative loop controllers stopped in <500ms and destroyed without crash." << std::endl;
}

// =============================================================================
// Generic Pin Rollback on Multi-Resource Allocation Failure
// =============================================================================
void test_generic_pin_rollback_on_i2c_overflow() {
    std::cout << "\n==================================================================" << std::endl;
    std::cout << " Generic GPIO Rollback on I2C Allocation Overflow " << std::endl;
    std::cout << "==================================================================" << std::endl;

    CapabilityFirewall& fw = CapabilityFirewall::instance();
    fw.reset();

    assert(fw.get_allocated_gpio_mask() == 0);
    assert(fw.get_allocated_i2c_mask() == 0);

    // Pod requests 3 generic GPIOs and 5 generic I2C buses (ESP32-S3 only has 2 buses)
    std::string doomed_manifest = R"({
        "spec": {
            "containers": [{
                "name": "c",
                "resources": {
                    "requests": {
                        "esp.io/gpio": "3",
                        "esp.io/i2c": "5"
                    }
                }
            }]
        }
    })";

    std::string err;
    bool registered = fw.register_pod("rollback-doomed-pod", doomed_manifest, err);
    std::cout << " -> register_pod doomed_manifest: registered=" << registered
              << ", error: " << err << std::endl;

    assert(!registered);
    assert(err.find("Insufficient allocatable I2C buses") != std::string::npos);

    // CRITICAL get_allocated_gpio_mask() == 0 (0 leaked pins)
    uint64_t gpio_mask = fw.get_allocated_gpio_mask();
    uint8_t i2c_mask = fw.get_allocated_i2c_mask();
    std::cout << " -> Allocated GPIO mask post-failure: 0x" << std::hex << gpio_mask << std::dec << std::endl;
    std::cout << " -> Allocated I2C mask post-failure: 0x" << std::hex << (int)i2c_mask << std::dec << std::endl;

    assert(gpio_mask == 0);
    assert(i2c_mask == 0);

    // Verify valid pod immediately after can allocate successfully
    std::string valid_manifest = R"({
        "spec": {
            "containers": [{
                "name": "c",
                "resources": {
                    "requests": {
                        "esp.io/gpio": "3",
                        "esp.io/i2c": "1"
                    }
                }
            }]
        }
    })";

    err.clear();
    bool valid_ok = fw.register_pod("rollback-valid-pod", valid_manifest, err);
    std::cout << " -> register_pod valid_manifest: registered=" << valid_ok << std::endl;

    assert(valid_ok);
    assert(fw.get_allocated_gpio_mask() != 0);
    assert(fw.get_allocated_i2c_mask() == 0x01); // Bus 0 claimed

    // Unregister valid pod and ensure clean state
    fw.unregister_pod("rollback-valid-pod");
    assert(fw.get_allocated_gpio_mask() == 0);
    assert(fw.get_allocated_i2c_mask() == 0);

    std::cout << "[PASS] Generic GPIO pins rolled back with exactly 0 leaked pins." << std::endl;
}

void test_multi_resource_permutations_rollback() {
    std::cout << "\n==================================================================" << std::endl;
    std::cout << " Multi-Resource Rollback Permutations             " << std::endl;
    std::cout << "==================================================================" << std::endl;

    CapabilityFirewall& fw = CapabilityFirewall::instance();
    fw.reset();

    // Permutation A: 5 generic GPIOs + 15 generic ADC channels (ESP32-S3 max 10 ADC channels)
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
        bool ok = fw.register_pod("perm-a-pod", manifest, err);
        assert(!ok);
        assert(err.find("Insufficient allocatable ADC1 channels") != std::string::npos);
        assert(fw.get_allocated_gpio_mask() == 0);
        assert(fw.get_allocated_adc1_mask() == 0);
    }

    // Permutation B: 4 specific safe GPIOs (2, 4, 15, 16) + invalid UART port 7 (allowed: 1, 2)
    {
        std::string manifest = R"({
            "spec": {
                "containers": [{
                    "name": "c",
                    "resources": {
                        "requests": {
                            "esp32.io/gpio-2": "1",
                            "esp32.io/gpio-4": "1",
                            "esp32.io/gpio-15": "1",
                            "esp32.io/gpio-16": "1",
                            "esp32.io/uart-7": "1"
                        }
                    }
                }]
            }
        })";
        std::string err;
        bool ok = fw.register_pod("perm-b-pod", manifest, err);
        assert(!ok);
        assert(err.find("Invalid UART port") != std::string::npos);
        assert(fw.get_allocated_gpio_mask() == 0);
        assert(fw.get_allocated_uart_mask() == 0);
    }

    // Permutation C: 6 generic GPIOs + 1 generic I2C + reserved GPIO 0 collision
    {
        std::string manifest = R"({
            "spec": {
                "containers": [{
                    "name": "c",
                    "resources": {
                        "requests": {
                            "esp.io/gpio": "6",
                            "esp.io/i2c": "1",
                            "esp.io/gpio-26": "1"
                        }
                    }
                }]
            }
        })";
        std::string err;
        bool ok = fw.register_pod("perm-c-pod", manifest, err);
        std::cout << " -> Perm C result: ok=" << ok << ", err=" << err << std::endl;
        assert(!ok);
        assert(err.find("Security Violation") != std::string::npos);
        assert(fw.get_allocated_gpio_mask() == 0);
        assert(fw.get_allocated_i2c_mask() == 0);
    }

    std::cout << "[PASS] All multi-resource failure permutations rolled back completely." << std::endl;
}

void test_concurrent_rollback_stress() {
    std::cout << "\n==================================================================" << std::endl;
    std::cout << " High-Concurrency Rollback & Allocation Race      " << std::endl;
    std::cout << "==================================================================" << std::endl;

    CapabilityFirewall& fw = CapabilityFirewall::instance();
    fw.reset();

    constexpr int NUM_THREADS = 10;
    std::vector<std::thread> threads;
    std::atomic<int> completed{0};

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([i, &fw, &completed]() {
            std::string doomed_manifest = R"({
                "spec": {
                    "containers": [{
                        "name": "c",
                        "resources": {
                            "requests": {
                                "esp.io/gpio": "2",
                                "esp.io/i2c": "9"
                            }
                        }
                    }]
                }
            })";
            std::string err;
            std::string pod_id = "stress-doomed-" + std::to_string(i);
            bool ok = fw.register_pod(pod_id, doomed_manifest, err);
            assert(!ok);

            // Valid pod registration & unregistration
            std::string valid_manifest = R"({
                "spec": {
                    "containers": [{
                        "name": "c",
                        "resources": {
                            "requests": {
                                "esp.io/gpio": "1"
                            }
                        }
                    }]
                }
            })";
            std::string v_pod_id = "stress-valid-" + std::to_string(i);
            std::string v_err;
            bool v_ok = fw.register_pod(v_pod_id, valid_manifest, v_err);
            if (v_ok) {
                osal::Thread::sleep_ms(10);
                fw.unregister_pod(v_pod_id);
            }
            completed++;
        });
    }

    for (auto& th : threads) {
        if (th.joinable()) th.join();
    }

    assert(completed == NUM_THREADS);
    assert(fw.get_allocated_gpio_mask() == 0);
    assert(fw.get_allocated_i2c_mask() == 0);
    assert(fw.get_allocated_adc1_mask() == 0);
    assert(fw.get_allocated_uart_mask() == 0);

    std::cout << "[PASS] Zero leaks under " << NUM_THREADS
              << " concurrent threads contending for failing and valid allocations." << std::endl;
}

// =============================================================================
// MAIN ENTRY POINT
// =============================================================================
int main() {
    std::cout << "==================================================================" << std::endl;
    std::cout << " Kube32 Container Termination & Pin Rollback Tests         " << std::endl;
    std::cout << " Loop termination and generic pin rollback                " << std::endl;
    std::cout << "==================================================================" << std::endl;

    // Uncooperative Guest Wasm Loop Termination & Clean Teardown
    test_uncooperative_wasm_loop_termination();
    test_stress_repeated_termination_and_destruction();
    test_concurrent_uncooperative_loops();
    test_stop_interrupts_blocking_host_call();
    test_lingering_container_is_reaped();

    // Generic Pin Rollback on Multi-Resource Allocation Failure
    test_generic_pin_rollback_on_i2c_overflow();
    test_multi_resource_permutations_rollback();
    test_concurrent_rollback_stress();

    std::cout << "\n==================================================================" << std::endl;
    std::cout << " ALL CONTAINER TERMINATION TESTS PASSED                    " << std::endl;
    std::cout << "==================================================================" << std::endl;

    return 0;
}
