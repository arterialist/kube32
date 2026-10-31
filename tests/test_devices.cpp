// Devices: the driver catalog and spec checks, the kube32.io/v1 Device
// resource as kubectl uses it (create, defaulting, validation, Table,
// label patches, the status subresource, discovery and OpenAPI entries), and
// the Device controller: driver pods and Services, status, label and spec
// changes, a deleted pod, a deleted Device, Devices from discovery reports
// (names, labels that stay, Absent and back), node placement and failed
// pods.

#include "api/route_handler.hpp"
#include "controller/device_controller.hpp"
#include "mem/monotonic_arena.hpp"
#include "models/device.hpp"
#include "models/kube_models.hpp"
#include "storage/store.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace kube32;
using namespace kube32::api;

namespace {

HttpResponse call(RouteHandler& h, HttpMethod m, const std::string& path, const std::string& body = "",
                  const std::string& content_type = "application/json", bool table = false, const std::string& query = "") {
    mem::MonotonicArena arena;
    HttpRequest req;
    req.method = m;
    req.path = path;
    req.query_string = query;
    req.body = body;
    req.content_type = content_type;
    req.as_table = table;
    return h.handle_request(req, arena);
}

models::JsonValue parse(const std::string& json) {
    models::JsonValue doc;
    std::string err;
    bool ok = models::parse_json(json, doc, err);
    assert(ok);
    return doc;
}

bool spec_ok(const std::string& json, models::DeviceSpec& out, std::string& why) {
    const models::JsonValue doc = parse(json);
    return models::parse_device_spec(&doc, out, why);
}

std::unique_ptr<storage::Store> ram_store() {
    storage::StoreOptions o;
    o.enable_flash_persistence = false;
    return storage::create_radix_store(o);
}

models::JsonValue get(storage::Store& store, const std::string& key) {
    std::string json;
    if (!store.get(key, json).ok()) return models::JsonValue();
    return parse(json);
}

void put(storage::Store& store, const std::string& key, const models::JsonValue& doc) {
    bool ok = store.put(key, doc.to_string(), true).ok();
    assert(ok);
}

const std::string kDevices = "/apis/kube32.io/v1/namespaces/default/devices";

const char* kKitchen = R"({"apiVersion":"kube32.io/v1","kind":"Device","metadata":{"name":"kitchen-env","labels":{"location":"kitchen"}},
    "spec":{"driver":"bme688","nodeName":"node-a","i2c":{"address":119,"mux":{"address":"0x70","channel":2}},"intervalMs":2000,
            "config":{"gas.heater-temp":320}},"status":{"phase":"Running"}})";

std::string node_json(const std::string& name, bool simulator, const std::string& report = "", const std::string& extra_labels = "") {
    std::string labels = R"("kubernetes.io/hostname":")" + name + "\"" + (simulator ? R"(,"kube32.io/simulator":"true")" : "") + extra_labels;
    std::string ann = report.empty() ? "" : R"(,"annotations":{"kube32.io/i2c-devices":)" + models::JsonValue{models::JsonType::String, false, 0, report, {}, {}}.to_string() + "}";
    return R"({"kind":"Node","metadata":{"name":")" + name + R"(","labels":{)" + labels + "}" + ann +
           R"(},"status":{"conditions":[{"type":"Ready","status":"True"}],"daemonEndpoints":{"kubeletEndpoint":{"Port":17000}}}})";
}

void test_catalog_and_spec() {
    size_t n = 0;
    const models::DeviceDriver* drivers = models::device_drivers(n);
    assert(n == 6);
    for (size_t i = 0; i < n; ++i) assert(drivers[i].memory_bytes == 3 * 1024);
    assert(models::find_device_driver("bme688")->address == 0x77 && models::find_device_driver("miniscale")->address == 0x26);
    assert(models::find_device_driver("pca9548") == nullptr);

    models::DeviceSpec s;
    std::string why;
    assert(spec_ok(R"({"driver":"bme688","nodeName":"a"})", s, why));
    assert(s.has_i2c && s.i2c.bus == 0 && s.i2c.address == 0x77 && s.i2c.mux == 0 && s.interval_ms == 5000 && s.image == "wasm://embedded/driver-bme688");
    assert(spec_ok(R"({"driver":"bme688","nodeName":"a","i2c":{"bus":1,"address":"0x76","mux":{"channel":5}}})", s, why));
    assert(s.i2c.bus == 1 && s.i2c.address == 0x76 && s.i2c.mux == 0x70 && s.i2c.channel == 5);
    assert(spec_ok(R"({"driver":"button","nodeSelector":{"kube32.io/board":"cardputer"},"pins":{"pin":0},"config":{"pull":"up","active":"low"}})", s, why));
    assert(!s.has_i2c && s.pins.size() == 1 && s.pins[0].second == 0 && s.config.size() == 2 && s.interval_ms == 50);
    assert(spec_ok(R"({"driver":"chip-temp","nodeName":"a"})", s, why) && s.interval_ms == 10000);

    struct Bad { const char* spec; const char* field; };
    const Bad bad[] = {
        {R"({"nodeName":"a"})", "spec.driver: Required value"},
        {R"({"driver":"ds18b20","nodeName":"a"})", "spec.driver: Unsupported value: \"ds18b20\""},
        {R"({"driver":"bme688"})", "spec.nodeName: Required value"},
        {R"({"driver":"bme688","nodeName":"a","i2c":{"bus":2}})", "spec.i2c.bus"},
        {R"({"driver":"bme688","nodeName":"a","i2c":{"address":"0x07"}})", "spec.i2c.address"},
        {R"({"driver":"bme688","nodeName":"a","i2c":{"address":"77"}})", "spec.i2c.address"},
        {R"({"driver":"bme688","nodeName":"a","i2c":{"mux":{"address":"0x71"}}})", "spec.i2c.mux.channel: Required value"},
        {R"({"driver":"bme688","nodeName":"a","i2c":{"mux":{"address":"0x60","channel":1}}})", "spec.i2c.mux.address"},
        {R"({"driver":"bme688","nodeName":"a","i2c":{"mux":{"channel":8}}})", "spec.i2c.mux.channel"},
        {R"({"driver":"bme688","nodeName":"a","i2c":{"address":"0x70","mux":{"channel":1}}})", "the multiplexer's own address"},
        {R"({"driver":"button","nodeName":"a"})", "spec.pins.pin: Required value"},
        {R"({"driver":"button","nodeName":"a","pins":{"pin":23}})", "spec.pins.pin: Invalid value: 23"},
        {R"({"driver":"button","nodeName":"a","pins":{"pin":4,"led":5}})", "spec.pins.led: Unsupported value"},
        {R"({"driver":"switch","nodeName":"a","pins":{"pin":0}})", "GPIO 0 (the boot button) is only for drivers that read their pins"},
        {R"({"driver":"chip-temp","nodeName":"a","i2c":{"bus":0}})", "spec.i2c: Forbidden"},
        {R"({"driver":"chip-temp","nodeName":"a","intervalMs":10})", "spec.intervalMs"},
        {R"({"driver":"chip-temp","nodeName":"a","config":{"a b":"x"}})", "spec.config.a b"},
        {R"({"driver":"chip-temp","nodeName":"a","image":"http://x"})", "spec.image"},
    };
    for (const auto& b : bad) {
        why.clear();
        assert(!spec_ok(b.spec, s, why));
        if (why.find(b.field) == std::string::npos) std::cout << b.spec << " -> " << why << std::endl;
        assert(why.find(b.field) != std::string::npos);
    }

    models::JsonValue dev = parse(R"({"spec":{"driver":"bme688","nodeName":"a","i2c":{"address":118,"mux":{"address":112,"channel":2}}}})");
    models::default_device(dev);
    const models::JsonValue* i2c = dev.get("spec")->get("i2c");
    assert(i2c->get_int("bus", -1) == 0 && i2c->get_string("address", "") == "0x76" && i2c->get("mux")->get_string("address", "") == "0x70");

    models::I2cLocation loc;
    assert(models::parse_i2c_device_annotation("bus=0,addr=0x77,mux=0x70:2", loc, why) && loc == (models::I2cLocation{0, 0x77, 0x70, 2}));
    assert(models::i2c_device_annotation(loc) == "bus=0,addr=0x77,mux=0x70:2");
    assert(models::i2c_location_text(loc) == "i2c0/0x70:2/0x77");
    assert(!models::parse_i2c_device_annotation("bus=0", loc, why) && !models::parse_i2c_device_annotation("bus=0,addr=0x77,mux=0x70", loc, why));
    assert(!models::parse_i2c_device_annotation("bus=0,addr=0x77,speed=1", loc, why));

    assert(models::discovered_device_name("bme688", "node-a", {0, 0x77, 0x70, 2}) == "bme688-node-a-b0-m70c2-77");
    assert(models::discovered_device_name("rfid", "node-a", {1, 0x28, 0, 0}) == "rfid-node-a-b1-28");
    const std::string long_name = models::discovered_device_name("miniscale", "kube32-cardputer-with-a-really-long-node-name-0123456789", {0, 0x26, 0x70, 3});
    assert(long_name.size() <= 63 && long_name.rfind("miniscale-kube32-cardputer", 0) == 0 && long_name.find("-b0-m70c3-26") != std::string::npos);
    assert(long_name != models::discovered_device_name("miniscale", "kube32-cardputer-with-a-really-long-node-name-0123456780", {0, 0x26, 0x70, 3}));

    std::vector<models::DiscoveredPart> parts = {{{0, 0x70, 0, 0}, "PCA9548A", ""}, {{0, 0x77, 0x70, 2}, "BME688", "bme688"}, {{0, 0x50, 0, 0}, "", ""}};
    const std::string json = models::discovered_parts_json(parts);
    assert(json == R"([{"bus":0,"address":"0x70","chip":"PCA9548A"},{"bus":0,"mux":"0x70","channel":2,"address":"0x77","chip":"BME688","driver":"bme688"},{"bus":0,"address":"0x50"}])");
    std::vector<models::DiscoveredPart> back;
    assert(models::parse_discovered_parts(json, back) && back.size() == 3 && back[1].where == parts[1].where && back[1].driver == "bme688");
    std::cout << "[PASS] test_catalog_and_spec" << std::endl;
}

void test_device_api() {
    auto store = ram_store();
    RouteHandler h(*store);

    // Created with the spec defaulted (an unquoted 0x77 arrives as 119) and the status left to the controller
    HttpResponse r = call(h, HttpMethod::Post, kDevices, kKitchen);
    assert(r.status_code == 201);
    models::JsonValue d = parse(r.body);
    assert(d.get_string("kind", "") == "Device" && d.get_string("apiVersion", "") == "kube32.io/v1");
    assert(d.get("spec")->get("i2c")->get_string("address", "") == "0x77" && d.get("spec")->get("i2c")->get_int("bus", -1) == 0);
    assert(!d.get("status") && !d.get("metadata")->get_string("uid", "").empty() && d.get("metadata")->get_int("generation", 0) == 1);
    assert(call(h, HttpMethod::Post, kDevices, kKitchen).status_code == 409);

    // Invalid specs are refused as kube-apiserver refuses them
    r = call(h, HttpMethod::Post, kDevices, R"({"metadata":{"name":"x"},"spec":{"driver":"ds18b20","nodeName":"a"}})");
    assert(r.status_code == 422 && r.body.find("Device.kube32.io \\\"x\\\" is invalid: spec.driver: Unsupported value") != std::string::npos);
    r = call(h, HttpMethod::Post, kDevices, R"({"metadata":{"name":"x"},"spec":{"driver":"bme688","nodeName":"a","i2c":{"address":"0x80"}}})");
    assert(r.status_code == 422);
    assert(call(h, HttpMethod::Post, kDevices, R"({"metadata":{"name":"x"},"spec":{"driver":"bme688"}})").status_code == 422);
    // Strict field validation, dry run
    assert(call(h, HttpMethod::Post, kDevices, R"({"metadata":{"name":"y"},"spce":{}})", "application/json", false, "fieldValidation=Strict").status_code == 400);
    assert(call(h, HttpMethod::Post, kDevices, R"({"metadata":{"name":"dry"},"spec":{"driver":"chip-temp","nodeName":"a"}})", "application/json", false,
                "dryRun=All").status_code == 201);
    assert(call(h, HttpMethod::Get, kDevices + "/dry").status_code == 404);

    // kubectl get dev: the Table
    r = call(h, HttpMethod::Get, kDevices, "", "application/json", true);
    assert(r.status_code == 200 && r.content_type.find("as=Table") != std::string::npos);
    models::JsonValue t = parse(r.body);
    const auto& cols = t.get("columnDefinitions")->arr_val;
    assert(cols.size() == 9 && cols[0].get_string("name", "") == "NAME" && cols[1].get_string("name", "") == "DRIVER" &&
           cols[2].get_string("name", "") == "NODE" && cols[3].get_string("name", "") == "ADDRESS" && cols[4].get_string("name", "") == "PHASE" &&
           cols[5].get_string("name", "") == "AGE" && cols[6].get_int("priority", 0) == 1);
    const auto& cells = t.get("rows")->arr_val[0].get("cells")->arr_val;
    assert(cells[0].str_val == "kitchen-env" && cells[1].str_val == "bme688" && cells[2].str_val == "node-a" &&
           cells[3].str_val == "i2c0/0x70:2/0x77" && cells[4].str_val == "Pending");
    // All namespaces, a label selector, one item as a Table
    r = call(h, HttpMethod::Get, "/apis/kube32.io/v1/devices", "", "application/json", false, "labelSelector=location%3Dkitchen");
    assert(r.status_code == 200 && parse(r.body).get("items")->arr_val.size() == 1 && parse(r.body).get_string("kind", "") == "DeviceList");
    assert(call(h, HttpMethod::Get, kDevices + "/kitchen-env", "", "application/json", true).body.find("i2c0/0x70:2/0x77") != std::string::npos);

    // The controller writes status through the status subresource; spec stays
    r = call(h, HttpMethod::Patch, kDevices + "/kitchen-env/status",
             R"({"spec":{"driver":"miniscale"},"metadata":{"labels":{"x":"y"}},"status":{"phase":"Running","chip":"BME688"}})", "application/merge-patch+json");
    assert(r.status_code == 200);
    d = parse(r.body);
    assert(d.get("status")->get_string("phase", "") == "Running" && d.get("spec")->get_string("driver", "") == "bme688");
    assert(!d.get("metadata")->get("labels")->get("x"));
    // kubectl label: a merge patch of the object; a status in it is ignored
    r = call(h, HttpMethod::Patch, kDevices + "/kitchen-env", R"({"metadata":{"labels":{"room":"2"}},"status":{"phase":"Failed"}})",
             "application/merge-patch+json");
    assert(r.status_code == 200);
    d = parse(r.body);
    assert(d.get("metadata")->get("labels")->get_string("room", "") == "2" && d.get("metadata")->get("labels")->get_string("location", "") == "kitchen");
    assert(d.get("status")->get_string("phase", "") == "Running" && d.get("metadata")->get_int("generation", 0) == 1);
    // A strategic merge patch that changes the spec: checked again, generation counts it
    r = call(h, HttpMethod::Patch, kDevices + "/kitchen-env", R"({"spec":{"intervalMs":10000}})", "application/strategic-merge-patch+json");
    assert(r.status_code == 200 && parse(r.body).get("metadata")->get_int("generation", 0) == 2);
    assert(call(h, HttpMethod::Patch, kDevices + "/kitchen-env", R"({"spec":{"i2c":{"bus":7}}})", "application/merge-patch+json").status_code == 422);
    // PUT keeps the stored status
    d = parse(call(h, HttpMethod::Get, kDevices + "/kitchen-env").body);
    d.remove("status");
    r = call(h, HttpMethod::Put, kDevices + "/kitchen-env", d.to_string());
    assert(r.status_code == 200 && parse(r.body).get("status")->get_string("chip", "") == "BME688");

    // Discovery and OpenAPI: kubectl finds devices, dev and devices/status
    r = call(h, HttpMethod::Get, "/apis/kube32.io/v1");
    assert(r.body.find(R"("name":"devices","singularName":"device","namespaced":true,"kind":"Device")") != std::string::npos);
    assert(r.body.find(R"("shortNames":["dev"])") != std::string::npos && r.body.find(R"("name":"devices/status")") != std::string::npos);
    r = call(h, HttpMethod::Get, "/openapi/v3/apis/kube32.io/v1");
    assert(r.body.find("/apis/kube32.io/v1/namespaces/{namespace}/devices/{name}") != std::string::npos);

    assert(call(h, HttpMethod::Delete, kDevices + "/kitchen-env/status").status_code == 405);
    assert(call(h, HttpMethod::Delete, kDevices + "/kitchen-env").status_code == 200);
    assert(call(h, HttpMethod::Get, kDevices + "/kitchen-env").status_code == 404);
    std::cout << "[PASS] test_device_api" << std::endl;
}

// The node's kubelet as far as the controller sees it: binds and runs a pod
void run_pod(storage::Store& store, const std::string& key, const std::string& node, bool ready = true) {
    models::JsonValue p = get(store, key);
    assert(p.is_object());
    p.get_mut("spec")->set("nodeName", models::JsonValue{models::JsonType::String, false, 0, node, {}, {}});
    models::JsonValue st = parse(ready ? R"({"phase":"Running","conditions":[{"type":"Ready","status":"True"}]})"
                                       : R"({"phase":"Pending","containerStatuses":[{"state":{"waiting":{"reason":"InsufficientMemory","message":"needs 9 KiB"}}}]})");
    p.set("status", st);
    put(store, key, p);
}

const models::JsonValue* env_var(const models::JsonValue& pod, const std::string& name) {
    for (const auto& e : pod.get("spec")->get("containers")->arr_val[0].get("env")->arr_val) {
        if (e.get_string("name", "") == name) return &e;
    }
    return nullptr;
}

void test_controller_lifecycle() {
    auto store = ram_store();
    RouteHandler h(*store);
    store->put("/registry/nodes/node-a", node_json("node-a", false), true);
    controller::DeviceController dc(*store);
    assert(call(h, HttpMethod::Post, kDevices, kKitchen).status_code == 201);
    dc.reconcile(1000);

    // The driver pod, as the contract has it
    const std::string pod_key = "/registry/pods/default/kitchen-env-driver";
    models::JsonValue p = get(*store, pod_key);
    assert(p.is_object());
    const models::JsonValue* meta = p.get("metadata");
    const models::JsonValue* labels = meta->get("labels");
    assert(labels->get_string("location", "") == "kitchen" && labels->get_string("kube32.io/device", "") == "kitchen-env" &&
           labels->get_string("kube32.io/driver", "") == "bme688");
    assert(models::has_owner(p, "Device", parse(call(h, HttpMethod::Get, kDevices + "/kitchen-env").body).get("metadata")->get_string("uid", "")));
    const models::JsonValue* ann = meta->get("annotations");
    assert(ann->get_string("kube32.io/tick-interval", "") == "2000ms" && ann->get_string("kube32.io/i2c-device", "") == "bus=0,addr=0x77,mux=0x70:2");
    assert(!ann->get("kube32.io/event-topics"));
    const models::JsonValue* spec = p.get("spec");
    assert(spec->get("nodeSelector")->get_string("kubernetes.io/hostname", "") == "node-a" && spec->get_string("nodeName", "").empty());
    assert(spec->get_string("restartPolicy", "") == "Always");
    const models::JsonValue& c = spec->get("containers")->arr_val[0];
    assert(c.get_string("image", "") == "wasm://embedded/driver-bme688");
    assert(c.get("ports")->arr_val[0].get_string("name", "") == "http" && c.get("ports")->arr_val[0].get_int("containerPort", 0) == 17000);
    assert(c.get("resources")->get("requests")->get_string("memory", "") == "3Ki" && c.get("resources")->get("limits")->get_string("memory", "") == "3Ki");
    assert(c.get("resources")->get("requests")->get_string("hardware.kube32.io/ipc", "") == "1");
    assert(c.get("securityContext")->get("capabilities")->get("add")->arr_val[0].str_val == "CHANNEL_IPC");
    assert(env_var(p, "DEVICE_NAME")->get_string("value", "") == "kitchen-env" && env_var(p, "DEVICE_NAMESPACE")->get_string("value", "") == "default");
    assert(env_var(p, "DEVICE_DRIVER")->get_string("value", "") == "bme688" && env_var(p, "DEVICE_TOPIC")->get_string("value", "") == "device.kitchen-env");
    assert(env_var(p, "DEVICE_INTERVAL_MS")->get_string("value", "") == "2000" && env_var(p, "I2C_BUS")->get_string("value", "") == "0");
    assert(env_var(p, "I2C_ADDR")->get_string("value", "") == "0x77" && env_var(p, "CONFIG_GAS_HEATER_TEMP")->get_string("value", "") == "320");
    assert(env_var(p, "NODE_NAME")->get("valueFrom")->get("fieldRef")->get_string("fieldPath", "") == "spec.nodeName");
    const std::string uid = meta->get_string("uid", "");

    // Its Service: port 80 to the driver's http port
    models::JsonValue svc = get(*store, "/registry/services/default/kitchen-env");
    assert(svc.get("spec")->get("selector")->get_string("kube32.io/device", "") == "kitchen-env");
    const models::JsonValue& sp = svc.get("spec")->get("ports")->arr_val[0];
    assert(sp.get_int("port", 0) == 80 && sp.get_string("targetPort", "") == "http" && models::has_owner(svc, "Device"));
    models::JsonValue dev = get(*store, "/registry/devices/default/kitchen-env");
    assert(dev.get("status")->get_string("phase", "") == "Pending" && dev.get("status")->get_string("podName", "") == "kitchen-env-driver");

    // The pod waits for memory, then runs
    run_pod(*store, pod_key, "node-a", false);
    dc.reconcile(2000);
    dev = get(*store, "/registry/devices/default/kitchen-env");
    assert(dev.get("status")->get_string("phase", "") == "Pending" && dev.get("status")->get_string("message", "") == "InsufficientMemory: needs 9 KiB");
    run_pod(*store, pod_key, "node-a");
    dc.reconcile(3000);
    dev = get(*store, "/registry/devices/default/kitchen-env");
    assert(dev.get("status")->get_string("phase", "") == "Running" && dev.get("status")->get_string("nodeName", "") == "node-a");

    // kubectl label: onto the same pod
    assert(call(h, HttpMethod::Patch, kDevices + "/kitchen-env", R"({"metadata":{"labels":{"floor":"1"}}})", "application/merge-patch+json").status_code == 200);
    dc.reconcile(4000);
    p = get(*store, pod_key);
    assert(p.get("metadata")->get_string("uid", "") == uid && p.get("metadata")->get("labels")->get_string("floor", "") == "1");
    assert(p.get("status")->get_string("phase", "") == "Running");

    // A spec change replaces the pod: the bound one terminates first
    assert(call(h, HttpMethod::Patch, kDevices + "/kitchen-env", R"({"spec":{"intervalMs":7000}})", "application/merge-patch+json").status_code == 200);
    dc.reconcile(5000);
    p = get(*store, pod_key);
    assert(!p.get("metadata")->get_string("deletionTimestamp", "").empty() || models::now_rfc3339().empty());
    store->remove(pod_key);   // the kubelet finalized it
    dc.reconcile(6000);
    p = get(*store, pod_key);
    assert(p.get("metadata")->get_string("uid", "") != uid && p.get("metadata")->get("annotations")->get_string("kube32.io/tick-interval", "") == "7000ms");

    // A deleted driver pod comes back
    store->remove(pod_key);
    dc.reconcile(7000);
    assert(get(*store, pod_key).is_object());

    // The Device goes: its pod and its Service go with it
    assert(call(h, HttpMethod::Delete, kDevices + "/kitchen-env").status_code == 200);
    dc.reconcile(8000);
    assert(!get(*store, pod_key).is_object() && !get(*store, "/registry/services/default/kitchen-env").is_object());
    std::cout << "[PASS] test_controller_lifecycle" << std::endl;
}

const char* kReport = R"([{"bus":0,"address":"0x70","chip":"PCA9548A"},{"bus":0,"address":"0x76"},
    {"bus":0,"mux":"0x70","channel":2,"address":"0x77","chip":"BME688","driver":"bme688"},
    {"bus":0,"mux":"0x70","channel":3,"address":"0x26","chip":"MiniScale","driver":"miniscale"},
    {"bus":0,"mux":"0x70","channel":4,"address":"0x28","chip":"WS1850S","driver":"rfid"}])";

void test_discovered_devices() {
    auto store = ram_store();
    RouteHandler h(*store);
    // A hand-written Device already covers the scale
    assert(call(h, HttpMethod::Post, kDevices, R"({"metadata":{"name":"scale"},"spec":{"driver":"miniscale","nodeName":"node-a",
        "i2c":{"address":"0x26","mux":{"address":"0x70","channel":3}}}})").status_code == 201);
    store->put("/registry/nodes/node-a", node_json("node-a", false, kReport), true);
    controller::DeviceController dc(*store);
    dc.reconcile(1000);

    std::vector<std::string> jsons;
    store->list("/registry/devices/", jsons);
    assert(jsons.size() == 3);
    const std::string bme = "/registry/devices/default/bme688-node-a-b0-m70c2-77";
    models::JsonValue d = get(*store, bme);
    assert(d.is_object());
    assert(d.get("metadata")->get("labels")->get_string("kube32.io/discovered", "") == "true");
    assert(d.get("metadata")->get("labels")->get_string("kube32.io/driver", "") == "bme688");
    assert(d.get("spec")->get_string("nodeName", "") == "node-a" && d.get("spec")->get("i2c")->get("mux")->get_int("channel", -1) == 2);
    assert(d.get("status")->get_string("chip", "") == "BME688");
    assert(get(*store, "/registry/devices/default/rfid-node-a-b0-m70c4-28").get("status")->get_string("chip", "") == "WS1850S");
    assert(get(*store, "/registry/devices/default/scale").get("status")->get_string("chip", "") == "MiniScale");
    assert(get(*store, "/registry/pods/default/bme688-node-a-b0-m70c2-77-driver").is_object());
    std::vector<std::string> events;
    store->list("/registry/events/default/", events);
    bool discovered_event = false;
    for (const auto& e : events) discovered_event |= e.find("BME688 found on node node-a at i2c0/0x70:2/0x77") != std::string::npos;
    assert(discovered_event);

    // kubectl label device ...: kept across discovery passes
    assert(call(h, HttpMethod::Patch, kDevices + "/bme688-node-a-b0-m70c2-77", R"({"metadata":{"labels":{"location":"kitchen"}}})",
                "application/merge-patch+json").status_code == 200);
    dc.reconcile(2000);
    dc.reconcile(3000);
    d = get(*store, bme);
    assert(d.get("metadata")->get("labels")->get_string("location", "") == "kitchen");
    assert(get(*store, "/registry/pods/default/bme688-node-a-b0-m70c2-77-driver").get("metadata")->get("labels")->get_string("location", "") == "kitchen");

    // Unplugged: Absent, no driver pod; the Device and its Service stay
    std::string without = kReport;
    without.erase(without.find(R"({"bus":0,"mux":"0x70","channel":2)"), std::string(R"({"bus":0,"mux":"0x70","channel":2,"address":"0x77","chip":"BME688","driver":"bme688"},)").size());
    store->put("/registry/nodes/node-a", node_json("node-a", false, without), true);
    dc.reconcile(4000);
    d = get(*store, bme);
    assert(d.get("status")->get_string("phase", "") == "Absent");
    assert(d.get("status")->get_string("message", "").find("no longer finds the part at i2c0/0x70:2/0x77") != std::string::npos);
    assert(!get(*store, "/registry/pods/default/bme688-node-a-b0-m70c2-77-driver").is_object());   // unbound: removed at once
    assert(get(*store, "/registry/services/default/bme688-node-a-b0-m70c2-77").is_object());
    assert(d.get("metadata")->get("labels")->get_string("location", "") == "kitchen");
    // Plugged back in: a driver again
    store->put("/registry/nodes/node-a", node_json("node-a", false, kReport), true);
    dc.reconcile(5000);
    assert(get(*store, bme).get("status")->get_string("phase", "") == "Pending");
    assert(get(*store, "/registry/pods/default/bme688-node-a-b0-m70c2-77-driver").is_object());
    jsons.clear();
    store->list("/registry/devices/", jsons);
    assert(jsons.size() == 3);

    // A Device written for a discovered part takes the place of the discovered one
    assert(call(h, HttpMethod::Post, kDevices, R"({"metadata":{"name":"reader","labels":{"door":"front"}},"spec":{"driver":"rfid",
        "nodeName":"node-a","i2c":{"address":"0x28","mux":{"address":"0x70","channel":4}}}})").status_code == 201);
    dc.reconcile(5500);
    assert(!get(*store, "/registry/devices/default/rfid-node-a-b0-m70c4-28").is_object());
    assert(!get(*store, "/registry/pods/default/rfid-node-a-b0-m70c4-28-driver").is_object());
    assert(get(*store, "/registry/pods/default/reader-driver").is_object());
    assert(get(*store, "/registry/devices/default/reader").get("status")->get_string("chip", "") == "WS1850S");
    dc.reconcile(5600);
    assert(!get(*store, "/registry/devices/default/rfid-node-a-b0-m70c4-28").is_object());

    // A hand-written Device is never Absent; a node without a report leaves Devices alone
    without = R"([{"bus":0,"address":"0x70","chip":"PCA9548A"}])";
    store->put("/registry/nodes/node-a", node_json("node-a", false, without), true);
    dc.reconcile(6000);
    assert(get(*store, "/registry/devices/default/scale").get("status")->get_string("phase", "") == "Pending");
    store->put("/registry/nodes/node-a", node_json("node-a", false), true);
    dc.reconcile(7000);
    assert(get(*store, bme).get("status")->get_string("phase", "") == "Pending");
    std::cout << "[PASS] test_discovered_devices" << std::endl;
}

void test_placement() {
    auto store = ram_store();
    RouteHandler h(*store);
    store->put("/registry/nodes/laptop", node_json("laptop", true, "", R"(,"kube32.io/board":"cardputer")"), true);
    store->put("/registry/nodes/board", node_json("board", false, "", R"(,"kube32.io/board":"cardputer")"), true);
    store->put("/registry/nodes/sim-only", node_json("sim-only", true, "", R"(,"room":"lab")"), true);
    controller::DeviceController dc(*store);
    assert(call(h, HttpMethod::Post, kDevices, R"({"metadata":{"name":"btn"},"spec":{"driver":"button","nodeSelector":{"kube32.io/board":"cardputer"},
        "pins":{"pin":10},"config":{"pull":"up"}}})").status_code == 201);
    assert(call(h, HttpMethod::Post, kDevices, R"({"metadata":{"name":"lab-temp"},"spec":{"driver":"chip-temp","nodeSelector":{"room":"lab"}}})").status_code == 201);
    assert(call(h, HttpMethod::Post, kDevices, R"({"metadata":{"name":"nowhere"},"spec":{"driver":"chip-temp","nodeSelector":{"room":"attic"}}})").status_code == 201);
    dc.reconcile(1000);

    // A board before a kube32-sim node; a kube32-sim node when no board matches
    models::JsonValue p = get(*store, "/registry/pods/default/btn-driver");
    assert(p.get("spec")->get("nodeSelector")->get_string("kubernetes.io/hostname", "") == "board");
    assert(p.get("spec")->get("containers")->arr_val[0].get("resources")->get("requests")->get_string("esp32.io/gpio-input-10", "") == "1");
    assert(env_var(p, "PIN_PIN")->get_string("value", "") == "10" && env_var(p, "CONFIG_PULL")->get_string("value", "") == "up");
    assert(!p.get("metadata")->get("annotations")->get("kube32.io/i2c-device"));
    assert(get(*store, "/registry/devices/default/btn").get("status")->get_string("nodeName", "") == "board");
    p = get(*store, "/registry/pods/default/lab-temp-driver");
    assert(p.get("spec")->get("nodeSelector")->get_string("kubernetes.io/hostname", "") == "sim-only");
    assert(p.get("spec")->get("containers")->arr_val[0].get("resources")->get("requests")->get_string("hardware.kube32.io/chip-temp", "") == "1");
    assert(p.get("metadata")->get("annotations")->get_string("kube32.io/tick-interval", "") == "10000ms");
    models::JsonValue d = get(*store, "/registry/devices/default/nowhere");
    assert(d.get("status")->get_string("phase", "") == "Pending" && d.get("status")->get_string("message", "") == "no Ready node matches spec.nodeSelector");
    assert(!get(*store, "/registry/pods/default/nowhere-driver").is_object());

    // The chosen node is kept while it matches
    store->put("/registry/nodes/another", node_json("another", false, "", R"(,"kube32.io/board":"cardputer")"), true);
    dc.reconcile(2000);
    assert(get(*store, "/registry/devices/default/btn").get("status")->get_string("nodeName", "") == "board");
    std::cout << "[PASS] test_placement" << std::endl;
}

void test_failed_pod_and_service_conflict() {
    auto store = ram_store();
    RouteHandler h(*store);
    store->put("/registry/nodes/node-a", node_json("node-a", false), true);
    controller::DeviceControllerConfig cfg;
    cfg.failed_retry_ms = 1000;
    cfg.failed_retry_max_ms = 4000;
    controller::DeviceController dc(*store, nullptr, cfg);
    // A Service of the same name made by someone else is left alone
    assert(call(h, HttpMethod::Post, "/api/v1/namespaces/default/services", R"({"metadata":{"name":"temp"},"spec":{"selector":{"app":"x"},"ports":[{"port":80}]}})").status_code == 201);
    assert(call(h, HttpMethod::Post, kDevices, R"({"metadata":{"name":"temp"},"spec":{"driver":"chip-temp","nodeName":"node-a"}})").status_code == 201);
    dc.reconcile(1000);
    assert(get(*store, "/registry/services/default/temp").get("spec")->get("selector")->get_string("app", "") == "x");
    assert(get(*store, "/registry/devices/default/temp").get("status")->get_string("message", "").find("does not belong to the Device") != std::string::npos);

    // ErrImagePull: Failed with the reason, replaced after the wait, then after a longer one
    const std::string key = "/registry/pods/default/temp-driver";
    auto fail = [&]() {
        models::JsonValue p = get(*store, key);
        p.set("status", parse(R"({"phase":"Failed","reason":"ErrImagePull","message":"image wasm://embedded/driver-chip-temp: not present"})"));
        put(*store, key, p);
    };
    fail();
    std::string uid = get(*store, key).get("metadata")->get_string("uid", "");
    dc.reconcile(2000);
    models::JsonValue d = get(*store, "/registry/devices/default/temp");
    assert(d.get("status")->get_string("phase", "") == "Failed" && d.get("status")->get_string("message", "").find("ErrImagePull") == 0);
    dc.reconcile(2500);
    assert(get(*store, key).get("metadata")->get_string("uid", "") == uid);
    dc.reconcile(3000);   // a second after it was seen failing: replaced (unbound: removed at once)
    assert(!get(*store, key).is_object());
    dc.reconcile(3100);
    uid = get(*store, key).get("metadata")->get_string("uid", "");
    fail();
    dc.reconcile(3200);
    dc.reconcile(4300);
    assert(get(*store, key).get("metadata")->get_string("uid", "") == uid);   // now two seconds
    dc.reconcile(5300);
    assert(!get(*store, key).is_object());
    std::cout << "[PASS] test_failed_pod_and_service_conflict" << std::endl;
}

} // namespace

int main() {
    test_catalog_and_spec();
    test_device_api();
    test_controller_lifecycle();
    test_discovered_devices();
    test_placement();
    test_failed_pod_and_service_conflict();
    std::cout << "All device tests passed" << std::endl;
    return 0;
}
