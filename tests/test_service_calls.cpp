// k32_http_call: pod-to-pod calls through Services, calls to hosts outside
// the cluster, and NetworkPolicy egress on both (app/service_client.hpp).
//
// Local targets run in-process; remote ones are reached over HTTP, here a
// scripted server standing in for another node's pod HTTP server and for a
// host outside the cluster.

#include "pod_test_util.hpp"
#include "app/service_client.hpp"
#include "app/pod_server.hpp"
#include "api/route_handler.hpp"
#include "controller/device_controller.hpp"
#include "mem/monotonic_arena.hpp"
#include "storage/store.hpp"
#include "runtime/k32_app_abi.h"
#include "runtime/pod_memory_pool.h"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

using namespace kube32;
using k32test::call_pod;

static std::string service_json(const std::string& name, const std::string& app, const std::string& ports) {
    return "{\"apiVersion\":\"v1\",\"kind\":\"Service\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\"},"
           "\"spec\":{\"selector\":{\"app\":\"" + app + "\"},\"ports\":" + ports + "}}";
}

static std::string pod_json(const std::string& name, const std::string& app, const std::string& ip) {
    return "{\"apiVersion\":\"v1\",\"kind\":\"Pod\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\",\"labels\":{\"app\":\"" + app + "\"}},"
           "\"spec\":{\"containers\":[{\"name\":\"c\",\"image\":\"wasm://x\",\"ports\":[{\"name\":\"http\",\"containerPort\":7000}]}]},"
           "\"status\":{\"phase\":\"Running\",\"podIP\":\"" + ip + "\"}}";
}

static int json_int(const std::string& body, const std::string& key) {
    size_t at = body.find("\"" + key + "\":");
    assert(at != std::string::npos);
    return std::atoi(body.c_str() + at + key.size() + 3);
}

// ---------------------------------------------------------------------------
// A node's pod HTTP server, scripted
// ---------------------------------------------------------------------------
struct FakeNode {
    osal::Socket listener;
    uint16_t port{0};
    std::atomic<bool> running{true};
    std::atomic<int> connections{0};
    std::atomic<int> warm{0};
    std::mutex mu;
    std::vector<std::string> heads;   // request heads seen
    std::thread th;

    bool start() {
        if (!listener.create(osal::SocketType::Tcp)) return false;
        listener.set_reuse_addr(true);
        for (uint16_t p = 18700; p < 18800; ++p) {
            if (listener.bind("127.0.0.1", p)) { port = p; break; }
        }
        if (!port || !listener.listen(4)) return false;
        th = std::thread([this]() { run(); });
        return true;
    }
    void stop() {
        running = false;
        listener.shutdown(osal::ShutdownMode::Both);
        listener.close();
        if (th.joinable()) th.join();
    }
    void run() {
        while (running) {
            osal::Socket c = listener.accept();
            if (!c.is_valid()) continue;
            connections++;
            std::thread([this](osal::Socket s) { serve(std::move(s)); }, std::move(c)).detach();
        }
    }
    void serve(osal::Socket s) {
        s.set_timeout(5000, 5000);
        std::string in;
        char buf[2048];
        for (;;) {
            size_t he;
            while ((he = in.find("\r\n\r\n")) == std::string::npos) {
                ssize_t n = s.recv(buf, sizeof buf);
                if (n <= 0) return;
                in.append(buf, static_cast<size_t>(n));
            }
            std::string head = in.substr(0, he);
            size_t cl_at = head.find("Content-Length: ");
            size_t cl = cl_at == std::string::npos ? 0 : std::strtoul(head.c_str() + cl_at + 16, nullptr, 10);
            while (in.size() < he + 4 + cl) {
                ssize_t n = s.recv(buf, sizeof buf);
                if (n <= 0) return;
                in.append(buf, static_cast<size_t>(n));
            }
            std::string body = in.substr(he + 4, cl);
            in.erase(0, he + 4 + cl);
            { std::lock_guard<std::mutex> l(mu); heads.push_back(head); }
            std::string path = head.substr(head.find(' ') + 1);
            path = path.substr(0, path.find(' '));
            std::string out;
            if (path == "/cl") out = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
            else if (path == "/large") out = "HTTP/1.1 200 OK\r\nContent-Length: 20000\r\n\r\n" + std::string(20000, 'x');
            else if (path == "/chunked") out = "HTTP/1.1 201 Created\r\nTransfer-Encoding: chunked\r\n\r\n3;ext=1\r\nhel\r\n2\r\nlo\r\n0\r\n\r\n";
            else if (path == "/echo") out = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
            else if (path == "/slow") { osal::Thread::sleep_ms(1500); out = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"; }
            else if (path == "/warm") {   // a driver before its first reading: 503 twice
                out = warm++ < 2 ? "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 15\r\n\r\nno reading yet\n"
                                 : "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nwarm";
            }
            else if (path == "/close") {
                out = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nuntil-close";
                s.send_all(out.data(), out.size());
                return;
            } else out = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            if (!s.send_all(out.data(), out.size())) return;
        }
    }
    std::string last_head() { std::lock_guard<std::mutex> l(mu); return heads.empty() ? "" : heads.back(); }
};

// A NetworkPolicy request to the API server; returns the HTTP status
static int policy_request(api::RouteHandler& api, api::HttpMethod method, const std::string& name, const std::string& spec = "") {
    api::HttpRequest r;
    r.method = method;
    r.path = "/apis/networking.k8s.io/v1/namespaces/default/networkpolicies" + std::string(method == api::HttpMethod::Post ? "" : "/" + name);
    if (!spec.empty()) {
        r.body = "{\"apiVersion\":\"networking.k8s.io/v1\",\"kind\":\"NetworkPolicy\",\"metadata\":{\"name\":\"" + name + "\"},\"spec\":" + spec + "}";
        r.content_type = method == api::HttpMethod::Patch ? "application/merge-patch+json" : "application/json";
    }
    mem::MonotonicArena arena;
    return api.handle_request(r, arena).status_code;
}

// End to end on the control plane: the svcprobe pod calls a host outside the
// cluster (the scripted server on 127.0.0.1) and Services, with
// NetworkPolicies created through the API server.
static void test_egress(storage::Store& store, FakeNode& node) {
    std::cout << "=== k32_http_call: egress and NetworkPolicy ===" << std::endl;
    api::RouteHandler api(store);
    app::ServiceClientOptions opts;
    // Host names, as a name server would answer them
    opts.resolve_host = [](const std::string& host, uint32_t, uint32_t& out) -> int {
        if (host == "fake.example" || host == "fake.lan") { out = 0x7F000001; return 0; }
        if (host == "slow.example") return ETIMEDOUT;
        return ENOENT;
    };
    app::ServiceClient client(app::make_store_service_resolver(store), opts, app::make_store_egress_source(store));
    app::install_service_client(&client);
    const std::string port = std::to_string(node.port);
    const std::string out = "http://127.0.0.1:" + port;

    // No policy: addresses, names, a <name>.<name> that is no Service
    k32test::Collect c = call_pod("default/a-0", "/call?u=" + out + "/cl");
    std::cout << "[external] " << c.body << std::endl;
    assert(json_int(c.body, "rc") == 200 && c.body.find("\"body\":\"hello\"") != std::string::npos);
    std::string head = node.last_head();
    assert(head.find("GET /cl HTTP/1.1\r\nHost: 127.0.0.1:" + port + "\r\n") == 0);
    assert(head.find("Connection: close") != std::string::npos && head.find("X-Kube32") == std::string::npos);
    c = call_pod("default/a-0", "/call?u=http://fake.example:" + port + "/cl");
    assert(json_int(c.body, "rc") == 200 && node.last_head().find("Host: fake.example:" + port + "\r\n") != std::string::npos);
    c = call_pod("default/a-0", "/call?u=http://fake.lan:" + port + "/cl");
    assert(json_int(c.body, "rc") == 200);
    c = call_pod("default/a-0", "/call?u=http://b.default/hello");   // <service>.<namespace> that is a Service stays in the cluster
    assert(json_int(c.body, "rc") == 200 && c.body.find("hello from b-0") != std::string::npos);
    c = call_pod("default/a-0", "/call?u=http://nosuch.example/");
    assert(json_int(c.body, "rc") == K32_ERR_NOENT);
    c = call_pod("default/a-0", "/call?u=http://slow.example/");
    assert(json_int(c.body, "rc") == K32_ERR_TIMEOUT);
    c = call_pod("default/a-0", "/call?u=https://fake.example/");
    assert(json_int(c.body, "rc") == K32_ERR_NOTSUP);
    c = call_pod("default/a-0", "/call?u=http://127.0.0.1:1/");
    assert(json_int(c.body, "rc") == K32_ERR_CONNREFUSED);
    // Larger than the buffer: the first bytes, and the connection is not read to its end
    c = call_pod("default/a-0", "/call?u=" + out + "/large&cap=1000");
    assert(json_int(c.body, "rc") == K32_ERR_TOOBIG && json_int(c.body, "len") == 1000);
    std::string bin("\x00\x01\xff\r\n\x7f", 6);
    c = call_pod("default/a-0", "/call?u=" + out + "/echo&m=POST", "POST", bin);
    assert(json_int(c.body, "rc") == 200 && json_int(c.body, "len") == 6);
    assert(node.last_head().find("Content-Type: application/octet-stream") != std::string::npos);
    c = call_pod("default/a-0", "/call?u=" + out + "/slow&t=300");
    assert(json_int(c.body, "rc") == K32_ERR_TIMEOUT);
    assert(client.stats().external.load() >= 9);

    // A policy selecting a with no egress rule: nothing, in the cluster or out of it
    assert(policy_request(api, api::HttpMethod::Post, "lockdown", R"({"podSelector":{"matchLabels":{"app":"a"}},"policyTypes":["Egress"]})") == 201);
    c = call_pod("default/a-0", "/call?u=" + out + "/cl");
    std::cout << "[denied] " << c.body << std::endl;
    assert(json_int(c.body, "rc") == K32_ERR_PERM);
    c = call_pod("default/a-0", "/call?u=http://b/hello");
    assert(json_int(c.body, "rc") == K32_ERR_PERM);
    c = call_pod("default/a-0", "/call?u=http://fake.example:" + port + "/cl");
    assert(json_int(c.body, "rc") == K32_ERR_PERM);
    c = call_pod("default/b-0", "/call?u=" + out + "/cl");   // b is not selected
    assert(json_int(c.body, "rc") == 200);

    // The fake host's address block on its port: allowed; b (same address,
    // port 7000) is not
    assert(policy_request(api, api::HttpMethod::Post, "loopback", R"({"podSelector":{"matchLabels":{"app":"a"}},"policyTypes":["Egress"],
        "egress":[{"to":[{"ipBlock":{"cidr":"127.0.0.0/8"}}],"ports":[{"port":)" + port + "}]}]}") == 201);
    c = call_pod("default/a-0", "/call?u=" + out + "/cl");
    std::cout << "[allowed] " << c.body << std::endl;
    assert(json_int(c.body, "rc") == 200);
    c = call_pod("default/a-0", "/call?u=http://fake.example:" + port + "/cl");
    assert(json_int(c.body, "rc") == 200);
    c = call_pod("default/a-0", "/call?u=http://b/hello");
    assert(json_int(c.body, "rc") == K32_ERR_PERM);
    // ... minus the fake host's own address
    assert(policy_request(api, api::HttpMethod::Patch, "loopback", R"({"egress":[{"to":[{"ipBlock":{"cidr":"127.0.0.0/8","except":["127.0.0.1/32"]}}]}]})") == 200);
    c = call_pod("default/a-0", "/call?u=" + out + "/cl");
    assert(json_int(c.body, "rc") == K32_ERR_PERM);

    // Pods by label on a named port: b-0's "http" is 7000, its Service's target
    assert(policy_request(api, api::HttpMethod::Post, "to-b", R"({"podSelector":{"matchLabels":{"app":"a"}},"policyTypes":["Egress"],
        "egress":[{"to":[{"podSelector":{"matchLabels":{"app":"b"}}}],"ports":[{"port":"http"}]}]})") == 201);
    c = call_pod("default/a-0", "/call?u=http://b/hello");
    assert(json_int(c.body, "rc") == 200 && c.body.find("hello from b-0") != std::string::npos);
    // r-0 on the other node: its "http" (7000) is not the port its Service
    // targets, a port range is
    assert(policy_request(api, api::HttpMethod::Post, "to-r", R"({"podSelector":{"matchLabels":{"app":"a"}},"policyTypes":["Egress"],
        "egress":[{"to":[{"namespaceSelector":{"matchLabels":{"kubernetes.io/metadata.name":"default"}},"podSelector":{"matchLabels":{"app":"r"}}}],
                   "ports":[{"port":"http"}]}]})") == 201);
    c = call_pod("default/a-0", "/call?u=http://r:8080/cl");
    assert(json_int(c.body, "rc") == K32_ERR_PERM);
    assert(policy_request(api, api::HttpMethod::Patch, "to-r", R"({"egress":[{"to":[{"podSelector":{"matchLabels":{"app":"r"}}}],
        "ports":[{"port":)" + std::to_string(node.port - 1) + ",\"endPort\":" + std::to_string(node.port + 1) + "}]}]}") == 200);
    c = call_pod("default/a-0", "/call?u=http://r:8080/cl");
    assert(json_int(c.body, "rc") == 200 && c.body.find("\"body\":\"hello\"") != std::string::npos);
    assert(client.stats().denied.load() >= 6);

    // k-0 answers at its node's pod port; a policy names the pod's own port
    // (its "http", 7000), by name or by number, not the node's
    c = call_pod("default/a-0", "/call?u=http://k/cl");
    assert(json_int(c.body, "rc") == K32_ERR_PERM);
    assert(policy_request(api, api::HttpMethod::Post, "to-k", R"({"podSelector":{"matchLabels":{"app":"a"}},"policyTypes":["Egress"],
        "egress":[{"to":[{"podSelector":{"matchLabels":{"app":"k"}}}],"ports":[{"port":"http"}]}]})") == 201);
    c = call_pod("default/a-0", "/call?u=http://k/cl");
    assert(json_int(c.body, "rc") == 200 && c.body.find("\"body\":\"hello\"") != std::string::npos);
    assert(policy_request(api, api::HttpMethod::Patch, "to-k", R"({"egress":[{"to":[{"podSelector":{"matchLabels":{"app":"k"}}}],"ports":[{"port":7000}]}]})") == 200);
    c = call_pod("default/a-0", "/call?u=http://k/cl");
    assert(json_int(c.body, "rc") == 200);
    assert(policy_request(api, api::HttpMethod::Patch, "to-k", R"({"egress":[{"to":[{"podSelector":{"matchLabels":{"app":"k"}}}],"ports":[{"port":)" +
                          port + "}]}]}") == 200);
    c = call_pod("default/a-0", "/call?u=http://k/cl");
    assert(json_int(c.body, "rc") == K32_ERR_PERM);

    // Policies gone: everything allowed again
    for (const char* name : {"lockdown", "loopback", "to-b", "to-r", "to-k"}) assert(policy_request(api, api::HttpMethod::Delete, name) == 200);
    c = call_pod("default/a-0", "/call?u=" + out + "/cl");
    assert(json_int(c.body, "rc") == 200);
    c = call_pod("default/a-0", "/call?u=http://b/hello");
    assert(json_int(c.body, "rc") == 200);
    std::cout << "[PASS] k32_http_call: hosts outside the cluster, NetworkPolicy egress" << std::endl;
}

// OnDemand Devices: a call to a Service whose Device has no driver running
// reports the use and waits for the driver, at most the call's timeout. On
// the control plane through the Device controller in-process (the driver on
// node "sim", whose pod server is the scripted node); on a worker through the
// activate subresource, with Services and Endpoints read from the API.
static void test_on_demand(storage::Store& store, FakeNode& node) {
    std::cout << "=== k32_http_call: OnDemand Devices ===" << std::endl;
    store.put("/registry/devices/default/env", R"({"apiVersion":"kube32.io/v1","kind":"Device","metadata":{"name":"env","namespace":"default",
        "uid":"u-env"},"spec":{"driver":"chip-temp","nodeName":"sim","idleSeconds":10}})", true);
    store.put("/registry/devices/default/never", R"({"apiVersion":"kube32.io/v1","kind":"Device","metadata":{"name":"never","namespace":"default",
        "uid":"u-never"},"spec":{"driver":"chip-temp","nodeName":"sim"}})", true);
    controller::DeviceControllerConfig cfg;
    cfg.reconcile_interval_ms = 5000;   // an activation does not wait for this
    cfg.resync_interval_ms = 60000;
    controller::DeviceController dc(store, nullptr, cfg);
    assert(dc.start());
    auto exists = [&](const std::string& key) { std::string j; return store.get(key, j).ok(); };
    for (int i = 0; i < 100 && !(exists("/registry/services/default/env") && exists("/registry/services/default/never")); ++i) osal::Thread::sleep_ms(20);
    assert(!exists("/registry/pods/default/env-driver"));

    // node "sim"'s kubelet: runs env's driver 300 ms after it is created, never never's
    std::atomic<bool> kubelet_on{true};
    std::thread kubelet([&]() {
        uint64_t seen = 0;
        while (kubelet_on) {
            std::string json;
            if (store.get("/registry/pods/default/env-driver", json).ok() && json.find("\"podIP\"") == std::string::npos) {
                if (!seen) seen = osal::System::get_uptime_ms();
                if (osal::System::get_uptime_ms() - seen >= 300) {
                    models::JsonValue p;
                    std::string err;
                    models::parse_json(json, p, err);
                    p.get_mut("spec")->set("nodeName", models::JsonValue{models::JsonType::String, false, 0, "sim", {}, {}});
                    models::JsonValue st;
                    models::parse_json(R"({"phase":"Running","podIP":"127.0.0.1","conditions":[{"type":"Ready","status":"True"}]})", st, err);
                    p.set("status", st);
                    store.put("/registry/pods/default/env-driver", p.to_string(), true);
                }
            }
            osal::Thread::sleep_ms(20);
        }
    });

    std::atomic<int> reports{0};
    app::ServiceClientOptions opts;
    opts.activate_device = [&](const std::string& ns, const std::string& name) {
        reports++;
        dc.activate(ns, name);
        return true;
    };
    opts.node_name = "laptop";
    app::ServiceClient cp(app::make_store_service_resolver(store), opts);
    app::install_service_client(&cp);

    // The first call starts the driver and waits for it
    uint64_t t0 = osal::System::get_uptime_ms();
    k32test::Collect c = call_pod("default/a-0", "/call?u=http://env/cl&t=5000");
    uint64_t took = osal::System::get_uptime_ms() - t0;
    std::cout << "[on demand] first call " << c.body << " in " << took << " ms" << std::endl;
    assert(json_int(c.body, "rc") == 200 && c.body.find("\"body\":\"hello\"") != std::string::npos);
    assert(took >= 300 && took < 3000 && cp.stats().activated.load() == 1 && reports.load() == 1);
    // Running: no wait, and no report before idleSeconds/4
    c = call_pod("default/a-0", "/call?u=http://env/cl");
    assert(json_int(c.body, "rc") == 200 && cp.stats().activated.load() == 1 && reports.load() == 1);

    // A driver that does not start: 503 with the reason once the call's time is up
    t0 = osal::System::get_uptime_ms();
    c = call_pod("default/a-0", "/call?u=http://never/&t=600");
    took = osal::System::get_uptime_ms() - t0;
    std::cout << "[on demand] no driver: " << c.body << " in " << took << " ms" << std::endl;
    assert(json_int(c.body, "rc") == 503 && c.body.find("\\\"device\\\":\\\"never\\\",\\\"ok\\\":false") != std::string::npos);
    assert(took >= 550 && took < 2000);
    {
        uint8_t buf[256];
        runtime::BufferResponseSink sink(buf, sizeof buf);
        runtime::ServiceCallRequest req;
        req.caller_pod = "default/a-0";
        req.method = "GET";
        req.url = "http://never/";
        req.timeout_ms = 300;
        assert(cp.call(req, sink) == 503 && sink.status() == 503);
        const std::string body(reinterpret_cast<const char*>(buf), sink.copied());
        std::cout << "[on demand] " << body << std::endl;
        assert(body.find("\"error\":\"the driver of Device never did not start within 300 ms") != std::string::npos);
    }
    kubelet_on = false;
    kubelet.join();
    dc.stop();

    // A worker: the Endpoints name the Device; the activate subresource starts it
    std::atomic<bool> started{false};
    std::atomic<int> posts{0};
    auto api_get = [&](const std::string& path, std::string& body) {
        if (path == "/api/v1/namespaces/default/services/kitchen") {
            body = service_json("kitchen", "x", "[{\"name\":\"http\",\"port\":80,\"targetPort\":\"http\"}]");
            return 200;
        }
        if (path == "/api/v1/namespaces/default/endpoints/kitchen") {
            body = R"({"kind":"Endpoints","metadata":{"name":"kitchen","annotations":{"kube32.io/on-demand-devices":"env:sim:60"}})";
            if (started) {
                body += R"(,"subsets":[{"addresses":[{"ip":"127.0.0.1","targetRef":{"kind":"Pod","namespace":"default","name":"env-driver"}}],)"
                        R"("ports":[{"name":"http","port":)" + std::to_string(node.port) + "}]}]";
            }
            body += "}";
            return 200;
        }
        return 404;
    };
    app::ServiceClientOptions wo;
    wo.activate_device = [&](const std::string& ns, const std::string& name) {
        assert(ns == "default" && name == "env");
        posts++;
        std::thread([&]() { osal::Thread::sleep_ms(250); started = true; }).detach();
        return true;
    };
    app::ServiceClient worker(app::make_api_service_resolver(api_get), wo);
    app::install_service_client(&worker);
    // The new driver answers 503 until its first reading: the call that started it asks again
    c = call_pod("default/a-0", "/call?u=http://kitchen/warm&t=5000");
    std::cout << "[on demand] worker: " << c.body << std::endl;
    assert(json_int(c.body, "rc") == 200 && c.body.find("\"body\":\"warm\"") != std::string::npos && node.warm.load() == 3);
    assert(posts.load() == 1 && worker.stats().activated.load() == 1);
    c = call_pod("default/a-0", "/call?u=http://kitchen/cl");
    assert(json_int(c.body, "rc") == 200 && posts.load() == 1);

    app::install_service_client(nullptr);
    for (const char* k : {"/registry/devices/default/env", "/registry/devices/default/never", "/registry/services/default/env",
                          "/registry/services/default/never", "/registry/pods/default/env-driver", "/registry/pods/default/never-driver"}) {
        store.remove(k);
    }
    std::cout << "[PASS] k32_http_call: OnDemand Devices" << std::endl;
}

int main() {
    std::cout << "=== k32_http_call ===" << std::endl;
    osal::Socket::init_networking();
    auto store = storage::create_radix_store(storage::StoreOptions{"/tmp/kube32_test_service_calls", false});
    FakeNode node;
    assert(node.start());

    // a, b: local pods; svc "a" selects a-0 (self calls); "r": a pod on the fake node;
    // "k": a pod on node "sim", whose pod server (the fake node) is not on its containerPort
    store->put("/registry/services/default/a", service_json("a", "a", "[{\"name\":\"http\",\"port\":80,\"targetPort\":\"http\"}]"), true);
    store->put("/registry/services/default/b", service_json("b", "b", "[{\"name\":\"http\",\"port\":80,\"targetPort\":7000}]"), true);
    store->put("/registry/services/default/r", service_json("r", "r", "[{\"port\":8080,\"targetPort\":" + std::to_string(node.port) + "}]"), true);
    store->put("/registry/pods/default/a-0", pod_json("a-0", "a", "127.0.0.1"), true);
    store->put("/registry/pods/default/b-0", pod_json("b-0", "b", "127.0.0.1"), true);
    store->put("/registry/pods/default/r-0", pod_json("r-0", "r", "127.0.0.1"), true);
    store->put("/registry/services/default/k", service_json("k", "k", "[{\"name\":\"http\",\"port\":80,\"targetPort\":\"http\"}]"), true);
    {
        std::string k = pod_json("k-0", "k", "127.0.0.1");
        k.replace(k.find("\"spec\":{"), 8, "\"spec\":{\"nodeName\":\"sim\",");
        store->put("/registry/pods/default/k-0", k, true);
        store->put("/registry/nodes/sim", "{\"kind\":\"Node\",\"metadata\":{\"name\":\"sim\"},\"status\":{\"daemonEndpoints\":{\"kubeletEndpoint\":{\"Port\":" +
                   std::to_string(node.port) + "}}}}", true);
    }

    app::ServiceClient client(app::make_store_service_resolver(*store));
    app::install_service_client(&client);

    auto rt = runtime::create_wasm_runtime();
    const std::vector<uint8_t> probe = k32test::svcprobe();
    k32test::deploy(*rt, "default/a-0", probe, 16 * 1024, "\"SERVICE_CALL\"");
    runtime::DeploySpec unmetered;   // b spins for the busy test below without running out of gas
    unmetered.gas_budget_per_call = 0;
    k32test::deploy(*rt, "default/b-0", probe, 16 * 1024, "\"SERVICE_CALL\"", unmetered);
    k32test::deploy(*rt, "default/nocap-0", probe, 16 * 1024);

    // Local Service: in-process, the callee's answer in the caller's buffer
    k32test::Collect c = call_pod("default/a-0", "/call?u=http://b/hello");
    std::cout << "[local] " << c.body << std::endl;
    assert(c.status == 200 && json_int(c.body, "rc") == 200);
    assert(c.body.find("hello from b-0") != std::string::npos);
    c = call_pod("default/a-0", "/call?u=http://b.default.svc.cluster.local:80/hello");
    assert(json_int(c.body, "rc") == 200);
    assert(client.stats().local.load() >= 2);

    // Binary bodies pass through unchanged (POST /echo through b)
    std::string bin("\x00\x01\xff\r\n\x7f", 6);
    c = call_pod("default/a-0", "/call?u=http://b/echo&m=POST", "POST", bin);
    assert(json_int(c.body, "rc") == 200 && json_int(c.body, "len") == 6);

    // CPU time (runtime/pod_cpu.hpp): b's work is charged to b, not to a,
    // which called it in-process on its own thread
    runtime::AppPodHost& host = runtime::AppPodHost::instance();
    auto cpu_of = [&](const char* pod) { return host.stats(pod).cpu_us; };
    {
        const uint64_t a0 = cpu_of("default/a-0"), b0 = cpu_of("default/b-0");
        c = call_pod("default/a-0", "/call?u=http://b/spin%3Fms%3D200");
        assert(json_int(c.body, "rc") == 200);
        const uint64_t a = cpu_of("default/a-0") - a0, b = cpu_of("default/b-0") - b0;
        std::cout << "[cpu] a calls b's 200 ms spin: a " << a << " us, b " << b << " us" << std::endl;
        // A loaded machine gives the spin less than its 200 ms of wall time
        assert(b >= 10000 && b <= 260000);
        assert(a < b / 10);
    }

    // A pod calling itself, and a cycle a -> b -> a: K32_ERR_LOOP, no deadlock
    c = call_pod("default/a-0", "/call?u=http://a/hello");
    std::cout << "[self] " << c.body << std::endl;
    assert(json_int(c.body, "rc") == K32_ERR_LOOP);
    c = call_pod("default/a-0", "/call?u=http://b/call%3Fu%3Dhttp://a/hello");
    std::cout << "[cycle] " << c.body << std::endl;
    assert(json_int(c.body, "rc") == 200 && c.body.find("\\\"rc\\\":-40") != std::string::npos);

    // No such Service; no pod behind one; bad URL
    c = call_pod("default/a-0", "/call?u=http://nosuch/hello");
    assert(json_int(c.body, "rc") == K32_ERR_NOENT);
    store->put("/registry/services/default/empty", service_json("empty", "nobody", "[{\"port\":80}]"), true);
    c = call_pod("default/a-0", "/call?u=http://empty/");
    assert(json_int(c.body, "rc") == K32_ERR_NOENT);
    c = call_pod("default/a-0", "/call?u=ftp://b/");
    assert(json_int(c.body, "rc") == K32_ERR_INVAL);

    // Larger than the buffer: K32_ERR_TOOBIG with the first bytes copied
    c = call_pod("default/a-0", "/call?u=http://b/big%3Fn%3D5000&cap=1000");
    std::cout << "[toobig] " << c.body.substr(0, 60) << std::endl;
    assert(json_int(c.body, "rc") == K32_ERR_TOOBIG && json_int(c.body, "len") == 1000);

    // Without SERVICE_CALL
    c = call_pod("default/nocap-0", "/call?u=http://b/hello");
    assert(json_int(c.body, "rc") == K32_ERR_PERM);

    // Too little stack left for a nested interpreter
    runtime::AppPodHost::instance().set_nested_stack_reserve(1u << 30);
    c = call_pod("default/a-0", "/call?u=http://b/hello");
    assert(json_int(c.body, "rc") == K32_ERR_NOMEM);
    runtime::AppPodHost::instance().set_nested_stack_reserve(128 * 1024);

    // A busy local pod: the call waits at most its timeout
    std::thread busy([]() { call_pod("default/b-0", "/spin?ms=1500"); });
    osal::Thread::sleep_ms(200);
    uint64_t t0 = osal::System::get_uptime_ms();
    uint64_t a0 = cpu_of("default/a-0");
    c = call_pod("default/a-0", "/call?u=http://b/hello&t=300");
    uint64_t took = osal::System::get_uptime_ms() - t0;
    std::cout << "[busy] " << c.body << " in " << took << " ms, a used " << cpu_of("default/a-0") - a0 << " us of CPU" << std::endl;
    assert(json_int(c.body, "rc") == K32_ERR_TIMEOUT && took < 1200);
    assert(cpu_of("default/a-0") - a0 < 50000);   // waiting for b's call lock is not CPU time
    busy.join();

    // Remote: Content-Length, chunked, keep-alive reuse, headers
    c = call_pod("default/a-0", "/call?u=http://r:8080/cl");
    std::cout << "[remote] " << c.body << std::endl;
    assert(json_int(c.body, "rc") == 200 && c.body.find("\"body\":\"hello\"") != std::string::npos);
    std::string head = node.last_head();
    assert(head.find("GET /cl HTTP/1.1") == 0);
    assert(head.find("X-Kube32-Pod: default/r-0") != std::string::npos);
    assert(head.find("X-Kube32-Call-Chain: default/a-0") != std::string::npos);
    c = call_pod("default/a-0", "/call?u=http://r:8080/chunked");
    assert(json_int(c.body, "rc") == 201 && c.body.find("\"body\":\"hello\"") != std::string::npos);
    c = call_pod("default/a-0", "/call?u=http://r:8080/echo&m=POST", "POST", bin);
    assert(json_int(c.body, "rc") == 200 && json_int(c.body, "len") == 6);
    assert(node.last_head().find("Content-Type: application/octet-stream") != std::string::npos);
    std::cout << "[remote] " << node.connections.load() << " connection(s) for 3 calls, " << client.stats().reused.load() << " reused" << std::endl;
    assert(node.connections.load() == 1 && client.stats().reused.load() == 2);
    c = call_pod("default/a-0", "/call?u=http://r:8080/close");
    assert(json_int(c.body, "rc") == 200 && c.body.find("until-close") != std::string::npos);
    c = call_pod("default/a-0", "/call?u=http://r:8080/missing");
    assert(json_int(c.body, "rc") == 404);
    c = call_pod("default/a-0", "/call?u=http://r:8080/big&cap=4");   // 404, body empty
    assert(json_int(c.body, "rc") == 404);

    // Remote timeout: honoured, and the caller's thread was free meanwhile
    t0 = osal::System::get_uptime_ms();
    a0 = cpu_of("default/a-0");
    c = call_pod("default/a-0", "/call?u=http://r:8080/slow&t=300");
    took = osal::System::get_uptime_ms() - t0;
    std::cout << "[remote timeout] " << c.body << " in " << took << " ms" << std::endl;
    assert(json_int(c.body, "rc") == K32_ERR_TIMEOUT && took >= 250 && took < 1000);
    assert(cpu_of("default/a-0") - a0 < 50000);   // blocked in the socket: not CPU time

    // Unreachable node
    store->put("/registry/services/default/gone", service_json("gone", "gone", "[{\"port\":80,\"targetPort\":1}]"), true);
    store->put("/registry/pods/default/gone-0", pod_json("gone-0", "gone", "127.0.0.1"), true);
    c = call_pod("default/a-0", "/call?u=http://gone/");
    assert(json_int(c.body, "rc") == K32_ERR_IO);

    // The receiving side: a pod server refuses a request whose call chain
    // already holds the target pod (508), and serves the others
    app::PodServerOptions po;
    po.port = 18690;
    po.worker_stack = 262144;
    app::PodServer server(po);
    assert(server.start());
    k32test::HttpReply h = k32test::http(po.port, "GET", "/hello", "X-Kube32-Pod: default/b-0\r\nX-Kube32-Call-Chain: default/a-0,default/b-0\r\n");
    assert(h.status == 508);
    h = k32test::http(po.port, "GET", "/hello", "X-Kube32-Pod: default/b-0\r\nX-Kube32-Call-Chain: default/a-0\r\n");
    assert(h.status == 200 && h.body == "hello from b-0\n");
    // With one interpreter stack per node, a call whose chain holds a pod of
    // this node is refused at once: that pod holds the stack, waiting
    static uint8_t interp[64];
    kube32_interp_stack_set(interp, sizeof(interp));
    h = k32test::http(po.port, "GET", "/hello", "X-Kube32-Pod: default/b-0\r\nX-Kube32-Call-Chain: default/a-0\r\n");
    assert(h.status == 508);
    h = k32test::http(po.port, "GET", "/hello", "X-Kube32-Pod: default/b-0\r\nX-Kube32-Call-Chain: default/elsewhere-0\r\n");
    assert(h.status == 200);
    kube32_interp_stack_set(nullptr, 0);
    server.stop();

    test_egress(*store, node);
    test_on_demand(*store, node);

    app::install_service_client(nullptr);
    node.stop();
    std::cout << "[PASS] k32_http_call: local, remote, loops, timeouts, limits" << std::endl;
    return 0;
}
