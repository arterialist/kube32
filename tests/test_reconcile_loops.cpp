// When the controllers run a pass (controller/reconcile_trigger.hpp): after a
// change to what they read, at a time they asked for, at the resync, and not
// otherwise. First the trigger on its own, with times given; then the
// Deployment, Device and DaemonSet controllers on their threads: no pass
// while nothing changes (Node heartbeats, Leases and Events included), a
// scaled Deployment gets its pods within one reconcile interval, a deleted
// pod comes back, a resync repairs what a missed change left, and a failed
// driver pod is replaced when its wait ends, with nothing else changing.

#include "api/route_handler.hpp"
#include "controller/deployment_controller.hpp"
#include "controller/device_controller.hpp"
#include "controller/reconcile_trigger.hpp"
#include "controller/workload_controller.hpp"
#include "mem/monotonic_arena.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"
#include "storage/store.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace kube32;

namespace {

std::unique_ptr<storage::Store> ram_store() {
    storage::StoreOptions o;
    o.enable_flash_persistence = false;
    return storage::create_radix_store(o);
}

api::HttpResponse call(api::RouteHandler& h, api::HttpMethod m, const std::string& path, const std::string& body = "",
                       const std::string& content_type = "application/json") {
    mem::MonotonicArena arena;
    api::HttpRequest req;
    req.method = m;
    req.path = path;
    req.body = body;
    req.content_type = content_type;
    return h.handle_request(req, arena);
}

models::JsonValue parse(const std::string& json) {
    models::JsonValue doc;
    std::string err;
    bool ok = models::parse_json(json, doc, err);
    assert(ok);
    return doc;
}

// Waits up to `ms` for `cond`; true when it held
bool eventually(const std::function<bool()>& cond, uint32_t ms = 2000) {
    const uint64_t end = osal::System::get_uptime_ms() + ms;
    while (osal::System::get_uptime_ms() < end) {
        if (cond()) return true;
        osal::Thread::sleep_ms(5);
    }
    return cond();
}

// The store as the controllers see it, with a switch that keeps them from
// hearing of changes: a change they missed
class MutedStore : public storage::Store {
public:
    explicit MutedStore(storage::Store& real) : real_(real) {}
    std::atomic<bool> muted{false};

    storage::Status get(const std::string& k, std::string& o) override { return real_.get(k, o); }
    storage::Status put(const std::string& k, const std::string& j, bool ow) override { return real_.put(k, j, ow); }
    storage::Status remove(const std::string& k) override { return real_.remove(k); }
    storage::Status list(const std::string& p, std::vector<std::string>& o) override { return real_.list(p, o); }
    storage::Status persist() override { return real_.persist(); }
    storage::Status recover() override { return real_.recover(); }
    storage::Status get_extended(const std::string& k, std::string& o, uint64_t* v) override { return real_.get_extended(k, o, v); }
    storage::Status put_extended(const std::string& k, const std::string& j, bool ow, bool p, uint64_t e, uint64_t* v) override {
        return real_.put_extended(k, j, ow, p, e, v);
    }
    storage::Status list_extended(const std::string& p, std::vector<std::string>& o, std::vector<std::string>* k) override {
        return real_.list_extended(p, o, k);
    }
    uint32_t watch(const std::string& prefix, storage::WatchCallback cb) override {
        return real_.watch(prefix, [this, cb](const storage::WatchEvent& ev) {
            if (!muted.load()) cb(ev);
        });
    }
    void unwatch(uint32_t id) override { real_.unwatch(id); }
    uint64_t current_version() const override { return real_.current_version(); }

private:
    storage::Store& real_;
};

std::string node_json(const std::string& name, const std::string& labels, const std::string& message, const std::string& report = "") {
    const std::string ann = report.empty() ? "" : R"(,"annotations":{"kube32.io/i2c-devices":)" +
                                                  models::JsonValue{models::JsonType::String, false, 0, report, {}, {}}.to_string() + "}";
    return R"({"kind":"Node","metadata":{"name":")" + name + R"(","labels":{"kubernetes.io/hostname":")" + name + "\"" + labels + "}" + ann +
           R"(},"status":{"conditions":[{"type":"Ready","status":"True","message":")" + message +
           R"("}],"allocatable":{"memory":"300Ki"},"daemonEndpoints":{"kubeletEndpoint":{"Port":7000}}}})";
}

// The writes a quiet cluster still makes: a Node heartbeat, its Lease, an Event
void background_writes(storage::Store& store, int n) {
    store.put("/registry/nodes/node-a", node_json("node-a", "", "heartbeat " + std::to_string(n)), true);
    store.put("/registry/leases/kube-node-lease/node-a", R"({"kind":"Lease","metadata":{"name":"node-a"},"spec":{"renewTime":")" +
                                                             std::to_string(n) + R"("}})", true);
    store.put("/registry/events/default/e" + std::to_string(n), R"({"kind":"Event","metadata":{"name":"e"}})", true);
}

std::vector<models::JsonValue> pods(storage::Store& store, const std::string& prefix = "/registry/pods/") {
    std::vector<std::string> jsons;
    store.list(prefix, jsons);
    std::vector<models::JsonValue> out;
    for (const auto& j : jsons) out.push_back(parse(j));
    return out;
}

// The node's kubelet as the controllers see it: every pod bound or pinned to it runs
void run_pods(storage::Store& store) {
    std::vector<std::string> jsons, keys;
    store.list_extended("/registry/pods/", jsons, &keys);
    for (size_t i = 0; i < jsons.size(); ++i) {
        models::JsonValue p = parse(jsons[i]);
        const models::JsonValue* st = p.get("status");
        if (st && st->get_string("phase", "") == "Running") continue;
        p.set("status", parse(R"({"phase":"Running","conditions":[{"type":"Ready","status":"True"}]})"));
        store.put(keys[i], p.to_string(), true);
    }
}


// The pass count once a loop has settled: unchanged for half a second (a
// pass a change started may still be due when a test begins to count)
template <typename Loop>
static uint64_t settled_passes(Loop& loop) {
    uint64_t passes = loop.passes();
    for (int i = 0; i < 50; ++i) {
        osal::Thread::sleep_ms(500);
        const uint64_t now = loop.passes();
        if (now == passes) break;
        passes = now;
    }
    return passes;
}

void test_trigger() {
    std::cout << "[RUN] test_trigger" << std::endl;
    auto store = ram_store();
    controller::ReconcileTrigger t(*store, controller::ReconcileTriggerConfig{100, 1000});
    t.watch("/registry/deployments/");
    t.watch_fields("/registry/nodes/", [](const models::JsonValue& n) { return controller::ReconcileTrigger::hash({n.get("metadata")->get("labels")->to_string()}); });
    store->put("/registry/nodes/node-a", node_json("node-a", "", "up"), true);
    using R = controller::ReconcileTrigger;
    assert(t.due(1000) == R::kChange);   // the first pass
    assert(t.due(1050) == R::kNone);

    // A change waits for the shortest interval, then is due once
    store->put("/registry/deployments/default/web", "{}", true);
    assert(t.due(1060) == R::kNone);
    assert(t.due(1100) == R::kChange);
    assert(t.due(1250) == R::kNone);
    // Prefixes not watched, and Node writes that move no field read, are no change
    store->put("/registry/pods/default/p", "{}", true);
    background_writes(*store, 1);
    assert(t.due(1300) == R::kNone);
    // A label, a new Node and a deleted one are
    store->put("/registry/nodes/node-a", node_json("node-a", R"(,"board":"x")", "up"), true);
    assert(t.due(1400) == R::kChange);
    store->put("/registry/nodes/node-b", node_json("node-b", "", "up"), true);
    assert(t.due(1500) == R::kChange);
    store->remove("/registry/nodes/node-b");
    assert(t.due(1600) == R::kChange);
    assert(t.due(1700) == R::kNone);

    // A time asked for; a pass forgets the times asked before it
    t.wake_at(1850);
    t.wake_at(1900);
    assert(t.due(1800) == R::kNone);
    assert(t.due(1850) == R::kTimer);
    assert(t.due(1950) == R::kNone);
    // The resync, a period after the last pass
    assert(t.due(2849) == R::kNone);
    assert(t.due(2850) == R::kResync);

    // A change made while a pass runs makes the next one due
    store->put("/registry/deployments/default/web", "{\"a\":1}", true);
    assert(t.due(3000) == R::kChange);
    store->put("/registry/deployments/default/web", "{\"a\":2}", true);
    assert(t.due(3100) == R::kChange);

    // wait() sleeps until a change comes, or until interrupted
    controller::ReconcileTrigger slow(*store, controller::ReconcileTriggerConfig{100, 60000});
    slow.watch("/registry/deployments/");
    assert(slow.due(1000) == R::kChange);
    std::thread writer([&] {
        osal::Thread::sleep_ms(50);
        store->put("/registry/deployments/default/web", "{\"a\":3}", true);
    });
    const uint64_t t0 = osal::System::get_uptime_ms();
    slow.wait(2000, 10000);
    assert(osal::System::get_uptime_ms() - t0 < 2000);
    writer.join();
    assert(slow.due(2000) == R::kChange);
    slow.interrupt();
    const uint64_t t1 = osal::System::get_uptime_ms();
    slow.wait(2100, 10000);
    assert(osal::System::get_uptime_ms() - t1 < 1000);
    std::cout << "[PASS] test_trigger" << std::endl;
}

void test_deployment_loop() {
    std::cout << "[RUN] test_deployment_loop" << std::endl;
    auto real = ram_store();
    MutedStore store(*real);
    api::RouteHandler h(*real);
    controller::DeploymentControllerConfig cfg;
    cfg.reconcile_interval_ms = 50;
    cfg.resync_interval_ms = 1500;
    controller::DeploymentController dc(store, nullptr, cfg);
    assert(dc.start());
    auto owned = [&] {
        size_t n = 0;
        for (const auto& p : pods(*real)) n += p.get("metadata")->get_string("deletionTimestamp", "").empty() ? 1 : 0;
        return n;
    };

    assert(call(h, api::HttpMethod::Post, "/apis/apps/v1/namespaces/default/deployments",
                R"({"apiVersion":"apps/v1","kind":"Deployment","metadata":{"name":"web"},"spec":{"replicas":1,
                    "selector":{"matchLabels":{"app":"web"}},"template":{"metadata":{"labels":{"app":"web"}},
                    "spec":{"containers":[{"name":"c","image":"wasm://embedded/echo"}]}}}})").status_code == 201);
    assert(eventually([&] { return owned() == 1; }));
    run_pods(*real);
    assert(eventually([&] {
        std::string d;
        real->get("/registry/deployments/default/web", d);
        return parse(d).get("status")->get_int("readyReplicas", 0) == 1;
    }));

    // Quiet: no pass, whatever else the cluster writes (the resync is
    // 1.5 s away: no longer wait to settle)
    osal::Thread::sleep_ms(200);
    const uint64_t quiet = dc.passes();
    const uint64_t revision = real->current_version();
    for (int i = 0; i < 10; ++i) {
        background_writes(*real, i);
        osal::Thread::sleep_ms(30);
    }
    osal::Thread::sleep_ms(200);
    assert(dc.passes() == quiet);
    assert(real->current_version() == revision + 30);   // the background writes, nothing of the controller

    // Scaled: the pods come within one reconcile interval (and a margin for a slow host)
    const uint64_t t0 = osal::System::get_uptime_ms();
    assert(call(h, api::HttpMethod::Patch, "/apis/apps/v1/namespaces/default/deployments/web", R"({"spec":{"replicas":3}})",
                "application/merge-patch+json").status_code == 200);
    assert(eventually([&] { return owned() == 3; }));
    std::cout << "  scaled to 3 in " << osal::System::get_uptime_ms() - t0 << " ms" << std::endl;
    assert(osal::System::get_uptime_ms() - t0 < 1000);

    // A deleted pod comes back
    std::vector<std::string> keys, jsons;
    real->list_extended("/registry/pods/", jsons, &keys);
    real->remove(keys[0]);
    assert(eventually([&] { return owned() == 3; }));

    // A change the controller misses is repaired by the resync
    osal::Thread::sleep_ms(200);
    const uint64_t before = dc.passes();
    store.muted = true;
    jsons.clear();
    keys.clear();
    real->list_extended("/registry/pods/", jsons, &keys);
    real->remove(keys[0]);
    store.muted = false;
    osal::Thread::sleep_ms(100);
    assert(owned() == 2 && dc.passes() == before);
    assert(eventually([&] { return owned() == 3; }, 3000));
    assert(dc.passes() > before);
    dc.stop();
    std::cout << "[PASS] test_deployment_loop" << std::endl;
}

void test_device_loop() {
    std::cout << "[RUN] test_device_loop" << std::endl;
    auto store = ram_store();
    api::RouteHandler h(*store);
    store->put("/registry/nodes/node-a", node_json("node-a", "", "up"), true);
    controller::DeviceControllerConfig cfg;
    cfg.reconcile_interval_ms = 50;
    cfg.resync_interval_ms = 60000;
    cfg.failed_retry_ms = 600;
    controller::DeviceController dc(*store, nullptr, cfg);
    assert(dc.start());
    assert(call(h, api::HttpMethod::Post, "/apis/kube32.io/v1/namespaces/default/devices",
                R"({"metadata":{"name":"temp"},"spec":{"driver":"chip-temp","nodeName":"node-a","activation":"Always"}})").status_code == 201);
    const std::string key = "/registry/pods/default/temp-driver";
    auto uid = [&] {
        std::string json;
        return store->get(key, json).ok() ? parse(json).get("metadata")->get_string("uid", "") : std::string();
    };
    assert(eventually([&] { return !uid().empty(); }));

    // Heartbeats are no change; a label, or a part discovery finds, is.
    // First the passes the Device's creation started end: the count holds
    // still for half a second
    const uint64_t quiet = settled_passes(dc);
    for (int i = 0; i < 10; ++i) {
        background_writes(*store, i);
        osal::Thread::sleep_ms(30);
    }
    osal::Thread::sleep_ms(200);
    assert(dc.passes() == quiet);
    const char* report = R"([{"bus":0,"address":"0x77","chip":"BME688","driver":"bme688"}])";
    store->put("/registry/nodes/node-a", node_json("node-a", "", "up", report), true);
    assert(eventually([&] {
        std::string d;
        return store->get("/registry/devices/default/bme688-node-a-b0-77", d).ok();
    }));

    // A failed driver pod is replaced when its wait is over, with nothing
    // else changing in between
    std::string json;
    store->get(key, json);
    models::JsonValue p = parse(json);
    const std::string failed_uid = p.get("metadata")->get_string("uid", "");
    p.set("status", parse(R"({"phase":"Failed","reason":"ErrImagePull","message":"not present"})"));
    store->put(key, p.to_string(), true);
    osal::Thread::sleep_ms(300);
    assert(uid() == failed_uid);
    assert(eventually([&] { const std::string u = uid(); return !u.empty() && u != failed_uid; }, 3000));
    dc.stop();
    std::cout << "[PASS] test_device_loop" << std::endl;
}

// An activation of an Idle Device makes a pass due at once, not after the
// shortest interval between passes; a use of a running driver makes none
void test_device_activation_loop() {
    std::cout << "[RUN] test_device_activation_loop" << std::endl;
    auto store = ram_store();
    api::RouteHandler h(*store);
    store->put("/registry/nodes/node-a", node_json("node-a", "", "up"), true);
    assert(call(h, api::HttpMethod::Post, "/apis/kube32.io/v1/namespaces/default/devices",
                R"({"metadata":{"name":"temp"},"spec":{"driver":"chip-temp","nodeName":"node-a"}})").status_code == 201);
    controller::DeviceControllerConfig cfg;
    cfg.reconcile_interval_ms = 10000;
    cfg.resync_interval_ms = 60000;
    controller::DeviceController dc(*store, nullptr, cfg);
    assert(dc.start());
    auto phase = [&] {
        std::string json;
        if (!store->get("/registry/devices/default/temp", json).ok()) return std::string();
        const models::JsonValue d = parse(json);
        return d.get("status") ? d.get("status")->get_string("phase", "") : std::string();
    };
    assert(eventually([&] { return phase() == "Idle"; }));
    const std::string key = "/registry/pods/default/temp-driver";
    std::string json;
    assert(!store->get(key, json).ok());
    osal::Thread::sleep_ms(100);
    const uint64_t t0 = osal::System::get_uptime_ms();
    dc.activate("default", "temp");
    assert(eventually([&] { return store->get(key, json).ok(); }, 1500));
    const uint64_t took = osal::System::get_uptime_ms() - t0;
    std::cout << "  driver pod created " << took << " ms after the activation" << std::endl;
    assert(took < 1500);
    // The driver exists: further uses only move the time of the last one
    const uint64_t quiet = settled_passes(dc);
    for (int i = 0; i < 5; ++i) dc.activate("default", "temp");
    osal::Thread::sleep_ms(300);
    assert(dc.passes() == quiet);
    dc.stop();
    std::cout << "[PASS] test_device_activation_loop" << std::endl;
}

void test_daemonset_loop() {
    std::cout << "[RUN] test_daemonset_loop" << std::endl;
    auto store = ram_store();
    api::RouteHandler h(*store);
    store->put("/registry/nodes/node-a", node_json("node-a", R"(,"board":"cardputer")", "up"), true);
    controller::WorkloadControllerConfig cfg;
    cfg.reconcile_interval_ms = 50;
    cfg.resync_interval_ms = 60000;
    controller::WorkloadController wc(*store, nullptr, cfg);
    assert(wc.start());
    assert(call(h, api::HttpMethod::Post, "/apis/apps/v1/namespaces/default/daemonsets",
                R"({"apiVersion":"apps/v1","kind":"DaemonSet","metadata":{"name":"hud"},"spec":{"selector":{"matchLabels":{"app":"hud"}},
                    "template":{"metadata":{"labels":{"app":"hud"}},"spec":{"nodeSelector":{"board":"cardputer"},
                    "containers":[{"name":"c","image":"wasm://embedded/echo"}]}}}})").status_code == 201);
    assert(eventually([&] { return pods(*store).size() == 1; }));
    const uint64_t quiet = settled_passes(wc);
    for (int i = 0; i < 10; ++i) {
        background_writes(*store, i);   // node-a without its label now: a change
        if (i == 0) {
            assert(eventually([&] { return pods(*store).empty(); }));
            osal::Thread::sleep_ms(200);
        }
    }
    assert(wc.passes() > quiet);
    // A node that joins with the label gets its pod; its heartbeats are no change
    store->put("/registry/nodes/node-b", node_json("node-b", R"(,"board":"cardputer")", "up"), true);
    assert(eventually([&] { return pods(*store).size() == 1; }));
    osal::Thread::sleep_ms(200);
    const uint64_t settled = wc.passes();
    for (int i = 0; i < 5; ++i) {
        store->put("/registry/nodes/node-b", node_json("node-b", R"(,"board":"cardputer")", "beat " + std::to_string(i)), true);
        osal::Thread::sleep_ms(30);
    }
    osal::Thread::sleep_ms(200);
    assert(wc.passes() == settled);
    wc.stop();
    std::cout << "[PASS] test_daemonset_loop" << std::endl;
}

} // namespace

int main() {
    test_trigger();
    test_deployment_loop();
    test_device_loop();
    test_device_activation_loop();
    test_daemonset_loop();
    std::cout << "All reconcile loop tests passed" << std::endl;
    return 0;
}
