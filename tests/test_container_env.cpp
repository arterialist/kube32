// Container environment: service links, envFrom and env valueFrom
// (ConfigMaps, Secrets, fieldRef), $(VAR) expansion, and the kubelet's
// CreateContainerConfigError handling.
#include "kubelet/pod_env.hpp"
#include "kubelet/kubelet.hpp"
#include "kubelet/kube_api_client.hpp"
#include "runtime/image_registry.hpp"
#include "runtime/wasm_runtime.hpp"
#include "hal/capability_firewall.hpp"
#include "osal/thread.hpp"

#include <cassert>
#include <iostream>
#include <map>
#include <string>

using namespace kube32;
using namespace kube32::kubelet;

namespace {

// API paths served by the tests: path -> body (200); anything else is 404
struct FakeApi {
    std::map<std::string, std::string> objects;
    std::vector<std::string> requested;
    int get(const std::string& path, std::string& body) {
        requested.push_back(path);
        auto it = objects.find(path);
        if (it == objects.end()) { body = "{\"kind\":\"Status\",\"code\":404}"; return 404; }
        body = it->second;
        return 200;
    }
};

std::string value_of(const EnvVars& env, const std::string& name) {
    for (const auto& kv : env) if (kv.first == name) return kv.second;
    return "<unset>";
}

bool has(const EnvVars& env, const std::string& name) {
    for (const auto& kv : env) if (kv.first == name) return true;
    return false;
}

PodEnvContext context_for(FakeApi& api) {
    PodEnvContext ctx;
    ctx.api_get = [&api](const std::string& p, std::string& b) { return api.get(p, b); };
    ctx.node_name = "node-a";
    ctx.node_ip = "10.0.0.7";
    ctx.api_host = "10.0.0.1";
    ctx.api_port = 6443;
    return ctx;
}

const char* kServices = R"({"kind":"ServiceList","items":[
  {"metadata":{"name":"echo-api","namespace":"default"},
   "spec":{"selector":{"app":"echo"},"ports":[{"name":"http","port":80,"targetPort":7000},{"name":"metrics","port":9100,"targetPort":9100,"protocol":"UDP"}]}},
  {"metadata":{"name":"self","namespace":"default"},
   "spec":{"selector":{"app":"me"},"ports":[{"port":80,"targetPort":"web"}]}},
  {"metadata":{"name":"headless","namespace":"default"},
   "spec":{"clusterIP":"None","selector":{"app":"echo"},"ports":[{"port":80}]}},
  {"metadata":{"name":"idle","namespace":"default"},
   "spec":{"selector":{"app":"nothing-runs"},"ports":[{"port":80}]}}
]})";

// As the API server derives them: echo-api's ready pod, a pod of it that is
// not ready, ports in name order; nothing ready behind idle; the starting
// pod's own Service, whose Endpoints do not count
const char* kEndpoints = R"({"kind":"EndpointsList","items":[
  {"metadata":{"name":"echo-api","namespace":"default"},"subsets":[
    {"notReadyAddresses":[{"ip":"10.0.0.9"}],"ports":[{"name":"http","port":7001,"protocol":"TCP"}]},
    {"addresses":[{"ip":"10.0.0.5"}],"ports":[{"name":"metrics","port":9100,"protocol":"UDP"},{"name":"http","port":7000,"protocol":"TCP"}]}]},
  {"metadata":{"name":"self","namespace":"default"},"subsets":[{"addresses":[{"ip":"10.0.0.8"}],"ports":[{"port":7999,"protocol":"TCP"}]}]},
  {"metadata":{"name":"headless","namespace":"default"},"subsets":[{"addresses":[{"ip":"10.0.0.5"}],"ports":[{"port":80,"protocol":"TCP"}]}]},
  {"metadata":{"name":"idle","namespace":"default"},"subsets":[{"notReadyAddresses":[{"ip":"10.0.0.6"}],"ports":[{"port":80,"protocol":"TCP"}]}]}
]})";

std::string pod_manifest(const std::string& extra_spec, const std::string& container_extra) {
    return R"({"metadata":{"name":"me-1","namespace":"default","uid":"uid-42","labels":{"app":"me","tier":"web"}},
               "spec":{"nodeName":"node-a")" + extra_spec + R"(,"containers":[{"name":"main","image":"wasm://envtest",
               "ports":[{"name":"web","containerPort":7000}])" + container_extra + "}]}}";
}

void test_expand_env_refs() {
    std::map<std::string, std::string> vars{{"A", "1"}, {"HOST", "h"}};
    auto lookup = [&](const std::string& n) -> const std::string* {
        auto it = vars.find(n);
        return it == vars.end() ? nullptr : &it->second;
    };
    assert(expand_env_refs("http://$(HOST):$(A)/", lookup) == "http://h:1/");
    assert(expand_env_refs("$$(A)", lookup) == "$(A)");          // escaped
    assert(expand_env_refs("$(MISSING)", lookup) == "$(MISSING)");
    assert(expand_env_refs("$(A", lookup) == "$(A");             // unclosed
    assert(expand_env_refs("cost $5", lookup) == "cost $5");
    assert(expand_env_refs("end$", lookup) == "end$");
    assert(service_env_name("echo-api") == "ECHO_API");
    std::cout << "[PASS] test_expand_env_refs" << std::endl;
}

void check_service_links(PodEnvContext& ctx, FakeApi& api) {
    EnvVars env;
    std::string err;
    assert(build_container_env(pod_manifest("", R"(,"env":[{"name":"ECHO_URL","value":"http://$(ECHO_API_SERVICE_HOST):$(ECHO_API_SERVICE_PORT)/echo"},
                                                         {"name":"SELF_SERVICE_PORT","value":"override"}])"), ctx, env, err));
    // The ready address of echo-api's Endpoints; its target ports are
    // advertised in the Service's port order
    assert(value_of(env, "ECHO_API_SERVICE_HOST") == "10.0.0.5");
    assert(value_of(env, "ECHO_API_SERVICE_PORT") == "7000");
    assert(value_of(env, "ECHO_API_SERVICE_PORT_HTTP") == "7000");
    assert(value_of(env, "ECHO_API_SERVICE_PORT_METRICS") == "9100");
    assert(value_of(env, "ECHO_API_PORT") == "tcp://10.0.0.5:7000");
    assert(value_of(env, "ECHO_API_PORT_7000_TCP") == "tcp://10.0.0.5:7000");
    assert(value_of(env, "ECHO_API_PORT_7000_TCP_PROTO") == "tcp");
    assert(value_of(env, "ECHO_API_PORT_7000_TCP_PORT") == "7000");
    assert(value_of(env, "ECHO_API_PORT_7000_TCP_ADDR") == "10.0.0.5");
    assert(value_of(env, "ECHO_API_PORT_9100_UDP") == "udp://10.0.0.5:9100");
    // The Service selecting the starting pod points at this node, named targetPort resolved
    assert(value_of(env, "SELF_SERVICE_HOST") == "10.0.0.7");
    assert(value_of(env, "SELF_PORT") == "tcp://10.0.0.7:7000");
    // env wins over a service variable, and may reference one
    assert(value_of(env, "SELF_SERVICE_PORT") == "override");
    assert(value_of(env, "ECHO_URL") == "http://10.0.0.5:7000/echo");
    // Headless Services and Services without a running pod get nothing
    assert(!has(env, "HEADLESS_SERVICE_HOST") && !has(env, "IDLE_SERVICE_HOST"));
    // The API server for every pod
    assert(value_of(env, "KUBERNETES_SERVICE_HOST") == "10.0.0.1");
    assert(value_of(env, "KUBERNETES_SERVICE_PORT") == "6443");
    assert(value_of(env, "KUBERNETES_PORT_6443_TCP") == "tcp://10.0.0.1:6443");

    // enableServiceLinks: false keeps only the API server's variables
    api.requested.clear();
    assert(build_container_env(pod_manifest(R"(,"enableServiceLinks":false)", ""), ctx, env, err));
    assert(!has(env, "ECHO_API_SERVICE_HOST") && has(env, "KUBERNETES_SERVICE_HOST"));
    assert(api.requested.empty());
}

void test_service_links() {
    FakeApi api;
    api.objects["/api/v1/namespaces/default/services"] = kServices;
    api.objects["/api/v1/namespaces/default/endpoints"] = kEndpoints;
    PodEnvContext ctx = context_for(api);
    check_service_links(ctx, api);

    // Lists read item by item (the kubelet streams them): the same variables
    // from two list requests, Services and Endpoints
    size_t items = 0;
    ctx.api_list = [&api, &items](const std::string& path, const std::function<void(std::string&&)>& on_item) {
        std::string body;
        const int status = api.get(path, body);
        models::JsonValue list;
        std::string perr;
        if (status != 200 || !models::parse_json(body, list, perr)) return status;
        for (const auto& item : list.get("items")->arr_val) { ++items; on_item(item.to_string()); }
        return status;
    };
    api.requested.clear();
    check_service_links(ctx, api);
    assert(items == 8);

    // Only Services selecting the starting pod: the Endpoints are not read
    api.objects["/api/v1/namespaces/default/services"] = R"({"items":[
      {"metadata":{"name":"self"},"spec":{"selector":{"app":"me"},"ports":[{"port":80,"targetPort":"web"}]}}]})";
    api.requested.clear();
    EnvVars env;
    std::string err;
    assert(build_container_env(pod_manifest("", ""), ctx, env, err));
    assert(value_of(env, "SELF_SERVICE_HOST") == "10.0.0.7" && value_of(env, "SELF_SERVICE_PORT") == "7000");
    assert(api.requested.size() == 1 && api.requested[0] == "/api/v1/namespaces/default/services");
    std::cout << "[PASS] test_service_links" << std::endl;
}

void test_configmap_and_secret_refs() {
    FakeApi api;
    api.objects["/api/v1/namespaces/default/services"] = R"({"items":[]})";
    api.objects["/api/v1/namespaces/default/configmaps/app-config"] =
        R"({"kind":"ConfigMap","metadata":{"name":"app-config"},"data":{"MODE":"fast","LEVEL":"3","bad key":"x","log.format":"json"}})";
    api.objects["/api/v1/namespaces/default/secrets/creds"] =
        R"({"kind":"Secret","metadata":{"name":"creds"},"data":{"TOKEN":"czNjcjN0","USER":"YWRtaW4="}})";
    api.objects["/api/v1/namespaces/default/secrets/plain"] =
        R"({"kind":"Secret","metadata":{"name":"plain"},"stringData":{"PIN":"1234"}})";
    PodEnvContext ctx = context_for(api);
    EnvVars env;
    std::string err;
    const std::string container = R"(,
        "envFrom":[{"configMapRef":{"name":"app-config"},"prefix":"CFG_"},{"secretRef":{"name":"creds"}},
                   {"configMapRef":{"name":"absent","optional":true}}],
        "env":[{"name":"MODE","valueFrom":{"configMapKeyRef":{"name":"app-config","key":"MODE"}}},
               {"name":"API_TOKEN","valueFrom":{"secretKeyRef":{"name":"creds","key":"TOKEN"}}},
               {"name":"PIN","valueFrom":{"secretKeyRef":{"name":"plain","key":"PIN"}}},
               {"name":"OPTIONAL","valueFrom":{"configMapKeyRef":{"name":"app-config","key":"NOPE","optional":true}}},
               {"name":"POD_NAME","valueFrom":{"fieldRef":{"fieldPath":"metadata.name"}}},
               {"name":"POD_IP","valueFrom":{"fieldRef":{"fieldPath":"status.podIP"}}},
               {"name":"TIER","valueFrom":{"fieldRef":{"fieldPath":"metadata.labels['tier']"}}},
               {"name":"GREETING","value":"user $(USER) in $(CFG_MODE) mode"}])";
    bool ok = build_container_env(pod_manifest("", container), ctx, env, err);
    if (!ok) std::cout << err << std::endl;
    assert(ok);
    assert(value_of(env, "CFG_MODE") == "fast" && value_of(env, "CFG_LEVEL") == "3");
    assert(value_of(env, "CFG_log.format") == "json");
    assert(!has(env, "CFG_bad key"));                        // not a valid variable name
    assert(value_of(env, "TOKEN") == "s3cr3t" && value_of(env, "USER") == "admin");
    assert(value_of(env, "MODE") == "fast");
    assert(value_of(env, "API_TOKEN") == "s3cr3t");
    assert(value_of(env, "PIN") == "1234");
    assert(!has(env, "OPTIONAL"));
    assert(value_of(env, "POD_NAME") == "me-1");
    assert(value_of(env, "POD_IP") == "10.0.0.7");
    assert(value_of(env, "TIER") == "web");
    assert(value_of(env, "GREETING") == "user admin in fast mode");
    // Each object is read once per container start
    size_t cm_reads = 0;
    for (const auto& p : api.requested) if (p == "/api/v1/namespaces/default/configmaps/app-config") ++cm_reads;
    assert(cm_reads == 1);

    // Missing objects and keys are CreateContainerConfigError, with the kubelet's messages
    assert(!build_container_env(pod_manifest("", R"(,"env":[{"name":"X","valueFrom":{"configMapKeyRef":{"name":"gone","key":"k"}}}])"), ctx, env, err));
    assert(err == "configmap \"gone\" not found");
    assert(!build_container_env(pod_manifest("", R"(,"env":[{"name":"X","valueFrom":{"secretKeyRef":{"name":"creds","key":"PASSWORD"}}}])"), ctx, env, err));
    assert(err == "couldn't find key PASSWORD in Secret default/creds");
    assert(!build_container_env(pod_manifest("", R"(,"envFrom":[{"secretRef":{"name":"nope"}}])"), ctx, env, err));
    assert(err == "secret \"nope\" not found");
    assert(!build_container_env(pod_manifest("", R"(,"env":[{"name":"X","valueFrom":{"resourceFieldRef":{"resource":"limits.memory"}}}])"), ctx, env, err));
    assert(err.find("unsupported valueFrom") != std::string::npos);
    // An unreachable API server is an error too (retried by the kubelet)
    PodEnvContext down = ctx;
    down.api_get = [](const std::string&, std::string& b) { b.clear(); return 0; };
    assert(!build_container_env(pod_manifest("", R"(,"envFrom":[{"configMapRef":{"name":"app-config"}}])"), down, env, err));
    assert(err.find("API server unreachable") != std::string::npos);
    std::cout << "[PASS] test_configmap_and_secret_refs" << std::endl;
}

// Kubelet: a missing ConfigMap leaves the container uncreated and Pending with
// CreateContainerConfigError; once it exists the next retry starts the pod.
class EnvClient : public KubeApiClient {
public:
    FakeApi api;
    std::vector<std::pair<models::KubePod, std::string>> pods;
    std::string last_patch;
    std::vector<std::string> events;
    bool register_node(const std::string&, bool& c) override { c = false; return true; }
    bool patch_node_status(const std::string&, const std::string&) override { return true; }
    bool list_pods(std::vector<models::KubePod>& out) override { for (auto& p : pods) out.push_back(p.first); return true; }
    bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out) override { out = pods; return true; }
    bool patch_pod_status(const std::string&, const std::string&, const std::string& p) override { last_patch = p; return true; }
    bool create_event(const std::string&, const std::string& e) override { events.push_back(e); return true; }
    int get(const std::string& path, std::string& body) override { return api.get(path, body); }
};

class EnvRuntime : public runtime::Runtime {
public:
    int deploys{0};
    std::vector<std::pair<std::string, std::string>> env;
    bool deploy_workload(const std::string&, const uint8_t*, size_t, uint32_t) override { return true; }
    bool deploy_workload_ex(const std::string&, const uint8_t*, size_t, const runtime::DeploySpec& spec, std::string&) override {
        ++deploys;
        env = spec.env;
        return true;
    }
    bool start(const std::string&) override { return true; }
    bool pause(const std::string&) override { return true; }
    bool terminate(const std::string&) override { return true; }
    runtime::ContainerStatus get_status(const std::string&) override {
        runtime::ContainerStatus s;
        s.phase = runtime::ContainerPhase::Running;
        return s;
    }
};

void test_kubelet_config_error_and_retry() {
    static const uint8_t k_min_wasm[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    runtime::ImageRegistry::instance().add({"envtest", k_min_wasm, sizeof(k_min_wasm)});
    hal::CapabilityFirewall::instance().reset();

    KubeletConfig cfg;
    cfg.node_name = "node-a";
    cfg.node_ip = "10.0.0.7";
    cfg.api_server_host = "127.0.0.1";
    cfg.restart_backoff_initial_ms = 200;
    EnvRuntime rt;
    auto client = std::make_unique<EnvClient>();
    EnvClient* c = client.get();
    c->api.objects["/api/v1/namespaces/default/services"] = R"({"items":[]})";
    models::KubePod pod;
    pod.name = "me-1";
    pod.namespace_name = "default";
    pod.node_name = "node-a";
    c->pods.emplace_back(pod, pod_manifest("", R"(,"env":[{"name":"MODE","valueFrom":{"configMapKeyRef":{"name":"app-config","key":"MODE"}}}])"));
    Kubelet k(cfg, rt, std::move(client));

    k.sync_assigned_pods();
    assert(rt.deploys == 0);
    assert(c->last_patch.find("\"phase\":\"Pending\"") != std::string::npos);
    assert(c->last_patch.find("\"waiting\":{\"reason\":\"CreateContainerConfigError\",\"message\":\"configmap \\\"app-config\\\" not found\"") != std::string::npos);
    assert(!c->events.empty() && c->events.back().find("Error: configmap \\\"app-config\\\" not found") != std::string::npos);
    LocalPodRecord rec;
    assert(k.get_pod_record("default/me-1", rec) && rec.execution_phase == PodExecutionPhase::CreateContainerConfigError);
    k.sync_assigned_pods();
    assert(rt.deploys == 0);                     // waits before retrying

    c->api.objects["/api/v1/namespaces/default/configmaps/app-config"] = R"({"data":{"MODE":"fast"}})";
    osal::Thread::sleep_ms(250);
    k.sync_assigned_pods();
    assert(rt.deploys == 1);
    assert(value_of(rt.env, "MODE") == "fast");
    // The control plane's kubelet uses loopback; pods get the node's address
    assert(value_of(rt.env, "KUBERNETES_SERVICE_HOST") == "10.0.0.7");
    assert(k.get_pod_record("default/me-1", rec) && rec.execution_phase == PodExecutionPhase::Running);
    assert(rec.restart_count == 0);              // never created before: not a restart
    assert(c->last_patch.find("\"restartCount\":0") != std::string::npos);
    hal::CapabilityFirewall::instance().reset();
    std::cout << "[PASS] test_kubelet_config_error_and_retry" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Container environment ===" << std::endl;
    test_expand_env_refs();
    test_service_links();
    test_configmap_and_secret_refs();
    test_kubelet_config_error_and_retry();
    std::cout << "=== All container environment tests passed ===" << std::endl;
    return 0;
}
