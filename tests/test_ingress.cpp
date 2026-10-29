// Ingress controller: networking.k8s.io/v1 Ingress routing (hosts, Exact and
// Prefix paths, longest match, defaultBackend, 404), Service resolution
// (selector, Running pods, numeric and named target ports), static bundles
// (gzip, single-page fallback, sub-path mounts), event streams fed by channel
// topics, proxying to a pod server on another address, and channel topics
// delivered to a pod's k32_event. The controller, the echo pod and the
// embedded bundles are the same code and bytes the firmware runs.
#include "app/ingress_controller.hpp"
#include "app/ingress_routes.hpp"
#include "app/pod_server.hpp"
#include "api/http_server.hpp"
#include "api/route_handler.hpp"
#include "api/table_serializer.hpp"
#include "embedded/embedded_blobs.hpp"
#include "ipc/channel.hpp"
#include "mem/monotonic_arena.hpp"
#include "osal/socket.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"
#include "runtime/app_pod.hpp"
#include "runtime/image_registry.hpp"
#include "runtime/pod_log.hpp"
#include "runtime/wasm_runtime.hpp"
#include "storage/store.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

using namespace kube32;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
static std::string ingress_json(const std::string& name, const std::string& annotations, const std::string& spec,
                                const std::string& ns = "default") {
    return "{\"apiVersion\":\"networking.k8s.io/v1\",\"kind\":\"Ingress\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"" + ns +
           "\",\"annotations\":{" + annotations + "}},\"spec\":" + spec + "}";
}

static std::string service_json(const std::string& name, const std::string& selector, const std::string& ports) {
    return "{\"apiVersion\":\"v1\",\"kind\":\"Service\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\"},"
           "\"spec\":{\"selector\":{" + selector + "},\"ports\":" + ports + "}}";
}

static std::string pod_json(const std::string& name, const std::string& labels, const std::string& phase, const std::string& ip,
                            const std::string& ports, bool terminating = false) {
    return "{\"apiVersion\":\"v1\",\"kind\":\"Pod\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\",\"labels\":{" + labels + "}" +
           (terminating ? ",\"deletionTimestamp\":\"2026-10-28T10:00:00Z\"" : "") + "},"
           "\"spec\":{\"containers\":[{\"name\":\"c\",\"image\":\"wasm://embedded/echo\",\"ports\":" + ports + "}]},"
           "\"status\":{\"phase\":\"" + phase + "\",\"podIP\":\"" + ip + "\"}}";
}

static std::string backend_service(const std::string& name, const std::string& port) {
    return "{\"service\":{\"name\":\"" + name + "\",\"port\":" + port + "}}";
}

static std::string backend_bundle(const std::string& name) {
    return std::string("{\"resource\":{\"apiGroup\":\"kube32.io\",\"kind\":\"StaticBundle\",\"name\":\"") + name + "\"}}";
}

static std::string path_rule(const std::string& path, const std::string& type, const std::string& backend) {
    return "{\"path\":\"" + path + "\",\"pathType\":\"" + type + "\",\"backend\":" + backend + "}";
}

// ---------------------------------------------------------------------------
// Pure routing
// ---------------------------------------------------------------------------
static void test_path_and_host_matching() {
    std::cout << "[RUN] test_path_and_host_matching" << std::endl;
    using app::PathMatch;
    assert(app::path_matches(PathMatch::Prefix, "/", "/"));
    assert(app::path_matches(PathMatch::Prefix, "/", "/anything/at/all"));
    assert(app::path_matches(PathMatch::Prefix, "/api", "/api"));
    assert(app::path_matches(PathMatch::Prefix, "/api", "/api/"));
    assert(app::path_matches(PathMatch::Prefix, "/api", "/api/issues"));
    assert(!app::path_matches(PathMatch::Prefix, "/api", "/apix"));
    assert(!app::path_matches(PathMatch::Prefix, "/api/v1", "/api"));
    assert(app::path_matches(PathMatch::Exact, "/events", "/events"));
    assert(!app::path_matches(PathMatch::Exact, "/events", "/events/"));
    assert(!app::path_matches(PathMatch::Exact, "/events", "/events/x"));

    assert(app::normalize_host("Example.COM:8080") == "example.com");
    assert(app::normalize_host("192.0.2.10") == "192.0.2.10");
    assert(app::normalize_host("[::1]:80") == "[::1]");
    assert(app::normalize_host("") == "");
    std::cout << "[PASS] test_path_and_host_matching" << std::endl;
}

static std::shared_ptr<app::RouteTable> sample_table() {
    std::vector<std::string> ingresses = {
        ingress_json("site",
                     "\"kube32.io/event-stream\":\"/events=site.events,kube32.cluster,kube32.presence\","
                     "\"kube32.io/cluster-telemetry\":\"/api/cluster\",\"kube32.io/stats-topic\":\"site.events\"",
                     "{\"rules\":[{\"http\":{\"paths\":[" +
                         path_rule("/api", "Prefix", backend_service("core", "{\"name\":\"http\"}")) + "," +
                         path_rule("/", "Prefix", backend_bundle("tessera-web")) + "]}}]}"),
        ingress_json("hosts", "",
                     "{\"rules\":[{\"host\":\"echo.example.com\",\"http\":{\"paths\":[" +
                         path_rule("/only", "Exact", backend_service("echo", "{\"number\":80}")) + "]}},"
                     "{\"host\":\"*.wild.example.com\",\"http\":{\"paths\":[" +
                         path_rule("/", "ImplementationSpecific", backend_service("echo", "{\"number\":80}")) + "]}}]}"),
        ingress_json("other-class", "", "{\"ingressClassName\":\"nginx\",\"rules\":[{\"http\":{\"paths\":[" +
                                            path_rule("/nginx", "Prefix", backend_service("echo", "{\"number\":80}")) + "]}}]}"),
        ingress_json("ties", "",
                     "{\"rules\":[{\"http\":{\"paths\":[" + path_rule("/tie", "Prefix", backend_service("echo", "{\"number\":80}")) + "," +
                         path_rule("/tie", "Exact", backend_bundle("echo")) + "," +
                         path_rule("/docs/", "Prefix", backend_bundle("echo")) + "]}}]}"),
    };
    std::vector<std::string> services = {
        service_json("core", "\"app\":\"site\",\"tier\":\"core\"", "[{\"name\":\"http\",\"port\":80,\"targetPort\":\"http\"}]"),
        service_json("echo", "\"app\":\"echo\"", "[{\"name\":\"web\",\"port\":80,\"targetPort\":7000}]"),
    };
    std::vector<std::string> pods = {
        pod_json("core-0", "\"app\":\"site\",\"tier\":\"core\"", "Running", "10.0.0.1", "[{\"name\":\"http\",\"containerPort\":7001}]"),
        pod_json("core-pending", "\"app\":\"site\",\"tier\":\"core\"", "Pending", "", "[{\"name\":\"http\",\"containerPort\":7001}]"),
        pod_json("core-old", "\"app\":\"site\",\"tier\":\"core\"", "Running", "10.0.0.9", "[{\"name\":\"http\",\"containerPort\":7001}]", true),
        pod_json("echo-1", "\"app\":\"echo\"", "Running", "10.0.0.3", "[]"),
        pod_json("echo-0", "\"app\":\"echo\"", "Running", "10.0.0.2", "[]"),
        pod_json("unrelated", "\"app\":\"other\"", "Running", "10.0.0.4", "[]"),
    };
    return app::build_route_table(ingresses, services, pods, nullptr);
}

static void test_route_table() {
    std::cout << "[RUN] test_route_table" << std::endl;
    auto t = sample_table();
    assert(t->ingresses.size() == 3);   // the nginx-class Ingress belongs to another controller

    const app::IngressRoute* r = t->match("192.0.2.10", "/api/issues");
    assert(r && r->kind == app::BackendKind::Service && r->service == "default/core");
    // Named targetPort resolved through the pod's containerPort; only the Running, not terminating pod
    assert(r->endpoints.size() == 1);
    assert(r->endpoints[0].pod == "default/core-0" && r->endpoints[0].ip == "10.0.0.1" && r->endpoints[0].port == 7001);

    r = t->match("192.0.2.10", "/apix");
    assert(r && r->kind == app::BackendKind::StaticBundle && r->bundle == "tessera-web");
    r = t->match("", "/");
    assert(r && r->kind == app::BackendKind::StaticBundle);

    // Annotation routes: Exact, longer than "/api" so they win
    r = t->match("x", "/api/cluster");
    assert(r && r->kind == app::BackendKind::Telemetry);
    r = t->match("x", "/api/cluster/x");
    assert(r && r->kind == app::BackendKind::Service);
    r = t->match("x", "/events");
    assert(r && r->kind == app::BackendKind::EventStream && r->stream_id != 0);
    assert(r->topics.size() == 3 && r->topics[0] == "site.events");
    assert(t->channel_topics.size() == 1 && t->channel_topics[0] == "site.events");
    assert(t->ingresses[r->ingress].stats_topic == "site.events");

    // Exact beats Prefix at equal length; a trailing slash in a Prefix rule is ignored
    r = t->match("x", "/tie");
    assert(r && r->kind == app::BackendKind::StaticBundle);
    r = t->match("x", "/tie/more");
    assert(r && r->kind == app::BackendKind::Service);
    r = t->match("x", "/docs");
    assert(r && r->kind == app::BackendKind::StaticBundle && r->path == "/docs");

    // Host rules: the exact host's rules only, then wildcard, then hostless
    r = t->match("Echo.Example.com:80", "/only");
    assert(r && r->kind == app::BackendKind::Service && r->service == "default/echo");
    assert(r->endpoints.size() == 2 && r->endpoints[0].pod == "default/echo-0" && r->endpoints[0].port == 7000);
    r = t->match("echo.example.com", "/api/issues");
    assert(r == nullptr);
    r = t->match("a.wild.example.com", "/whatever");
    assert(r && r->service == "default/echo");
    r = t->match("a.b.wild.example.com", "/api/x");
    assert(r && r->service == "default/core");   // two labels: not the wildcard, falls to hostless rules

    // Ingress of another class is not served
    r = t->match("x", "/nginx");
    assert(r && r->kind == app::BackendKind::StaticBundle);   // "/" of the site Ingress, not /nginx

    std::cout << "[PASS] test_route_table" << std::endl;
}

static void test_default_backend_and_errors() {
    std::cout << "[RUN] test_default_backend_and_errors" << std::endl;
    std::vector<std::string> ingresses = {
        ingress_json("only-api", "", "{\"rules\":[{\"http\":{\"paths\":[" +
                                         path_rule("/api", "Prefix", backend_service("missing", "{\"number\":80}")) + "," +
                                         path_rule("/port", "Prefix", backend_service("svc", "{\"number\":81}")) + "," +
                                         path_rule("/nopods", "Prefix", backend_service("svc", "{\"number\":80}")) + "," +
                                         path_rule("/other", "Prefix", "{\"resource\":{\"apiGroup\":\"example.com\",\"kind\":\"Bucket\",\"name\":\"b\"}}") +
                                         "]}}]}"),
    };
    std::vector<std::string> services = {service_json("svc", "\"app\":\"nobody\"", "[{\"port\":80}]")};
    auto t = app::build_route_table(ingresses, services, {}, nullptr);
    assert(t->match("x", "/") == nullptr);   // nothing matches, no defaultBackend: 404
    const app::IngressRoute* r = t->match("x", "/api/x");
    assert(r && r->endpoints.empty() && r->unavailable.find("not found") != std::string::npos);
    r = t->match("x", "/port");
    assert(r && r->endpoints.empty() && r->unavailable.find("no matching port") != std::string::npos);
    r = t->match("x", "/nopods");
    assert(r && r->endpoints.empty() && r->unavailable.find("no running pod") != std::string::npos);
    assert(t->match("x", "/other") == nullptr);   // unsupported resource backends are skipped

    ingresses.push_back(ingress_json("fallback", "", "{\"defaultBackend\":" + backend_bundle("tessera-web") + "}"));
    t = app::build_route_table(ingresses, services, {}, nullptr);
    r = t->match("x", "/");
    assert(r && r->kind == app::BackendKind::StaticBundle && r->bundle == "tessera-web");
    r = t->match("x", "/api/still-the-rule");
    assert(r && r->kind == app::BackendKind::Service);

    // Counters survive a rebuild of the same Ingress
    t->ingresses[0].counters->requests.store(5);
    auto t2 = app::build_route_table(ingresses, services, {}, t.get());
    assert(t2->ingresses[0].counters->requests.load() == 5);
    std::cout << "[PASS] test_default_backend_and_errors" << std::endl;
}

static void test_bundle_paths() {
    std::cout << "[RUN] test_bundle_paths" << std::endl;
    app::IngressRoute root;
    root.path = "/";
    root.match = app::PathMatch::Prefix;
    assert(app::bundle_relative_path(root, "/") == "/");
    assert(app::bundle_relative_path(root, "/app.js") == "/app.js");
    app::IngressRoute sub;
    sub.path = "/echo/ui";
    sub.match = app::PathMatch::Prefix;
    assert(app::bundle_relative_path(sub, "/echo/ui") == "/");
    assert(app::bundle_relative_path(sub, "/echo/ui/") == "/");
    assert(app::bundle_relative_path(sub, "/echo/ui/index.html") == "/index.html");
    std::cout << "[PASS] test_bundle_paths" << std::endl;
}

// ---------------------------------------------------------------------------
// API server: Ingress objects through the Kubernetes API
// ---------------------------------------------------------------------------
static api::HttpResponse call(api::RouteHandler& h, api::HttpMethod m, const std::string& uri, const std::string& body = "",
                              bool table = false) {
    api::HttpRequest req;
    req.method = m;
    req.raw_uri = uri;
    api::normalize_uri(uri, req.path, req.query_string);
    req.body = body;
    req.as_table = table;
    mem::MonotonicArena arena;
    return h.handle_request(req, arena);
}

static void test_ingress_api() {
    std::cout << "[RUN] test_ingress_api" << std::endl;
    auto store = storage::create_radix_store(storage::StoreOptions{"/tmp/kube32_test_ingress_api", false});
    api::RouteHandler h(*store);

    api::HttpResponse r = call(h, api::HttpMethod::Get, "/apis");
    assert(r.body.find("\"networking.k8s.io\"") != std::string::npos);
    r = call(h, api::HttpMethod::Get, "/apis/networking.k8s.io/v1");
    assert(r.status_code == 200 && r.body.find("\"ingresses\"") != std::string::npos && r.body.find("\"ing\"") != std::string::npos);
    r = call(h, api::HttpMethod::Get, "/openapi/v3");
    assert(r.body.find("apis/networking.k8s.io/v1") != std::string::npos);
    r = call(h, api::HttpMethod::Get, "/openapi/v3/apis/networking.k8s.io/v1");
    assert(r.body.find("\"kind\":\"Ingress\"") != std::string::npos && r.body.find("fieldValidation") != std::string::npos);

    std::string ing = ingress_json("web", "", "{\"rules\":[{\"host\":\"a.example\",\"http\":{\"paths\":[" +
                                                  path_rule("/", "Prefix", backend_bundle("tessera-web")) + "]}},{\"http\":{\"paths\":[" +
                                                  path_rule("/", "Prefix", backend_bundle("tessera-web")) + "]}}]}");
    r = call(h, api::HttpMethod::Post, "/apis/networking.k8s.io/v1/namespaces/default/ingresses?fieldValidation=Strict", ing);
    assert(r.status_code == 201);
    r = call(h, api::HttpMethod::Post, "/apis/networking.k8s.io/v1/namespaces/default/ingresses", ing);
    assert(r.status_code == 409);
    r = call(h, api::HttpMethod::Post, "/apis/networking.k8s.io/v1/namespaces/default/ingresses?fieldValidation=Strict",
             "{\"kind\":\"Ingress\",\"metadata\":{\"name\":\"x\"},\"spek\":{}}");
    assert(r.status_code == 400);
    r = call(h, api::HttpMethod::Patch, "/apis/networking.k8s.io/v1/namespaces/default/ingresses/web",
             "{\"metadata\":{\"annotations\":{\"kube32.io/stats-topic\":\"t\"}}}");
    assert(r.status_code == 200 && r.body.find("kube32.io/stats-topic") != std::string::npos);
    r = call(h, api::HttpMethod::Get, "/apis/networking.k8s.io/v1/ingresses", "", true);
    assert(r.status_code == 200 && r.body.find("\"HOSTS\"") != std::string::npos && r.body.find("a.example,*") != std::string::npos);
    r = call(h, api::HttpMethod::Get, "/apis/networking.k8s.io/v1/namespaces/default/ingresses/web", "", true);
    assert(r.body.find("\"kind\":\"Table\"") != std::string::npos && r.body.find("\"ADDRESS\"") != std::string::npos);

    // Services take PATCH (kubectl apply on an existing object)
    r = call(h, api::HttpMethod::Post, "/api/v1/namespaces/default/services", service_json("svc", "\"app\":\"a\"", "[{\"port\":80}]"));
    assert(r.status_code == 201);
    r = call(h, api::HttpMethod::Patch, "/api/v1/namespaces/default/services/svc", "{\"spec\":{\"selector\":{\"app\":\"b\"}}}");
    assert(r.status_code == 200 && r.body.find("\"app\":\"b\"") != std::string::npos);

    r = call(h, api::HttpMethod::Delete, "/apis/networking.k8s.io/v1/namespaces/default/ingresses/web");
    assert(r.status_code == 200);
    r = call(h, api::HttpMethod::Get, "/apis/networking.k8s.io/v1/namespaces/default/ingresses/web");
    assert(r.status_code == 404);
    std::cout << "[PASS] test_ingress_api" << std::endl;
}

// ---------------------------------------------------------------------------
// End to end over sockets
// ---------------------------------------------------------------------------
struct HttpResult {
    int status{0};
    std::string head;
    std::string body;
    bool has_header(const std::string& line) const { return head.find(line) != std::string::npos; }
};

static std::string dechunk(const std::string& in) {
    std::string out;
    size_t pos = 0;
    while (pos < in.size()) {
        size_t eol = in.find("\r\n", pos);
        if (eol == std::string::npos) break;
        size_t n = std::strtoul(in.c_str() + pos, nullptr, 16);
        if (n == 0) break;
        out.append(in, eol + 2, n);
        pos = eol + 2 + n + 2;
    }
    return out;
}

static HttpResult http(uint16_t port, const std::string& method, const std::string& path, const std::string& extra_headers = "",
                       const std::string& body = "") {
    HttpResult res;
    osal::Socket s;
    assert(s.create(osal::SocketType::Tcp));
    assert(s.connect("127.0.0.1", port, 2000));
    s.set_timeout(5000, 5000);
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n" + extra_headers +
                      "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    assert(s.send_all(req.data(), req.size()));
    std::string raw;
    char buf[4096];
    while (true) {
        ssize_t n = s.recv(buf, sizeof(buf));
        if (n <= 0) break;
        raw.append(buf, static_cast<size_t>(n));
    }
    size_t he = raw.find("\r\n\r\n");
    if (he == std::string::npos) return res;
    res.head = raw.substr(0, he + 2);
    res.body = raw.substr(he + 4);
    res.status = std::atoi(raw.c_str() + 9);
    if (res.has_header("Transfer-Encoding: chunked")) res.body = dechunk(res.body);
    return res;
}

// Pod server stand-in on "another node": answers with the pod and URI it receives
class RecordingHandler : public api::HttpHandler {
public:
    bool handle(api::HttpConnection& conn) override {
        const api::HttpRequest& req = conn.request();
        return conn.send_simple(200, "text/plain", "pod=" + req.target_pod + " uri=" + req.raw_uri);
    }
};

static void put(storage::Store& store, const std::string& key, const std::string& json) {
    assert(store.put(key, json, true).ok());
}

static bool wait_for(const std::function<bool()>& cond, uint32_t ms) {
    uint64_t end = osal::System::get_uptime_ms() + ms;
    while (osal::System::get_uptime_ms() < end) {
        if (cond()) return true;
        osal::Thread::sleep_ms(20);
    }
    return cond();
}

static void test_controller_end_to_end() {
    std::cout << "[RUN] test_controller_end_to_end" << std::endl;
    const uint16_t base = static_cast<uint16_t>(21000 + (getpid() % 2000) * 4);
    const uint16_t ingress_port = base;
    const uint16_t remote_port = base + 1;

    ipc::ChannelManager::instance().start_background(0, 65536);
    app::install_default_platform_hooks();

    // A local request-handling pod (the embedded echo image) subscribed to echo.events
    size_t n = 0;
    const runtime::EmbeddedImage* imgs = embedded::images(n);
    runtime::ImageRegistry::instance().add_all(imgs, n);
    const runtime::EmbeddedImage* echo = runtime::ImageRegistry::instance().resolve("wasm://embedded/echo");
    assert(echo);
    auto rt = runtime::create_wasm_runtime();
    runtime::DeploySpec spec;
    spec.memory_limit_bytes = 64 * 1024;
    spec.event_topics = {"echo.events"};
    std::string err;
    assert(rt->deploy_workload_ex("default/echo-0", echo->data, echo->len, spec, err));
    assert(runtime::AppPodHost::instance().pod_handles_requests("default/echo-0"));

    // A pod server on another address (same host, its own port)
    api::HttpServerOptions ro;
    ro.port = remote_port;
    ro.workers = 1;
    RecordingHandler recorder;
    api::HttpServer remote(ro, static_cast<storage::Store*>(nullptr), nullptr);
    remote.set_handler(&recorder);
    assert(remote.start());

    auto store = storage::create_radix_store(storage::StoreOptions{"/tmp/kube32_test_ingress_e2e", false});
    put(*store, "/registry/services/default/echo", service_json("echo", "\"app\":\"echo\"", "[{\"name\":\"http\",\"port\":80,\"targetPort\":\"http\"}]"));
    put(*store, "/registry/pods/default/echo-0", pod_json("echo-0", "\"app\":\"echo\"", "Running", "127.0.0.1", "[{\"name\":\"http\",\"containerPort\":7000}]"));
    put(*store, "/registry/services/default/remote", service_json("remote", "\"app\":\"remote\"", "[{\"port\":80,\"targetPort\":" + std::to_string(remote_port) + "}]"));
    put(*store, "/registry/pods/default/remote-0", pod_json("remote-0", "\"app\":\"remote\"", "Running", "127.0.0.1", "[]"));
    put(*store, "/registry/ingresses/default/site",
        ingress_json("site", "\"kube32.io/cluster-telemetry\":\"/api/cluster\"",
                     "{\"rules\":[{\"http\":{\"paths\":[" + path_rule("/", "Prefix", backend_bundle("tessera-web")) + "]}}]}"));
    put(*store, "/registry/ingresses/default/echo",
        ingress_json("echo", "\"kube32.io/event-stream\":\"/echo/events=echo.events,kube32.presence\",\"kube32.io/stats-topic\":\"echo.stats\"",
                     "{\"rules\":[{\"http\":{\"paths\":[" + path_rule("/echo", "Prefix", backend_service("echo", "{\"name\":\"http\"}")) + "," +
                         path_rule("/echo/ui", "Prefix", backend_bundle("echo")) + "," +
                         path_rule("/remote", "Prefix", backend_service("remote", "{\"number\":80}")) + "]}}]}"));

    app::IngressControllerOptions io;
    io.store = store.get();
    io.local_node_name = "esp32-node-01";
    io.address = []() { return std::string("127.0.0.1"); };
    io.port = ingress_port;
    io.workers = 2;
    io.worker_stack = 262144;
    app::IngressController ingress(io);
    assert(ingress.start());

    // Local pod: direct k32_handle call, full path forwarded
    HttpResult r = http(ingress_port, "GET", "/echo/hello?x=1");
    assert(r.status == 200);
    assert(r.body.find("\"pod\":\"echo\"") != std::string::npos);
    assert(r.body.find("GET /echo/hello?x=1") != std::string::npos);

    // Pod on another address: proxied with X-Kube32-Pod
    r = http(ingress_port, "GET", "/remote/a/b?q=2");
    assert(r.status == 200);
    assert(r.body == "pod=default/remote-0 uri=/remote/a/b?q=2");

    // Static bundle: gzip-encoded files keep their encoding
    r = http(ingress_port, "GET", "/", "Accept-Encoding: gzip\r\n");
    assert(r.status == 200 && r.has_header("Content-Encoding: gzip") && r.has_header("text/html"));
    assert(r.body.size() > 2 && static_cast<unsigned char>(r.body[0]) == 0x1f && static_cast<unsigned char>(r.body[1]) == 0x8b);
    r = http(ingress_port, "GET", "/app.js", "Accept-Encoding: gzip\r\n");
    assert(r.status == 200 && r.has_header("Content-Encoding: gzip") && r.has_header("application/javascript"));
    r = http(ingress_port, "GET", "/issues/12", "Accept-Encoding: gzip\r\n");   // client-side route: index.html
    assert(r.status == 200 && r.has_header("text/html"));
    r = http(ingress_port, "GET", "/missing.js", "Accept-Encoding: gzip\r\n");
    assert(r.status == 404);
    r = http(ingress_port, "GET", "/");
    assert(r.status == 406);
    // A plain (not gzip) file in a bundle mounted below "/"
    r = http(ingress_port, "GET", "/echo/ui/");
    assert(r.status == 200 && !r.has_header("Content-Encoding") && r.body.find("<title>echo</title>") != std::string::npos);
    r = http(ingress_port, "POST", "/echo/ui/");
    assert(r.status == 405);

    // Telemetry route
    r = http(ingress_port, "GET", "/api/cluster");
    assert(r.status == 200 && r.body.find("\"ingress\"") != std::string::npos);

    // Event stream: a message on echo.events reaches the SSE client, named by its "type"
    osal::Socket sse;
    assert(sse.create(osal::SocketType::Tcp));
    assert(sse.connect("127.0.0.1", ingress_port, 2000));
    sse.set_timeout(3000, 3000);
    std::string sreq = "GET /echo/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    assert(sse.send_all(sreq.data(), sreq.size()));
    std::string stream;
    char buf[2048];
    assert(wait_for([&]() {
        ssize_t got = sse.recv(buf, sizeof(buf));
        if (got > 0) stream.append(buf, static_cast<size_t>(got));
        return stream.find("\"viewers\":1") != std::string::npos;
    }, 3000));
    assert(stream.find("text/event-stream") != std::string::npos);
    const char* ev = "{\"type\":\"echo\",\"n\":7}";
    ipc::ChannelManager::instance().publish("test", "echo.events", reinterpret_cast<const uint8_t*>(ev), std::strlen(ev));
    assert(wait_for([&]() {
        ssize_t got = sse.recv(buf, sizeof(buf));
        if (got > 0) stream.append(buf, static_cast<size_t>(got));
        return stream.find("event: echo\ndata: {\"type\":\"echo\",\"n\":7}") != std::string::npos;
    }, 3000));

    // The same message reached the echo pod's k32_event (kube32.io/event-topics)
    assert(wait_for([]() {
        std::vector<runtime::PodLogRecord> lines;
        runtime::PodLogStore::runtime().tail("default/echo-0", 20, lines);
        for (const auto& l : lines) if (l.text.find("echo got event on echo.events") != std::string::npos) return true;
        return false;
    }, 3000));

    // Stats line on the Ingress's stats topic, and the address in its status
    std::string stats;
    uint32_t sub = ipc::ChannelManager::instance().subscribe("echo.stats", [&stats](std::string_view, uint32_t, const uint8_t* d, size_t len) {
        stats.assign(reinterpret_cast<const char*>(d), len);
    });
    http(ingress_port, "GET", "/echo/x");
    ingress.tick();
    assert(wait_for([&]() { return !stats.empty(); }, 3000));
    assert(stats.find("\"ingress\":\"default/echo\"") != std::string::npos && stats.find("\"viewers\":1") != std::string::npos);
    ipc::ChannelManager::instance().unsubscribe(sub);
    std::string stored;
    assert(store->get("/registry/ingresses/default/echo", stored).ok());
    assert(stored.find("\"loadBalancer\":{\"ingress\":[{\"ip\":\"127.0.0.1\"}]}") != std::string::npos);

    // Pod deleted: 503 while the Service selects nothing
    store->remove("/registry/pods/default/echo-0");
    assert(wait_for([&]() { return http(ingress_port, "GET", "/echo/hello").status == 503; }, 3000));

    // Ingress deleted: its paths fall to what is left (the site's "/" bundle:
    // 404 for a file it does not have, index.html for an extension-less path)
    // and its stream is closed
    store->remove("/registry/ingresses/default/echo");
    assert(wait_for([&]() { return http(ingress_port, "GET", "/echo/hello.json").status == 404; }, 3000));
    r = http(ingress_port, "GET", "/echo/hello", "Accept-Encoding: gzip\r\n");
    assert(r.status == 200 && r.has_header("text/html"));
    ingress.tick();
    assert(wait_for([&]() {
        ssize_t got = sse.recv(buf, sizeof(buf));
        return got == 0;
    }, 3000));
    assert(http(ingress_port, "GET", "/", "Accept-Encoding: gzip\r\n").status == 200);   // the other Ingress still serves
    // Last Ingress deleted: nothing matches
    store->remove("/registry/ingresses/default/site");
    assert(wait_for([&]() { return http(ingress_port, "GET", "/").status == 404; }, 3000));

    sse.close();
    ingress.stop();
    remote.stop();
    rt->terminate("default/echo-0");
    ipc::ChannelManager::instance().shutdown();
    std::cout << "[PASS] test_controller_end_to_end" << std::endl;
}

int main() {
    std::cout << "=== Ingress controller tests ===" << std::endl;
    osal::Socket::init_networking();
    test_path_and_host_matching();
    test_route_table();
    test_default_backend_and_errors();
    test_bundle_paths();
    test_ingress_api();
    test_controller_end_to_end();
    std::cout << "=== All ingress tests passed ===" << std::endl;
    return 0;
}
