// NetworkPolicy egress: k32_http_call URL resolution, policy compilation and
// evaluation, the networking.k8s.io/v1 NetworkPolicy resource, and the
// worker's view of the policies that its kubelet keeps current.

#include "net/network_policy.hpp"
#include "app/service_client.hpp"
#include "app/egress_policy.hpp"
#include "api/route_handler.hpp"
#include "api/watch_hub.hpp"
#include "kubelet/kubelet.hpp"
#include "kubelet/kube_api_client.hpp"
#include "models/kube_models.hpp"
#include "storage/store.hpp"
#include "mem/monotonic_arena.hpp"
#include "runtime/k32_app_abi.h"
#include "osal/socket.hpp"
#include "osal/thread.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace kube32;

namespace {

models::JsonValue json(const std::string& text) {
    models::JsonValue doc;
    std::string err;
    bool ok = models::parse_json(text, doc, err);
    if (!ok) std::cout << "bad test JSON: " << err << "\n" << text << std::endl;
    assert(ok);
    return doc;
}

std::string policy(const std::string& name, const std::string& spec, const std::string& ns = "default") {
    return "{\"apiVersion\":\"networking.k8s.io/v1\",\"kind\":\"NetworkPolicy\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"" + ns +
           "\"},\"spec\":" + spec + "}";
}

net::NetworkPolicyRules compile(const std::string& name, const std::string& spec) {
    return net::compile_network_policy(json(policy(name, spec)));
}

std::string why_invalid(const std::string& spec) {
    std::string why;
    return net::validate_network_policy(json(policy("x", spec)), why) ? std::string() : why;
}

uint32_t ip(const char* text) {
    uint32_t v = 0;
    assert(net::parse_ipv4(text, v));
    return v;
}

net::EgressTarget external(const char* addr, uint16_t port) {
    net::EgressTarget t;
    t.ip = ip(addr);
    t.port = port;
    return t;
}

// Whether the policies of namespace "default" let a pod with `labels` make
// the connection; `pod` answers for the target, `asked` counts the questions
bool allows(const net::NamespacePolicies& ps, const net::LabelMap& labels, const net::EgressTarget& t, const net::PeerPod* pod = nullptr,
            int* asked = nullptr) {
    auto peer = [&]() -> const net::PeerPod* {
        if (asked) ++*asked;
        return pod;
    };
    return net::egress_allowed(net::select_egress_policies(ps, labels), "default", t, peer);
}

// ---------------------------------------------------------------------------
// The API, in-process
// ---------------------------------------------------------------------------
api::HttpResponse request(api::RouteHandler& h, api::HttpMethod method, const std::string& target, const std::string& body = "",
                          const std::string& content_type = "application/json", bool table = false) {
    api::HttpRequest r;
    r.method = method;
    size_t q = target.find('?');
    r.path = target.substr(0, q);
    r.query_string = q == std::string::npos ? "" : target.substr(q + 1);
    r.body = body;
    r.content_type = body.empty() ? "" : content_type;
    if (table) {
        r.accept = "application/json;as=Table;v=v1;g=meta.k8s.io";
        r.as_table = true;
    }
    mem::MonotonicArena arena;
    return h.handle_request(r, arena);
}

// Reads from fd until `needle` appears or the deadline passes
std::string read_until(int fd, const std::string& needle, uint32_t timeout_ms) {
    std::string acc;
    uint64_t t0 = osal::Thread::get_time_ms();
    char buf[1024];
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (osal::Thread::get_time_ms() - t0 < timeout_ms) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) acc.append(buf, static_cast<size_t>(n));
        else if (n == 0) break;
        if (acc.find(needle) != std::string::npos) break;
    }
    return acc;
}

std::unique_ptr<storage::Store> mem_store() {
    storage::StoreOptions o;
    o.enable_flash_persistence = false;
    return storage::create_radix_store(o);
}

// ---------------------------------------------------------------------------
// k32_http_call URLs
// ---------------------------------------------------------------------------
void test_url_resolution() {
    std::cout << "[RUN] test_url_resolution" << std::endl;
    using T = app::ServiceClient::Target;
    T t;
    // Services: one label, <svc>.<ns> (external when no such Service), the cluster suffixes
    assert(app::ServiceClient::parse_target("http://echo/x", t) && t.service == "echo" && t.ns.empty() && !t.external && !t.or_external);
    assert(t.path == "/x" && t.port == 0);
    assert(app::ServiceClient::parse_target("http://echo.shop", t) && t.service == "echo" && t.ns == "shop" && t.or_external && t.path == "/");
    assert(app::ServiceClient::parse_target("http://echo.shop.svc:8080/p?q=1", t) && t.service == "echo" && t.ns == "shop" && !t.or_external);
    assert(t.port == 8080 && t.path == "/p?q=1");
    assert(app::ServiceClient::parse_target("http://echo.shop.svc.cluster.local/", t) && t.ns == "shop" && !t.external && !t.or_external);
    // Hosts outside the cluster: addresses, names of three labels or more, names no Service can have
    assert(app::ServiceClient::parse_target("http://10.1.2.3:8123/api/states", t) && t.external && t.ip == ip("10.1.2.3") && t.port == 8123);
    assert(t.host == "10.1.2.3" && t.path == "/api/states");
    assert(app::ServiceClient::parse_target("http://api.example.com/v1", t) && t.external && t.ip == 0 && t.host == "api.example.com");
    assert(app::ServiceClient::parse_target("http://Printer.LAN/", t) && t.external);
    assert(app::ServiceClient::parse_target("http://my_host/", t) == false);
    assert(app::ServiceClient::parse_target("http://homeassistant.local:8123/", t) && t.service == "homeassistant" && t.ns == "local" && t.or_external);
    assert(app::ServiceClient::parse_target("http://host?x=1", t) && t.path == "/?x=1");
    assert(app::ServiceClient::parse_target("http://host/page#top", t) && t.path == "/page");
    assert(app::ServiceClient::parse_target("https://example.com/", t) && t.https);
    for (const char* bad : {"ftp://x/", "http:///", "http://a b/", "http://user@host/", "http://[::1]/", "http://host:0/", "http://host:70000/",
                            "http://0.0.0.0/", "http://host/a b", "http://host:/"}) {
        assert(!app::ServiceClient::parse_target(bad, t));
    }
    std::cout << "[PASS] test_url_resolution" << std::endl;
}

// ---------------------------------------------------------------------------
// Compilation and evaluation
// ---------------------------------------------------------------------------
void test_selectors_and_addresses() {
    std::cout << "[RUN] test_selectors_and_addresses" << std::endl;
    uint32_t net = 0, mask = 0;
    assert(net::parse_cidr("10.1.2.3/16", net, mask) && net == ip("10.1.0.0") && mask == 0xFFFF0000u);
    assert(net::parse_cidr("0.0.0.0/0", net, mask) && net == 0 && mask == 0);
    assert(net::parse_cidr("192.168.4.1/32", net, mask) && mask == 0xFFFFFFFFu);
    assert(!net::parse_cidr("10.0.0.0/33", net, mask) && !net::parse_cidr("10.0.0/8", net, mask) && !net::parse_cidr("10.0.0.0", net, mask));
    uint32_t a = 0;
    assert(!net::parse_ipv4("1.2.3.256", a) && !net::parse_ipv4("1.2.3", a) && !net::parse_ipv4("1.2.3.4.", a) && !net::parse_ipv4("01234.1.1.1", a));
    assert(net::format_ipv4(ip("172.16.5.4")) == "172.16.5.4");

    net::NetworkPolicyRules p = compile("sel", R"({"podSelector":{"matchLabels":{"app":"web"},"matchExpressions":[
        {"key":"tier","operator":"In","values":["fe","be"]},{"key":"env","operator":"NotIn","values":["dev"]},
        {"key":"team","operator":"Exists"},{"key":"legacy","operator":"DoesNotExist"}]},"policyTypes":["Egress"]})");
    assert(p.pod_selector.matches({{"app", "web"}, {"tier", "fe"}, {"team", "a"}}));
    assert(p.pod_selector.matches({{"app", "web"}, {"tier", "be"}, {"team", "a"}, {"env", "prod"}}));
    assert(!p.pod_selector.matches({{"app", "web"}, {"tier", "fe"}, {"team", "a"}, {"env", "dev"}}));
    assert(!p.pod_selector.matches({{"app", "web"}, {"tier", "fe"}}));                     // team missing
    assert(!p.pod_selector.matches({{"app", "web"}, {"tier", "fe"}, {"team", "a"}, {"legacy", "1"}}));
    assert(!p.pod_selector.matches({{"app", "web"}, {"team", "a"}}));                      // tier missing
    assert(net::LabelSelector().matches({}));
    std::cout << "[PASS] test_selectors_and_addresses" << std::endl;
}

void test_egress_evaluation() {
    std::cout << "[RUN] test_egress_evaluation" << std::endl;
    net::NamespacePolicies ps;
    // web: 10/8 without 10.1/16 on 80 and 8000-8100; db pods on their "postgres" port
    ps.push_back(compile("web-out", R"({"podSelector":{"matchLabels":{"app":"web"}},"policyTypes":["Egress"],"egress":[
        {"to":[{"ipBlock":{"cidr":"10.0.0.0/8","except":["10.1.0.0/16"]}}],"ports":[{"port":80},{"port":8000,"endPort":8100,"protocol":"TCP"}]}]})"));
    ps.push_back(compile("web-db", R"({"podSelector":{"matchLabels":{"app":"web"}},"policyTypes":["Egress"],"egress":[
        {"to":[{"podSelector":{"matchLabels":{"app":"db"}}}],"ports":[{"port":"postgres"}]}]})"));
    // tier=x: no egress at all
    ps.push_back(compile("lockdown", R"({"podSelector":{"matchLabels":{"tier":"x"}},"policyTypes":["Egress"],"egress":[]})"));
    // Every pod: ingress only, which restricts no egress
    ps.push_back(compile("ingress-only", R"({"podSelector":{},"ingress":[{"from":[{"podSelector":{}}]}]})"));
    // api: role=cache pods of namespaces labelled team=a, any port
    ps.push_back(compile("api-cache", R"({"podSelector":{"matchLabels":{"app":"api"}},"policyTypes":["Egress"],"egress":[
        {"to":[{"namespaceSelector":{"matchLabels":{"team":"a"}},"podSelector":{"matchLabels":{"role":"cache"}}}]}]})"));
    // dns: UDP only, which a TCP connection never matches; an IPv6 block matches nothing
    ps.push_back(compile("dns", R"({"podSelector":{"matchLabels":{"app":"dns"}},"policyTypes":["Egress"],"egress":[
        {"ports":[{"port":53,"protocol":"UDP"}]},{"to":[{"ipBlock":{"cidr":"::/0"}}]}]})"));
    // all: everything (an empty rule), and egress rules without policyTypes count as Egress
    ps.push_back(compile("all", R"({"podSelector":{"matchLabels":{"app":"all"}},"egress":[{}]})"));
    // namespace peer only: every pod of namespaces labelled team=b
    ps.push_back(compile("team-b", R"({"podSelector":{"matchLabels":{"app":"nsonly"}},"policyTypes":["Egress"],"egress":[
        {"to":[{"namespaceSelector":{"matchLabels":{"team":"b"}}}]}]})"));
    assert(ps[3].egress == false && ps[6].egress == true);

    const net::LabelMap web{{"app", "web"}};
    // Not selected by an egress policy: everything allowed, the target never asked about
    int asked = 0;
    assert(net::select_egress_policies(ps, {{"app", "other"}}).empty());
    assert(allows(ps, {{"app", "other"}}, external("203.0.113.8", 443), nullptr, &asked) && asked == 0);

    // ipBlock with except, ports and endPort
    assert(allows(ps, web, external("10.2.3.4", 80), nullptr, &asked));
    assert(!allows(ps, web, external("10.1.2.3", 80)));
    assert(allows(ps, web, external("10.2.3.4", 8000)) && allows(ps, web, external("10.2.3.4", 8100)));
    assert(!allows(ps, web, external("10.2.3.4", 8101)) && !allows(ps, web, external("10.2.3.4", 7999)));
    assert(!allows(ps, web, external("11.0.0.1", 80), nullptr, &asked));
    assert(asked == 0);   // pod selectors never pick hosts outside the cluster: no question asked

    // In-cluster: pod selector and named port (union of web-out and web-db)
    net::PeerPod db;
    db.labels = {{"app", "db"}};
    db.namespace_labels = {{"kubernetes.io/metadata.name", "default"}};
    db.ports = {{"postgres", 5432}};
    net::EgressTarget to_db;
    to_db.ip = ip("192.168.4.2");
    to_db.port = 5432;
    to_db.pod_namespace = "default";
    asked = 0;
    assert(allows(ps, web, to_db, &db, &asked) && asked == 1);
    to_db.port = 5433;
    assert(!allows(ps, web, to_db, &db));
    to_db.port = 5432;
    to_db.pod_namespace = "other";   // a podSelector alone picks pods of the policy's namespace
    assert(!allows(ps, web, to_db, &db));
    to_db.pod_namespace = "default";
    net::PeerPod cache = db;
    cache.labels = {{"app", "cache"}};
    assert(!allows(ps, web, to_db, &cache));
    // An in-cluster pod inside 10/8 on port 80: web-out's ipBlock applies to it too
    net::EgressTarget pod_in_block = to_db;
    pod_in_block.ip = ip("10.9.9.9");
    pod_in_block.port = 80;
    assert(allows(ps, web, pod_in_block, &cache));

    // Empty egress: nothing allowed
    assert(!allows(ps, {{"tier", "x"}}, external("10.2.3.4", 80)));
    assert(!allows(ps, {{"tier", "x"}}, to_db, &db));
    // Selected by lockdown and web-out: the union allows what web-out allows
    assert(allows(ps, {{"tier", "x"}, {"app", "web"}}, external("10.2.3.4", 80)));

    // namespaceSelector with podSelector
    net::PeerPod redis;
    redis.labels = {{"role", "cache"}};
    redis.namespace_labels = {{"team", "a"}};
    net::EgressTarget to_redis;
    to_redis.ip = ip("192.168.4.3");
    to_redis.port = 6379;
    to_redis.pod_namespace = "caches";
    assert(allows(ps, {{"app", "api"}}, to_redis, &redis));
    redis.namespace_labels = {{"team", "b"}};
    assert(!allows(ps, {{"app", "api"}}, to_redis, &redis));
    assert(!allows(ps, {{"app", "api"}}, external("192.168.4.3", 6379)));   // selectors never pick external hosts
    assert(allows(ps, {{"app", "nsonly"}}, to_redis, &redis));
    assert(!allows(ps, {{"app", "nsonly"}}, to_redis, nullptr));            // the target pod could not be read

    // UDP ports and IPv6 blocks never match a TCP connection over IPv4
    assert(!allows(ps, {{"app", "dns"}}, external("10.0.0.53", 53)));
    // An empty rule allows everything
    assert(allows(ps, {{"app", "all"}}, external("198.51.100.1", 1)) && allows(ps, {{"app", "all"}}, to_redis, &redis));

    // policyTypes [Egress] without rules: nothing allowed
    net::NamespacePolicies deny{compile("deny", R"({"podSelector":{},"policyTypes":["Egress"]})")};
    assert(!allows(deny, {}, external("198.51.100.1", 80)));
    std::cout << "[PASS] test_egress_evaluation" << std::endl;
}

void test_validation_and_defaults() {
    std::cout << "[RUN] test_validation_and_defaults" << std::endl;
    assert(why_invalid(R"({"podSelector":{}})").empty());
    assert(why_invalid(R"({"egress":[{"to":[{"ipBlock":{"cidr":"10.0.0.0/8","except":["10.1.0.0/16"]}}],"ports":[{"port":"http"},{"port":80,"endPort":90}]}]})").empty());
    assert(why_invalid(R"({"egress":[{"to":[{"ipBlock":{"cidr":"2001:db8::/64","except":["2001:db8::/96"]}}]}]})").empty());
    assert(why_invalid(R"({"egress":[{"to":[{"ipBlock":{"cidr":"10.0.0.0/33"}}]}]})").find("spec.egress[0].to[0].ipBlock.cidr: Invalid value") == 0);
    assert(why_invalid(R"({"egress":[{"to":[{"ipBlock":{"cidr":"10.0.0.0/8","except":["11.0.0.0/16"]}}]}]})").find("must be a strict subset") != std::string::npos);
    assert(why_invalid(R"({"egress":[{"to":[{"ipBlock":{"cidr":"10.0.0.0/8","except":["10.0.0.0/8"]}}]}]})").find("strict subset") != std::string::npos);
    assert(why_invalid(R"({"egress":[{"to":[{"ipBlock":{"cidr":"10.0.0.0/8"},"podSelector":{}}]}]})").find("may not specify both") != std::string::npos);
    assert(why_invalid(R"({"egress":[{"to":[{}]}]})").find("must specify a peer") != std::string::npos);
    assert(why_invalid(R"({"egress":[{"ports":[{"port":"http","endPort":90}]}]})").find("endPort") != std::string::npos);
    assert(why_invalid(R"({"egress":[{"ports":[{"port":90,"endPort":80}]}]})").find("must be greater than or equal to `port`") != std::string::npos);
    assert(why_invalid(R"({"egress":[{"ports":[{"endPort":80}]}]})").find("endPort") != std::string::npos);
    assert(why_invalid(R"({"egress":[{"ports":[{"port":0}]}]})").find("between 1 and 65535") != std::string::npos);
    assert(why_invalid(R"({"egress":[{"ports":[{"port":"Not_A_Name"}]}]})").find("IANA_SVC_NAME") != std::string::npos);
    assert(why_invalid(R"({"egress":[{"ports":[{"port":80,"protocol":"ICMP"}]}]})").find("protocol: Unsupported value") != std::string::npos);
    assert(why_invalid(R"({"policyTypes":["Egress","Sideways"]})").find("spec.policyTypes[1]: Unsupported value") == 0);
    assert(why_invalid(R"({"policyTypes":["Egress","Egress"]})").find("Duplicate") != std::string::npos);
    assert(why_invalid(R"({"podSelector":{"matchExpressions":[{"key":"a","operator":"In"}]}})").find("values: Required value") != std::string::npos);
    assert(why_invalid(R"({"podSelector":{"matchExpressions":[{"key":"a","operator":"Exists","values":["x"]}]}})").find("Forbidden") != std::string::npos);
    assert(why_invalid(R"({"ingress":[{"from":[{"ipBlock":{"cidr":"bogus"}}]}]})").find("spec.ingress[0].from[0]") == 0);

    // Defaults: policyTypes from the rules present, protocol TCP
    models::JsonValue d = json(policy("d", R"({"podSelector":{},"egress":[{"ports":[{"port":80}]}]})"));
    net::default_network_policy(d);
    const std::string s = d.to_string();
    assert(s.find("\"policyTypes\":[\"Ingress\",\"Egress\"]") != std::string::npos);
    assert(s.find("\"protocol\":\"TCP\"") != std::string::npos);
    models::JsonValue i = json(policy("i", R"({"podSelector":{},"egress":[]})"));
    net::default_network_policy(i);
    assert(i.to_string().find("\"policyTypes\":[\"Ingress\"]") != std::string::npos);
    models::JsonValue keep = json(policy("k", R"({"policyTypes":["Egress"]})"));
    net::default_network_policy(keep);
    assert(keep.to_string().find("\"policyTypes\":[\"Egress\"]") != std::string::npos);
    assert(keep.to_string().find("\"podSelector\":{}") != std::string::npos);

    // A stored policy that does not compile denies its namespace's pods
    net::NetworkPolicyRules broken = compile("broken", R"({"podSelector":{"matchLabels":{"app":"web"}},"egress":[{"to":[{"ipBlock":{"cidr":"nope"}}]}]})");
    assert(broken.egress && broken.egress_rules.empty() && broken.pod_selector.requirements.empty());
    std::cout << "[PASS] test_validation_and_defaults" << std::endl;
}

// ---------------------------------------------------------------------------
// The API resource
// ---------------------------------------------------------------------------
void test_api_resource() {
    std::cout << "[RUN] test_api_resource" << std::endl;
    auto store = mem_store();
    api::RouteHandler h(*store);
    const std::string base = "/apis/networking.k8s.io/v1/namespaces/default/networkpolicies";

    // Discovery and OpenAPI: kubectl get netpol, kubectl apply
    api::HttpResponse r = request(h, api::HttpMethod::Get, "/apis/networking.k8s.io/v1");
    assert(r.status_code == 200);
    assert(r.body.find("{\"name\":\"networkpolicies\",\"singularName\":\"networkpolicy\",\"namespaced\":true,\"kind\":\"NetworkPolicy\"") != std::string::npos);
    assert(r.body.find("\"shortNames\":[\"netpol\"]") != std::string::npos);
    r = request(h, api::HttpMethod::Get, "/openapi/v3/apis/networking.k8s.io/v1");
    assert(r.status_code == 200 && r.body.find("\"/apis/networking.k8s.io/v1/namespaces/{namespace}/networkpolicies/{name}\"") != std::string::npos);
    assert(r.body.find("\"kind\":\"NetworkPolicy\"") != std::string::npos);

    // Watch the namespace while the policy is created, changed and deleted
    api::WatchHubOptions wo;
    wo.poll_interval_ms = 50;
    api::WatchHub hub(*store, wo);
    assert(hub.start());
    h.set_watch_hub(&hub);
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    osal::Socket client(sv[0]);
    {
        api::HttpRequest w;
        w.method = api::HttpMethod::Get;
        w.path = base;
        w.query_string = "watch=true";
        mem::MonotonicArena arena;
        api::HttpResponse wr = h.handle_request(w, arena, &client);
        assert(wr.socket_taken && wr.status_code == 200);
    }
    std::string head = read_until(sv[1], "\r\n\r\n", 1000);
    assert(head.find("HTTP/1.1 200 OK\r\n") == 0);

    // Create: validated (422), then defaulted
    r = request(h, api::HttpMethod::Post, base, policy("bad", R"({"egress":[{"to":[{"ipBlock":{"cidr":"10.0.0.0/8","except":["12.0.0.0/8"]}}]}]})"));
    assert(r.status_code == 422);
    assert(r.body.find("NetworkPolicy.networking.k8s.io \\\"bad\\\" is invalid: spec.egress[0].to[0].ipBlock.except[0]") != std::string::npos);
    r = request(h, api::HttpMethod::Post, base, policy("web-egress", R"({"podSelector":{"matchLabels":{"app":"web"}},
        "egress":[{"to":[{"ipBlock":{"cidr":"10.0.0.0/8"}}],"ports":[{"port":80}]}]})"));
    assert(r.status_code == 201);
    assert(r.body.find("\"policyTypes\":[\"Ingress\",\"Egress\"]") != std::string::npos && r.body.find("\"protocol\":\"TCP\"") != std::string::npos);
    assert(r.body.find("\"uid\":\"") != std::string::npos && r.body.find("\"generation\":1") != std::string::npos);
    std::string stored;
    assert(store->get("/registry/networkpolicies/default/web-egress", stored).ok());
    std::string got = read_until(sv[1], "\"name\":\"web-egress\"", 2000);
    assert(got.find("{\"type\":\"ADDED\",\"object\":{") != std::string::npos && got.find("\"kind\":\"NetworkPolicy\"") != std::string::npos);
    assert(request(h, api::HttpMethod::Post, base, policy("web-egress", "{}")).status_code == 409);
    r = request(h, api::HttpMethod::Post, base, policy("deny-all", R"({"podSelector":{},"policyTypes":["Egress"]})"));
    assert(r.status_code == 201);
    assert(request(h, api::HttpMethod::Post, "/apis/networking.k8s.io/v1/namespaces/team/networkpolicies",
                   policy("sets", R"({"podSelector":{"matchExpressions":[{"key":"tier","operator":"In","values":["fe","be"]},
                       {"key":"legacy","operator":"DoesNotExist"}]},"policyTypes":["Egress"]})", "team")).status_code == 201);
    // fieldValidation=Strict, as kubectl apply sends it
    assert(request(h, api::HttpMethod::Post, base + "?fieldValidation=Strict",
                   "{\"apiVersion\":\"networking.k8s.io/v1\",\"kind\":\"NetworkPolicy\",\"metadata\":{\"name\":\"s\"},\"spek\":{}}").status_code == 400);

    // Read: item, namespace list, all namespaces, Table
    r = request(h, api::HttpMethod::Get, base + "/web-egress");
    assert(r.status_code == 200 && r.body.find("\"name\":\"web-egress\"") != std::string::npos);
    r = request(h, api::HttpMethod::Get, base);
    assert(r.status_code == 200 && r.body.find("\"kind\":\"NetworkPolicyList\",\"apiVersion\":\"networking.k8s.io/v1\"") == 1);
    assert(r.body.find("\"deny-all\"") != std::string::npos && r.body.find("\"sets\"") == std::string::npos);
    r = request(h, api::HttpMethod::Get, "/apis/networking.k8s.io/v1/networkpolicies");
    assert(r.status_code == 200 && r.body.find("\"sets\"") != std::string::npos && r.body.find("\"deny-all\"") != std::string::npos);
    r = request(h, api::HttpMethod::Get, "/apis/networking.k8s.io/v1/networkpolicies", "", "", true);
    assert(r.status_code == 200 && r.body.find("\"kind\":\"Table\"") != std::string::npos);
    assert(r.body.find("{\"name\":\"POD-SELECTOR\",\"type\":\"string\",\"priority\":0}") != std::string::npos);
    assert(r.body.find("\"cells\":[\"web-egress\",\"app=web\",") != std::string::npos);
    assert(r.body.find("\"cells\":[\"deny-all\",\"<none>\",") != std::string::npos);
    assert(r.body.find("\"cells\":[\"sets\",\"!legacy,tier in (be,fe)\",") != std::string::npos);
    r = request(h, api::HttpMethod::Get, base + "/deny-all", "", "", true);
    assert(r.status_code == 200 && r.body.find("\"cells\":[\"deny-all\",\"<none>\",") != std::string::npos);
    r = request(h, api::HttpMethod::Get, base + "?labelSelector=nope%3D1");
    assert(r.status_code == 200 && r.body.find("\"items\":[]") != std::string::npos);

    // Update: merge patch (the egress list is replaced), PUT; both validated
    r = request(h, api::HttpMethod::Patch, base + "/web-egress", R"({"spec":{"egress":[{"ports":[{"port":443}]}]}})", "application/merge-patch+json");
    assert(r.status_code == 200 && r.body.find("\"port\":443") != std::string::npos && r.body.find("10.0.0.0/8") == std::string::npos);
    got = read_until(sv[1], "\"port\":443", 2000);
    assert(got.find("\"type\":\"MODIFIED\"") != std::string::npos);
    r = request(h, api::HttpMethod::Patch, base + "/web-egress", R"({"spec":{"egress":[{"ports":[{"port":70000}]}]}})", "application/merge-patch+json");
    assert(r.status_code == 422);
    r = request(h, api::HttpMethod::Put, base + "/deny-all", policy("deny-all", R"({"podSelector":{"matchLabels":{"app":"x"}},"policyTypes":["Egress"]})"));
    assert(r.status_code == 200 && r.body.find("\"app\":\"x\"") != std::string::npos);
    assert(request(h, api::HttpMethod::Put, base + "/deny-all", policy("deny-all", R"({"policyTypes":["Up"]})")).status_code == 422);

    // Delete
    r = request(h, api::HttpMethod::Delete, base + "/deny-all");
    assert(r.status_code == 200);
    got = read_until(sv[1], "\"type\":\"DELETED\"", 2000);
    assert(got.find("\"type\":\"DELETED\"") != std::string::npos && got.find("\"deny-all\"") != std::string::npos);
    assert(request(h, api::HttpMethod::Get, base + "/deny-all").status_code == 404);
    assert(request(h, api::HttpMethod::Delete, base + "/deny-all").status_code == 404);

    ::close(sv[1]);
    h.set_watch_hub(nullptr);
    hub.stop();
    std::cout << "[PASS] test_api_resource" << std::endl;
}

// ---------------------------------------------------------------------------
// A worker: the kubelet keeps the policies, the calls are checked against them
// ---------------------------------------------------------------------------
class InProcessClient : public kubelet::KubeApiClient {
public:
    explicit InProcessClient(api::RouteHandler& h) : h_(h) {}
    bool register_node(std::string_view, bool& conflict) override { conflict = false; return true; }
    bool patch_node_status(const std::string&, std::string_view) override { return true; }
    bool list_pods(std::vector<models::KubePod>&) override { return true; }
    bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out) override {
        std::string body;
        if (get("/api/v1/pods", body) != 200) return false;
        models::JsonValue doc = json(body);
        for (const auto& item : doc.get("items")->arr_val) {
            std::string j = item.to_string();
            out.emplace_back(models::parse_pod_manifest(j), j);
        }
        return true;
    }
    bool patch_pod_status(const std::string&, const std::string&, const std::string&) override { return true; }
    int get(const std::string& path, std::string& body) override {
        if (path.find("/networkpolicies") != std::string::npos) ++policy_lists;
        if (policies_missing && path.find("/networkpolicies") != std::string::npos) { body = "{}"; return 404; }
        api::HttpResponse r = request(h_, api::HttpMethod::Get, path);
        body = r.body;
        return r.status_code;
    }
    int policy_lists{0};
    bool policies_missing{false};
private:
    api::RouteHandler& h_;
};

class IdleRuntime : public runtime::Runtime {
public:
    bool deploy_workload(const std::string&, const uint8_t*, size_t, uint32_t) override { return true; }
    bool deploy_workload_ex(const std::string&, const uint8_t*, size_t, const runtime::DeploySpec&, std::string&) override { return true; }
    bool start(const std::string&) override { return true; }
    bool pause(const std::string&) override { return true; }
    bool terminate(const std::string&) override { return true; }
    runtime::ContainerStatus get_status(const std::string&) override { return runtime::ContainerStatus(); }
};

std::string pod(const std::string& ns, const std::string& name, const std::string& node, const std::string& labels, const std::string& phase,
                const std::string& ports = "[]") {
    return "{\"apiVersion\":\"v1\",\"kind\":\"Pod\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"" + ns + "\",\"uid\":\"u-" + name +
           "\",\"labels\":" + labels + "},\"spec\":{\"nodeName\":\"" + node + "\",\"containers\":[{\"name\":\"c\",\"image\":\"wasm://x\",\"ports\":" +
           ports + "}]},\"status\":{\"phase\":\"" + phase + "\",\"podIP\":\"127.0.0.1\",\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}]}}";
}

int32_t call(app::ServiceClient& c, const std::string& caller, const std::string& url) {
    runtime::ServiceCallRequest req;
    req.caller_pod = caller;
    req.method = "GET";
    req.url = url;
    req.call_chain = caller;
    req.timeout_ms = 2000;
    uint8_t buf[256];
    runtime::BufferResponseSink sink(buf, sizeof buf);
    return c.call(req, sink);
}

void test_worker_view() {
    std::cout << "[RUN] test_worker_view" << std::endl;
    auto store = mem_store();
    api::RouteHandler h(*store);
    const std::string base = "/apis/networking.k8s.io/v1/namespaces/default/networkpolicies";
    // client-0 runs here (Succeeded: listed, never started by this test's
    // kubelet); web-0 runs on another node behind Service "web", and nothing
    // listens on its port: a call the policies allow fails to connect (I/O
    // error), one they deny never tries (K32_ERR_PERM)
    store->put("/registry/pods/default/client-0", pod("default", "client-0", "node-a", "{\"app\":\"client\"}", "Succeeded"), true);
    store->put("/registry/pods/default/web-0", pod("default", "web-0", "node-b", "{\"app\":\"web\"}", "Running", "[{\"name\":\"http\",\"containerPort\":1}]"), true);
    store->put("/registry/services/default/web",
               "{\"apiVersion\":\"v1\",\"kind\":\"Service\",\"metadata\":{\"name\":\"web\",\"namespace\":\"default\"},"
               "\"spec\":{\"selector\":{\"app\":\"web\"},\"ports\":[{\"name\":\"http\",\"port\":80,\"targetPort\":\"http\"}]}}", true);

    auto policies = std::make_shared<net::NodeNetworkPolicies>();
    kubelet::KubeletConfig cfg;
    cfg.node_name = "node-a";
    cfg.network_policies = policies;
    cfg.network_policy_refresh_ms = 300;
    IdleRuntime rt;
    auto owned = std::make_unique<InProcessClient>(h);
    InProcessClient* api_client = owned.get();
    kubelet::Kubelet k(cfg, rt, std::move(owned));
    auto api_get = [api_client](const std::string& path, std::string& body) { return api_client->get(path, body); };
    app::ServiceClient client(app::make_api_service_resolver(api_get, 0), app::ServiceClientOptions{},
                              app::make_node_egress_source(policies, api_get, 0));
    const std::string out = "http://127.0.0.1:1/";
    const std::string svc = "http://web/";

    // Before the first sync the node knows nothing: denied. The first sync
    // lists every namespace's policies once the refresh interval has passed
    // since the start.
    assert(call(client, "default/client-0", out) == K32_ERR_PERM);
    osal::Thread::sleep_ms(350);
    k.sync_assigned_pods();
    net::LabelMap labels;
    std::shared_ptr<const net::NamespacePolicies> ns_policies;
    assert(policies->lookup("default/client-0", labels, ns_policies) && labels.size() == 1 && labels[0].second == "client");
    assert(ns_policies && ns_policies->empty());
    assert(!policies->lookup("default/web-0", labels, ns_policies));   // another node's pod
    assert(call(client, "default/client-0", out) == K32_ERR_CONNREFUSED);
    assert(call(client, "default/client-0", svc) == K32_ERR_IO);

    // A new policy reaches the node with the next refresh, not before
    assert(request(h, api::HttpMethod::Post, base, policy("lockdown", R"({"podSelector":{"matchLabels":{"app":"client"}},"policyTypes":["Egress"]})")).status_code == 201);
    const int lists = api_client->policy_lists;
    k.sync_assigned_pods();
    assert(api_client->policy_lists == lists);
    assert(call(client, "default/client-0", out) == K32_ERR_CONNREFUSED);
    osal::Thread::sleep_ms(350);
    k.sync_assigned_pods();
    assert(api_client->policy_lists == lists + 1);
    assert(call(client, "default/client-0", out) == K32_ERR_PERM);
    assert(call(client, "default/client-0", svc) == K32_ERR_PERM);
    assert(client.stats().denied.load() >= 3);

    // Pods behind the Service on their named port: the target pod is read
    // from the API server
    assert(request(h, api::HttpMethod::Post, base, policy("to-web", R"({"podSelector":{"matchLabels":{"app":"client"}},"policyTypes":["Egress"],
        "egress":[{"to":[{"podSelector":{"matchLabels":{"app":"web"}}}],"ports":[{"port":"http"}]}]})")).status_code == 201);
    osal::Thread::sleep_ms(350);
    k.sync_assigned_pods();
    assert(call(client, "default/client-0", svc) == K32_ERR_IO);
    assert(call(client, "default/client-0", out) == K32_ERR_PERM);

    // An address block for the outside
    assert(request(h, api::HttpMethod::Post, base, policy("loopback", R"({"podSelector":{"matchLabels":{"app":"client"}},"policyTypes":["Egress"],
        "egress":[{"to":[{"ipBlock":{"cidr":"127.0.0.0/8"}}],"ports":[{"port":1}]}]})")).status_code == 201);
    osal::Thread::sleep_ms(350);
    k.sync_assigned_pods();
    assert(call(client, "default/client-0", out) == K32_ERR_CONNREFUSED);
    assert(request(h, api::HttpMethod::Delete, base + "/loopback").status_code == 200);
    osal::Thread::sleep_ms(350);
    k.sync_assigned_pods();
    assert(call(client, "default/client-0", out) == K32_ERR_PERM);

    // New labels count from the next sync: no policy selects the pod then
    store->put("/registry/pods/default/client-0", pod("default", "client-0", "node-a", "{\"app\":\"other\"}", "Succeeded"), true);
    k.sync_assigned_pods();
    assert(call(client, "default/client-0", out) == K32_ERR_CONNREFUSED);

    // A namespace new to the node is loaded in the same sync
    store->put("/registry/pods/team/t-0", pod("team", "t-0", "node-a", "{}", "Succeeded"), true);
    assert(request(h, api::HttpMethod::Post, "/apis/networking.k8s.io/v1/namespaces/team/networkpolicies",
                   policy("closed", R"({"podSelector":{},"policyTypes":["Egress"]})", "team")).status_code == 201);
    k.sync_assigned_pods();
    assert(policies->lookup("team/t-0", labels, ns_policies) && ns_policies->size() == 1);
    assert(call(client, "team/t-0", out) == K32_ERR_PERM);
    assert(policies->namespaces(false).size() == 2 && policies->policy_count() == 3);

    // A pod gone from the node is forgotten, and so is a namespace without pods
    store->remove("/registry/pods/team/t-0");
    k.sync_assigned_pods();
    assert(!policies->lookup("team/t-0", labels, ns_policies));
    assert(call(client, "team/t-0", out) == K32_ERR_PERM);
    assert(policies->namespaces(false).size() == 1);

    // An API server without NetworkPolicy has none to enforce
    api_client->policies_missing = true;
    store->put("/registry/pods/old/o-0", pod("old", "o-0", "node-a", "{}", "Succeeded"), true);
    k.sync_assigned_pods();
    assert(policies->lookup("old/o-0", labels, ns_policies) && ns_policies->empty());
    assert(call(client, "old/o-0", out) == K32_ERR_CONNREFUSED);
    std::cout << "[PASS] test_worker_view" << std::endl;
}

// Name lookups through the platform resolver: an address, a name the system
// knows, a name that cannot exist
void test_resolver() {
    std::cout << "[RUN] test_resolver" << std::endl;
    auto never = []() { return false; };
    uint32_t addr = 0;
    assert(osal::Socket::resolve_ipv4("localhost", 5000, never, addr) == 0 && addr == ip("127.0.0.1"));
    assert(osal::Socket::resolve_ipv4("127.0.0.1", 5000, never, addr) == 0 && addr == ip("127.0.0.1"));
    const int rc = osal::Socket::resolve_ipv4("no-such-host.invalid", 5000, never, addr);
    assert(rc == ENOENT || rc == ETIMEDOUT);
    auto stop = []() { return true; };
    const int stopped = osal::Socket::resolve_ipv4("no-such-host-2.invalid", 5000, stop, addr);
    assert(stopped == ECANCELED || stopped == ENOENT);
    // Connect: refused at once, and an abandoned attempt gives up
    osal::Socket s;
    assert(s.create(osal::SocketType::Tcp));
    const int err = s.connect_within(ip("127.0.0.1"), 1, 1000, never);
    assert(err == ECONNREFUSED);
    std::cout << "[PASS] test_resolver" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== NetworkPolicy egress ===" << std::endl;
    osal::Socket::init_networking();
    test_url_resolution();
    test_selectors_and_addresses();
    test_egress_evaluation();
    test_validation_and_defaults();
    test_api_resource();
    test_worker_view();
    test_resolver();
    std::cout << "[PASS] NetworkPolicy egress" << std::endl;
    return 0;
}
