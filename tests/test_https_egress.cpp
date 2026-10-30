// k32_http_call to https:// hosts: TLS made by the control plane for its own
// pods, and for pods on workers through the egress gateway; certificate
// checks, failed handshakes, the session limit, timeouts, the response limit,
// NetworkPolicy and the gateway's checks of its callers
// (app/service_client.hpp, app/egress_gateway.hpp).
//
// The hosts are local HTTPS servers. The self-signed certificate of one of
// them is the root the clients trust; another server's is not; a third
// server does not speak TLS.

#include "pod_test_util.hpp"
#include "https_test_host.hpp"
#include "app/egress_gateway.hpp"
#include "app/service_client.hpp"
#include "api/http_server.hpp"
#include "api/route_handler.hpp"
#include "mem/monotonic_arena.hpp"
#include "storage/store.hpp"
#include "runtime/k32_app_abi.h"
#include "osal/system.hpp"
#include "osal/thread.hpp"
#include "osal/tls.hpp"

#include <atomic>
#include <cassert>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

using namespace kube32;
using k32test::call_pod;

static int json_int(const std::string& body, const std::string& key) {
    size_t at = body.find("\"" + key + "\":");
    assert(at != std::string::npos);
    return std::atoi(body.c_str() + at + key.size() + 3);
}

static int rc_of(const k32test::Collect& c) { return json_int(c.body, "rc"); }

using k32test::HttpsHost;

static std::string pod_json(const std::string& name, const std::string& app, const std::string& node) {
    return "{\"apiVersion\":\"v1\",\"kind\":\"Pod\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\",\"labels\":{\"app\":\"" + app + "\"}},"
           "\"spec\":{\"nodeName\":\"" + node + "\",\"containers\":[{\"name\":\"c\",\"image\":\"wasm://x\"}]},"
           "\"status\":{\"phase\":\"Running\",\"podIP\":\"127.0.0.1\"}}";
}

static std::string node_json(const std::string& name, const std::string& ip) {
    return "{\"apiVersion\":\"v1\",\"kind\":\"Node\",\"metadata\":{\"name\":\"" + name + "\"},"
           "\"status\":{\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"" + ip + "\"}]}}";
}

// A NetworkPolicy request to the API server; returns the HTTP status
static int policy_request(api::RouteHandler& api, api::HttpMethod method, const std::string& name, const std::string& spec = "") {
    api::HttpRequest r;
    r.method = method;
    r.path = "/apis/networking.k8s.io/v1/namespaces/default/networkpolicies" + std::string(method == api::HttpMethod::Post ? "" : "/" + name);
    if (!spec.empty()) {
        r.body = "{\"apiVersion\":\"networking.k8s.io/v1\",\"kind\":\"NetworkPolicy\",\"metadata\":{\"name\":\"" + name + "\"},\"spec\":" + spec + "}";
        r.content_type = "application/json";
    }
    mem::MonotonicArena arena;
    return api.handle_request(r, arena).status_code;
}

struct Hosts {
    HttpsHost& trusted;
    HttpsHost& rogue;
    HttpsHost& plain;
    std::string url(const HttpsHost& h, const std::string& path, const std::string& name = "localhost") const {
        return "https://" + name + ":" + std::to_string(h.port) + path;
    }
};

// The answers every node gives a pod's https call, whichever node makes the
// connection: `label` names the path in the output
static void common_calls(const Hosts& h, const std::string& label) {
    k32test::Collect c = call_pod("default/a-0", "/call?u=" + h.url(h.trusted, "/hello"));
    std::cout << "[" << label << "] " << c.body << std::endl;
    assert(rc_of(c) == 200 && c.body.find("\"body\":\"hello over tls\"") != std::string::npos);
    assert(h.trusted.head().find("GET /hello HTTP/1.1\r\nHost: localhost:" + std::to_string(h.trusted.port) + "\r\n") == 0);
    // The certificate names 127.0.0.1 as well
    c = call_pod("default/a-0", "/call?u=https://127.0.0.1:" + std::to_string(h.trusted.port) + "/hello");
    assert(rc_of(c) == 200);
    // Body and headers through; status and chunked framing as the host sent them
    std::string bin("\x00\x01\xff\r\n\x7f", 6);
    c = call_pod("default/a-0", "/call?u=" + h.url(h.trusted, "/echo") + "&m=POST", "POST", bin);
    assert(rc_of(c) == 200 && json_int(c.body, "len") == 6);
    assert(h.trusted.head().find("Content-Type: application/octet-stream") != std::string::npos);
    c = call_pod("default/a-0", "/call?u=" + h.url(h.trusted, "/chunked"));
    assert(rc_of(c) == 201 && c.body.find("\"body\":\"hello\"") != std::string::npos);
    c = call_pod("default/a-0", "/call?u=" + h.url(h.trusted, "/down"));
    assert(rc_of(c) == 503 && c.body.find("\"body\":\"down\"") != std::string::npos);
    c = call_pod("default/a-0", "/call?u=" + h.url(h.trusted, "/large") + "&cap=1000");
    assert(rc_of(c) == K32_ERR_TOOBIG && json_int(c.body, "len") == 1000);
    // A response head of several KB, as public sites send
    c = call_pod("default/a-0", "/call?u=" + h.url(h.trusted, "/bighead"));
    assert(rc_of(c) == 200 && c.body.find("\"body\":\"ok\"") != std::string::npos);

    // Certificates: another root, another name; a host without TLS
    c = call_pod("default/a-0", "/call?u=" + h.url(h.rogue, "/hello"));
    std::cout << "[" << label << " untrusted] " << c.body << std::endl;
    assert(rc_of(c) == K32_ERR_CERT);
    c = call_pod("default/a-0", "/call?u=" + h.url(h.trusted, "/hello", "wrong.example"));
    std::cout << "[" << label << " name mismatch] " << c.body << std::endl;
    assert(rc_of(c) == K32_ERR_CERT);
    c = call_pod("default/a-0", "/call?u=" + h.url(h.plain, "/hello"));
    std::cout << "[" << label << " no tls] " << c.body << std::endl;
    assert(rc_of(c) == K32_ERR_TLS);
    c = call_pod("default/a-0", "/call?u=https://nosuch.example/");
    assert(rc_of(c) == K32_ERR_NOENT);
    c = call_pod("default/a-0", "/call?u=https://127.0.0.1:1/");
    assert(rc_of(c) == K32_ERR_CONNREFUSED);

    // The caller's timeout
    uint64_t t0 = osal::System::get_uptime_ms();
    c = call_pod("default/a-0", "/call?u=" + h.url(h.trusted, "/slow") + "&t=300");
    uint64_t took = osal::System::get_uptime_ms() - t0;
    std::cout << "[" << label << " timeout] " << c.body << " in " << took << " ms" << std::endl;
    assert(rc_of(c) == K32_ERR_TIMEOUT && took >= 250 && took < 1200);
    osal::Thread::sleep_ms(300);   // the session the call held ends at its deadline too

    // One TLS session: a second call while it is held is refused at once
    std::atomic<int> slow_rc{0};
    std::thread holder([&]() { slow_rc = rc_of(call_pod("default/c-0", "/call?u=" + h.url(h.trusted, "/slow") + "&t=5000")); });
    osal::Thread::sleep_ms(400);
    t0 = osal::System::get_uptime_ms();
    c = call_pod("default/a-0", "/call?u=" + h.url(h.trusted, "/hello"));
    took = osal::System::get_uptime_ms() - t0;
    std::cout << "[" << label << " busy] " << c.body << " in " << took << " ms" << std::endl;
    assert(rc_of(c) == K32_ERR_BUSY && took < 500);
    holder.join();
    assert(slow_rc == 200);
    c = call_pod("default/a-0", "/call?u=" + h.url(h.trusted, "/hello"));
    assert(rc_of(c) == 200);
}

// NetworkPolicy of the calling pod: nothing for app=a, then the host's port
static void policy_calls(api::RouteHandler& api, const Hosts& h, const std::string& label) {
    assert(policy_request(api, api::HttpMethod::Post, "lockdown", R"({"podSelector":{"matchLabels":{"app":"a"}},"policyTypes":["Egress"]})") == 201);
    k32test::Collect c = call_pod("default/a-0", "/call?u=" + h.url(h.trusted, "/hello"));
    std::cout << "[" << label << " denied] " << c.body << std::endl;
    assert(rc_of(c) == K32_ERR_PERM);
    c = call_pod("default/c-0", "/call?u=" + h.url(h.trusted, "/hello"));   // not selected
    assert(rc_of(c) == 200);
    assert(policy_request(api, api::HttpMethod::Post, "tls-port", R"({"podSelector":{"matchLabels":{"app":"a"}},"policyTypes":["Egress"],
        "egress":[{"ports":[{"protocol":"TCP","port":)" + std::to_string(h.trusted.port) + "}]}]}") == 201);
    c = call_pod("default/a-0", "/call?u=" + h.url(h.trusted, "/hello"));
    assert(rc_of(c) == 200);
    c = call_pod("default/a-0", "/call?u=" + h.url(h.rogue, "/hello"));   // another port
    assert(rc_of(c) == K32_ERR_PERM);
    for (const char* name : {"lockdown", "tls-port"}) assert(policy_request(api, api::HttpMethod::Delete, name) == 200);
    c = call_pod("default/a-0", "/call?u=" + h.url(h.rogue, "/hello"));
    assert(rc_of(c) == K32_ERR_CERT);
}

int main() {
    std::cout << "=== k32_http_call: https ===" << std::endl;
    if (!osal::tls_available()) {
        std::cout << "[SKIP] this build has no TLS" << std::endl;
        return 0;
    }
    std::signal(SIGPIPE, SIG_IGN);   // hosts write to connections the callers gave up on
    osal::Socket::init_networking();
    std::string err;
    auto ca = osal::TlsServer::create({"localhost"}, {"127.0.0.1"}, nullptr, nullptr, err);
    auto other = osal::TlsServer::create({"localhost"}, {"127.0.0.1"}, nullptr, nullptr, err);
    assert(ca && other);
    HttpsHost trusted, rogue, plain;
    trusted.tls = ca.get();
    rogue.tls = other.get();
    assert(trusted.start(18600) && rogue.start(18603) && plain.start(18606));
    Hosts hosts{trusted, rogue, plain};

    auto store = storage::create_radix_store(storage::StoreOptions{"/tmp/kube32_test_https_egress", false});
    store->put("/registry/nodes/node-a", node_json("node-a", "127.0.0.1"), true);
    store->put("/registry/nodes/node-b", node_json("node-b", "10.9.9.9"), true);
    store->put("/registry/pods/default/a-0", pod_json("a-0", "a", "node-a"), true);
    store->put("/registry/pods/default/c-0", pod_json("c-0", "c", "node-a"), true);
    store->put("/registry/pods/default/b-0", pod_json("b-0", "b", "node-b"), true);
    api::RouteHandler policies(*store);

    auto rt = runtime::create_wasm_runtime();
    const std::vector<uint8_t> probe = k32test::svcprobe();
    for (const char* pod : {"default/a-0", "default/b-0", "default/c-0", "default/ghost-0"}) {
        k32test::deploy(*rt, pod, probe, 16 * 1024, "\"SERVICE_CALL\"");
    }

    // The control plane: trusts the first host's certificate, one TLS session
    std::unique_ptr<osal::TlsClient> roots = osal::TlsClient::create(ca->certificate_pem(), err);
    assert(roots);
    app::ServiceClientOptions cp_opts;
    cp_opts.resolve_host = [](const std::string& host, uint32_t, uint32_t& out) -> int {
        if (host == "localhost" || host == "wrong.example") { out = 0x7F000001; return 0; }
        return ENOENT;
    };
    cp_opts.response_head_bytes = 8192;
    cp_opts.tls = roots.get();
    cp_opts.tls_sessions = 1;
    cp_opts.tls_stack_bytes = 262144;
    app::ServiceClient cp(app::make_store_service_resolver(*store), cp_opts, app::make_store_egress_source(*store));

    // Pods of the control plane: TLS here
    {
        std::cout << "=== control plane ===" << std::endl;
        app::install_service_client(&cp);
        common_calls(hosts, "control plane");
        policy_calls(policies, hosts, "control plane");
        assert(cp.stats().https.load() >= 10 && cp.stats().busy.load() >= 1);
        std::cout << "[PASS] https from control-plane pods" << std::endl;
    }

    // Pods of a worker: through the egress gateway on the API server
    {
        std::cout << "=== worker, egress gateway ===" << std::endl;
        static const std::string kToken = "egress-test-token-0123456789";
        api::HttpServerOptions o;
        o.workers = 2;
        o.worker_stack_bytes = 262144;
        o.watch_streams = 0;
        std::unique_ptr<api::HttpServer> server;
        for (uint16_t p = 18650; p < 18660 && !server; ++p) {
            o.port = p;
            server = std::make_unique<api::HttpServer>(o, *store);
            if (!server->start()) server.reset();
        }
        assert(server);
        server->route_handler().set_bearer_token(kToken);
        app::EgressGateway gateway(cp, *store, server->route_handler());
        server->set_handler(&gateway);

        auto no_api = [](const std::string&, std::string&) { return 404; };
        app::ServiceClientOptions wo;
        wo.gateway_host = "127.0.0.1";
        wo.gateway_port = o.port;
        wo.gateway_token = kToken;
        app::ServiceClient worker(app::make_api_service_resolver(no_api), wo);
        app::install_service_client(&worker);
        const uint64_t https_before = cp.stats().https.load();
        common_calls(hosts, "gateway");
        assert(worker.stats().forwarded.load() >= 10 && cp.stats().https.load() > https_before);
        policy_calls(policies, hosts, "gateway");

        // A pod the store does not know: its policies could not select it
        k32test::Collect c = call_pod("default/ghost-0", "/call?u=" + hosts.url(trusted, "/hello"));
        std::cout << "[gateway unknown pod] " << c.body << std::endl;
        assert(rc_of(c) == K32_ERR_PERM);
        // A node without the cluster's token
        app::ServiceClientOptions bad = wo;
        bad.gateway_token = "not-the-cluster-token-000";
        app::ServiceClient stranger(app::make_api_service_resolver(no_api), bad);
        app::install_service_client(&stranger);
        c = call_pod("default/a-0", "/call?u=" + hosts.url(trusted, "/hello"));
        assert(rc_of(c) == K32_ERR_PERM);

        // Without a token: the caller must be the pod's node. a-0's node
        // has this address, b-0's another
        server->route_handler().set_bearer_token("");
        wo.gateway_token.clear();
        app::ServiceClient open_worker(app::make_api_service_resolver(no_api), wo);
        app::install_service_client(&open_worker);
        c = call_pod("default/a-0", "/call?u=" + hosts.url(trusted, "/hello"));
        assert(rc_of(c) == 200);
        c = call_pod("default/b-0", "/call?u=" + hosts.url(trusted, "/hello"));
        std::cout << "[gateway other node's pod] " << c.body << std::endl;
        assert(rc_of(c) == K32_ERR_PERM);

        // A node that gives up frees its session at once: the host here
        // answers after 1.5 s, the next call comes 0.9 s after the start
        {
            osal::Socket s;
            assert(s.create(osal::SocketType::Tcp) && s.connect("127.0.0.1", o.port, 2000));
            const std::string inner = "GET " + hosts.url(trusted, "/slow") + " HTTP/1.1\r\n\r\n";
            const std::string head = "POST /apis/kube32.io/v1/egress?pod=default/a-0&timeout=5000 HTTP/1.1\r\nHost: cp\r\nContent-Length: " +
                                     std::to_string(inner.size()) + "\r\n\r\n";
            assert(s.send_all(head.data(), head.size()) && s.send_all(inner.data(), inner.size()));
            osal::Thread::sleep_ms(300);
            s.close();
            osal::Thread::sleep_ms(600);
            c = call_pod("default/a-0", "/call?u=" + hosts.url(trusted, "/hello"));
            std::cout << "[gateway after a node gave up] " << c.body << std::endl;
            assert(rc_of(c) == 200);
        }

        // The wire format
        const std::string target = "/apis/kube32.io/v1/egress?pod=default/a-0&timeout=3000&limit=5";
        k32test::HttpReply r = k32test::http(o.port, "POST", target, "Content-Type: message/http\r\n",
                                             "GET " + hosts.url(trusted, "/hello") + " HTTP/1.1\r\nAccept: */*\r\n\r\n");
        std::cout << "[gateway wire] " << r.status << " " << r.body << std::endl;
        assert(r.status == 200 && r.has("X-Kube32-Egress-Result: 200") && r.body == "hello ");   // limit + 1 bytes
        assert(trusted.head().find("Accept: */*") != std::string::npos);
        r = k32test::http(o.port, "POST", target, "", "GET http://localhost:" + std::to_string(trusted.port) + "/hello HTTP/1.1\r\n\r\n");
        assert(r.status == 400 && r.has("X-Kube32-Egress-Result: -22"));
        r = k32test::http(o.port, "POST", target, "", "not a request");
        assert(r.status == 400);
        r = k32test::http(o.port, "POST", target, "", "GET\nX-Injected:\x01 " + hosts.url(trusted, "/hello") + " HTTP/1.1\r\n\r\n");
        assert(r.status == 400 && r.has("X-Kube32-Egress-Result: -22"));
        r = k32test::http(o.port, "GET", target);
        assert(r.status == 405);
        r = k32test::http(o.port, "POST", "/apis/kube32.io/v1/egress?pod=default/a-0", "", "GET " + hosts.url(rogue, "/") + " HTTP/1.1\r\n\r\n");
        assert(r.status == 502 && r.has("X-Kube32-Egress-Result: -129"));

        // A control plane without the gateway
        server->set_handler(nullptr);
        c = call_pod("default/a-0", "/call?u=" + hosts.url(trusted, "/hello"));
        assert(rc_of(c) == K32_ERR_NOTSUP);
        server->set_handler(&gateway);

        app::install_service_client(nullptr);
        server->stop();
        std::cout << "[PASS] https from worker pods through the egress gateway" << std::endl;
    }

    // No TLS client and no gateway: https is not available
    {
        app::ServiceClient plain_client(app::make_store_service_resolver(*store));
        app::install_service_client(&plain_client);
        k32test::Collect c = call_pod("default/a-0", "/call?u=" + hosts.url(trusted, "/hello"));
        assert(rc_of(c) == K32_ERR_NOTSUP);
        app::install_service_client(nullptr);
    }

    trusted.stop();
    rogue.stop();
    plain.stop();
    std::cout << "[PASS] k32_http_call: https" << std::endl;
    return 0;
}
