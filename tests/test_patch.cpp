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

int main() {
    test_apply_changes_one_container_field();
    test_keyed_lists_add_delete_order();
    test_null_and_directives_on_maps();
    std::cout << "All patch tests passed" << std::endl;
    return 0;
}
