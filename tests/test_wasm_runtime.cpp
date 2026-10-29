#include "runtime/wasm_runtime.hpp"
#include "runtime/container_lifecycle.hpp"
#include <iostream>
#include <cassert>
#include <vector>

using namespace kube32::runtime;

#include "osal/thread.hpp"
#include "wasm3.h"

// Wasm module exporting "app_main" returning 42
static const uint8_t wasm_return_42[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // Header
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,       // Type: () -> i32
    0x03, 0x02, 0x01, 0x00,                         // Function 0: type 0
    0x07, 0x0c, 0x01, 0x08, 'a','p','p','_','m','a','i','n', 0x00, 0x00, // Export: app_main -> func 0
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2a, 0x0b // Code: i32.const 42, end
};

// Same module, app_main returns 0
static const uint8_t wasm_return_0[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x0c, 0x01, 0x08, 'a','p','p','_','m','a','i','n', 0x00, 0x00,
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x00, 0x0b
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
    assert(status.exit_code == 136);                     // SIGFPE
    assert(status.termination_reason == "Error");
    assert(status.exit_message == "wasm trap: integer divide by zero");
    assert(status.restart_count == 0);   // a trap is not a restart

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
    assert(status.exit_code == 134);                     // SIGABRT
    assert(status.termination_reason == "Error");
    assert(status.exit_message == "wasm trap: unreachable");

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
    assert(status.exit_code == 139);                     // SIGSEGV: a bad access, not memory exhaustion
    assert(status.termination_reason == "Error");
    assert(status.exit_message == "wasm trap: out of bounds memory access");

    std::cout << "[PASS] test_oob_memory_trap_containment" << std::endl;
}

// "grow_abort": memory.grow by 4 pages, drop the result, unreachable (the
// shape of an allocator aborting when it gets no memory). One page, no maximum.
static const uint8_t wasm_grow_abort[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,             // Type: () -> void
    0x03, 0x02, 0x01, 0x00,
    0x05, 0x03, 0x01, 0x00, 0x01,                   // Memory: min 1 page
    0x07, 0x0e, 0x01, 0x0a, 'g','r','o','w','_','a','b','o','r','t', 0x00, 0x00,
    0x0a, 0x0a, 0x01, 0x08, 0x00, 0x41, 0x04, 0x40, 0x00, 0x1a, 0x00, 0x0b
};

// Same, but the module declares a maximum of 1 page itself.
static const uint8_t wasm_grow_abort_own_max[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x05, 0x04, 0x01, 0x01, 0x01, 0x01,             // Memory: min 1, max 1
    0x07, 0x0e, 0x01, 0x0a, 'g','r','o','w','_','a','b','o','r','t', 0x00, 0x00,
    0x0a, 0x0a, 0x01, 0x08, 0x00, 0x41, 0x04, 0x40, 0x00, 0x1a, 0x00, 0x0b
};

void test_memory_exhaustion_is_oomkilled() {
    auto runtime = create_wasm_runtime();

    // memory.grow refused at the pod's 64 KiB limit, then abort: OOMKilled
    assert(runtime->deploy_workload("oom-at-limit", wasm_grow_abort, sizeof(wasm_grow_abort), 64 * 1024));
    assert(runtime->execute_function("oom-at-limit", "grow_abort") < 0);
    ContainerStatus s = runtime->get_status("oom-at-limit");
    assert(s.phase == ContainerPhase::Failed);
    assert(s.exit_code == 137 && s.termination_reason == "OOMKilled");
    assert(s.exit_message.find("memory.grow refused at the pod memory limit of 64 KiB") != std::string::npos);
    assert(s.exit_message.find("unreachable") != std::string::npos);

    // The grow fits the limit: the abort is an ordinary abort
    assert(runtime->deploy_workload("abort-with-room", wasm_grow_abort, sizeof(wasm_grow_abort), 512 * 1024));
    assert(runtime->execute_function("abort-with-room", "grow_abort") < 0);
    s = runtime->get_status("abort-with-room");
    assert(s.exit_code == 134 && s.termination_reason == "Error");

    // Refused by the module's own maximum, not the pod's limit: not OOMKilled
    assert(runtime->deploy_workload("abort-own-max", wasm_grow_abort_own_max, sizeof(wasm_grow_abort_own_max), 512 * 1024));
    assert(runtime->execute_function("abort-own-max", "grow_abort") < 0);
    s = runtime->get_status("abort-own-max");
    assert(s.exit_code == 134 && s.termination_reason == "Error");

    std::cout << "[PASS] test_memory_exhaustion_is_oomkilled" << std::endl;
}

void test_trap_report_mapping() {
    TrapContext ctx;
    ctx.memory_limit_bytes = 64 * 1024;
    auto check = [&](const char* trap, int32_t code, const char* reason) {
        TrapReport t = describe_trap(trap, ctx);
        if (t.exit_code != code || t.reason != reason) {
            std::cout << "describe_trap(" << trap << ") = " << t.exit_code << " " << t.reason << std::endl;
            assert(false);
        }
        return t;
    };
    check(m3Err_trapOutOfBoundsMemoryAccess, 139, "Error");
    check(m3Err_trapStackOverflow, 139, "Error");
    check(m3Err_trapIndirectCallTypeMismatch, 139, "Error");
    check(m3Err_trapTableIndexOutOfRange, 139, "Error");
    check(m3Err_trapDivisionByZero, 136, "Error");
    check(m3Err_trapIntegerOverflow, 136, "Error");
    check(m3Err_trapIntegerConversion, 136, "Error");
    check(m3Err_trapUnreachable, 134, "Error");
    check(m3Err_trapAbort, 134, "Error");
    check(m3Err_trapUnsupportedInstruction, 132, "Error");
    check(m3Err_mallocFailed, 137, "OOMKilled");
    check(m3Err_memoryLimitExceeded, 137, "OOMKilled");
    // Out of gas: the kubelet stopping the container (SIGKILL), or a call
    // spending its whole execution budget (SIGXCPU)
    ctx.stop_requested = true;
    assert(check(m3Err_trapOutOfGas, 137, "Error").message.find("stopped by the kubelet") != std::string::npos);
    ctx.stop_requested = false;
    ctx.gas_budget = 1000000;
    assert(check(m3Err_trapOutOfGas, 152, "Error").message.find("execution budget of 1000000 gas units") != std::string::npos);
    // A segfault after a refused grow stays a segfault, with the hint
    ctx.memory_grow_error = m3Err_memoryLimitExceeded;
    assert(check(m3Err_trapOutOfBoundsMemoryAccess, 139, "Error").message.find("after a memory.grow was refused") != std::string::npos);
    check(m3Err_trapUnreachable, 137, "OOMKilled");
    ctx.memory_grow_error = m3Err_wasmMemoryOverflow;   // the module's own maximum
    check(m3Err_trapUnreachable, 134, "Error");
    std::cout << "[PASS] test_trap_report_mapping" << std::endl;
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

    // app_main's return value is the exit code: 42 is a failure
    ContainerStatus s = controller.get_status();
    assert(s.phase == ContainerPhase::Terminated);
    assert(s.exit_code == 42);
    assert(s.termination_reason == "Error");
    assert(s.exit_message == "entry point returned 42");

    assert(controller.stop());

    ContainerConfig cfg0 = cfg;
    cfg0.pod_id = "worker-thread-pod-0";
    ContainerLifecycleController ok_controller(cfg0);
    assert(ok_controller.deploy(wasm_return_0, sizeof(wasm_return_0)));
    assert(ok_controller.start());
    kube32::osal::Thread::sleep_ms(50);
    s = ok_controller.get_status();
    assert(s.phase == ContainerPhase::Terminated);
    assert(s.exit_code == 0);
    assert(s.termination_reason == "Completed");
    assert(ok_controller.stop());

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
    test_memory_exhaustion_is_oomkilled();
    test_trap_report_mapping();
    test_container_lifecycle_thread();

    std::cout << "All Wasm Runtime tests passed successfully!" << std::endl;
    return 0;
}
