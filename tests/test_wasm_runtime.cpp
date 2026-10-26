#include "runtime/wasm_runtime.hpp"
#include "runtime/container_lifecycle.hpp"
#include <iostream>
#include <cassert>
#include <vector>

using namespace kube32::runtime;

#include "osal/thread.hpp"

// Wasm module exporting "app_main" returning 42
static const uint8_t wasm_return_42[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // Header
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,       // Type: () -> i32
    0x03, 0x02, 0x01, 0x00,                         // Function 0: type 0
    0x07, 0x0c, 0x01, 0x08, 'a','p','p','_','m','a','i','n', 0x00, 0x00, // Export: app_main -> func 0
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2a, 0x0b // Code: i32.const 42, end
};

// Wasm module exporting "test_div_zero" dividing 1 by 0
static const uint8_t wasm_div_zero[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // Header
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,       // Type: () -> i32
    0x03, 0x02, 0x01, 0x00,                         // Function 0: type 0
    0x07, 0x11, 0x01, 0x0d, 't','e','s','t','_','d','i','v','_','z','e','r','o', 0x00, 0x00, // Export: test_div_zero
    0x0a, 0x09, 0x01, 0x07, 0x00, 0x41, 0x01, 0x41, 0x00, 0x6d, 0x0b // Code: i32.const 1, i32.const 0, i32.div_s, end
};

// Wasm module exporting "test_unreachable" executing unreachable opcode (0x00)
static const uint8_t wasm_unreachable[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // Header
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,             // Type: () -> void
    0x03, 0x02, 0x01, 0x00,                         // Function 0: type 0
    0x07, 0x14, 0x01, 0x10, 't','e','s','t','_','u','n','r','e','a','c','h','a','b','l','e', 0x00, 0x00,
    0x0a, 0x05, 0x01, 0x03, 0x00, 0x00, 0x0b       // Code: unreachable, end
};

// Wasm module with 1 memory page (64KiB) exporting "test_oob" loading at offset 0x100000 (1MB)
static const uint8_t wasm_oob_memory[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // Header
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,       // Type: () -> i32
    0x03, 0x02, 0x01, 0x00,                         // Function 0: type 0
    0x05, 0x03, 0x01, 0x00, 0x01,                   // Memory: min 1 page (64KB)
    0x07, 0x0c, 0x01, 0x08, 't','e','s','t','_','o','o','b', 0x00, 0x00, // Export: test_oob
    0x0a, 0x0b, 0x01, 0x09, 0x00, 0x41, 0x80, 0x80, 0x40, 0x28, 0x02, 0x00, 0x0b // Code: i32.const 1048576, i32.load 2 0, end
};

void test_successful_workload_execution() {
    auto runtime = create_wasm_runtime();
    std::string pod_id = "test-pod-42";

    bool deployed = runtime->deploy_workload(pod_id, wasm_return_42, sizeof(wasm_return_42), 64 * 1024);
    assert(deployed);
    assert(runtime->is_deployed(pod_id));

    int32_t result = runtime->execute_function(pod_id, "app_main");
    assert(result == 42);

    assert(runtime->terminate(pod_id));
    assert(!runtime->is_deployed(pod_id));

    std::cout << "[PASS] test_successful_workload_execution" << std::endl;
}

void test_division_by_zero_trap_containment() {
    auto runtime = create_wasm_runtime();
    std::string pod_id = "test-pod-divzero";

    bool deployed = runtime->deploy_workload(pod_id, wasm_div_zero, sizeof(wasm_div_zero), 64 * 1024);
    assert(deployed);

    int32_t result = runtime->execute_function(pod_id, "test_div_zero");
    assert(result < 0); // Negative error code returned

    ContainerStatus status = runtime->get_status(pod_id);
    assert(status.phase == ContainerPhase::Failed);
    assert(status.exit_code == 136);
    assert(status.termination_reason == "IntegerDivisionByZero");
    assert(status.restart_count == 1);

    std::cout << "[PASS] test_division_by_zero_trap_containment" << std::endl;
}

void test_unreachable_trap_containment() {
    auto runtime = create_wasm_runtime();
    std::string pod_id = "test-pod-unreachable";

    bool deployed = runtime->deploy_workload(pod_id, wasm_unreachable, sizeof(wasm_unreachable), 64 * 1024);
    assert(deployed);

    int32_t result = runtime->execute_function(pod_id, "test_unreachable");
    assert(result < 0);

    ContainerStatus status = runtime->get_status(pod_id);
    assert(status.phase == ContainerPhase::Failed);
    assert(status.exit_code == 134);
    assert(status.termination_reason == "UnreachableExecuted");

    std::cout << "[PASS] test_unreachable_trap_containment" << std::endl;
}

void test_oob_memory_trap_containment() {
    auto runtime = create_wasm_runtime();
    std::string pod_id = "test-pod-oob";

    bool deployed = runtime->deploy_workload(pod_id, wasm_oob_memory, sizeof(wasm_oob_memory), 64 * 1024);
    assert(deployed);

    int32_t result = runtime->execute_function(pod_id, "test_oob");
    assert(result < 0);

    ContainerStatus status = runtime->get_status(pod_id);
    assert(status.phase == ContainerPhase::Failed);
    assert(status.exit_code == 137);
    assert(status.termination_reason == "OOMKilled");

    std::cout << "[PASS] test_oob_memory_trap_containment" << std::endl;
}

void test_container_lifecycle_thread() {
    ContainerConfig cfg;
    cfg.pod_id = "worker-thread-pod";
    cfg.container_name = "worker-thread-pod";
    cfg.memory_limit_bytes = 64 * 1024;

    ContainerLifecycleController controller(cfg);
    assert(controller.deploy(wasm_return_42, sizeof(wasm_return_42)));
    assert(controller.start());

    // Allow worker thread to run entrypoint
    kube32::osal::Thread::sleep_ms(50);

    ContainerStatus s = controller.get_status();
    assert(s.phase == ContainerPhase::Terminated);
    assert(s.exit_code == 0);
    assert(s.termination_reason == "Completed");

    assert(controller.stop());

    std::cout << "[PASS] test_container_lifecycle_thread" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Running Wasm3 Sandboxed Runtime Tests  " << std::endl;
    std::cout << "========================================" << std::endl;

    test_successful_workload_execution();
    test_division_by_zero_trap_containment();
    test_unreachable_trap_containment();
    test_oob_memory_trap_containment();
    test_container_lifecycle_thread();

    std::cout << "All Wasm Runtime tests passed successfully!" << std::endl;
    return 0;
}
