// Checkpoints and moves (controller/checkpoint_controller.hpp,
// docs/migration.md) against a node that plays the kubelet's checkpoint and
// thaw endpoints and the API server's checkpoint data on one port: Save and
// Move with every phase, a bare pod and a Deployment's pod moved with one pod
// counted throughout, rollback after a RestoreError and after the deadline,
// thaw after a failure before the replacement, blockers from the spec and
// from the node, the eviction API, retention, periodic checkpoints, restore
// after node loss, and the Auto policies (memory pressure, placement
// fastest) with their cooldowns on a fake clock.

#include "api/route_handler.hpp"
#include "controller/checkpoint_controller.hpp"
#include "controller/deployment_controller.hpp"
#include "controller/node_lifecycle_controller.hpp"
#include "embedded/embedded_blobs.hpp"
#include "mem/monotonic_arena.hpp"
#include "models/kube_models.hpp"
#include "models/migration.hpp"
#include "osal/socket.hpp"
#include "runtime/image_registry.hpp"
#include "scheduler/scheduler.hpp"
#include "storage/store.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace kube32;
using models::JsonValue;

#define CHECK(c)                                                                                \
    do {                                                                                        \
        if (!(c)) {                                                                             \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #c << std::endl;    \
            std::abort();                                                                       \
        }                                                                                       \
    } while (0)

namespace {

// The kubelet endpoints of every node in the tests (all at 127.0.0.1:port)
// and the API server's checkpoint data, on one listener
struct FakeNode {
    osal::Socket listener;
    uint16_t port{0};
    std::atomic<bool> running{true};
    std::thread th;
    std::mutex mu;
    std::vector<std::string> requests;           // "<METHOD> <path>"
    std::map<std::string, std::string> data;     // checkpoint name -> stored bytes
    int checkpoint_status{200};
    std::string checkpoint_body{"K32S-state-of-the-pod"};
    int put_status{201};

    void start() {
        CHECK(listener.create(osal::SocketType::Tcp));
        listener.set_reuse_addr(true);
        for (uint16_t p = 18200; p < 18300 && !port; ++p) {
            if (listener.bind("127.0.0.1", p)) port = p;
        }
        CHECK(port && listener.listen(8));
        th = std::thread([this] {
            while (running) {
                osal::Socket c = listener.accept();
                if (c.is_valid()) serve(c);
            }
        });
    }
    void stop() {
        running = false;
        listener.shutdown(osal::ShutdownMode::Both);
        listener.close();
        th.join();
    }
    void serve(osal::Socket& s) {
        s.set_timeout(3000, 3000);
        std::string in;
        char buf[2048];
        size_t he;
        while ((he = in.find("\r\n\r\n")) == std::string::npos) {
            const ssize_t n = s.recv(buf, sizeof(buf));
            if (n <= 0) return;
            in.append(buf, static_cast<size_t>(n));
        }
        const std::string head = in.substr(0, he);
        const size_t cl_at = head.find("Content-Length: ");
        const size_t cl = cl_at == std::string::npos ? 0 : std::strtoul(head.c_str() + cl_at + 16, nullptr, 10);
        while (in.size() < he + 4 + cl) {
            const ssize_t n = s.recv(buf, sizeof(buf));
            if (n <= 0) return;
            in.append(buf, static_cast<size_t>(n));
        }
        const std::string body = in.substr(he + 4, cl);
        const std::string method = head.substr(0, head.find(' '));
        std::string path = head.substr(head.find(' ') + 1);
        path = path.substr(0, path.find(' '));
        std::string out;
        {
            std::lock_guard<std::mutex> l(mu);
            requests.push_back(method + " " + path);
            if (path.rfind("/checkpoint/", 0) == 0) {
                if (checkpoint_status == 200) {
                    out = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nX-Kube32-Image-Sha256: " + std::string(64, 'a') +
                          "\r\nX-Kube32-Snapshot-Sha256: " + std::string(64, 'b') + "\r\nX-Kube32-Pod-Time-Ms: 4242\r\nContent-Length: " +
                          std::to_string(checkpoint_body.size()) + "\r\nConnection: close\r\n\r\n" + checkpoint_body;
                } else {
                    const std::string j = R"J({"reason":"NotMovable","blockers":["open file notes.txt (descriptor 3)"]})J";
                    out = "HTTP/1.1 " + std::to_string(checkpoint_status) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
                          std::to_string(j.size()) + "\r\nConnection: close\r\n\r\n" + j;
                }
            } else if (path.rfind("/thaw/", 0) == 0) {
                out = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            } else if (method == "PUT" && path.find("/checkpoints/") != std::string::npos) {
                const std::string name = path.substr(path.find("/checkpoints/") + 13, path.rfind("/data") - path.find("/checkpoints/") - 13);
                if (put_status < 300) data[name] = body;
                out = "HTTP/1.1 " + std::to_string(put_status) + " X\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
            } else {
                out = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            }
        }
        s.send_all(out.data(), out.size());
    }
    std::vector<std::string> take() {
        std::lock_guard<std::mutex> l(mu);
        std::vector<std::string> r;
        r.swap(requests);
        return r;
    }
};

FakeNode* g_node = nullptr;
uint64_t g_wall = 1798000000000ULL;   // the fake wall clock

bool has(const std::vector<std::string>& v, const std::string& prefix) {
    for (const auto& s : v) {
        if (s.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

JsonValue parse(const std::string& json) {
    JsonValue d;
    std::string err;
    CHECK(models::parse_json(json, d, err));
    return d;
}

std::string node_json(const std::string& name, double mops = 0, bool unschedulable = false, bool pressure = false) {
    return R"({"kind":"Node","metadata":{"name":")" + name + R"(","labels":{"kubernetes.io/hostname":")" + name + R"("},"annotations":{"kube32.io/interp-mops":")" +
           std::to_string(mops) + R"("}},"spec":{"unschedulable":)" + (unschedulable ? "true" : "false") +
           R"(},"status":{"conditions":[{"type":"Ready","status":"True"},{"type":"MemoryPressure","status":")" + (pressure ? "True" : "False") +
           R"("}],"allocatable":{"memory":"300Ki"},"addresses":[{"type":"InternalIP","address":"127.0.0.1"}],"daemonEndpoints":{"kubeletEndpoint":{"Port":)" +
           std::to_string(g_node->port) + "}}}}";
}

const std::string kRunning = R"("status":{"phase":"Running","containerStatuses":[{"name":"app","state":{"running":{}}}]})";

std::string pod_json(const std::string& name, const std::string& node, const std::string& annotations = "", const std::string& requests = "\"memory\":\"16Ki\"") {
    return R"({"kind":"Pod","apiVersion":"v1","metadata":{"name":")" + name + R"(","namespace":"default","uid":"uid-)" + name +
           R"(","annotations":{)" + annotations + R"(}},"spec":{"nodeName":")" + node + R"(","containers":[{"name":"app","image":"wasm://embedded/echo","resources":{"requests":{)" +
           requests + "}}}]}," + kRunning + "}";
}

struct Cluster {
    std::unique_ptr<storage::Store> store;
    std::unique_ptr<scheduler::Scheduler> sched;
    std::unique_ptr<controller::CheckpointController> ctl;
    uint64_t now{1000};

    explicit Cluster(std::vector<std::string> nodes) {
        storage::StoreOptions o;
        o.enable_flash_persistence = false;
        store = storage::create_radix_store(o);
        sched = scheduler::create_scheduler(*store);
        for (const auto& n : nodes) store->put("/registry/nodes/" + parse(n).get("metadata")->get_string("name", ""), n, true);
        controller::CheckpointControllerConfig cfg;
        cfg.api_port = g_node->port;
        cfg.freeze_timeout_ms = 1000;
        cfg.unix_time_ms = [] { return g_wall; };
        ctl = std::make_unique<controller::CheckpointController>(*store, sched.get(), cfg);
        g_node->take();
    }
    void pass(uint64_t advance_ms = 1000) {
        now += advance_ms;
        g_wall += advance_ms;
        ctl->reconcile(now);
    }
    void put(const std::string& key, const std::string& json) { CHECK(store->put(key, json, true).ok()); }
    JsonValue get(const std::string& key) {
        std::string json;
        if (!store->get(key, json).ok()) return JsonValue{};
        return parse(json);
    }
    JsonValue pod(const std::string& name) { return get("/registry/pods/default/" + name); }
    JsonValue ck(const std::string& name) { return get(std::string(models::kCheckpointPrefix) + "default/" + name); }
    std::vector<JsonValue> checkpoints() {
        std::vector<std::string> jsons;
        store->list(models::kCheckpointPrefix, jsons);
        std::vector<JsonValue> out;
        for (const auto& j : jsons) out.push_back(parse(j));
        return out;
    }
    std::string phase(const std::string& ck_name) { return ck(ck_name).get("status")->get_string("phase", ""); }
    void checkpoint(const std::string& name, const std::string& pod, const std::string& action, const std::string& target = "") {
        put(std::string(models::kCheckpointPrefix) + "default/" + name, models::checkpoint_json("default", name, pod, action, target, ""));
    }
    // What the kubelet on the pod's node reports
    void report(const std::string& name, const std::string& status) {
        JsonValue p = pod(name);
        p.set("status", parse(status));
        put("/registry/pods/default/" + name, p.to_string());
    }
    void running(const std::string& name) { report(name, R"({"phase":"Running","containerStatuses":[{"name":"app","state":{"running":{}}}]})"); }
    bool event(const std::string& reason, const std::string& text = "") {
        std::vector<std::string> jsons;
        store->list("/registry/events/", jsons);
        for (const auto& j : jsons) {
            const JsonValue e = parse(j);
            if (e.get_string("reason", "") == reason && e.get_string("message", "").find(text) != std::string::npos) return true;
        }
        return false;
    }
    api::HttpResponse call(api::HttpMethod m, const std::string& path, const std::string& body = "") {
        api::RouteHandler h(*store, sched.get());
        mem::MonotonicArena arena;
        api::HttpRequest req;
        req.method = m;
        req.path = path;
        req.body = body;
        req.content_type = "application/json";
        return h.handle_request(req, arena);
    }
    std::string annotation(const JsonValue& o, const char* key) {
        const JsonValue* a = o.get("metadata")->get("annotations");
        return a ? a->get_string(key, "") : "";
    }
};

std::string node_of(const JsonValue& pod) { return pod.get("spec")->get_string("nodeName", ""); }
std::string uid_of(const JsonValue& pod) { return pod.get("metadata")->get_string("uid", ""); }

void test_save() {
    Cluster c({node_json("node-a"), node_json("node-b")});
    c.put("/registry/pods/default/echo", pod_json("echo", "node-a"));
    c.checkpoint("s1", "echo", "Save");
    c.pass();
    const auto reqs = g_node->take();
    CHECK(has(reqs, "POST /checkpoint/default/echo/app?leave=running&timeoutMs=1000"));
    CHECK(has(reqs, "PUT /apis/kube32.io/v1/namespaces/default/checkpoints/s1/data"));
    CHECK(g_node->data["s1"] == g_node->checkpoint_body);
    const JsonValue st = *c.ck("s1").get("status");
    // The data API writes the snapshot's facts (sizeBytes, snapshotSha256, ...)
    CHECK(st.get_string("phase", "") == "Saved");
    CHECK(st.get_string("nodeName", "") == "node-a");
    CHECK(!st.get_string("takenAt", "").empty());
    // The pod is where it was, the same object
    CHECK(node_of(c.pod("echo")) == "node-a" && uid_of(c.pod("echo")) == "uid-echo");
    CHECK(c.event("Checkpointed", "s1"));
    std::cout << "[PASS] Save: snapshot stored through the data subresource, phase Saved with its fields" << std::endl;
}

void test_move_bare_pod() {
    Cluster c({node_json("node-a"), node_json("node-b")});
    c.put("/registry/pods/default/echo", pod_json("echo", "node-a"));
    c.checkpoint("m1", "echo", "Move");
    c.pass();
    const auto reqs = g_node->take();
    CHECK(has(reqs, "POST /checkpoint/default/echo/app?leave=frozen"));
    CHECK(!has(reqs, "POST /thaw/"));
    CHECK(c.phase("m1") == "Moving");
    CHECK(c.ck("m1").get("status")->get_string("targetNode", "") == "node-b");
    CHECK(c.ck("m1").get("status")->get_string("newPodName", "") == "echo");
    // Same name, a new uid, bound to the target, restored from the checkpoint
    JsonValue p = c.pod("echo");
    CHECK(node_of(p) == "node-b");
    CHECK(uid_of(p) != "uid-echo");
    CHECK(c.annotation(p, models::kAnnotationRestoreFrom) == "m1");
    CHECK(p.get("status")->get_string("phase", "") == "Pending");
    CHECK(!p.get("status")->get("containerStatuses"));
    CHECK(c.event("Moving", "node-a to node node-b"));
    // Not running yet: still Moving
    c.pass();
    CHECK(c.phase("m1") == "Moving");
    c.running("echo");
    c.pass();
    CHECK(c.phase("m1") == "Completed");
    p = c.pod("echo");
    CHECK(c.annotation(p, models::kAnnotationRestoreFrom).empty());
    CHECK(node_of(p) == "node-b");
    CHECK(c.event("Moved", "from node node-a to node node-b"));
    std::cout << "[PASS] Move of a bare pod: same name, new uid, restored on the target, Completed" << std::endl;
}

// Deployment pods owned by the Deployment controller: no extra pod and no
// gap at any step of the move
void test_move_deployment_pod_keeps_replicas() {
    Cluster c({node_json("node-a"), node_json("node-b")});
    controller::DeploymentController deployments(*c.store, c.sched.get());
    c.put("/registry/deployments/default/web",
          R"({"apiVersion":"apps/v1","kind":"Deployment","metadata":{"name":"web","namespace":"default","uid":"dep-1","generation":1},)"
          R"("spec":{"replicas":1,"selector":{"matchLabels":{"app":"web"}},"template":{"metadata":{"labels":{"app":"web"}},)"
          R"("spec":{"containers":[{"name":"app","image":"wasm://embedded/echo","resources":{"requests":{"memory":"16Ki"}}}]}}}})");
    deployments.reconcile();
    auto owned = [&]() {
        std::vector<std::string> jsons;
        c.store->list("/registry/pods/default/", jsons);
        std::vector<JsonValue> out;
        for (const auto& j : jsons) {
            JsonValue p = parse(j);
            if (models::has_owner(p, "Deployment")) out.push_back(p);
        }
        return out;
    };
    CHECK(owned().size() == 1);
    const std::string name = owned()[0].get("metadata")->get_string("name", "");
    const std::string first = node_of(owned()[0]);
    const std::string other = first == "node-a" ? "node-b" : "node-a";
    c.running(name);
    const std::string uid = uid_of(c.pod(name));
    auto exact = [&]() {
        deployments.reconcile();
        const auto pods = owned();
        CHECK(pods.size() == 1);
        CHECK(pods[0].get("metadata")->get_string("name", "") == name);
        CHECK(pods[0].get("metadata")->get_string("deletionTimestamp", "").empty());
    };
    // kubectl drain's eviction
    const api::HttpResponse r = c.call(api::HttpMethod::Post, "/api/v1/namespaces/default/pods/" + name + "/eviction",
                                       R"({"apiVersion":"policy/v1","kind":"Eviction","metadata":{"name":")" + name + R"(","namespace":"default"}})");
    CHECK(r.status_code == 201);
    exact();
    c.pass();   // snapshot, replacement
    exact();
    JsonValue p = c.pod(name);
    CHECK(node_of(p) == other);
    CHECK(uid_of(p) != uid);   // what kubectl drain waits for
    c.pass();
    exact();
    c.running(name);
    c.pass();
    exact();
    const auto cks = c.checkpoints();
    CHECK(cks.size() == 1);
    CHECK(cks[0].get("status")->get_string("phase", "") == "Completed");
    CHECK(cks[0].get("metadata")->get("labels")->get_string(models::kLabelTrigger, "") == "eviction");
    std::cout << "[PASS] Move of a Deployment's pod: one pod at every step, restored on the other node" << std::endl;
}

void test_rollback_on_restore_error() {
    Cluster c({node_json("node-a"), node_json("node-b")});
    c.put("/registry/pods/default/echo", pod_json("echo", "node-a"));
    c.checkpoint("m1", "echo", "Move");
    c.pass();
    CHECK(c.phase("m1") == "Moving");
    const std::string moved_uid = uid_of(c.pod("echo"));
    c.report("echo", R"({"phase":"Running","containerStatuses":[{"name":"app","state":{"waiting":{"reason":"RestoreError","message":"snapshot sha256 does not match"}}}]})");
    c.pass();
    CHECK(c.phase("m1") == "Failed");
    CHECK(c.ck("m1").get("status")->get_string("message", "").find("RestoreError: snapshot sha256 does not match") != std::string::npos);
    JsonValue p = c.pod("echo");
    CHECK(node_of(p) == "node-a");
    CHECK(uid_of(p) != moved_uid);
    CHECK(c.annotation(p, models::kAnnotationRestoreFrom) == "m1");
    CHECK(c.event("MoveFailed", "restoring it on node node-a from the same checkpoint"));
    // Back on the source from the same checkpoint
    c.running("echo");
    c.pass();
    CHECK(c.annotation(c.pod("echo"), models::kAnnotationRestoreFrom).empty());
    CHECK(c.event("Restored", "Restored from checkpoint m1"));
    std::cout << "[PASS] RestoreError on the target: restored on the source from the same checkpoint, Failed" << std::endl;
}

void test_rollback_on_deadline() {
    Cluster c({node_json("node-a"), node_json("node-b")});
    c.put("/registry/pods/default/echo", pod_json("echo", "node-a"));
    c.checkpoint("m1", "echo", "Move");
    c.pass();
    CHECK(c.phase("m1") == "Moving");
    c.pass(30000);
    CHECK(c.phase("m1") == "Moving");
    c.pass(31000);
    CHECK(c.phase("m1") == "Failed");
    CHECK(c.ck("m1").get("status")->get_string("message", "").find("not running on node node-b after 60.0 s") != std::string::npos);
    CHECK(node_of(c.pod("echo")) == "node-a");
    std::cout << "[PASS] No Running within the deadline: back on the source" << std::endl;
}

void test_failure_before_replacement_thaws() {
    Cluster c({node_json("node-a"), node_json("node-b")});
    c.put("/registry/pods/default/echo", pod_json("echo", "node-a"));
    g_node->put_status = 507;
    c.checkpoint("m1", "echo", "Move");
    c.pass();
    g_node->put_status = 201;
    const auto reqs = g_node->take();
    CHECK(has(reqs, "POST /thaw/default/echo/app"));
    CHECK(c.phase("m1") == "Failed");
    CHECK(c.ck("m1").get("status")->get_string("message", "").find("HTTP 507") != std::string::npos);
    const JsonValue p = c.pod("echo");
    CHECK(node_of(p) == "node-a" && uid_of(p) == "uid-echo");
    CHECK(c.event("MoveFailed"));
    std::cout << "[PASS] Data not stored: the pod is thawed where it runs, Failed" << std::endl;
}

void test_blockers() {
    Cluster c({node_json("node-a"), node_json("node-b")});
    c.put("/registry/pods/default/drv", pod_json("drv", "node-a", "", R"("memory":"16Ki","esp32.io/gpio-2":"1")"));
    c.checkpoint("b1", "drv", "Move");
    c.pass();
    CHECK(!has(g_node->take(), "POST /checkpoint/"));
    CHECK(c.phase("b1") == "Failed");
    JsonValue held = c.ck("b1");
    const JsonValue* b = held.get("status")->get("blockers");
    CHECK(b && b->arr_val.size() == 1 && b->arr_val[0].str_val == "grant esp32.io/gpio-2");
    // An embedded module with an entry point
    c.put("/registry/pods/default/main", R"({"kind":"Pod","metadata":{"name":"main","namespace":"default","uid":"uid-main"},)"
                                         R"("spec":{"nodeName":"node-a","containers":[{"name":"app","image":"wasm://embedded/i2cscan"}]},)" + kRunning + "}");
    c.checkpoint("b0", "main", "Move");
    c.pass();
    held = c.ck("b0");
    b = held.get("status")->get("blockers");
    CHECK(c.phase("b0") == "Failed" && b && b->arr_val.size() == 1 && b->arr_val[0].str_val.rfind("entry point ", 0) == 0);
    CHECK(!has(g_node->take(), "POST /checkpoint/"));
    // From the node: an open file
    c.put("/registry/pods/default/echo", pod_json("echo", "node-a"));
    g_node->checkpoint_status = 409;
    c.checkpoint("b2", "echo", "Move");
    c.pass();
    g_node->checkpoint_status = 200;
    const auto reqs = g_node->take();
    CHECK(has(reqs, "POST /checkpoint/default/echo/app"));
    CHECK(!has(reqs, "POST /thaw/"));
    CHECK(c.phase("b2") == "Failed");
    held = c.ck("b2");
    b = held.get("status")->get("blockers");
    CHECK(b && b->arr_val.size() == 1 && b->arr_val[0].str_val == "open file notes.txt (descriptor 3)");
    CHECK(node_of(c.pod("echo")) == "node-a");
    // Never: neither moved nor saved
    c.put("/registry/pods/default/keep", pod_json("keep", "node-a", R"("kube32.io/migration":"Never")"));
    c.checkpoint("b3", "keep", "Move");
    c.pass();
    CHECK(c.phase("b3") == "Failed");
    c.checkpoint("b4", "keep", "Save");
    c.pass();
    CHECK(c.phase("b4") == "Failed");
    std::cout << "[PASS] Blockers from the spec and from the node: refused and named, the pod stays" << std::endl;
}

void test_targets() {
    Cluster c({node_json("node-a"), node_json("node-b", 0, true), node_json("node-c")});
    c.put("/registry/pods/default/echo", pod_json("echo", "node-a"));
    c.checkpoint("t1", "echo", "Move", "node-b");   // cordoned
    c.pass();
    CHECK(c.phase("t1") == "Failed");
    CHECK(c.ck("t1").get("status")->get_string("message", "").find("unschedulable") != std::string::npos);
    c.checkpoint("t2", "echo", "Move", "node-a");   // its own node
    c.pass();
    CHECK(c.phase("t2") == "Failed");
    c.checkpoint("t3", "echo", "Move");             // the scheduler picks: never a nor b
    c.pass();
    CHECK(c.phase("t3") == "Moving");
    CHECK(node_of(c.pod("echo")) == "node-c");
    // One node only: none fits
    Cluster one({node_json("node-a")});
    one.put("/registry/pods/default/echo", pod_json("echo", "node-a"));
    one.checkpoint("t4", "echo", "Move");
    one.pass();
    CHECK(one.phase("t4") == "Failed");
    CHECK(one.ck("t4").get("status")->get_string("message", "").rfind("no node fits", 0) == 0);
    std::cout << "[PASS] Targets: never the source, never a cordoned node, none fits on one node" << std::endl;
}

void test_interrupted_saving() {
    Cluster c({node_json("node-a"), node_json("node-b")});
    c.put("/registry/pods/default/echo", pod_json("echo", "node-a"));
    c.checkpoint("m1", "echo", "Move");
    JsonValue ck = c.ck("m1");
    ck.set("status", parse(R"({"phase":"Saving","nodeName":"node-a"})"));
    c.put(std::string(models::kCheckpointPrefix) + "default/m1", ck.to_string());
    c.pass();
    CHECK(has(g_node->take(), "POST /thaw/default/echo/app"));
    CHECK(c.phase("m1") == "Failed");
    std::cout << "[PASS] A snapshot cut short by a restart: thawed, Failed" << std::endl;
}

void test_eviction_api() {
    Cluster c({node_json("node-a"), node_json("node-b")});
    c.put("/registry/pods/default/echo", pod_json("echo", "node-a"));
    c.put("/registry/pods/default/drv", pod_json("drv", "node-a", "", R"("memory":"16Ki","esp32.io/gpio-4":"1")"));
    c.put("/registry/pods/default/keep", pod_json("keep", "node-a", R"("kube32.io/migration":"Never")"));
    CHECK(c.call(api::HttpMethod::Post, "/api/v1/namespaces/default/pods/nope/eviction").status_code == 404);
    CHECK(c.call(api::HttpMethod::Get, "/api/v1/namespaces/default/pods/echo/eviction").status_code == 405);
    // Movable: a Move, once
    CHECK(c.call(api::HttpMethod::Post, "/api/v1/namespaces/default/pods/echo/eviction").status_code == 201);
    CHECK(c.call(api::HttpMethod::Post, "/api/v1/namespaces/default/pods/echo/eviction").status_code == 201);
    CHECK(c.checkpoints().size() == 1);
    CHECK(c.checkpoints()[0].get("spec")->get_string("action", "") == "Move");
    // Not movable: deleted as DELETE does, with the Eviction's grace period
    CHECK(c.call(api::HttpMethod::Post, "/api/v1/namespaces/default/pods/drv/eviction").status_code == 201);
    CHECK(!c.pod("drv").get("metadata")->get_string("deletionTimestamp", "").empty());
    CHECK(c.call(api::HttpMethod::Post, "/api/v1/namespaces/default/pods/keep/eviction",
                 R"({"apiVersion":"policy/v1","kind":"Eviction","deleteOptions":{"gracePeriodSeconds":0}})").status_code == 201);
    CHECK(c.pod("keep").is_null());
    CHECK(c.checkpoints().size() == 1);
    // kubectl drain finds the subresource in discovery
    CHECK(c.call(api::HttpMethod::Get, "/api/v1").body.find(R"("name":"pods/eviction")") != std::string::npos);
    // A move the eviction asked for that the node refuses: evicted after all
    g_node->checkpoint_status = 409;
    c.pass();
    g_node->checkpoint_status = 200;
    CHECK(c.checkpoints()[0].get("status")->get_string("phase", "") == "Failed");
    CHECK(!c.pod("echo").get("metadata")->get_string("deletionTimestamp", "").empty());
    std::cout << "[PASS] Eviction API: a Move for a movable pod, a deletion otherwise, a refused move evicts" << std::endl;
}

void test_retention() {
    Cluster c({node_json("node-a"), node_json("node-b")});
    c.put("/registry/pods/default/echo", pod_json("echo", "node-a"));
    c.checkpoint("s1", "echo", "Save");
    c.pass();
    CHECK(c.phase("s1") == "Saved");
    c.pass(59 * 60000);
    CHECK(!c.ck("s1").is_null());
    c.pass(2 * 60000);
    CHECK(c.ck("s1").is_null());
    // Failed: kept an hour too
    c.checkpoint("f1", "nope", "Save");
    c.pass();
    CHECK(c.phase("f1") == "Failed");
    c.pass(61 * 60000);
    CHECK(c.ck("f1").is_null());
    // Not while a pod is to be restored from it
    c.checkpoint("s2", "echo", "Save");
    c.pass();
    JsonValue p = c.pod("echo");
    p.get_mut("metadata")->get_mut("annotations")->set(models::kAnnotationRestoreFrom, JsonValue{models::JsonType::String, false, 0, "s2", {}, {}});
    p.get_mut("status")->set("phase", JsonValue{models::JsonType::String, false, 0, "Pending", {}, {}});
    c.put("/registry/pods/default/echo", p.to_string());
    c.pass(2 * 3600000);
    CHECK(!c.ck("s2").is_null());
    std::cout << "[PASS] Retention: finished checkpoints kept an hour, one a pod restores from kept" << std::endl;
}

void test_periodic() {
    Cluster c({node_json("node-a"), node_json("node-b")});
    c.put("/registry/pods/default/echo", pod_json("echo", "node-a", R"("kube32.io/checkpoint-interval":"30s")"));
    c.put("/registry/pods/default/fast", pod_json("fast", "node-a", R"("kube32.io/checkpoint-interval":"2s")"));
    c.put("/registry/pods/default/never", pod_json("never", "node-a", R"("kube32.io/checkpoint-interval":"10s","kube32.io/migration":"Never")"));
    auto periodic = [&](const std::string& pod, const std::string& phase) {
        int n = 0;
        for (const auto& ck : c.checkpoints()) {
            if (ck.get("spec")->get_string("podName", "") == pod && ck.get("status")->get_string("phase", "") == phase) ++n;
        }
        return n;
    };
    c.pass();                    // asks for one of echo and of fast
    CHECK(periodic("echo", "Pending") == 1 && periodic("fast", "Pending") == 1 && periodic("never", "Pending") == 0);
    c.pass();                    // takes one
    c.pass();                    // and the other
    CHECK(periodic("echo", "Saved") == 1 && periodic("fast", "Saved") == 1);
    c.pass(5000);                // fast: 2 s is raised to 10 s
    CHECK(periodic("fast", "Pending") == 0);
    c.pass(5000);
    CHECK(periodic("fast", "Pending") == 1);
    c.pass();
    c.pass();                    // the older periodic one of fast goes
    CHECK(periodic("fast", "Saved") == 1);
    CHECK(periodic("echo", "Pending") == 0);
    c.pass(20000);
    CHECK(periodic("echo", "Pending") == 1);
    for (const auto& ck : c.checkpoints()) CHECK(ck.get("metadata")->get("labels")->get_string(models::kLabelTrigger, "") == "periodic");
    std::cout << "[PASS] Periodic checkpoints at the pod's interval (at least 10 s), the latest kept" << std::endl;
}

void test_restore_after_node_loss() {
    Cluster c({node_json("node-a"), node_json("node-b")});
    c.put("/registry/pods/default/echo", pod_json("echo", "node-a", R"("kube32.io/checkpoint-interval":"60s")"));
    c.pass();
    c.pass();
    std::string name;
    CHECK(controller::latest_periodic_checkpoint(*c.store, "default", "echo", name));
    // node-a stops renewing its Lease, node-b keeps renewing
    controller::NodeLifecycleConfig nl;
    nl.not_ready_threshold_ms = 1000;
    nl.eviction_threshold_ms = 2000;
    nl.unix_time_ms = [] { return g_wall; };
    controller::NodeLifecycleController lifecycle(*c.store, nl);
    for (uint64_t t = 1000; t <= 5000; t += 500) {
        c.put("/registry/leases/kube-node-lease/node-b", R"({"kind":"Lease","metadata":{"name":"node-b"},"spec":{"renewTime":")" + std::to_string(t) +
                                                            R"(","leaseDurationSeconds":1}})");
        lifecycle.reconcile_nodes(t);
    }
    JsonValue p = c.pod("echo");
    CHECK(node_of(p).empty());
    CHECK(c.annotation(p, models::kAnnotationRestoreFrom) == name);
    CHECK(c.event("Evicted", "restores from checkpoint " + name));
    // The scheduler binds it to node-b, whose kubelet restores it
    p.get_mut("spec")->set("nodeName", JsonValue{models::JsonType::String, false, 0, "node-b", {}, {}});
    c.put("/registry/pods/default/echo", p.to_string());
    c.running("echo");
    c.pass(7000);
    CHECK(c.annotation(c.pod("echo"), models::kAnnotationRestoreFrom).empty());
    CHECK(c.event("Restored", "Restored from checkpoint " + name + ", taken "));
    CHECK(c.event("Restored", "ago on node node-b"));
    std::cout << "[PASS] Node lost: the pod restores on another node from its periodic checkpoint, Restored names its age" << std::endl;
}

void test_restore_failure_starts_afresh() {
    Cluster c({node_json("node-a"), node_json("node-b")});
    c.put("/registry/pods/default/echo", pod_json("echo", "node-b", R"("kube32.io/restore-from":"gone")"));
    // As the kubelet ends it: Failed, terminated with RestoreError, not restarted
    c.report("echo", R"({"phase":"Failed","containerStatuses":[{"name":"app","state":{"terminated":{"reason":"RestoreError","message":"no data"}}}]})");
    c.pass();
    const JsonValue p = c.pod("echo");
    CHECK(c.annotation(p, models::kAnnotationRestoreFrom).empty());
    CHECK(uid_of(p) != "uid-echo" && node_of(p) == "node-b");
    CHECK(p.get("status")->get_string("phase", "") == "Pending");
    CHECK(c.event("MoveFailed", "starts afresh"));
    std::cout << "[PASS] A restore that fails outside a move: the pod is replaced without the annotation and starts afresh" << std::endl;
}

void test_memory_pressure() {
    Cluster c({node_json("node-a", 0, false, true), node_json("node-b")});
    c.put("/registry/pods/default/small", pod_json("small", "node-a", R"("kube32.io/migration":"Auto")", R"("memory":"16Ki")"));
    c.put("/registry/pods/default/big", pod_json("big", "node-a", R"("kube32.io/migration":"Auto")", R"("memory":"32Ki")"));
    c.put("/registry/pods/default/plain", pod_json("plain", "node-a", "", R"("memory":"64Ki")"));
    c.pass();
    auto cks = c.checkpoints();
    CHECK(cks.size() == 1);
    CHECK(cks[0].get("spec")->get_string("podName", "") == "big");
    CHECK(cks[0].get("metadata")->get("labels")->get_string(models::kLabelTrigger, "") == "memory-pressure");
    c.pass();   // the move
    CHECK(node_of(c.pod("big")) == "node-b");
    c.running("big");
    c.pass(10000);   // still under pressure: one move per node per minute
    CHECK(c.checkpoints().size() == 1);
    c.pass(55000);
    cks = c.checkpoints();
    CHECK(cks.size() == 2);
    bool small = false;
    for (const auto& ck : cks) small |= ck.get("spec")->get_string("podName", "") == "small";
    CHECK(small);
    std::cout << "[PASS] Memory pressure: Auto pods only, the biggest first, one move per node per minute" << std::endl;
}

void test_placement_fastest() {
    Cluster c({node_json("node-a", 5), node_json("node-b", 12), node_json("node-c", 8)});
    c.put("/registry/pods/default/calc", pod_json("calc", "node-a", R"("kube32.io/migration":"Auto","kube32.io/placement":"fastest")"));
    c.pass();
    auto cks = c.checkpoints();
    CHECK(cks.size() == 1);
    CHECK(cks[0].get("spec")->get("target")->get_string("nodeName", "") == "node-b");
    CHECK(cks[0].get("metadata")->get("labels")->get_string(models::kLabelTrigger, "") == "placement");
    c.pass();
    CHECK(node_of(c.pod("calc")) == "node-b");
    c.running("calc");
    c.pass();
    // A faster node joins: no second move within ten minutes
    c.put("/registry/nodes/node-d", node_json("node-d", 50));
    c.pass(60000);
    CHECK(c.checkpoints().size() == 1);
    c.pass(10 * 60000);
    CHECK(c.checkpoints().size() == 2);
    c.pass();
    CHECK(node_of(c.pod("calc")) == "node-d");
    c.running("calc");
    c.pass();
    // node-d leaves (drain): back to the fastest node that remains
    c.put("/registry/nodes/node-d", node_json("node-d", 50, true));
    CHECK(c.call(api::HttpMethod::Post, "/api/v1/namespaces/default/pods/calc/eviction").status_code == 201);
    c.pass();
    CHECK(node_of(c.pod("calc")) == "node-b");
    // Not 2x faster: stays
    Cluster d({node_json("node-a", 5), node_json("node-b", 9)});
    d.put("/registry/pods/default/calc", pod_json("calc", "node-a", R"("kube32.io/migration":"Auto","kube32.io/placement":"fastest")"));
    d.pass();
    CHECK(d.checkpoints().empty());
    std::cout << "[PASS] Placement fastest: 2x faster nodes only, one move per ten minutes, back when the node leaves" << std::endl;
}

void test_policy_parsing() {
    CHECK(models::parse_duration_ms("60s") == 60000 && models::parse_duration_ms("2m") == 120000 && models::parse_duration_ms("1h") == 3600000);
    CHECK(models::parse_duration_ms("500ms") == 500 && models::parse_duration_ms("750") == 750 && models::parse_duration_ms("x") == 0);
    CHECK(models::parse_duration_ms("5d") == 0);
    CHECK(models::migration_policy(parse(pod_json("p", "n"))) == models::MigrationPolicy::OnDrain);
    CHECK(models::migration_policy(parse(pod_json("p", "n", R"("kube32.io/migration":"Auto")"))) == models::MigrationPolicy::Auto);
    CHECK(models::migration_policy(parse(pod_json("p", "n", R"("kube32.io/migration":"Never")"))) == models::MigrationPolicy::Never);
    const JsonValue ck = parse(models::checkpoint_json("ns", "c1", "pod", "Move", "node-b", "eviction"));
    CHECK(ck.get("spec")->get("target")->get_string("nodeName", "") == "node-b");
    CHECK(ck.get("metadata")->get("labels")->get_string(models::kLabelTrigger, "") == "eviction");
    CHECK(ck.get("status")->get_string("phase", "") == "Pending");
    std::cout << "[PASS] Durations, policies and Checkpoint objects" << std::endl;
}

// On its own thread: no pass while nothing it reads changes (heartbeats,
// pods without migration annotations), a pass soon after a Checkpoint
void test_event_driven() {
    Cluster c({node_json("node-a"), node_json("node-b")});
    controller::CheckpointControllerConfig cfg;
    cfg.reconcile_interval_ms = 20;
    cfg.resync_interval_ms = 600000;
    cfg.api_port = g_node->port;
    cfg.stack_bytes = 65536;
    controller::CheckpointController ctl(*c.store, c.sched.get(), cfg);
    c.put("/registry/pods/default/echo", pod_json("echo", "node-a"));
    CHECK(ctl.start());
    auto wait_for = [](const std::function<bool()>& cond) {
        for (int i = 0; i < 400 && !cond(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return cond();
    };
    CHECK(wait_for([&] { return ctl.passes() >= 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint64_t idle = ctl.passes();
    for (int i = 0; i < 20; ++i) {
        JsonValue n = parse(node_json("node-a"));
        n.get_mut("status")->set("allocatable", parse(R"({"memory":")" + std::to_string(100 + i) + R"(Ki"})"));
        c.put("/registry/nodes/node-a", n.to_string());
        c.put("/registry/pods/default/other", pod_json("other", "node-b", "", R"("memory":")" + std::to_string(16 + i) + R"(Ki")"));
        c.put("/registry/events/default/e" + std::to_string(i), R"({"kind":"Event","metadata":{"name":"e"}})");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(ctl.passes() == idle);
    c.checkpoint("s1", "echo", "Save");
    CHECK(wait_for([&] { return c.phase("s1") == "Saved"; }));
    ctl.stop();
    std::cout << "[PASS] Event-driven: no pass on heartbeats or unrelated pods, a Checkpoint is taken within a pass" << std::endl;
}

} // namespace

int main() {
    size_t n = 0;
    const runtime::EmbeddedImage* imgs = embedded::images(n);
    runtime::ImageRegistry::instance().add_all(imgs, n);
    FakeNode node;
    node.start();
    g_node = &node;
    test_policy_parsing();
    test_save();
    test_move_bare_pod();
    test_move_deployment_pod_keeps_replicas();
    test_rollback_on_restore_error();
    test_rollback_on_deadline();
    test_failure_before_replacement_thaws();
    test_blockers();
    test_targets();
    test_interrupted_saving();
    test_eviction_api();
    test_retention();
    test_periodic();
    test_restore_after_node_loss();
    test_restore_failure_starts_afresh();
    test_memory_pressure();
    test_placement_fastest();
    test_event_driven();
    node.stop();
    std::cout << "All checkpoint controller tests passed" << std::endl;
    return 0;
}
