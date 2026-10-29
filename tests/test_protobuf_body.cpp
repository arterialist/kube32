// Typed-client request bodies in the Kubernetes protobuf encoding: the bytes
// kubectl create deployment|job|service sends, decoded to the JSON a
// kubectl apply of the same object carries.

#include "api/protobuf_body.hpp"
#include "models/kube_models.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace kube32;
using models::JsonValue;

// Equality of two documents, object keys in any order
static bool same(const JsonValue& a, const JsonValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case models::JsonType::Boolean: return a.bool_val == b.bool_val;
        case models::JsonType::Number: return a.num_val == b.num_val;
        case models::JsonType::String: return a.str_val == b.str_val;
        case models::JsonType::Array:
            if (a.arr_val.size() != b.arr_val.size()) return false;
            for (size_t i = 0; i < a.arr_val.size(); i++) if (!same(a.arr_val[i], b.arr_val[i])) return false;
            return true;
        case models::JsonType::Object:
            if (a.obj_val.size() != b.obj_val.size()) return false;
            for (const auto& kv : a.obj_val) {
                const JsonValue* o = b.get(kv.first);
                if (!o || !same(kv.second, *o)) return false;
            }
            return true;
        default: return true;
    }
}

static void expect(const std::string& body, const std::string& json) {
    std::string out, why;
    bool ok = api::protobuf_body_to_json(body, out, why);
    if (!ok) std::cerr << why << std::endl;
    assert(ok);
    JsonValue got, want;
    std::string err;
    assert(models::parse_json(out, got, err));
    assert(models::parse_json(json, want, err));
    if (!same(got, want)) std::cerr << "got:  " << out << "\nwant: " << json << std::endl;
    assert(same(got, want));
}

// Length-delimited field
static std::string pb(int number, const std::string& payload) {
    std::string out;
    out += static_cast<char>(number << 3 | 2);
    size_t len = payload.size();
    while (len >= 0x80) { out += static_cast<char>(len | 0x80); len >>= 7; }
    out += static_cast<char>(len);
    return out + payload;
}

// Varint field
static std::string pbv(int number, uint64_t value) {
    std::string out;
    out += static_cast<char>(number << 3);
    while (value >= 0x80) { out += static_cast<char>(value | 0x80); value >>= 7; }
    out += static_cast<char>(value);
    return out;
}

static std::string envelope(const std::string& api_version, const std::string& kind, const std::string& raw) {
    return std::string("k8s\0", 4) + pb(1, pb(1, api_version) + pb(2, kind)) + pb(2, raw);
}

// kubectl create deployment probe --image=wasm://embedded/echo:v1 --replicas=2 --port=8080
static const char kDeployment[] =
    "\x6b\x38\x73\x00\x0a\x15\x0a\x07\x61\x70\x70\x73\x2f\x76\x31\x12"
    "\x0a\x44\x65\x70\x6c\x6f\x79\x6d\x65\x6e\x74\x12\xd3\x01\x0a\x23"
    "\x0a\x05\x70\x72\x6f\x62\x65\x12\x00\x1a\x00\x22\x00\x2a\x00\x32"
    "\x00\x38\x00\x42\x00\x5a\x0c\x0a\x03\x61\x70\x70\x12\x05\x70\x72"
    "\x6f\x62\x65\x12\x9d\x01\x08\x02\x12\x0e\x0a\x0c\x0a\x03\x61\x70"
    "\x70\x12\x05\x70\x72\x6f\x62\x65\x1a\x80\x01\x0a\x1e\x0a\x00\x12"
    "\x00\x1a\x00\x22\x00\x2a\x00\x32\x00\x38\x00\x42\x00\x5a\x0c\x0a"
    "\x03\x61\x70\x70\x12\x05\x70\x72\x6f\x62\x65\x12\x5e\x12\x40\x0a"
    "\x04\x65\x63\x68\x6f\x12\x17\x77\x61\x73\x6d\x3a\x2f\x2f\x65\x6d"
    "\x62\x65\x64\x64\x65\x64\x2f\x65\x63\x68\x6f\x3a\x76\x31\x2a\x00"
    "\x32\x0b\x0a\x00\x10\x00\x18\x90\x3f\x22\x00\x2a\x00\x42\x00\x6a"
    "\x00\x72\x00\x80\x01\x00\x88\x01\x00\x90\x01\x00\xa2\x01\x00\x1a"
    "\x00\x32\x00\x42\x00\x4a\x00\x52\x00\x58\x00\x60\x00\x68\x00\x82"
    "\x01\x00\x8a\x01\x00\x9a\x01\x00\xc2\x01\x00\x22\x02\x0a\x00\x28"
    "\x00\x38\x00\x1a\x0c\x08\x00\x10\x00\x18\x00\x20\x00\x28\x00\x38"
    "\x00\x1a\x00\x22\x00";

// kubectl create job j1 --image=wasm://embedded/echo:v1 -- echo hi
static const char kJob[] =
    "\x6b\x38\x73\x00\x0a\x0f\x0a\x08\x62\x61\x74\x63\x68\x2f\x76\x31"
    "\x12\x03\x4a\x6f\x62\x12\x94\x01\x0a\x12\x0a\x02\x6a\x31\x12\x00"
    "\x1a\x00\x22\x00\x2a\x00\x32\x00\x38\x00\x42\x00\x12\x74\x32\x72"
    "\x0a\x10\x0a\x00\x12\x00\x1a\x00\x22\x00\x2a\x00\x32\x00\x38\x00"
    "\x42\x00\x12\x5e\x12\x3b\x0a\x02\x6a\x31\x12\x17\x77\x61\x73\x6d"
    "\x3a\x2f\x2f\x65\x6d\x62\x65\x64\x64\x65\x64\x2f\x65\x63\x68\x6f"
    "\x3a\x76\x31\x1a\x04\x65\x63\x68\x6f\x1a\x02\x68\x69\x2a\x00\x42"
    "\x00\x6a\x00\x72\x00\x80\x01\x00\x88\x01\x00\x90\x01\x00\xa2\x01"
    "\x00\x1a\x05\x4e\x65\x76\x65\x72\x32\x00\x42\x00\x4a\x00\x52\x00"
    "\x58\x00\x60\x00\x68\x00\x82\x01\x00\x8a\x01\x00\x9a\x01\x00\xc2"
    "\x01\x00\x1a\x08\x20\x00\x28\x00\x30\x00\x3a\x00\x1a\x00\x22\x00";

// kubectl create service clusterip web --tcp=80:8080
static const char kServiceInt[] =
    "\x6b\x38\x73\x00\x0a\x0d\x0a\x02\x76\x31\x12\x07\x53\x65\x72\x76"
    "\x69\x63\x65\x12\x69\x0a\x1f\x0a\x03\x77\x65\x62\x12\x00\x1a\x00"
    "\x22\x00\x2a\x00\x32\x00\x38\x00\x42\x00\x5a\x0a\x0a\x03\x61\x70"
    "\x70\x12\x03\x77\x65\x62\x12\x42\x0a\x1b\x0a\x07\x38\x30\x2d\x38"
    "\x30\x38\x30\x12\x03\x54\x43\x50\x18\x50\x22\x07\x08\x00\x10\x90"
    "\x3f\x1a\x00\x28\x00\x12\x0a\x0a\x03\x61\x70\x70\x12\x03\x77\x65"
    "\x62\x1a\x00\x22\x09\x43\x6c\x75\x73\x74\x65\x72\x49\x50\x3a\x00"
    "\x42\x00\x52\x00\x5a\x00\x60\x00\x68\x00\x1a\x02\x0a\x00\x1a\x00"
    "\x22\x00";

// kubectl create service clusterip named --tcp=80:http
static const char kServiceName[] =
    "\x6b\x38\x73\x00\x0a\x0d\x0a\x02\x76\x31\x12\x07\x53\x65\x72\x76"
    "\x69\x63\x65\x12\x72\x0a\x23\x0a\x05\x6e\x61\x6d\x65\x64\x12\x00"
    "\x1a\x00\x22\x00\x2a\x00\x32\x00\x38\x00\x42\x00\x5a\x0c\x0a\x03"
    "\x61\x70\x70\x12\x05\x6e\x61\x6d\x65\x64\x12\x47\x0a\x1e\x0a\x07"
    "\x38\x30\x2d\x68\x74\x74\x70\x12\x03\x54\x43\x50\x18\x50\x22\x0a"
    "\x08\x01\x10\x00\x1a\x04\x68\x74\x74\x70\x28\x00\x12\x0c\x0a\x03"
    "\x61\x70\x70\x12\x05\x6e\x61\x6d\x65\x64\x1a\x00\x22\x09\x43\x6c"
    "\x75\x73\x74\x65\x72\x49\x50\x3a\x00\x42\x00\x52\x00\x5a\x00\x60"
    "\x00\x68\x00\x1a\x02\x0a\x00\x1a\x00\x22\x00";

int main() {
    std::cout << "=== protobuf request bodies ===" << std::endl;

    expect(std::string(kDeployment, sizeof kDeployment - 1), R"({"apiVersion":"apps/v1","kind":"Deployment",
        "metadata":{"name":"probe","labels":{"app":"probe"}},
        "spec":{"replicas":2,"selector":{"matchLabels":{"app":"probe"}},"strategy":{},
            "template":{"metadata":{"labels":{"app":"probe"}},
                "spec":{"containers":[{"name":"echo","image":"wasm://embedded/echo:v1","ports":[{"containerPort":8080}],"resources":{}}]}}}})");

    expect(std::string(kJob, sizeof kJob - 1), R"({"apiVersion":"batch/v1","kind":"Job",
        "metadata":{"name":"j1"},
        "spec":{"template":{"metadata":{},
            "spec":{"containers":[{"name":"j1","image":"wasm://embedded/echo:v1","command":["echo","hi"],"resources":{}}],"restartPolicy":"Never"}}}})");

    expect(std::string(kServiceInt, sizeof kServiceInt - 1), R"({"apiVersion":"v1","kind":"Service",
        "metadata":{"name":"web","labels":{"app":"web"}},
        "spec":{"ports":[{"name":"80-8080","protocol":"TCP","port":80,"targetPort":8080}],"selector":{"app":"web"},"type":"ClusterIP"}})");

    expect(std::string(kServiceName, sizeof kServiceName - 1), R"({"apiVersion":"v1","kind":"Service",
        "metadata":{"name":"named","labels":{"app":"named"}},
        "spec":{"ports":[{"name":"80-http","protocol":"TCP","port":80,"targetPort":"http"}],"selector":{"app":"named"},"type":"ClusterIP"}})");

    // Fields beyond the ones kubectl sets: namespace, annotations, set
    // resources, args, a rolling-update strategy and a match expression
    std::string meta = pb(1, "d") + pb(3, "team-a") + pb(12, pb(1, "note") + pb(2, "hello"));
    std::string quantity = pb(1, "100m");
    std::string resources = pb(1, pb(1, "cpu") + pb(2, quantity)) + pb(2, pb(1, "memory") + pb(2, pb(1, "16Mi")));
    std::string ctr = pb(1, "c") + pb(2, "img") + pb(4, "--flag") + pb(8, resources) + pb(6, pb(1, "web") + pbv(3, 8080));
    std::string tmpl = pb(1, pb(11, pb(1, "app") + pb(2, "d"))) + pb(2, pb(2, ctr));
    std::string expr = pb(1, "tier") + pb(2, "In") + pb(3, "a") + pb(3, "b");
    std::string strategy = pb(1, "RollingUpdate") + pb(2, pb(1, pbv(1, 1) + pb(3, "25%")) + pb(2, pbv(2, 1)));
    std::string spec = pbv(1, 3) + pb(2, pb(2, expr)) + pb(3, tmpl) + pb(4, strategy) + pb(14, "unknown") + pbv(15, 7);
    expect(envelope("apps/v1", "Deployment", pb(1, meta) + pb(2, spec) + pb(3, "status")),
           R"({"apiVersion":"apps/v1","kind":"Deployment",
        "metadata":{"name":"d","namespace":"team-a","annotations":{"note":"hello"}},
        "spec":{"replicas":3,"selector":{"matchExpressions":[{"key":"tier","operator":"In","values":["a","b"]}]},
            "strategy":{"type":"RollingUpdate","rollingUpdate":{"maxUnavailable":"25%","maxSurge":1}},
            "template":{"metadata":{"labels":{"app":"d"}},
                "spec":{"containers":[{"name":"c","image":"img","args":["--flag"],
                    "ports":[{"name":"web","containerPort":8080}],
                    "resources":{"limits":{"cpu":"100m"},"requests":{"memory":"16Mi"}}}]}}}})");

    // Other kinds and malformed bodies are refused without a crash
    std::string out, why;
    assert(!api::protobuf_body_to_json(envelope("apps/v1", "StatefulSet", ""), out, why));
    assert(why.find("apps/v1 StatefulSet in protobuf is not supported") != std::string::npos);
    std::string full(kDeployment, sizeof kDeployment - 1);
    for (size_t cut = 0; cut < full.size(); cut++) api::protobuf_body_to_json(full.substr(0, cut), out, why);
    for (size_t i = 4; i < full.size(); i++) {
        std::string bad = full;
        bad[i] = static_cast<char>(bad[i] ^ 0xff);
        api::protobuf_body_to_json(bad, out, why);
    }
    assert(!api::protobuf_body_to_json(envelope("apps/v1", "Deployment", pb(2, pb(3, pb(2, pb(2, std::string("\x0a\x7f", 2)))))), out, why));

    std::cout << "[PASS] protobuf request bodies" << std::endl;
    return 0;
}
