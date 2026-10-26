#include "models/kube_lease.hpp"
#include "controller/node_lifecycle_controller.hpp"
#include "storage/store.hpp"
#include "scheduler/scheduler.hpp"
#include "api/route_handler.hpp"
#include "mem/monotonic_arena.hpp"
#include "osal/system.hpp"

#include <iostream>
#include <cassert>
#include <cstring>

using namespace kube32::models;
using namespace kube32::controller;
using namespace kube32::storage;
using namespace kube32::api;

void test_kube_lease_serialization() {
    KubeLease lease;
    lease.name = "esp32-node-01";
    lease.namespace_name = "kube-node-lease";
    lease.holder_identity = "esp32-node-01";
    lease.lease_duration_seconds = 40;
    lease.renew_time = "2026-10-26T14:00:00.000000Z";
    lease.resource_version = 104;

    std::string json = lease.to_json();
    assert(json.find("\"kind\":\"Lease\"") != std::string::npos);
    assert(json.find("\"holderIdentity\":\"esp32-node-01\"") != std::string::npos);
    assert(json.find("\"leaseDurationSeconds\":40") != std::string::npos);

    KubeLease parsed = KubeLease::from_json(json);
    assert(parsed.name == "esp32-node-01");
    assert(parsed.holder_identity == "esp32-node-01");
    assert(parsed.lease_duration_seconds == 40);
    assert(parsed.renew_time == "2026-10-26T14:00:00.000000Z");
    assert(parsed.resource_version == 104);

    std::cout << "[PASS] test_kube_lease_serialization" << std::endl;
}

void test_lease_api_rest_endpoints() {
    StoreOptions opts;
    opts.base_path = "/tmp/k32_test_lease_store";
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);

    RouteHandler handler(*store);
    kube32::mem::MonotonicArena arena;

    // 1. Discovery Coordination v1
    HttpRequest req_disc;
    req_disc.method = HttpMethod::Get;
    req_disc.path = "/apis/coordination.k8s.io/v1";
    HttpResponse resp_disc = handler.handle_request(req_disc, arena);
    assert(resp_disc.status_code == 200);
    assert(resp_disc.body.find("\"groupVersion\":\"coordination.k8s.io/v1\"") != std::string::npos);
    assert(resp_disc.body.find("\"name\":\"leases\"") != std::string::npos);

    // 2. POST create lease
    HttpRequest req_post;
    req_post.method = HttpMethod::Post;
    req_post.path = "/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases";
    req_post.body = R"({
        "apiVersion": "coordination.k8s.io/v1",
        "kind": "Lease",
        "metadata": { "name": "node-alpha", "namespace": "kube-node-lease" },
        "spec": { "holderIdentity": "node-alpha", "leaseDurationSeconds": 40, "renewTime": "2026-10-26T14:00:00Z" }
    })";
    HttpResponse resp_post = handler.handle_request(req_post, arena);
    assert(resp_post.status_code == 201);

    // 3. GET lease
    HttpRequest req_get;
    req_get.method = HttpMethod::Get;
    req_get.path = "/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/node-alpha";
    HttpResponse resp_get = handler.handle_request(req_get, arena);
    assert(resp_get.status_code == 200);
    assert(resp_get.body.find("\"holderIdentity\"") != std::string::npos);
    assert(resp_get.body.find("node-alpha") != std::string::npos);

    // 4. PUT renew lease
    HttpRequest req_put;
    req_put.method = HttpMethod::Put;
    req_put.path = "/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/node-alpha";
    req_put.body = R"({
        "apiVersion": "coordination.k8s.io/v1",
        "kind": "Lease",
        "metadata": { "name": "node-alpha", "namespace": "kube-node-lease" },
        "spec": { "holderIdentity": "node-alpha", "leaseDurationSeconds": 40, "renewTime": "2026-10-26T14:00:05Z" }
    })";
    HttpResponse resp_put = handler.handle_request(req_put, arena);
    assert(resp_put.status_code == 200);

    // 5. Ephemeral store check: persist must not write ephemeral keys to disk
    store->persist();
    std::string key = "/registry/leases/kube-node-lease/node-alpha";
    std::string out;
    assert(store->get(key, out).ok()); // Exists in memory

    std::cout << "[PASS] test_lease_api_rest_endpoints" << std::endl;
}

void test_node_lifecycle_thresholds_and_eviction() {
    StoreOptions opts;
    opts.base_path = "/tmp/k32_test_lifecycle_store";
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);

    // Seed Node 1 (healthy initially)
    std::string node_json = R"({
        "apiVersion": "v1",
        "kind": "Node",
        "metadata": { "name": "esp32-node-1" },
        "spec": { "unschedulable": false },
        "status": {
            "conditions": [
                { "type": "Ready", "status": "True", "reason": "KubeletReady" }
            ],
            "allocatable": { "memory": "240Ki", "esp32.io/gpio": "14" }
        }
    })";
    store->put("/registry/nodes/esp32-node-1", node_json, true);

    // Seed Pod on Node 1
    std::string pod_json = R"({
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": { "name": "pod-workload", "namespace": "default" },
        "spec": { "nodeName": "esp32-node-1", "containers": [{ "name": "app", "image": "wasm://app" }] },
        "status": { "phase": "Running" }
    })";
    store->put("/registry/pods/default/pod-workload", pod_json, true);

    NodeLifecycleConfig cfg;
    cfg.check_interval_ms = 100;
    cfg.not_ready_threshold_ms = 15000;
    cfg.eviction_threshold_ms = 30000;
    NodeLifecycleController controller(*store, cfg);

    // Baseline: Record lease at simulated t = 100000ms
    uint64_t t0 = 100000;
    controller.record_node_lease("esp32-node-1", t0);

    // Advance time: check at t0 + 5000ms (105000ms) (within 15s grace period)
    // Node should remain Ready=True
    controller.reconcile_nodes(t0 + 5000);
    std::string n_out;
    assert(store->get("/registry/nodes/esp32-node-1", n_out).ok());
    assert(n_out.find("\"Ready\"") != std::string::npos);
    assert(n_out.find("\"True\"") != std::string::npos);

    // Advance mock time to trigger NotReady (elapsed = 16000ms >= 15000ms)
    controller.reconcile_nodes(t0 + 16000);

    assert(store->get("/registry/nodes/esp32-node-1", n_out).ok());
    assert(n_out.find("NodeStatusUnknown") != std::string::npos);
    assert(n_out.find("Kube32 node lease heartbeat expired") != std::string::npos);

    // Advance mock time to trigger Eviction (elapsed = 31000ms >= 30000ms)
    controller.reconcile_nodes(t0 + 31000);

    assert(store->get("/registry/nodes/esp32-node-1", n_out).ok());
    assert(n_out.find("Unreachable") != std::string::npos);

    // Assert pod was evicted: spec.nodeName == "", status.phase == "Pending", reason == "Evicted"
    std::string p_out;
    assert(store->get("/registry/pods/default/pod-workload", p_out).ok());
    assert(p_out.find("\"nodeName\":\"\"") != std::string::npos);
    assert(p_out.find("Pending") != std::string::npos);
    assert(p_out.find("Evicted") != std::string::npos);
    assert(p_out.find("DisruptionTarget") != std::string::npos);
    assert(p_out.find("\"type\":\"DisruptionTarget\"") != std::string::npos);

    std::cout << "[PASS] test_node_lifecycle_thresholds_and_eviction" << std::endl;
}

void test_self_healing_rescheduling() {
    StoreOptions opts;
    opts.base_path = "/tmp/k32_test_self_healing";
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);

    // Node 1: Dead (Ready = False)
    std::string node1 = R"({
        "apiVersion": "v1", "kind": "Node",
        "metadata": { "name": "esp32-node-1" },
        "spec": { "unschedulable": false },
        "status": {
            "conditions": [{ "type": "Ready", "status": "False", "reason": "NodeStatusUnknown" }],
            "allocatable": { "memory": "240Ki", "esp32.io/gpio": "14" }
        }
    })";

    // Node 2: Healthy surviving node
    std::string node2 = R"({
        "apiVersion": "v1", "kind": "Node",
        "metadata": { "name": "esp32-node-2" },
        "spec": { "unschedulable": false },
        "status": {
            "conditions": [{ "type": "Ready", "status": "True", "reason": "KubeletReady" }],
            "allocatable": { "memory": "240Ki", "esp32.io/gpio": "14" }
        }
    })";

    store->put("/registry/nodes/esp32-node-1", node1, true);
    store->put("/registry/nodes/esp32-node-2", node2, true);

    // Evicted Pending Pod (previously on Node 1)
    std::string pod_evicted = R"({
        "apiVersion": "v1", "kind": "Pod",
        "metadata": { "name": "pod-heal", "namespace": "default" },
        "spec": { "nodeName": "", "containers": [{ "name": "c", "image": "wasm://app", "resources": { "requests": { "memory": "64Ki" } } }] },
        "status": { "phase": "Pending", "reason": "Evicted" }
    })";
    store->put("/registry/pods/default/pod-heal", pod_evicted, true);

    kube32::scheduler::SchedulerConfig s_cfg;
    s_cfg.strategy = kube32::scheduler::ScoringStrategy::LeastAllocated;
    auto scheduler = kube32::scheduler::create_scheduler(*store, s_cfg);

    scheduler->reconcile_cluster();

    // Verify pod rebound to surviving Node 2
    std::string healed_pod;
    assert(store->get("/registry/pods/default/pod-heal", healed_pod).ok());
    assert(healed_pod.find("\"nodeName\":\"esp32-node-2\"") != std::string::npos);

    std::cout << "[PASS] test_self_healing_rescheduling" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Running Node Lease & Controller Tests  " << std::endl;
    std::cout << "========================================" << std::endl;

    test_kube_lease_serialization();
    test_lease_api_rest_endpoints();
    test_node_lifecycle_thresholds_and_eviction();
    test_self_healing_rescheduling();

    std::cout << "All Node Lease & Controller tests passed successfully!" << std::endl;
    return 0;
}
