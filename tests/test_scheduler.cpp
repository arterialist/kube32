#include "scheduler/scheduler.hpp"
#include "storage/store.hpp"
#include "models/kube_models.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace kube32::scheduler;
using namespace kube32::storage;

void test_quantity_parser() {
    std::cout << "[RUN] test_quantity_parser" << std::endl;

    assert(parse_quantity("16Ki") == 16 * 1024);
    assert(parse_quantity("240Ki") == 240 * 1024);
    assert(parse_quantity("3840Ki") == 3840 * 1024);
    assert(parse_quantity("4Mi") == 4 * 1024 * 1024);
    assert(parse_quantity("1Gi") == 1024 * 1024 * 1024);
    assert(parse_quantity("14") == 14);
    assert(parse_quantity("2") == 2);
    assert(parse_quantity("0") == 0);
    assert(parse_quantity("") == 0);

    std::cout << "[PASS] test_quantity_parser" << std::endl;
}

void test_predicates() {
    std::cout << "[RUN] test_predicates" << std::endl;

    std::string ready_node = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-01\",\"labels\":{\"role\":\"worker\"}},"
                             "\"spec\":{\"unschedulable\":false},"
                             "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                             "\"allocatable\":{\"memory\":\"240Ki\",\"esp.io/gpio\":\"14\",\"esp.io/i2c\":\"2\"}}}";

    std::string not_ready_node = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-02\"},"
                                 "\"spec\":{\"unschedulable\":false},"
                                 "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"False\"}]}}";

    std::string unsched_node = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-03\"},"
                               "\"spec\":{\"unschedulable\":true},"
                               "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}]}}";

    // 1. NodeSchedulablePredicate
    {
        auto pred = std::make_shared<NodeSchedulablePredicate>();
        std::string reason;
        ResourceSpec req, alloc, used;
        assert(pred->evaluate("", ready_node, req, alloc, used, reason));
        assert(!pred->evaluate("", not_ready_node, req, alloc, used, reason));
        assert(!pred->evaluate("", unsched_node, req, alloc, used, reason));
    }

    // 2. NodeMemoryPredicate
    {
        auto pred = std::make_shared<NodeMemoryPredicate>();
        std::string reason;
        ResourceSpec alloc, used;
        alloc.memory_bytes = 240 * 1024;
        used.memory_bytes = 100 * 1024;

        ResourceSpec req_fit;
        req_fit.memory_bytes = 64 * 1024;
        assert(pred->evaluate("", ready_node, req_fit, alloc, used, reason));

        ResourceSpec req_overflow;
        req_overflow.memory_bytes = 200 * 1024; // 100 + 200 > 240
        assert(!pred->evaluate("", ready_node, req_overflow, alloc, used, reason));
    }

    // 3. NodePeripheralPredicate
    {
        auto pred = std::make_shared<NodePeripheralPredicate>();
        std::string reason;
        ResourceSpec alloc, used;
        alloc.gpio_pins = 14;
        used.gpio_pins = 10;
        alloc.i2c_buses = 2;
        used.i2c_buses = 0;

        ResourceSpec req_fit;
        req_fit.gpio_pins = 4;
        assert(pred->evaluate("", ready_node, req_fit, alloc, used, reason));

        ResourceSpec req_excess_gpio;
        req_excess_gpio.gpio_pins = 5; // 10 + 5 > 14
        assert(!pred->evaluate("", ready_node, req_excess_gpio, alloc, used, reason));
    }

    // 4. NodeSelectorPredicate
    {
        auto pred = std::make_shared<NodeSelectorPredicate>();
        std::string reason;
        ResourceSpec req, alloc, used;

        std::string pod_match = "{\"spec\":{\"nodeSelector\":{\"role\":\"worker\"}}}";
        std::string pod_mismatch = "{\"spec\":{\"nodeSelector\":{\"role\":\"gpu\"}}}";

        assert(pred->evaluate(pod_match, ready_node, req, alloc, used, reason));
        assert(!pred->evaluate(pod_mismatch, ready_node, req, alloc, used, reason));
    }

    std::cout << "[PASS] test_predicates" << std::endl;
}

void test_scheduler_atomic_binding() {
    std::cout << "[RUN] test_scheduler_atomic_binding" << std::endl;

    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);

    // Register 2 nodes
    std::string node1 = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-01\"},"
                        "\"spec\":{\"unschedulable\":false},"
                        "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                        "\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"192.168.4.1\"}],"
                        "\"allocatable\":{\"memory\":\"240Ki\",\"esp.io/gpio\":\"14\"}}}";

    std::string node2 = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-02\"},"
                        "\"spec\":{\"unschedulable\":false},"
                        "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                        "\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"192.168.4.2\"}],"
                        "\"allocatable\":{\"memory\":\"240Ki\",\"esp.io/gpio\":\"14\"}}}";

    store->put("/registry/nodes/esp32-node-01", node1, true);
    store->put("/registry/nodes/esp32-node-02", node2, true);

    auto sched = create_scheduler(*store);

    // Schedule small pod
    std::string pod1 = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"small-pod\",\"namespace\":\"default\"},"
                       "\"spec\":{\"containers\":[{\"name\":\"c1\",\"resources\":{\"requests\":{\"memory\":\"32Ki\",\"esp.io/gpio\":\"2\"}}}]}}";
    ScheduleResult res1 = sched->schedule_pod_manifest(pod1);
    assert(res1.scheduled);
    assert(res1.target_node == "esp32-node-01"); // deterministic tie-break
    assert(pod1.find("\"phase\":\"Running\"") != std::string::npos);
    assert(pod1.find("\"nodeName\":\"esp32-node-01\"") != std::string::npos);

    // Commit small pod to store
    store->put("/registry/pods/default/small-pod", pod1, true);

    // Schedule second pod: should pick node2 under LeastAllocated
    std::string pod2 = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"second-pod\",\"namespace\":\"default\"},"
                       "\"spec\":{\"containers\":[{\"name\":\"c2\",\"resources\":{\"requests\":{\"memory\":\"32Ki\",\"esp.io/gpio\":\"2\"}}}]}}";
    ScheduleResult res2 = sched->schedule_pod_manifest(pod2);
    assert(res2.scheduled);
    assert(res2.target_node == "esp32-node-02"); // node2 has 100% free memory!
    assert(pod2.find("\"phase\":\"Running\"") != std::string::npos);

    // Schedule excessive pod that exceeds all nodes
    std::string pod_giant = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"giant-pod\",\"namespace\":\"default\"},"
                            "\"spec\":{\"containers\":[{\"name\":\"cg\",\"resources\":{\"requests\":{\"memory\":\"500Ki\"}}}]}}";
    ScheduleResult res3 = sched->schedule_pod_manifest(pod_giant);
    assert(!res3.scheduled);
    assert(pod_giant.find("\"phase\":\"Pending\"") != std::string::npos);
    assert(pod_giant.find("\"reason\":\"Unschedulable\"") != std::string::npos);

    std::cout << "[PASS] test_scheduler_atomic_binding" << std::endl;
}

void test_deleted_node_reconciliation() {
    std::cout << "[RUN] test_deleted_node_reconciliation" << std::endl;

    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);

    // Register 2 nodes
    std::string node1 = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-01\"},"
                        "\"spec\":{\"unschedulable\":false},"
                        "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                        "\"allocatable\":{\"memory\":\"240Ki\",\"esp.io/gpio\":\"14\"}}}";

    std::string node2 = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-02\"},"
                        "\"spec\":{\"unschedulable\":false},"
                        "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                        "\"allocatable\":{\"memory\":\"240Ki\",\"esp.io/gpio\":\"14\"}}}";

    store->put("/registry/nodes/esp32-node-01", node1, true);
    store->put("/registry/nodes/esp32-node-02", node2, true);

    auto sched = create_scheduler(*store);

    // Register a running pod on esp32-node-01
    std::string pod = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"migrating-pod\",\"namespace\":\"default\"},"
                      "\"spec\":{\"nodeName\":\"esp32-node-01\",\"containers\":[{\"name\":\"c1\",\"resources\":{\"requests\":{\"memory\":\"32Ki\"}}}]},"
                      "\"status\":{\"phase\":\"Running\"}}";
    store->put("/registry/pods/default/migrating-pod", pod, true);

    // Delete esp32-node-01 from registry
    store->remove("/registry/nodes/esp32-node-01");

    // Pass 1: Reconciliation detects assigned_node is absent from ready_nodes, evicts to Pending
    sched->reconcile_cluster();

    std::string updated_pod;
    store->get("/registry/pods/default/migrating-pod", updated_pod);
    assert(updated_pod.find("\"phase\":\"Pending\"") != std::string::npos);
    assert(updated_pod.find("\"nodeName\":\"\"") != std::string::npos);

    // Pass 2: Reconciliation re-evaluates Pending pod and schedules onto surviving esp32-node-02
    sched->reconcile_cluster();

    store->get("/registry/pods/default/migrating-pod", updated_pod);
    assert(updated_pod.find("\"phase\":\"Running\"") != std::string::npos);
    assert(updated_pod.find("\"nodeName\":\"esp32-node-02\"") != std::string::npos);

    std::cout << "[PASS] test_deleted_node_reconciliation" << std::endl;
}

int main() {
    std::cout << "=== Running Kube32 Autonomous Scheduler Unit Tests ===" << std::endl;
    test_quantity_parser();
    test_predicates();
    test_scheduler_atomic_binding();
    test_deleted_node_reconciliation();
    std::cout << "=== All Scheduler Tests Passed Successfully ===" << std::endl;
    return 0;
}
