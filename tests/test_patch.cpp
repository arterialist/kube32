// JSON merge patch (RFC 7386) and strategic merge patch, as kubectl sends them
#include "models/kube_models.hpp"
#include <cassert>
#include <iostream>
#include <string>

using kube32::models::JsonValue;

static JsonValue parse(const std::string& s) {
    JsonValue v;
    std::string err;
    bool ok = kube32::models::parse_json(s, v, err);
    if (!ok) std::cerr << "bad test JSON: " << err << "\n" << s << std::endl;
    assert(ok);
    return v;
}

static std::string strategic(const std::string& target, const std::string& patch) {
    JsonValue t = parse(target);
    kube32::models::apply_patch(t, parse(patch), "application/strategic-merge-patch+json");
    return t.to_string();
}

static std::string merge(const std::string& target, const std::string& patch) {
    JsonValue t = parse(target);
    kube32::models::apply_patch(t, parse(patch), "application/merge-patch+json");
    return t.to_string();
}

static void expect(const std::string& got, const std::string& want) {
    const std::string w = parse(want).to_string();
    if (got != w) std::cerr << "got  " << got << "\nwant " << w << std::endl;
    assert(got == w);
}

// What kubectl apply sends when one container's resources change: the
// container by its name and the list order. The image and env stay.
static void test_apply_changes_one_container_field() {
    std::cout << "[RUN] test_apply_changes_one_container_field" << std::endl;
    const std::string deploy =
        R"({"spec":{"template":{"spec":{"containers":[{"name":"api","image":"wasm://embedded/tessera-api:v1",)"
        R"("env":[{"name":"A","value":"1"},{"name":"B","value":"2"}],)"
        R"("resources":{"requests":{"memory":"48Ki","esp32.io/psram":"1280Ki"}}}]}}}})";
    const std::string patch =
        R"({"spec":{"template":{"spec":{"$setElementOrder/containers":[{"name":"api"}],)"
        R"("containers":[{"name":"api","resources":{"requests":{"memory":"16Ki"}}}]}}}})";
    expect(strategic(deploy, patch),
           R"({"spec":{"template":{"spec":{"containers":[{"name":"api","image":"wasm://embedded/tessera-api:v1",)"
           R"("env":[{"name":"A","value":"1"},{"name":"B","value":"2"}],)"
           R"("resources":{"requests":{"memory":"16Ki","esp32.io/psram":"1280Ki"}}}]}}}})");
}

static void test_keyed_lists_add_delete_order() {
    std::cout << "[RUN] test_keyed_lists_add_delete_order" << std::endl;
    // env by name: one changes, one is deleted, one is added, order as given
    expect(strategic(R"({"env":[{"name":"A","value":"1"},{"name":"B","value":"2"},{"name":"C","value":"3"}]})",
                     R"({"$setElementOrder/env":[{"name":"D"},{"name":"A"},{"name":"C"}],)"
                     R"("env":[{"name":"A","value":"9"},{"$patch":"delete","name":"B"},{"name":"D","value":"4"}]})"),
           R"({"env":[{"name":"D","value":"4"},{"name":"A","value":"9"},{"name":"C","value":"3"}]})");
    // container ports by containerPort, Service ports by port
    expect(strategic(R"({"ports":[{"containerPort":7000,"name":"http"}]})", R"({"ports":[{"containerPort":7000,"protocol":"TCP"}]})"),
           R"({"ports":[{"containerPort":7000,"name":"http","protocol":"TCP"}]})");
    expect(strategic(R"({"ports":[{"port":80,"targetPort":"http"}]})", R"({"ports":[{"port":81,"targetPort":"http"}]})"),
           R"({"ports":[{"port":80,"targetPort":"http"},{"port":81,"targetPort":"http"}]})");
    // a list without a merge key is replaced; $patch: replace replaces a keyed one
    expect(strategic(R"({"args":["a","b"]})", R"({"args":["c"]})"), R"({"args":["c"]})");
    expect(strategic(R"({"env":[{"name":"A","value":"1"}]})", R"({"env":[{"$patch":"replace"},{"name":"B","value":"2"}]})"),
           R"({"env":[{"name":"B","value":"2"}]})");
    // finalizers merge, and $deleteFromPrimitiveList removes from them
    expect(strategic(R"({"finalizers":["a","b"]})", R"({"finalizers":["c"],"$deleteFromPrimitiveList/finalizers":["a"]})"),
           R"({"finalizers":["b","c"]})");
}

static void test_null_and_directives_on_maps() {
    std::cout << "[RUN] test_null_and_directives_on_maps" << std::endl;
    // null removes a member in both kinds of patch
    expect(merge(R"({"metadata":{"labels":{"a":"1","b":"2"}}})", R"({"metadata":{"labels":{"a":null}}})"),
           R"({"metadata":{"labels":{"b":"2"}}})");
    expect(strategic(R"({"metadata":{"labels":{"a":"1","b":"2"}}})", R"({"metadata":{"labels":{"a":null}}})"),
           R"({"metadata":{"labels":{"b":"2"}}})");
    // JSON merge patch replaces lists whole
    expect(merge(R"({"containers":[{"name":"api","image":"x"}]})", R"({"containers":[{"name":"api"}]})"),
           R"({"containers":[{"name":"api"}]})");
    // $patch: replace on a map, $retainKeys
    expect(strategic(R"({"strategy":{"type":"RollingUpdate","rollingUpdate":{"maxSurge":1}}})",
                     R"({"strategy":{"$retainKeys":["type"],"type":"Recreate"}})"),
           R"({"strategy":{"type":"Recreate"}})");
    expect(strategic(R"({"selector":{"a":"1"}})", R"({"selector":{"$patch":"replace","b":"2"}})"), R"({"selector":{"b":"2"}})");
}

// A kubelet heartbeat (strategic merge) over a Node whose conditions two
// writers set: settled times as Kubernetes defines them
static std::string heartbeat(const std::string& node, const std::string& patch, uint64_t now_ms) {
    JsonValue n = parse(node);
    const JsonValue* stored = kube32::models::node_conditions(n);
    const JsonValue before = stored ? *stored : JsonValue{};
    const JsonValue p = parse(patch);
    kube32::models::apply_patch(n, p, "application/strategic-merge-patch+json");
    kube32::models::settle_node_condition_times(n, stored ? &before : nullptr, kube32::models::node_conditions(p), now_ms);
    return n.to_string();
}

static void test_node_condition_times() {
    std::cout << "[RUN] test_node_condition_times" << std::endl;
    const uint64_t now = 1793620800000ULL;   // 2026-11-02T12:00:00Z
    // The node lifecycle controller set Ready Unknown at 11:50; the kubelet
    // is back and reports Ready True, which it has been since 10:00 by its
    // own count: the condition on the Node changes now. FirmwareUpdate,
    // which the patch does not carry, keeps its entry.
    const std::string unknown = R"({"status":{"conditions":[
        {"type":"Ready","status":"Unknown","reason":"NodeStatusUnknown","lastHeartbeatTime":"2026-11-02T11:49:40Z","lastTransitionTime":"2026-11-02T11:50:00Z"},
        {"type":"FirmwareUpdate","status":"False","reason":"Idle","lastTransitionTime":"2026-11-01T08:00:00Z"}]}})";
    expect(heartbeat(unknown, R"({"status":{"conditions":[
        {"type":"Ready","status":"True","reason":"KubeletReady","lastHeartbeatTime":"2026-11-02T12:00:00Z","lastTransitionTime":"2026-11-02T10:00:00Z"}]}})", now),
           R"({"status":{"conditions":[
        {"type":"Ready","status":"True","reason":"KubeletReady","lastHeartbeatTime":"2026-11-02T12:00:00Z","lastTransitionTime":"2026-11-02T12:00:00Z"},
        {"type":"FirmwareUpdate","status":"False","reason":"Idle","lastTransitionTime":"2026-11-01T08:00:00Z"}]}})");

    // The same status again (a rebooted kubelet counts from its boot): the
    // stored transition stays
    const std::string ready = R"({"status":{"conditions":[
        {"type":"Ready","status":"True","reason":"KubeletReady","lastHeartbeatTime":"2026-11-02T11:59:55Z","lastTransitionTime":"2026-11-01T09:00:00Z"}]}})";
    expect(heartbeat(ready, R"({"status":{"conditions":[
        {"type":"Ready","status":"True","reason":"KubeletReady","lastHeartbeatTime":"2026-11-02T12:00:00Z","lastTransitionTime":"2026-11-02T11:58:00Z"}]}})", now),
           R"({"status":{"conditions":[
        {"type":"Ready","status":"True","reason":"KubeletReady","lastHeartbeatTime":"2026-11-02T12:00:00Z","lastTransitionTime":"2026-11-01T09:00:00Z"}]}})");

    // A change the kubelet saw and timed after the stored one keeps its time
    expect(heartbeat(ready, R"({"status":{"conditions":[
        {"type":"Ready","status":"False","reason":"KubeletNotReady","lastHeartbeatTime":"2026-11-02T12:00:00Z","lastTransitionTime":"2026-11-02T11:59:58Z"}]}})", now),
           R"({"status":{"conditions":[
        {"type":"Ready","status":"False","reason":"KubeletNotReady","lastHeartbeatTime":"2026-11-02T12:00:00Z","lastTransitionTime":"2026-11-02T11:59:58Z"}]}})");

    // A kubelet without a wall clock: the API server stamps the write; with
    // no clock there either, the times of the earlier status are dropped
    expect(heartbeat(ready, R"({"status":{"conditions":[{"type":"Ready","status":"False","reason":"KubeletNotReady"}]}})", now),
           R"({"status":{"conditions":[
        {"type":"Ready","status":"False","reason":"KubeletNotReady","lastHeartbeatTime":"2026-11-02T12:00:00Z","lastTransitionTime":"2026-11-02T12:00:00Z"}]}})");
    expect(heartbeat(ready, R"({"status":{"conditions":[{"type":"Ready","status":"False","reason":"KubeletNotReady"}]}})", 0),
           R"({"status":{"conditions":[{"type":"Ready","status":"False","reason":"KubeletNotReady"}]}})");
    expect(heartbeat(ready, R"({"status":{"conditions":[{"type":"Ready","status":"True","reason":"KubeletReady"}]}})", 0),
           R"({"status":{"conditions":[{"type":"Ready","status":"True","reason":"KubeletReady","lastTransitionTime":"2026-11-01T09:00:00Z"}]}})");

    // A first registration: conditions without times get the write's time
    JsonValue created = parse(R"({"status":{"conditions":[{"type":"Ready","status":"False"}]}})");
    const JsonValue sent = *kube32::models::node_conditions(created);
    kube32::models::settle_node_condition_times(created, nullptr, &sent, now);
    expect(created.to_string(), R"({"status":{"conditions":[
        {"type":"Ready","status":"False","lastHeartbeatTime":"2026-11-02T12:00:00Z","lastTransitionTime":"2026-11-02T12:00:00Z"}]}})");
    std::cout << "[PASS] test_node_condition_times" << std::endl;
}

// JSON Patch: the patched document, or the failure with the document left as
// it was
static std::string jpatch(const std::string& target, const std::string& ops, int want_status = 0) {
    JsonValue t = parse(target);
    const std::string before = t.to_string();
    std::string err;
    const int status = kube32::models::apply_patch(t, parse(ops), "application/json-patch+json", &err);
    if (status != want_status) std::cerr << "status " << status << " (" << err << ") for " << ops << std::endl;
    assert(status == want_status);
    if (status != 0) {
        assert(!err.empty());
        if (t.to_string() != before) std::cerr << "after " << t.to_string() << "\nbefore " << before << "\nops " << ops << std::endl;
        assert(t.to_string() == before);
    }
    return t.to_string();
}

static void test_json_patch_operations() {
    std::cout << "[RUN] test_json_patch_operations" << std::endl;
    const std::string doc = R"({"metadata":{"labels":{"app":"web"}},"spec":{"replicas":2,"ports":[80,443]}})";
    // add: a new member, an existing member (replaced), an element, the end of an array
    expect(jpatch(doc, R"([{"op":"add","path":"/metadata/labels/tier","value":"front"}])"),
           R"({"metadata":{"labels":{"app":"web","tier":"front"}},"spec":{"replicas":2,"ports":[80,443]}})");
    expect(jpatch(doc, R"([{"op":"add","path":"/metadata/labels/app","value":"api"}])"),
           R"({"metadata":{"labels":{"app":"api"}},"spec":{"replicas":2,"ports":[80,443]}})");
    expect(jpatch(doc, R"([{"op":"add","path":"/spec/ports/1","value":8080}])"),
           R"({"metadata":{"labels":{"app":"web"}},"spec":{"replicas":2,"ports":[80,8080,443]}})");
    expect(jpatch(doc, R"([{"op":"add","path":"/spec/ports/-","value":8443},{"op":"add","path":"/spec/ports/2","value":9}])"),
           R"({"metadata":{"labels":{"app":"web"}},"spec":{"replicas":2,"ports":[80,443,9,8443]}})");
    // add of an object value, and of the root
    expect(jpatch(doc, R"([{"op":"add","path":"/status","value":{"phase":"Running"}}])"),
           R"({"metadata":{"labels":{"app":"web"}},"spec":{"replicas":2,"ports":[80,443]},"status":{"phase":"Running"}})");
    expect(jpatch(doc, R"([{"op":"add","path":"","value":{"a":1}}])"), R"({"a":1})");
    // remove: a member, an element
    expect(jpatch(doc, R"([{"op":"remove","path":"/metadata/labels/app"},{"op":"remove","path":"/spec/ports/0"}])"),
           R"({"metadata":{"labels":{}},"spec":{"replicas":2,"ports":[443]}})");
    // replace: a member, an element, the root
    expect(jpatch(doc, R"([{"op":"replace","path":"/spec/replicas","value":5},{"op":"replace","path":"/spec/ports/1","value":"x"}])"),
           R"({"metadata":{"labels":{"app":"web"}},"spec":{"replicas":5,"ports":[80,"x"]}})");
    expect(jpatch(doc, R"([{"op":"replace","path":"","value":[1]}])"), "[1]");
    // move and copy
    expect(jpatch(doc, R"([{"op":"move","path":"/spec/replicas2","from":"/spec/replicas"}])"),
           R"({"metadata":{"labels":{"app":"web"}},"spec":{"ports":[80,443],"replicas2":2}})");
    expect(jpatch(doc, R"([{"op":"move","path":"/spec/ports/0","from":"/spec/ports/1"}])"),
           R"({"metadata":{"labels":{"app":"web"}},"spec":{"replicas":2,"ports":[443,80]}})");
    expect(jpatch(doc, R"([{"op":"copy","path":"/metadata/annotations","from":"/metadata/labels"},)"
                       R"({"op":"add","path":"/metadata/annotations/extra","value":"1"}])"),
           R"({"metadata":{"labels":{"app":"web"},"annotations":{"app":"web","extra":"1"}},"spec":{"replicas":2,"ports":[80,443]}})");
    expect(jpatch(doc, R"([{"op":"move","path":"/spec/replicas","from":"/spec/replicas"}])"), doc);
    // test: equal values pass whatever the member order or number spelling
    expect(jpatch(doc, R"([{"op":"test","path":"/spec","value":{"ports":[80,443],"replicas":2.0}},)"
                       R"({"op":"test","path":"/metadata/labels/app","value":"web"}])"), doc);
    jpatch(doc, R"([{"op":"test","path":"/spec","value":{"ports":[443,80],"replicas":2}}])", 422);
    jpatch(doc, R"([{"op":"test","path":"/spec/replicas","value":"2"}])", 422);
    std::cout << "[PASS] test_json_patch_operations" << std::endl;
}

static void test_json_patch_pointers() {
    std::cout << "[RUN] test_json_patch_pointers" << std::endl;
    // ~1 is "/" and ~0 is "~", as in annotation keys; "" is a member name, "-" on an object is a key
    const std::string doc = R"({"metadata":{"annotations":{"a/b":"1","c~d":"2","":"3"}},"list":[]})";
    expect(jpatch(doc, R"([{"op":"replace","path":"/metadata/annotations/a~1b","value":"x"},)"
                       R"({"op":"remove","path":"/metadata/annotations/c~0d"},{"op":"replace","path":"/metadata/annotations/","value":"y"},)"
                       R"({"op":"add","path":"/metadata/annotations/-","value":"z"},{"op":"add","path":"/metadata/annotations/~01","value":"w"}])"),
           R"({"metadata":{"annotations":{"a/b":"x","":"y","-":"z","~1":"w"}},"list":[]})");
    // ~ not followed by 0 or 1, a pointer without a leading slash
    jpatch(doc, R"([{"op":"add","path":"/metadata/annotations/a~2","value":"1"}])", 422);
    jpatch(doc, R"([{"op":"add","path":"/metadata/annotations/a~","value":"1"}])", 422);
    jpatch(doc, R"([{"op":"add","path":"metadata","value":"1"}])", 422);
    // array indices: no leading zero, no sign, no text, in range; "-" only appends
    const std::string arr = R"({"list":[10,20,30]})";
    expect(jpatch(arr, R"([{"op":"add","path":"/list/3","value":40},{"op":"replace","path":"/list/0","value":11}])"), R"({"list":[11,20,30,40]})");
    for (const char* bad : {"/list/01", "/list/+1", "/list/-1", "/list/x", "/list/4", "/list/", "/list/99999999999"}) {
        jpatch(arr, std::string(R"([{"op":"add","path":")") + bad + R"(","value":1}])", 422);
    }
    jpatch(arr, R"([{"op":"remove","path":"/list/3"}])", 422);
    jpatch(arr, R"([{"op":"remove","path":"/list/-"}])", 422);
    jpatch(arr, R"([{"op":"replace","path":"/list/-","value":1}])", 422);
    jpatch(arr, R"([{"op":"test","path":"/list/-","value":1}])", 422);
    // a pointer through a scalar or a missing parent
    jpatch(arr, R"([{"op":"add","path":"/list/0/x","value":1}])", 422);
    jpatch(arr, R"([{"op":"add","path":"/missing/x","value":1}])", 422);
    // a deep path is a loop, not a chain of calls
    std::string ptr, open, close;
    for (int i = 0; i < 28; ++i) {   // the parser's nesting limit is 32
        open += "{\"k\":";
        close += "}";
        ptr += "/k";
    }
    expect(jpatch(open + "0" + close, "[{\"op\":\"replace\",\"path\":\"" + ptr + "\",\"value\":1}]"), open + "1" + close);
    std::cout << "[PASS] test_json_patch_pointers" << std::endl;
}

static void test_json_patch_failures_leave_the_document() {
    std::cout << "[RUN] test_json_patch_failures_leave_the_document" << std::endl;
    const std::string doc = R"({"metadata":{"labels":{"app":"web","x":"1"}},"spec":{"replicas":2,"ports":[80,443]}})";
    // A failure after changes of every kind: the document is whole
    const std::string changes =
        R"({"op":"add","path":"/metadata/labels/new","value":"1"},{"op":"add","path":"/metadata/labels/app","value":"api"},)"
        R"({"op":"remove","path":"/metadata/labels/x"},{"op":"remove","path":"/spec/ports/0"},{"op":"add","path":"/spec/ports/0","value":7},)"
        R"({"op":"replace","path":"/spec/replicas","value":9},{"op":"move","path":"/spec/moved","from":"/spec/ports"},)"
        R"({"op":"copy","path":"/spec/copied","from":"/spec/moved"},{"op":"move","path":"/metadata/labels","from":"/spec/copied"},)"
        R"({"op":"replace","path":"","value":{"a":1}})";
    // the same changes alone do apply
    jpatch(doc, "[" + changes + "]");
    for (const char* failing : {
             R"({"op":"test","path":"/a","value":2})",
             R"({"op":"test","path":"/nope","value":1})",
             R"({"op":"remove","path":"/nope"})",
             R"({"op":"replace","path":"/nope","value":1})",
             R"({"op":"add","path":"/nope/deeper","value":1})",
             R"({"op":"move","from":"/nope","path":"/a"})",
             R"({"op":"move","from":"/a","path":"/nope/x"})",
             R"({"op":"copy","from":"/nope","path":"/a"})",
             R"({"op":"frobnicate","path":"/a"})",
             R"({"path":"/a","value":1})",
             R"({"op":"add","value":1})",
             R"({"op":"add","path":"/a"})",
             R"({"op":"move","path":"/a"})",
             R"(7)"}) {
        jpatch(doc, "[" + changes + "," + failing + "]", 422);
    }
    // A change that does not apply midway: the first operations are undone
    jpatch(doc, R"([{"op":"remove","path":"/spec/ports/0"},{"op":"move","from":"/spec/replicas","path":"/spec/ports/5"}])", 422);
    jpatch(doc, R"([{"op":"move","from":"/spec/ports","path":"/spec/nope/x"}])", 422);
    // A failed test is a 422 that names the path
    JsonValue t = parse(doc);
    std::string err;
    assert(kube32::models::apply_patch(t, parse(R"([{"op":"test","path":"/spec/replicas","value":3}])"), "application/json-patch+json", &err) == 422);
    assert(err.find("/spec/replicas") != std::string::npos && err.find("test failed") != std::string::npos);
    // A test on a missing path fails; a test of null on a null member passes
    jpatch(doc, R"([{"op":"test","path":"/spec/nothing","value":null}])", 422);
    expect(jpatch(R"({"a":null})", R"([{"op":"test","path":"/a","value":null}])"), R"({"a":null})");
    // Moving a value into itself, and removing the root
    jpatch(doc, R"([{"op":"move","from":"/spec","path":"/spec/inside"}])", 422);
    jpatch(doc, R"([{"op":"remove","path":""}])", 422);
    // The patch itself: not an array (400), too long (413)
    jpatch(doc, R"({"op":"remove","path":"/spec"})", 400);
    jpatch(doc, R"("x")", 400);
    std::string many = "[";
    for (int i = 0; i < 10001; ++i) many += (i ? "," : "") + std::string(R"({"op":"test","path":"/spec/replicas","value":2})");
    jpatch(doc, many + "]", 413);
    // An empty patch changes nothing
    expect(jpatch(doc, "[]"), doc);
    // The merge patch types never fail
    JsonValue m = parse(doc);
    assert(kube32::models::apply_patch(m, parse(R"({"spec":{"replicas":4}})"), "application/merge-patch+json") == 0);
    std::cout << "[PASS] test_json_patch_failures_leave_the_document" << std::endl;
}

int main() {
    test_apply_changes_one_container_field();
    test_keyed_lists_add_delete_order();
    test_null_and_directives_on_maps();
    test_node_condition_times();
    test_json_patch_operations();
    test_json_patch_pointers();
    test_json_patch_failures_leave_the_document();
    std::cout << "All patch tests passed" << std::endl;
    return 0;
}
