#include "models/kube_lease.hpp"
#include "controller/node_lifecycle_controller.hpp"
#include "controller/deployment_controller.hpp"
#include "controller/workload_controller.hpp"
#include "storage/store.hpp"
#include "scheduler/scheduler.hpp"
#include "api/route_handler.hpp"
#include "mem/monotonic_arena.hpp"
#include "osal/system.hpp"

#include <functional>
#include <iostream>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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

static void put_lease(Store& store, const std::string& node, const std::string& renew_time, int duration_s = 15) {
    store.put("/registry/leases/kube-node-lease/" + node,
              "{\"apiVersion\":\"coordination.k8s.io/v1\",\"kind\":\"Lease\",\"metadata\":{\"name\":\"" + node +
              "\",\"namespace\":\"kube-node-lease\"},\"spec\":{\"holderIdentity\":\"" + node +
              "\",\"leaseDurationSeconds\":" + std::to_string(duration_s) + ",\"renewTime\":\"" + renew_time + "\"}}", true);
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

    // The kubelet renews its 15 s lease; the controller notes when it sees it
    uint64_t t0 = 100000;
    put_lease(*store, "esp32-node-1", "2026-10-26T14:00:00.000000Z");
    controller.reconcile_nodes(t0);
    assert(controller.last_renewal_seen("esp32-node-1") == t0);

    // A renewal at t0 + 10 s moves the deadline
    put_lease(*store, "esp32-node-1", "2026-10-26T14:00:10.000000Z");
    controller.reconcile_nodes(t0 + 10000);
    controller.reconcile_nodes(t0 + 20000);
    std::string n_out;
    assert(store->get("/registry/nodes/esp32-node-1", n_out).ok());
    assert(n_out.find("NodeStatusUnknown") == std::string::npos);

    // No renewal for leaseDurationSeconds: NotReady (Ready=Unknown) and NoSchedule
    controller.reconcile_nodes(t0 + 10000 + 15000);
    assert(store->get("/registry/nodes/esp32-node-1", n_out).ok());
    assert(n_out.find("NodeStatusUnknown") != std::string::npos);
    assert(n_out.find("\"status\":\"Unknown\"") != std::string::npos);
    assert(n_out.find("node.kubernetes.io/unreachable") != std::string::npos);
    assert(n_out.find("NoExecute") == std::string::npos);
    std::string p_out;
    assert(store->get("/registry/pods/default/pod-workload", p_out).ok());
    assert(p_out.find("Evicted") == std::string::npos);   // not evicted yet

    // Eviction delay (30 s - 15 s) after NotReady: pods go back to the scheduler
    controller.reconcile_nodes(t0 + 10000 + 30000);
    assert(store->get("/registry/nodes/esp32-node-1", n_out).ok());
    assert(n_out.find("NoExecute") != std::string::npos);
    assert(store->get("/registry/pods/default/pod-workload", p_out).ok());
    assert(p_out.find("\"nodeName\":\"\"") != std::string::npos);
    assert(p_out.find("Pending") != std::string::npos);
    assert(p_out.find("Evicted") != std::string::npos);
    assert(p_out.find("\"type\":\"DisruptionTarget\"") != std::string::npos);

    // A renewal clears the taints; the kubelet reports Ready itself
    put_lease(*store, "esp32-node-1", "2026-10-26T14:01:00.000000Z");
    controller.reconcile_nodes(t0 + 60000);
    assert(store->get("/registry/nodes/esp32-node-1", n_out).ok());
    assert(n_out.find("node.kubernetes.io/unreachable") == std::string::npos);
    assert(controller.last_renewal_seen("esp32-node-1") == t0 + 60000);

    std::cout << "[PASS] test_node_lifecycle_thresholds_and_eviction" << std::endl;
}

// A control-plane restart loses the Leases (they are not persisted) but keeps
// the Nodes: a node that was Ready gets a full lease from the restart, a node
// that was already NotReady stays NotReady and is not given a fresh grace.
void test_node_lifecycle_after_control_plane_restart() {
    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);
    store->put("/registry/nodes/alive", R"({"kind":"Node","metadata":{"name":"alive"},"spec":{},
        "status":{"conditions":[{"type":"Ready","status":"True","reason":"KubeletReady"}]}})", true);
    store->put("/registry/nodes/dead", R"({"kind":"Node","metadata":{"name":"dead"},"spec":{"taints":[{"key":"node.kubernetes.io/unreachable","effect":"NoSchedule"}]},
        "status":{"conditions":[{"type":"Ready","status":"Unknown","reason":"NodeStatusUnknown"}]}})", true);
    store->put("/registry/pods/default/on-dead", R"({"kind":"Pod","metadata":{"name":"on-dead","namespace":"default"},
        "spec":{"nodeName":"dead","containers":[{"name":"c","image":"wasm://x"}]},"status":{"phase":"Running"}})", true);

    NodeLifecycleConfig cfg;
    cfg.not_ready_threshold_ms = 15000;
    cfg.eviction_threshold_ms = 30000;
    NodeLifecycleController controller(*store, cfg);
    const uint64_t boot = 5000;
    controller.reconcile_nodes(boot);
    controller.reconcile_nodes(boot + 14000);
    std::string out;
    store->get("/registry/nodes/alive", out);
    assert(out.find("\"status\":\"True\"") != std::string::npos);   // still within its grace
    store->get("/registry/pods/default/on-dead", out);
    assert(out.find("\"nodeName\":\"dead\"") != std::string::npos);  // eviction delay runs from the restart

    // The live node renews; the dead one does not
    put_lease(*store, "alive", "2026-10-26T15:00:00.000000Z");
    controller.reconcile_nodes(boot + 15000);
    controller.reconcile_nodes(boot + 16000);
    store->get("/registry/nodes/alive", out);
    assert(out.find("\"status\":\"True\"") != std::string::npos);
    store->get("/registry/pods/default/on-dead", out);
    assert(out.find("\"nodeName\":\"\"") != std::string::npos);     // evicted 15 s after the restart
    std::cout << "[PASS] test_node_lifecycle_after_control_plane_restart" << std::endl;
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

// --- DaemonSet and Job controller -------------------------------------------

static std::string ready_node_json(const std::string& name, const std::string& board) {
    return "{\"kind\":\"Node\",\"metadata\":{\"name\":\"" + name + "\",\"labels\":{\"kubernetes.io/hostname\":\"" + name +
           "\",\"board\":\"" + board + "\"}},\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
           "\"allocatable\":{\"memory\":\"4Mi\"}}}";
}

static HttpResponse api_call(RouteHandler& h, HttpMethod m, const std::string& path, const std::string& body = "") {
    kube32::mem::MonotonicArena arena;
    HttpRequest req;
    req.method = m;
    req.path = path;
    req.body = body;
    return h.handle_request(req, arena);
}

struct OwnedPods {
    std::vector<JsonValue> pods;
    size_t count() const { return pods.size(); }
};

static OwnedPods owned_pods(Store& store, const std::string& kind, const std::string& uid) {
    OwnedPods out;
    std::vector<std::string> jsons;
    store.list("/registry/pods/", jsons);
    for (const auto& j : jsons) {
        JsonValue p;
        std::string err;
        if (parse_json(j, p, err) && has_owner(p, kind, uid)) out.pods.push_back(p);
    }
    return out;
}

static void set_pod_phase(Store& store, const JsonValue& pod, const std::string& phase) {
    JsonValue p = pod;
    JsonValue* st = p.get_mut("status");
    st->set("phase", JsonValue{JsonType::String, false, 0, phase, {}, {}});
    const JsonValue* meta = p.get("metadata");
    store.put("/registry/pods/" + meta->get_string("namespace", "default") + "/" + meta->get_string("name", ""), p.to_string(), true);
}

void test_daemonset_controller() {
    std::cout << "[RUN] test_daemonset_controller" << std::endl;
    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);
    store->put("/registry/nodes/node-a", ready_node_json("node-a", "cardputer"), true);
    store->put("/registry/nodes/node-b", ready_node_json("node-b", "devkit"), true);
    store->put("/registry/nodes/node-c", ready_node_json("node-c", "cardputer"), true);
    auto sched = kube32::scheduler::create_scheduler(*store);
    RouteHandler api(*store, sched.get());

    // The API only stores the DaemonSet; no pod exists until the controller runs
    const std::string ds = "{\"apiVersion\":\"apps/v1\",\"kind\":\"DaemonSet\",\"metadata\":{\"name\":\"agent\",\"namespace\":\"default\"},"
                           "\"spec\":{\"template\":{\"metadata\":{\"labels\":{\"app\":\"agent\"}},\"spec\":{\"nodeSelector\":{\"board\":\"cardputer\"},"
                           "\"containers\":[{\"name\":\"a\",\"image\":\"wasm://embedded/echo:v1\",\"resources\":{\"requests\":{\"memory\":\"64Ki\"}}}]}}}}";
    HttpResponse created = api_call(api, HttpMethod::Post, "/apis/apps/v1/namespaces/default/daemonsets", ds);
    assert(created.status_code == 201);
    JsonValue dsdoc;
    std::string err;
    assert(parse_json(created.body, dsdoc, err));
    const std::string uid = dsdoc.get("metadata")->get_string("uid", "");
    assert(!uid.empty());
    assert(api_call(api, HttpMethod::Post, "/apis/apps/v1/namespaces/default/daemonsets", ds).status_code == 409);
    std::vector<std::string> all;
    store->list("/registry/pods/", all);
    assert(all.empty());

    WorkloadController wc(*store, sched.get());
    wc.reconcile();
    OwnedPods pods = owned_pods(*store, "DaemonSet", uid);
    assert(pods.count() == 2);   // node-a and node-c match the nodeSelector
    for (const auto& p : pods.pods) {
        const std::string node = p.get("spec")->get_string("nodeName", "");
        assert(node == "node-a" || node == "node-c");             // bound by the scheduler
        assert(p.get("status")->get_string("phase", "") == "Pending");   // the kubelet reports the rest
        assert(!p.get("status")->get("podIP"));
    }
    wc.reconcile();
    assert(owned_pods(*store, "DaemonSet", uid).count() == 2);   // stable

    std::string stored;
    assert(store->get("/registry/daemonsets/default/agent", stored).ok());
    assert(stored.find("\"desiredNumberScheduled\":2") != std::string::npos);
    assert(stored.find("\"currentNumberScheduled\":2") != std::string::npos);
    assert(stored.find("\"numberReady\":0") != std::string::npos);

    // A node that leaves takes its pod with it
    store->remove("/registry/nodes/node-c");
    wc.reconcile();
    pods = owned_pods(*store, "DaemonSet", uid);
    size_t live = 0;
    for (const auto& p : pods.pods) if (p.get("metadata")->get_string("deletionTimestamp", "").empty()) ++live;
    assert(live == 1);

    // Deleting the DaemonSet removes its pods
    assert(api_call(api, HttpMethod::Delete, "/apis/apps/v1/namespaces/default/daemonsets/agent").status_code == 200);
    wc.reconcile();
    pods = owned_pods(*store, "DaemonSet", uid);
    for (const auto& p : pods.pods) assert(!p.get("metadata")->get_string("deletionTimestamp", "").empty());
    std::cout << "[PASS] test_daemonset_controller" << std::endl;
}

static void set_pod_ready(Store& store, const JsonValue& pod) {
    JsonValue p = pod;
    JsonValue* st = p.get_mut("status");
    st->set("phase", JsonValue{JsonType::String, false, 0, "Running", {}, {}});
    JsonValue conds;
    std::string perr;
    parse_json("[{\"type\":\"Ready\",\"status\":\"True\"}]", conds, perr);
    st->set("conditions", conds);
    const JsonValue* meta = p.get("metadata");
    store.put("/registry/pods/" + meta->get_string("namespace", "default") + "/" + meta->get_string("name", ""), p.to_string(), true);
}

static size_t count_where(const OwnedPods& pods, const std::function<bool(const JsonValue&)>& pred) {
    size_t n = 0;
    for (const auto& p : pods.pods) if (pred(p)) ++n;
    return n;
}

void test_daemonset_rolling_update() {
    std::cout << "[RUN] test_daemonset_rolling_update" << std::endl;
    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);
    store->put("/registry/nodes/node-a", ready_node_json("node-a", "x"), true);
    store->put("/registry/nodes/node-b", ready_node_json("node-b", "x"), true);
    auto sched = kube32::scheduler::create_scheduler(*store);
    RouteHandler api(*store, sched.get());
    const std::string ds = "{\"apiVersion\":\"apps/v1\",\"kind\":\"DaemonSet\",\"metadata\":{\"name\":\"agent\",\"namespace\":\"default\"},"
                           "\"spec\":{\"template\":{\"metadata\":{\"labels\":{\"app\":\"agent\"}},\"spec\":{"
                           "\"containers\":[{\"name\":\"a\",\"image\":\"wasm://embedded/echo:v1\",\"resources\":{\"requests\":{\"memory\":\"64Ki\"}}}]}}}}";
    HttpResponse created = api_call(api, HttpMethod::Post, "/apis/apps/v1/namespaces/default/daemonsets", ds);
    assert(created.status_code == 201);
    JsonValue dsdoc;
    std::string err;
    assert(parse_json(created.body, dsdoc, err));
    const std::string uid = dsdoc.get("metadata")->get_string("uid", "");
    assert(dsdoc.get("metadata")->get_int("generation", 0) == 1);

    WorkloadController wc(*store, sched.get());
    wc.reconcile();
    OwnedPods pods = owned_pods(*store, "DaemonSet", uid);
    assert(pods.count() == 2);
    const std::string rev1 = pods.pods[0].get("metadata")->get("labels")->get_string("controller-revision-hash", "");
    assert(!rev1.empty());
    for (const auto& p : pods.pods) set_pod_ready(*store, p);
    wc.reconcile();

    // New image: generation moves, pods are replaced one node at a time
    HttpResponse patched = api_call(api, HttpMethod::Patch, "/apis/apps/v1/namespaces/default/daemonsets/agent",
        "{\"spec\":{\"template\":{\"spec\":{\"containers\":[{\"name\":\"a\",\"image\":\"wasm://embedded/echo:v2\"}]}}}}");
    assert(patched.status_code == 200);
    assert(patched.body.find("\"generation\":2") != std::string::npos);
    auto terminating = [](const JsonValue& p) { return !p.get("metadata")->get_string("deletionTimestamp", "").empty(); };
    for (int node = 0; node < 2; ++node) {
        wc.reconcile();
        pods = owned_pods(*store, "DaemonSet", uid);
        assert(count_where(pods, terminating) == 1);       // one at a time
        assert(pods.count() == 2);                          // no replacement while the old pod terminates
        wc.reconcile();
        assert(count_where(owned_pods(*store, "DaemonSet", uid), terminating) == 1);
        // The kubelet finalizes the old pod; its node gets the new template
        for (const auto& p : pods.pods) {
            if (!terminating(p)) continue;
            const JsonValue* m = p.get("metadata");
            store->remove("/registry/pods/default/" + m->get_string("name", ""));
        }
        wc.reconcile();
        pods = owned_pods(*store, "DaemonSet", uid);
        assert(pods.count() == 2);
        for (const auto& p : pods.pods) {
            if (p.get("metadata")->get("labels")->get_string("controller-revision-hash", "") != rev1 &&
                p.get("status")->get_string("phase", "") != "Running") set_pod_ready(*store, p);
        }
    }
    wc.reconcile();
    pods = owned_pods(*store, "DaemonSet", uid);
    assert(count_where(pods, terminating) == 0);
    for (const auto& p : pods.pods) {
        assert(p.get("metadata")->get("labels")->get_string("controller-revision-hash", "") != rev1);
        assert(p.get("spec")->get("containers")->arr_val[0].get_string("image", "") == "wasm://embedded/echo:v2");
    }
    std::string stored;
    store->get("/registry/daemonsets/default/agent", stored);
    assert(stored.find("\"updatedNumberScheduled\":2") != std::string::npos);
    assert(stored.find("\"observedGeneration\":2") != std::string::npos);
    std::cout << "[PASS] test_daemonset_rolling_update" << std::endl;
}

void test_job_controller() {
    std::cout << "[RUN] test_job_controller" << std::endl;
    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);
    store->put("/registry/nodes/node-a", ready_node_json("node-a", "devkit"), true);
    auto sched = kube32::scheduler::create_scheduler(*store);
    RouteHandler api(*store, sched.get());

    // Job pod templates need restartPolicy Never or OnFailure
    const std::string bad = "{\"apiVersion\":\"batch/v1\",\"kind\":\"Job\",\"metadata\":{\"name\":\"bad\"},"
                            "\"spec\":{\"template\":{\"spec\":{\"containers\":[{\"name\":\"c\",\"image\":\"wasm://x\"}]}}}}";
    HttpResponse rejected = api_call(api, HttpMethod::Post, "/apis/batch/v1/namespaces/default/jobs", bad);
    assert(rejected.status_code == 422);
    assert(rejected.body.find("spec.template.spec.restartPolicy") != std::string::npos);

    const std::string job = "{\"apiVersion\":\"batch/v1\",\"kind\":\"Job\",\"metadata\":{\"name\":\"batch\",\"namespace\":\"default\"},"
                            "\"spec\":{\"completions\":2,\"parallelism\":1,\"backoffLimit\":1,\"template\":{\"spec\":{\"restartPolicy\":\"Never\","
                            "\"containers\":[{\"name\":\"c\",\"image\":\"wasm://embedded/echo:v1\"}]}}}}";
    HttpResponse created = api_call(api, HttpMethod::Post, "/apis/batch/v1/namespaces/default/jobs", job);
    assert(created.status_code == 201);
    JsonValue jdoc;
    std::string err;
    assert(parse_json(created.body, jdoc, err));
    const std::string uid = jdoc.get("metadata")->get_string("uid", "");

    WorkloadController wc(*store, sched.get());
    wc.reconcile();
    OwnedPods pods = owned_pods(*store, "Job", uid);
    assert(pods.count() == 1);   // parallelism 1
    assert(pods.pods[0].get("status")->get_string("phase", "") == "Pending");
    assert(pods.pods[0].get("spec")->get_string("nodeName", "") == "node-a");
    assert(pods.pods[0].get("metadata")->get("labels")->get_string("job-name", "") == "batch");

    // The kubelet reports the first pod Succeeded: a second one is started
    set_pod_phase(*store, pods.pods[0], "Succeeded");
    wc.reconcile();
    pods = owned_pods(*store, "Job", uid);
    assert(pods.count() == 2);
    std::string stored;
    store->get("/registry/jobs/default/batch", stored);
    assert(stored.find("\"succeeded\":1") != std::string::npos && stored.find("\"active\":1") != std::string::npos);
    assert(stored.find("\"Complete\"") == std::string::npos);

    // Second completion: the Job is Complete and starts nothing more
    for (const auto& p : pods.pods) {
        if (p.get("status")->get_string("phase", "") == "Pending") set_pod_phase(*store, p, "Succeeded");
    }
    wc.reconcile();
    wc.reconcile();
    assert(owned_pods(*store, "Job", uid).count() == 2);
    store->get("/registry/jobs/default/batch", stored);
    assert(stored.find("\"type\":\"Complete\",\"status\":\"True\"") != std::string::npos);
    assert(stored.find("\"succeeded\":2") != std::string::npos && stored.find("\"active\":0") != std::string::npos);

    // Failures past backoffLimit fail the Job
    const std::string flaky = "{\"apiVersion\":\"batch/v1\",\"kind\":\"Job\",\"metadata\":{\"name\":\"flaky\",\"namespace\":\"default\"},"
                              "\"spec\":{\"backoffLimit\":1,\"template\":{\"spec\":{\"restartPolicy\":\"Never\","
                              "\"containers\":[{\"name\":\"c\",\"image\":\"wasm://embedded/echo:v1\"}]}}}}";
    created = api_call(api, HttpMethod::Post, "/apis/batch/v1/namespaces/default/jobs", flaky);
    assert(created.status_code == 201);
    assert(parse_json(created.body, jdoc, err));
    const std::string fuid = jdoc.get("metadata")->get_string("uid", "");
    for (int round = 0; round < 2; ++round) {
        wc.reconcile();
        for (const auto& p : owned_pods(*store, "Job", fuid).pods) {
            if (p.get("status")->get_string("phase", "") == "Pending") set_pod_phase(*store, p, "Failed");
        }
    }
    wc.reconcile();
    store->get("/registry/jobs/default/flaky", stored);
    assert(stored.find("\"type\":\"Failed\",\"status\":\"True\",\"reason\":\"BackoffLimitExceeded\"") != std::string::npos);
    assert(stored.find("\"failed\":2") != std::string::npos);
    size_t before = owned_pods(*store, "Job", fuid).count();
    wc.reconcile();
    assert(owned_pods(*store, "Job", fuid).count() == before);   // no new pods after failing
    std::cout << "[PASS] test_job_controller" << std::endl;
}

// A kubectl manifest has exactly four top-level keys, so adding "status"
// on create grows doc's member vector. Fields read after that must not come
// through pointers taken before it (run under ASan to catch a regression).
void test_pod_create_four_key_manifest() {
    std::cout << "[RUN] test_pod_create_four_key_manifest" << std::endl;
    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);
    store->put("/registry/nodes/node-a", ready_node_json("node-a", "cardputer"), true);
    store->put("/registry/nodes/node-b", ready_node_json("node-b", "devkit"), true);
    auto sched = kube32::scheduler::create_scheduler(*store);
    RouteHandler api(*store, sched.get());

    auto pod = [](const std::string& name, const std::string& node_name) {
        return "{\"apiVersion\":\"v1\",\"kind\":\"Pod\",\"metadata\":{\"name\":\"" + name + "\",\"labels\":{\"app\":\"x\"}},"
               "\"spec\":{" + (node_name.empty() ? std::string() : "\"nodeName\":\"" + node_name + "\",") +
               "\"nodeSelector\":{\"board\":\"cardputer\"},\"containers\":[{\"name\":\"c\",\"image\":\"wasm://embedded/echo:v1\","
               "\"resources\":{\"requests\":{\"memory\":\"4Ki\"}}}]}}";
    };
    HttpResponse scheduled = api_call(api, HttpMethod::Post, "/api/v1/namespaces/default/pods", pod("sched", ""));
    assert(scheduled.status_code == 201);
    HttpResponse pinned = api_call(api, HttpMethod::Post, "/api/v1/namespaces/default/pods", pod("pinned", "node-b"));
    assert(pinned.status_code == 201);

    std::string json;
    JsonValue doc;
    std::string err;
    assert(store->get("/registry/pods/default/sched", json).ok() && parse_json(json, doc, err));
    assert(doc.get("spec")->get_string("nodeName", "") == "node-a");
    assert(doc.get("status")->get_string("phase", "") == "Pending");
    assert(store->get("/registry/pods/default/pinned", json).ok() && parse_json(json, doc, err));
    assert(doc.get("spec")->get_string("nodeName", "") == "node-b");   // an explicit binding is kept
    std::cout << "[PASS] test_pod_create_four_key_manifest" << std::endl;
}

// --- NoExecute taint and tolerations ----------------------------------------

static const char* const kNotReadyKey = "node.kubernetes.io/not-ready";
static const char* const kUnreachableKey = "node.kubernetes.io/unreachable";

static JsonValue stored_pod(Store& store, const std::string& name) {
    std::string json, err;
    JsonValue doc;
    assert(store.get("/registry/pods/default/" + name, json).ok() && parse_json(json, doc, err));
    return doc;
}

static const JsonValue* toleration_for(const JsonValue& pod, const std::string& key, const std::string& effect = "NoExecute") {
    const JsonValue* tols = pod.get("spec")->get("tolerations");
    if (!tols || !tols->is_array()) return nullptr;
    for (const auto& t : tols->arr_val) {
        if (t.get_string("key", "") == key && t.get_string("effect", "") == effect) return &t;
    }
    return nullptr;
}

static size_t toleration_count(const JsonValue& pod) {
    const JsonValue* tols = pod.get("spec")->get("tolerations");
    return tols && tols->is_array() ? tols->arr_val.size() : 0;
}

// Tolerations as the DefaultTolerationSeconds plugin leaves them: Exists,
// NoExecute and 300 s (or no tolerationSeconds, `seconds` -1)
static void assert_toleration(const JsonValue& pod, const std::string& key, int64_t seconds) {
    const JsonValue* t = toleration_for(pod, key);
    assert(t);
    assert(t->get_string("operator", "") == "Exists");
    if (seconds < 0) assert(!t->get("tolerationSeconds"));
    else assert(t->get_int("tolerationSeconds", -1) == seconds);
}

static std::string pod_manifest(const std::string& name, const std::string& node, const std::string& tolerations_json = "") {
    return "{\"apiVersion\":\"v1\",\"kind\":\"Pod\",\"metadata\":{\"name\":\"" + name + "\"},\"spec\":{" +
           (node.empty() ? std::string() : "\"nodeName\":\"" + node + "\",") +
           (tolerations_json.empty() ? std::string() : "\"tolerations\":" + tolerations_json + ",") +
           "\"containers\":[{\"name\":\"c\",\"image\":\"wasm://embedded/echo:v1\"}]}}";
}

void test_default_tolerations_on_create() {
    std::cout << "[RUN] test_default_tolerations_on_create" << std::endl;
    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);
    store->put("/registry/nodes/node-a", ready_node_json("node-a", "devkit"), true);
    auto sched = kube32::scheduler::create_scheduler(*store);
    RouteHandler api(*store, sched.get());
    const std::string pods = "/api/v1/namespaces/default/pods";

    // No tolerations: not-ready and unreachable NoExecute for 300 s each
    HttpResponse created = api_call(api, HttpMethod::Post, pods, pod_manifest("plain", "node-a"));
    assert(created.status_code == 201);
    assert(created.body.find("\"tolerationSeconds\":300") != std::string::npos);
    JsonValue pod = stored_pod(*store, "plain");
    assert(toleration_count(pod) == 2);
    assert_toleration(pod, kNotReadyKey, 300);
    assert_toleration(pod, kUnreachableKey, 300);

    // The pod's own toleration of a taint is kept; only the other one is added
    assert(api_call(api, HttpMethod::Post, pods, pod_manifest("own", "node-a",
        "[{\"key\":\"node.kubernetes.io/unreachable\",\"operator\":\"Exists\",\"effect\":\"NoExecute\"}]")).status_code == 201);
    pod = stored_pod(*store, "own");
    assert(toleration_count(pod) == 2);
    assert_toleration(pod, kUnreachableKey, -1);
    assert_toleration(pod, kNotReadyKey, 300);

    // An empty effect covers NoExecute; a NoSchedule toleration does not
    assert(api_call(api, HttpMethod::Post, pods, pod_manifest("effects", "node-a",
        "[{\"key\":\"node.kubernetes.io/unreachable\",\"operator\":\"Exists\"},"
        "{\"key\":\"node.kubernetes.io/not-ready\",\"operator\":\"Exists\",\"effect\":\"NoSchedule\"}]")).status_code == 201);
    pod = stored_pod(*store, "effects");
    assert(toleration_count(pod) == 3);
    assert(!toleration_for(pod, kUnreachableKey));
    assert_toleration(pod, kNotReadyKey, 300);

    // An empty key with Exists tolerates every taint: nothing is added
    assert(api_call(api, HttpMethod::Post, pods, pod_manifest("all", "node-a", "[{\"operator\":\"Exists\"}]")).status_code == 201);
    assert(toleration_count(stored_pod(*store, "all")) == 1);

    // Admission runs on updates as well: a replaced spec gets them again
    assert(api_call(api, HttpMethod::Put, pods + "/plain", pod_manifest("plain", "node-a")).status_code == 200);
    pod = stored_pod(*store, "plain");
    assert(toleration_count(pod) == 2);
    assert_toleration(pod, kUnreachableKey, 300);

    // Pods the controllers create get the same; DaemonSet pods tolerate both
    // taints without a limit, also over a toleration with seconds in the template
    const std::string tmpl_spec = "\"containers\":[{\"name\":\"c\",\"image\":\"wasm://embedded/echo:v1\",\"resources\":{\"requests\":{\"memory\":\"4Ki\"}}}]";
    assert(api_call(api, HttpMethod::Post, "/apis/apps/v1/namespaces/default/deployments",
        "{\"apiVersion\":\"apps/v1\",\"kind\":\"Deployment\",\"metadata\":{\"name\":\"web\"},\"spec\":{\"replicas\":1,"
        "\"selector\":{\"matchLabels\":{\"app\":\"web\"}},\"template\":{\"metadata\":{\"labels\":{\"app\":\"web\"}},\"spec\":{" + tmpl_spec + "}}}}").status_code == 201);
    assert(api_call(api, HttpMethod::Post, "/apis/batch/v1/namespaces/default/jobs",
        "{\"apiVersion\":\"batch/v1\",\"kind\":\"Job\",\"metadata\":{\"name\":\"once\"},\"spec\":{\"template\":{\"spec\":{"
        "\"restartPolicy\":\"Never\"," + tmpl_spec + "}}}}").status_code == 201);
    assert(api_call(api, HttpMethod::Post, "/apis/apps/v1/namespaces/default/daemonsets",
        "{\"apiVersion\":\"apps/v1\",\"kind\":\"DaemonSet\",\"metadata\":{\"name\":\"agent\"},\"spec\":{\"template\":{"
        "\"metadata\":{\"labels\":{\"app\":\"agent\"}},\"spec\":{\"tolerations\":[{\"key\":\"node.kubernetes.io/unreachable\","
        "\"operator\":\"Exists\",\"effect\":\"NoExecute\",\"tolerationSeconds\":60}]," + tmpl_spec + "}}}}").status_code == 201);
    DeploymentController dc(*store, sched.get());
    dc.reconcile();
    WorkloadController wc(*store, sched.get());
    wc.reconcile();
    std::vector<std::string> all;
    store->list("/registry/pods/", all);
    size_t checked = 0;
    for (const auto& j : all) {
        JsonValue p;
        std::string err;
        assert(parse_json(j, p, err));
        if (has_owner(p, "Deployment") || has_owner(p, "Job")) {
            assert(toleration_count(p) == 2);
            assert_toleration(p, kNotReadyKey, 300);
            assert_toleration(p, kUnreachableKey, 300);
            ++checked;
        } else if (has_owner(p, "DaemonSet")) {
            assert(toleration_count(p) == 2);
            assert_toleration(p, kNotReadyKey, -1);
            assert_toleration(p, kUnreachableKey, -1);
            ++checked;
        }
    }
    assert(checked == 3);
    std::cout << "[PASS] test_default_tolerations_on_create" << std::endl;
}

// A Ready node "edge" whose kubelet last renewed its 15 s lease at kRenewed,
// pods bound to it through the API, and a lifecycle controller that adds the
// NoExecute taint 30 s after the last renewal. at() moves the controller's
// monotonic and wall clocks together and runs a pass.
struct LostNode {
    static constexpr uint64_t kRenewed = 100000;
    static constexpr uint64_t kWallAtRenewal = 1790000000000ULL;   // 2026-09-21
    std::unique_ptr<Store> store;
    std::unique_ptr<RouteHandler> api;
    std::unique_ptr<NodeLifecycleController> controller;
    uint64_t wall{kWallAtRenewal};

    LostNode() {
        StoreOptions opts;
        opts.enable_flash_persistence = false;
        store = create_radix_store(opts);
        store->put("/registry/nodes/edge", ready_node_json("edge", "cardputer"), true);
        api = std::make_unique<RouteHandler>(*store);
        controller = make_controller();
        put_lease(*store, "edge", "2026-09-21T00:00:00.000000Z");
        at(0);
    }
    std::unique_ptr<NodeLifecycleController> make_controller() {
        NodeLifecycleConfig cfg;
        cfg.not_ready_threshold_ms = 15000;
        cfg.eviction_threshold_ms = 30000;
        cfg.unix_time_ms = [this] { return wall; };
        return std::make_unique<NodeLifecycleController>(*store, cfg);
    }
    // `since_renewal_ms` after the last renewal
    void at(uint64_t since_renewal_ms) {
        wall = kWallAtRenewal + since_renewal_ms;
        controller->reconcile_nodes(kRenewed + since_renewal_ms);
    }
    void pod(const std::string& name, const std::string& tolerations_json = "") {
        assert(api_call(*api, HttpMethod::Post, "/api/v1/namespaces/default/pods", pod_manifest(name, "edge", tolerations_json)).status_code == 201);
    }
    bool bound(const std::string& name) {
        return stored_pod(*store, name).get("spec")->get_string("nodeName", "") == "edge";
    }
    bool evicted(const std::string& name) {
        JsonValue p = stored_pod(*store, name);
        return p.get("spec")->get_string("nodeName", "").empty() && p.get("status")->get_string("reason", "") == "Evicted";
    }
    const JsonValue* no_execute_taint(JsonValue& node) {
        std::string json, err;
        assert(store->get("/registry/nodes/edge", json).ok() && parse_json(json, node, err));
        const JsonValue* taints = node.get("spec")->get("taints");
        if (!taints) return nullptr;
        for (const auto& t : taints->arr_val) {
            if (t.get_string("key", "") == kUnreachableKey && t.get_string("effect", "") == "NoExecute") return &t;
        }
        return nullptr;
    }
};

static std::string unreachable_toleration(int64_t seconds) {
    return "[{\"key\":\"node.kubernetes.io/unreachable\",\"operator\":\"Exists\",\"effect\":\"NoExecute\"" +
           (seconds < 0 ? std::string() : ",\"tolerationSeconds\":" + std::to_string(seconds)) + "}]";
}

void test_no_eviction_at_32s() {
    std::cout << "[RUN] test_no_eviction_at_32s" << std::endl;
    LostNode n;
    n.pod("web");   // default tolerations: 300 s
    n.at(15000);    // lease expired: NotReady and NoSchedule
    n.at(30000);    // NoExecute
    n.at(32000);
    JsonValue node;
    const JsonValue* taint = n.no_execute_taint(node);
    assert(taint);
    assert(taint->get_string("timeAdded", "") == format_rfc3339(LostNode::kWallAtRenewal + 30000));
    assert(n.bound("web"));
    std::string p = stored_pod(*n.store, "web").to_string();
    assert(p.find("Evicted") == std::string::npos && p.find("DisruptionTarget") == std::string::npos);
    std::cout << "[PASS] test_no_eviction_at_32s" << std::endl;
}

void test_eviction_after_toleration_seconds() {
    std::cout << "[RUN] test_eviction_after_toleration_seconds" << std::endl;
    LostNode n;
    n.pod("web");                                // 300 s by default
    n.pod("short", unreachable_toleration(60));
    n.at(15000);
    n.at(30000);                                 // the taint goes on here
    n.at(30000 + 59000);
    assert(n.bound("short") && n.bound("web"));
    n.at(30000 + 60000);
    assert(n.evicted("short"));
    assert(n.bound("web"));
    n.at(30000 + 299000);
    assert(n.bound("web"));
    n.at(30000 + 300000);
    assert(n.evicted("web"));
    JsonValue p = stored_pod(*n.store, "web");
    assert(p.get("status")->get_string("phase", "") == "Pending");
    assert(p.to_string().find("\"type\":\"DisruptionTarget\"") != std::string::npos);
    std::cout << "[PASS] test_eviction_after_toleration_seconds" << std::endl;
}

void test_zero_toleration_seconds_evicts_at_once() {
    std::cout << "[RUN] test_zero_toleration_seconds_evicts_at_once" << std::endl;
    LostNode n;
    n.pod("zero", unreachable_toleration(0));
    n.pod("web");
    // A pod stored without tolerations (not admitted by this API server) does not tolerate the taint
    n.store->put("/registry/pods/default/bare", R"({"kind":"Pod","metadata":{"name":"bare","namespace":"default"},
        "spec":{"nodeName":"edge","containers":[{"name":"c","image":"wasm://x"}]},"status":{"phase":"Running"}})", true);
    n.at(15000);
    n.at(29000);
    assert(n.bound("zero") && n.bound("bare"));   // NotReady alone evicts nothing
    n.at(30000);
    assert(n.evicted("zero"));
    assert(n.evicted("bare"));
    assert(n.bound("web"));
    std::cout << "[PASS] test_zero_toleration_seconds_evicts_at_once" << std::endl;
}

void test_toleration_without_seconds_never_evicts() {
    std::cout << "[RUN] test_toleration_without_seconds_never_evicts" << std::endl;
    LostNode n;
    n.pod("stays", unreachable_toleration(-1));
    n.pod("wildcard", "[{\"operator\":\"Exists\"}]");
    n.at(15000);
    n.at(30000);
    n.at(30000 + 300000);
    n.at(24ULL * 3600 * 1000);
    assert(n.bound("stays"));
    assert(n.bound("wildcard"));
    std::cout << "[PASS] test_toleration_without_seconds_never_evicts" << std::endl;
}

void test_recovery_before_expiry_keeps_pods() {
    std::cout << "[RUN] test_recovery_before_expiry_keeps_pods" << std::endl;
    LostNode n;
    n.pod("web");
    n.pod("short", unreachable_toleration(60));
    n.at(15000);
    n.at(30000);
    n.at(30000 + 45000);

    // The kubelet renews again: both taints go, the pods stay bound
    put_lease(*n.store, "edge", "2026-09-21T00:01:15.000000Z");
    n.at(30000 + 50000);
    JsonValue node;
    assert(!n.no_execute_taint(node));
    assert(node.to_string().find(kUnreachableKey) == std::string::npos);
    n.at(30000 + 120000);   // past the 60 s the taint would have allowed
    assert(n.bound("web") && n.bound("short"));

    // A later outage runs the tolerations from the new taint, not the old one
    const uint64_t renewed = 30000 + 120000;
    put_lease(*n.store, "edge", "2026-09-21T00:02:30.000000Z");
    n.at(renewed);
    n.at(renewed + 15000);
    n.at(renewed + 30000);
    const JsonValue* taint = n.no_execute_taint(node);
    assert(taint && taint->get_string("timeAdded", "") == format_rfc3339(LostNode::kWallAtRenewal + renewed + 30000));
    n.at(renewed + 30000 + 59000);
    assert(n.bound("short"));
    n.at(renewed + 30000 + 60000);
    assert(n.evicted("short"));
    assert(n.bound("web"));
    std::cout << "[PASS] test_recovery_before_expiry_keeps_pods" << std::endl;
}

// The taint's timeAdded carries the tolerations over a control-plane restart;
// a taint added while the wall clock was unset gets one once it is set.
void test_no_execute_time_added_over_restart() {
    std::cout << "[RUN] test_no_execute_time_added_over_restart" << std::endl;
    LostNode n;
    n.pod("web");
    n.at(15000);
    n.at(30000);
    n.at(30000 + 200000);
    assert(n.bound("web"));

    // A new controller (leases are gone with the old one) sees the taint 200 s old
    n.controller = n.make_controller();
    n.store->remove("/registry/leases/kube-node-lease/edge");
    n.at(30000 + 200000);
    n.at(30000 + 299000);
    assert(n.bound("web"));
    n.at(30000 + 300000);
    assert(n.evicted("web"));

    // No wall clock when the taint goes on: no timeAdded until there is one
    LostNode m;
    m.pod("web");
    m.wall = 0;
    m.controller->reconcile_nodes(LostNode::kRenewed + 30000);
    JsonValue node;
    const JsonValue* taint = m.no_execute_taint(node);
    assert(taint && !taint->get("timeAdded"));
    m.at(30000 + 10000);
    taint = m.no_execute_taint(node);
    assert(taint && taint->get_string("timeAdded", "") == format_rfc3339(LostNode::kWallAtRenewal + 30000));
    m.at(30000 + 299000);
    assert(m.bound("web"));
    m.at(30000 + 300000);
    assert(m.evicted("web"));
    std::cout << "[PASS] test_no_execute_time_added_over_restart" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Running Node Lease & Controller Tests  " << std::endl;
    std::cout << "========================================" << std::endl;

    test_kube_lease_serialization();
    test_lease_api_rest_endpoints();
    test_node_lifecycle_thresholds_and_eviction();
    test_node_lifecycle_after_control_plane_restart();
    test_self_healing_rescheduling();
    test_daemonset_controller();
    test_daemonset_rolling_update();
    test_job_controller();
    test_pod_create_four_key_manifest();
    test_default_tolerations_on_create();
    test_no_eviction_at_32s();
    test_eviction_after_toleration_seconds();
    test_zero_toleration_seconds_evicts_at_once();
    test_toleration_without_seconds_never_evicts();
    test_recovery_before_expiry_keeps_pods();
    test_no_execute_time_added_over_restart();

    std::cout << "All Node Lease & Controller tests passed successfully!" << std::endl;
    return 0;
}
