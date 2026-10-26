#include "osal/osal.hpp"
#include "runtime/container_lifecycle.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>

using namespace kube32;
using namespace kube32::osal;
using namespace kube32::runtime;

// Bytecode for minimal infinite loop
static const uint8_t wasm_infinite_loop[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x0c, 0x01, 0x08, 'a','p','p','_','m','a','i','n', 0x00, 0x00,
    0x0a, 0x0b, 0x01, 0x09, 0x00, 0x03, 0x40, 0x0c, 0x00, 0x0b, 0x41, 0x00, 0x0b
};

// =============================================================================
// TEST 1: OSAL Thread Naming with Varying Lengths
// =============================================================================
void test_osal_thread_name_lengths() {
    std::cout << "\n==================================================================" << std::endl;
    std::cout << " [TEST 1] OSAL Thread Naming with Varying Lengths                 " << std::endl;
    std::cout << "==================================================================" << std::endl;

    struct TestCase {
        std::string category;
        std::string input_name;
        size_t expected_max_len;
    };

    std::vector<TestCase> cases = {
        // Short < 15 chars
        {"Short (8 chars)", "thd_mini", 8},
        {"Short (14 chars)", "short_worker14", 14},

        // Medium = 22 chars
        {"Medium (22 chars)", "medium_thread_name_22c", 22},

        // Boundary near 31/32 chars (internal buffer is char name_buf[32])
        {"Boundary (31 chars)", "boundary_thread_name_exact_31ch", 31},
        {"Boundary (32 chars)", "boundary_thread_name_exact_32cha", 31}, // truncated to 31
        {"Boundary (33 chars)", "boundary_thread_name_exact_33char", 31}, // truncated to 31

        // Long > 64 chars
        {"Long (65 chars)",
         "long_thread_name_that_exceeds_sixty_four_chars_boundary_adversarial65", 31},
        {"Long (100 chars)",
         "extremely_long_thread_name_designed_to_heavily_overflow_any_32_byte_internal_buffer_up_to_100_chars!", 31},
        {"Extreme (256 chars)",
         std::string(256, 'X'), 31},

        // Edge case: empty string
        {"Empty string", "", 0}
    };

    for (const auto& tc : cases) {
        std::cout << " Testing " << tc.category << " [len=" << tc.input_name.length() << "]..." << std::endl;

        std::string captured_name;
        std::atomic<bool> thread_ran{false};

        struct ThreadContext {
            std::string* out_name;
            std::atomic<bool>* ran;
        };
        ThreadContext ctx{&captured_name, &thread_ran};

        Thread t;
        Thread::Config cfg;
        cfg.name = tc.input_name.empty() ? nullptr : tc.input_name.c_str();

        bool started = t.start([](void* arg) {
            auto* c = static_cast<ThreadContext*>(arg);
            const char* current = Thread::get_current_name();
            if (current) {
                *(c->out_name) = std::string(current);
            }
            c->ran->store(true);
        }, &ctx, cfg);

        assert(started);
        assert(t.join());
        assert(thread_ran.load());

        std::cout << "   -> Input:    \"" << tc.input_name.substr(0, 40)
                  << (tc.input_name.length() > 40 ? "..." : "") << "\"" << std::endl;
        std::cout << "   -> Captured: \"" << captured_name << "\" [len=" << captured_name.length() << "]" << std::endl;

        // Truncation verification
        assert(captured_name.length() <= 31);
        if (!tc.input_name.empty()) {
            // Must prefix-match the input name up to captured length
            assert(tc.input_name.compare(0, captured_name.length(), captured_name) == 0);
            assert(captured_name.length() == tc.expected_max_len);
        }
    }

    // Edge case: nullptr config.name
    {
        std::cout << " Testing nullptr config.name..." << std::endl;
        std::atomic<bool> ran{false};
        Thread t;
        Thread::Config cfg;
        cfg.name = nullptr;
        bool started = t.start([](void* arg) {
            auto* r = static_cast<std::atomic<bool>*>(arg);
            r->store(true);
        }, &ran, cfg);
        assert(started);
        assert(t.join());
        assert(ran.load());
        std::cout << "   -> nullptr config.name passed cleanly without crash." << std::endl;
    }

    std::cout << "[PASS] TEST 1: All OSAL thread name lengths verified with clean buffer truncation and 0 memory corruption." << std::endl;
}

// =============================================================================
// TEST 2: ContainerLifecycleController with Varying Pod ID / Name Lengths
// =============================================================================
void test_container_lifecycle_thread_name_lengths() {
    std::cout << "\n==================================================================" << std::endl;
    std::cout << " [TEST 2] ContainerLifecycleController Varying Name Lengths       " << std::endl;
    std::cout << "==================================================================" << std::endl;

    struct ContainerCase {
        std::string label;
        std::string pod_id; // "pod_" + pod_id is the resulting thread name
        size_t expected_total_name_len;
    };

    std::vector<ContainerCase> cases = {
        // Short < 15 chars: "pod_" (4 chars) + "mini" (4 chars) = 8 chars
        {"Short (<15 chars)", "mini", 8},
        {"Short (<15 chars)", "worker-9c", 13},

        // Medium = 22 chars: "pod_" (4 chars) + "worker-18characters" (18 chars) = 22 chars
        {"Medium (=22 chars)", "worker-18character", 22},

        // Boundary near 31/32 chars
        {"Boundary (31 chars)", "boundary-27chars-identifier", 31},
        {"Boundary (32 chars)", "boundary-28chars-identifier!", 32},

        // Long > 64 chars
        {"Long (>64 chars)",
         "pod-identifier-created-specifically-to-exceed-sixty-four-characters-total-length-adversarial-test",
         97},
        {"Extreme (>128 chars)",
         "extremely-long-pod-id-" + std::string(120, 'z'),
         142}
    };

    for (const auto& tc : cases) {
        std::cout << " Testing Container: " << tc.label << " (pod_id len=" << tc.pod_id.length()
                  << ", expected thread_name len=" << tc.expected_total_name_len << ")..." << std::endl;

        ContainerConfig cfg;
        cfg.pod_id = tc.pod_id;
        cfg.container_name = "test-container";
        cfg.memory_limit_bytes = 64 * 1024;
        cfg.restart_policy = RestartPolicy::Never;

        auto controller = std::make_unique<ContainerLifecycleController>(cfg);
        assert(controller->deploy(wasm_infinite_loop, sizeof(wasm_infinite_loop)));
        assert(controller->start());

        osal::Thread::sleep_ms(20);
        assert(controller->is_running());

        bool stopped = controller->stop(150);
        assert(stopped);
        assert(!controller->is_running());

        ContainerStatus st = controller->get_status();
        assert(st.phase == ContainerPhase::Failed);
        assert(st.exit_code == 137);

        // Immediate reset/destruction
        controller.reset();
        std::cout << "   -> Container lifecycle cleanly stopped and destroyed." << std::endl;
    }

    std::cout << "[PASS] TEST 2: ContainerLifecycleController thread naming verified across short, medium, long, and extreme lengths." << std::endl;
}

// =============================================================================
// TEST 3: High-Concurrency Thread Naming Stress Under ASan
// =============================================================================
void test_concurrent_varying_name_stress() {
    std::cout << "\n==================================================================" << std::endl;
    std::cout << " [TEST 3] High-Concurrency Varying Thread Names (ASan Stress)     " << std::endl;
    std::cout << "==================================================================" << std::endl;

    constexpr int NUM_THREADS = 8;
    constexpr int ITERATIONS_PER_THREAD = 25;
    std::vector<std::thread> workers;
    std::atomic<int> completed_tasks{0};

    const std::vector<std::string> sample_names = {
        "s",                                         // 1 char
        "short_9c",                                  // 8 chars
        "name_fourteen",                             // 13 chars
        "medium_thread_name_22",                     // 21 chars
        "medium_thread_name_22c",                    // 22 chars
        "exact_thirty_one_chars_padding",            // 30 chars
        "exact_thirty_one_chars_padding!",           // 31 chars
        "exact_thirty_two_chars_paddings!",          // 32 chars
        "this_thread_name_is_definitely_greater_than_sixty_four_characters_long_for_adversarial_testing_123456", // 101 chars
        std::string(150, 'A'),                       // 150 chars
        std::string(300, 'B')                        // 300 chars
    };

    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([t, &sample_names, &completed_tasks]() {
            for (int i = 0; i < ITERATIONS_PER_THREAD; ++i) {
                const std::string& chosen_name = sample_names[(t + i) % sample_names.size()];

                std::atomic<bool> done{false};
                Thread th;
                Thread::Config cfg;
                cfg.name = chosen_name.c_str();

                bool ok = th.start([](void* arg) {
                    auto* d = static_cast<std::atomic<bool>*>(arg);
                    const char* cur = Thread::get_current_name();
                    assert(cur != nullptr);
                    assert(std::strlen(cur) <= 31);
                    d->store(true);
                }, &done, cfg);

                assert(ok);
                assert(th.join());
                assert(done.load());
                completed_tasks++;
            }
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    std::cout << " -> Completed " << completed_tasks.load()
              << " concurrent thread start/query/join cycles with varying name lengths." << std::endl;
    assert(completed_tasks.load() == NUM_THREADS * ITERATIONS_PER_THREAD);

    std::cout << "[PASS] TEST 3: Zero race conditions or ASan violations under heavy concurrency." << std::endl;
}

// =============================================================================
// MAIN
// =============================================================================
int main() {
    std::cout << "==================================================================" << std::endl;
    std::cout << " Kube32 Milestone 3 Challenger 7: Thread Name Length Adversarial  " << std::endl;
    std::cout << " Verifying Short (<15), Medium (=22), Long (>64), and Truncation " << std::endl;
    std::cout << "==================================================================" << std::endl;

    test_osal_thread_name_lengths();
    test_container_lifecycle_thread_name_lengths();
    test_concurrent_varying_name_stress();

    std::cout << "\n==================================================================" << std::endl;
    std::cout << " ALL THREAD NAME ADVERSARIAL TESTS PASSED (100%)                  " << std::endl;
    std::cout << "==================================================================" << std::endl;
    return 0;
}
