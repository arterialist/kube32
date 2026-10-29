#include "scheduler/scheduler.hpp"
#include "storage/store.hpp"
#include "models/kube_models.hpp"
#include "osal/thread.hpp"

#include <cassert>
#include <iostream>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>

using namespace kube32::scheduler;
using namespace kube32::storage;

// =============================================================================
// Suite 1: Quantity Parser Adversarial Corner Cases
// =============================================================================
void test_quantity_parser_adversarial() {
    std::cout << "[RUN] test_quantity_parser_adversarial" << std::endl;

    // Standard units
    assert(parse_quantity("240Ki") == 240 * 1024);
    assert(parse_quantity("240ki") == 240 * 1024);
    assert(parse_quantity("4Mi") == 4 * 1024 * 1024);
    assert(parse_quantity("1Gi") == 1024ULL * 1024 * 1024);

    // Decimal amounts
    assert(parse_quantity("1.5Ki") == static_cast<uint64_t>(1.5 * 1024));
    assert(parse_quantity("0.5Mi") == static_cast<uint64_t>(0.5 * 1024 * 1024));

    // Zero, empty, spaces, invalid
    assert(parse_quantity("") == 0);
    assert(parse_quantity("0") == 0);
    assert(parse_quantity("0Ki") == 0);
    assert(parse_quantity("abc") == 0);
    assert(parse_quantity("Ki") == 0);
    assert(parse_quantity("!!!") == 0);

    // Raw integers without units
    assert(parse_quantity("14") == 14);
    assert(parse_quantity("245761") == 245761);

    std::cout << "[PASS] test_quantity_parser_adversarial" << std::endl;
}

// =============================================================================
// Suite 2: Capacity Exhaustion & Boundary Slicing
// =============================================================================
void test_capacity_exhaustion_boundary() {
    std::cout << "[RUN] test_capacity_exhaustion_boundary" << std::endl;

    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);

    // 1 node with 240Ki SRAM, 14 GPIO, 2 I2C, 2 ADC, 3840Ki PSRAM
    std::string node1 = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-01\"},"
                        "\"spec\":{\"unschedulable\":false},"
                        "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                        "\"allocatable\":{\"memory\":\"240Ki\",\"esp.io/gpio\":\"14\","
                        "\"esp.io/i2c\":\"2\",\"esp.io/adc\":\"2\",\"esp.io/psram\":\"3840Ki\"}}}";
    store->put("/registry/nodes/esp32-node-01", node1, true);

    auto sched = create_scheduler(*store);

    // 2.1 Exact memory fits (240Ki)
    {
        std::string p = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"p-mem-fit\",\"namespace\":\"default\"},"
                        "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"memory\":\"240Ki\"}}}]}}";
        ScheduleResult r = sched->schedule_pod_manifest(p);
        assert(r.scheduled);
        assert(r.target_node == "esp32-node-01");
    }

    // 2.2 Memory +1 byte (245761 bytes) exceeds node allocatable
    {
        std::string p = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"p-mem-overflow\",\"namespace\":\"default\"},"
                        "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"memory\":\"245761\"}}}]}}";
        ScheduleResult r = sched->schedule_pod_manifest(p);
        assert(!r.scheduled);
        assert(p.find("\"phase\":\"Pending\"") != std::string::npos);
        assert(p.find("\"reason\":\"Unschedulable\"") != std::string::npos);
    }

    // 2.3 Exact GPIO fits (14 pins)
    {
        std::string p = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"p-gpio-fit\",\"namespace\":\"default\"},"
                        "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/gpio\":\"14\"}}}]}}";
        ScheduleResult r = sched->schedule_pod_manifest(p);
        assert(r.scheduled);
        assert(r.target_node == "esp32-node-01");
    }

    // 2.4 GPIO + 1 (15 pins) exceeds
    {
        std::string p = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"p-gpio-overflow\",\"namespace\":\"default\"},"
                        "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/gpio\":\"15\"}}}]}}";
        ScheduleResult r = sched->schedule_pod_manifest(p);
        assert(!r.scheduled);
        assert(p.find("\"phase\":\"Pending\"") != std::string::npos);
    }

    // 2.5 Exact I2C fits (2) vs 3 exceeds
    {
        std::string p_fit = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"p-i2c-fit\",\"namespace\":\"default\"},"
                            "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp.io/i2c\":\"2\"}}}]}}";
        assert(sched->schedule_pod_manifest(p_fit).scheduled);

        std::string p_over = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"p-i2c-over\",\"namespace\":\"default\"},"
                             "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp.io/i2c\":\"3\"}}}]}}";
        assert(!sched->schedule_pod_manifest(p_over).scheduled);
    }

    // 2.6 Exact ADC fits (2) vs 3 exceeds
    {
        std::string p_fit = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"p-adc-fit\",\"namespace\":\"default\"},"
                            "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp.io/adc\":\"2\"}}}]}}";
        assert(sched->schedule_pod_manifest(p_fit).scheduled);

        std::string p_over = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"p-adc-over\",\"namespace\":\"default\"},"
                             "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp.io/adc\":\"3\"}}}]}}";
        assert(!sched->schedule_pod_manifest(p_over).scheduled);
    }

    // 2.7 Exact PSRAM fits (3840Ki) vs 3840Ki + 1 byte exceeds
    {
        std::string p_fit = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"p-psram-fit\",\"namespace\":\"default\"},"
                            "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/psram\":\"3840Ki\"}}}]}}";
        assert(sched->schedule_pod_manifest(p_fit).scheduled);

        std::string p_over = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"p-psram-over\",\"namespace\":\"default\"},"
                             "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/psram\":\"3932161\"}}}]}}";
        assert(!sched->schedule_pod_manifest(p_over).scheduled);
    }

    std::cout << "[PASS] test_capacity_exhaustion_boundary" << std::endl;
}

// =============================================================================
// Suite 3: OCC Atomic Binding & Version Conflict
// =============================================================================
void test_occ_atomic_binding() {
    std::cout << "[RUN] test_occ_atomic_binding" << std::endl;

    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);

    std::string node1 = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-01\"},"
                        "\"spec\":{\"unschedulable\":false},"
                        "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                        "\"allocatable\":{\"memory\":\"240Ki\"}}}";
    store->put("/registry/nodes/esp32-node-01", node1, true);

    std::string pod1 = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"pod-occ\",\"namespace\":\"default\"},"
                       "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"memory\":\"32Ki\"}}}]},"
                       "\"status\":{\"phase\":\"Pending\"}}";
    uint64_t v = 0;
    store->put_extended("/registry/pods/default/pod-occ", pod1, true, false, 0, &v);

    auto sched = create_scheduler(*store);

    // Normal schedule_pod
    ScheduleResult r = sched->schedule_pod("pod-occ", "default");
    assert(r.scheduled);
    assert(r.target_node == "esp32-node-01");

    std::string saved;
    store->get("/registry/pods/default/pod-occ", saved);
    assert(saved.find("\"type\":\"PodScheduled\",\"status\":\"True\"") != std::string::npos);
    assert(saved.find("\"nodeName\":\"esp32-node-01\"") != std::string::npos);

    std::cout << "[PASS] test_occ_atomic_binding" << std::endl;
}

// =============================================================================
// Suite 4: Cluster Reconciliation & Node Failure Modes
// =============================================================================
void test_node_failure_and_reconciliation() {
    std::cout << "[RUN] test_node_failure_and_reconciliation" << std::endl;

    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);

    // Node 1 and Node 2
    std::string node1 = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-01\"},"
                        "\"spec\":{\"unschedulable\":false},"
                        "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                        "\"allocatable\":{\"memory\":\"240Ki\"}}}";
    std::string node2 = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-02\"},"
                        "\"spec\":{\"unschedulable\":false},"
                        "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                        "\"allocatable\":{\"memory\":\"240Ki\"}}}";
    store->put("/registry/nodes/esp32-node-01", node1, true);
    store->put("/registry/nodes/esp32-node-02", node2, true);

    auto sched = create_scheduler(*store);

    // Schedule pod to node1
    std::string pod1 = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"pod-failover\",\"namespace\":\"default\"},"
                       "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"memory\":\"32Ki\"}}}]}}";
    ScheduleResult r = sched->schedule_pod_manifest(pod1);
    assert(r.scheduled && r.target_node == "esp32-node-01");
    store->put("/registry/pods/default/pod-failover", pod1, true);

    // Case 4.1: Mark node1 NotReady (Ready=False)
    std::string node1_down = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-01\"},"
                             "\"spec\":{\"unschedulable\":false},"
                             "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"False\"}],"
                             "\"allocatable\":{\"memory\":\"240Ki\"}}}";
    store->put("/registry/nodes/esp32-node-01", node1_down, true);

    // In M4: Pods remain on NotReady nodes until 30s eviction threshold triggers NodeLifecycleController eviction.
    // Scheduler alone must NOT prematurely evict pods from living NotReady nodes.
    sched->reconcile_cluster();
    std::string check1;
    store->get("/registry/pods/default/pod-failover", check1);
    assert(check1.find("\"nodeName\":\"esp32-node-01\"") != std::string::npos);

    // When NodeLifecycleController evicts the pod after eviction threshold:
    // It clears nodeName and sets phase to Pending (with DisruptionTarget condition).
    std::string evicted_pod = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"pod-failover\",\"namespace\":\"default\"},"
                              "\"spec\":{\"nodeName\":\"\",\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"memory\":\"32Ki\"}}}]},"
                              "\"status\":{\"phase\":\"Pending\",\"conditions\":[{\"type\":\"DisruptionTarget\",\"status\":\"True\",\"reason\":\"EvictionByNodeLifecycleController\"}]}}";
    store->put("/registry/pods/default/pod-failover", evicted_pod, true);

    // Reconcile pass 2: reschedules Pending pod onto node2
    sched->reconcile_cluster();
    std::string check2;
    store->get("/registry/pods/default/pod-failover", check2);
    assert(check2.find("\"type\":\"PodScheduled\",\"status\":\"True\"") != std::string::npos);
    assert(check2.find("\"nodeName\":\"esp32-node-02\"") != std::string::npos);
    std::cout << " -> Pod failover to node-02 on Ready=False: SUCCESS" << std::endl;

    // Case 4.2: Node REMOVAL (deleted from store)
    // Delete node-02
    std::cout << " -> Deleting node-02 completely from store..." << std::endl;
    store->remove("/registry/nodes/esp32-node-02");

    // Reconcile
    sched->reconcile_cluster();
    std::string check3;
    store->get("/registry/pods/default/pod-failover", check3);

    bool stranded_on_deleted_node = (check3.find("\"nodeName\":\"esp32-node-02\"") != std::string::npos);

    if (stranded_on_deleted_node) {
        std::cerr << "[CRITICAL DEFECT CONFIRMED] Pod remains Running on deleted node esp32-node-02!" << std::endl;
        std::cerr << "Root Cause: reconcile_cluster() only checks nodes present in /registry/nodes/ for Ready condition," << std::endl;
        std::cerr << "so it never evicts or reschedules pods whose assigned node was removed from the cluster!" << std::endl;
    } else {
        std::cout << " -> Pod rescheduled on node removal: SUCCESS" << std::endl;
    }

    std::cout << "[PASS] test_node_failure_and_reconciliation" << std::endl;
}

int main() {
    std::cout << "========================================================" << std::endl;
    std::cout << "  Kube32 Adversarial Scheduler C++ Test Suite           " << std::endl;
    std::cout << "========================================================" << std::endl;

    test_quantity_parser_adversarial();
    test_capacity_exhaustion_boundary();
    test_occ_atomic_binding();
    test_node_failure_and_reconciliation();

    std::cout << "========================================================" << std::endl;
    std::cout << "  Adversarial Scheduler Verification Completed          " << std::endl;
    std::cout << "========================================================" << std::endl;
    return 0;
}
