// The webhook example (apps/webhook) on a worker: events of the forwarded
// topics queued by k32_event and POSTed as JSON by k32_tick, through the
// control plane's egress gateway, to a local HTTPS host. The JSON and its
// escaping, delivery, retries with back-off, rejected events, the bounded
// queue, a refused certificate and the example's NetworkPolicy.

#include "pod_test_util.hpp"
#include "https_test_host.hpp"
#include "app/egress_gateway.hpp"
#include "app/service_client.hpp"
#include "api/http_server.hpp"
#include "api/route_handler.hpp"
#include "mem/monotonic_arena.hpp"
#include "runtime/k32_app_abi.h"
#include "runtime/pod_log.hpp"
#include "storage/store.hpp"
#include "osal/system.hpp"
#include "osal/tls.hpp"

#include <cassert>
#include <csignal>
#include <iostream>
#include <string>

using namespace kube32;

namespace {

const std::string kPod = "default/webhook-0";

bool log_has(const std::string& pod, const std::string& needle) {
    std::vector<runtime::PodLogRecord> lines;
    runtime::PodLogStore::runtime().tail(pod, 0, lines);
    for (const auto& l : lines) if (l.text.find(needle) != std::string::npos) return true;
    return false;
}

std::string pod_json(const std::string& name) {
    return "{\"apiVersion\":\"v1\",\"kind\":\"Pod\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\",\"labels\":{\"app\":\"webhook\"}},"
           "\"spec\":{\"nodeName\":\"node-a\",\"containers\":[{\"name\":\"c\",\"image\":\"wasm://embedded/webhook\"}]},"
           "\"status\":{\"phase\":\"Running\",\"podIP\":\"127.0.0.1\"}}";
}

int policy_request(api::RouteHandler& api, api::HttpMethod method, const std::string& name, const std::string& spec = "") {
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

void event(const std::string& pod, const std::string& topic, const std::string& payload) {
    runtime::AppPodHost::instance().event(pod, topic, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
}

std::string status(const std::string& pod) { return k32test::call_pod(pod, "/").body; }

} // namespace

int main() {
    std::cout << "=== webhook example ===" << std::endl;
    if (!osal::tls_available()) {
        std::cout << "[SKIP] this build has no TLS" << std::endl;
        return 0;
    }
    std::signal(SIGPIPE, SIG_IGN);
    osal::Socket::init_networking();
    std::string err;
    auto ca = osal::TlsServer::create({"hooks.example"}, {}, nullptr, nullptr, err);
    auto other = osal::TlsServer::create({"hooks.example"}, {}, nullptr, nullptr, err);
    assert(ca && other);
    k32test::HttpsHost hooks, rogue;
    hooks.tls = ca.get();
    rogue.tls = other.get();
    assert(hooks.start(18610) && rogue.start(18615));

    // The control plane: store, TLS client, egress gateway on the API server
    auto store = storage::create_radix_store(storage::StoreOptions{"/tmp/kube32_test_webhook_app", false});
    store->put("/registry/nodes/node-a",
               R"({"apiVersion":"v1","kind":"Node","metadata":{"name":"node-a"},"status":{"addresses":[{"type":"InternalIP","address":"127.0.0.1"}]}})", true);
    store->put("/registry/pods/" + kPod, pod_json("webhook-0"), true);
    store->put("/registry/pods/default/webhook-rogue-0", pod_json("webhook-rogue-0"), true);
    std::unique_ptr<osal::TlsClient> roots = osal::TlsClient::create(ca->certificate_pem(), err);
    assert(roots);
    app::ServiceClientOptions cp_opts;
    cp_opts.resolve_host = [](const std::string& host, uint32_t, uint32_t& out) -> int {
        if (host == "hooks.example") { out = 0x7F000001; return 0; }
        return ENOENT;
    };
    cp_opts.tls = roots.get();
    cp_opts.tls_stack_bytes = 262144;
    app::ServiceClient cp(app::make_store_service_resolver(*store), cp_opts, app::make_store_egress_source(*store));
    static const std::string kToken = "webhook-test-token-0123456789";
    api::HttpServerOptions o;
    o.workers = 2;
    o.worker_stack_bytes = 262144;
    o.watch_streams = 0;
    std::unique_ptr<api::HttpServer> server;
    for (uint16_t p = 18660; p < 18670 && !server; ++p) {
        o.port = p;
        server = std::make_unique<api::HttpServer>(o, *store);
        if (!server->start()) server.reset();
    }
    assert(server);
    server->route_handler().set_bearer_token(kToken);
    app::EgressGateway gateway(cp, *store, server->route_handler());
    server->set_handler(&gateway);

    // The worker the pod runs on
    app::ServiceClientOptions wo;
    wo.gateway_host = "127.0.0.1";
    wo.gateway_port = o.port;
    wo.gateway_token = kToken;
    app::ServiceClient worker(app::make_api_service_resolver([](const std::string&, std::string&) { return 404; }), wo);
    app::install_service_client(&worker);

    const std::vector<uint8_t> wasm = k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/images/webhook.wasm");
    assert(!wasm.empty() && "images/webhook.wasm (make -C apps/webhook)");
    auto rt = runtime::create_wasm_runtime();
    const std::string url = "https://hooks.example:" + std::to_string(hooks.port) + "/hook/s3cr3t-token";
    runtime::DeploySpec spec;
    spec.env = {{"WEBHOOK_URL", url}, {"NODE_NAME", "node-a"}, {"WEBHOOK_TIMEOUT_MS", "3000"}};
    k32test::deploy(*rt, kPod, wasm, 8 * 1024, "\"SERVICE_CALL\"", spec);
    auto& host = runtime::AppPodHost::instance();
    // The URL's path holds the credential: only scheme and host are logged
    assert(log_has(kPod, "webhook: forwarding rfid.tag,ir.received to https://hooks.example:" + std::to_string(hooks.port)));
    assert(!log_has(kPod, "s3cr3t"));

    // An RFID tag: one POST with topic, payload, node and time
    uint32_t now = 10000;
    assert(host.tick(kPod, now) == 0);   // nothing queued
    event(kPod, "rfid.tag", "04a1b2c3d4e580");
    event(kPod, "other.topic", "not forwarded");
    assert(host.tick(kPod, now += 1000) == 0);
    std::vector<std::string> got = hooks.hooks();
    assert(got.size() == 1);
    std::cout << "[webhook] " << got[0] << std::endl;
    assert(got[0].rfind(R"({"topic":"rfid.tag","payload":"04a1b2c3d4e580","node":"node-a","time":"20)", 0) == 0);
    assert(got[0].size() == got[0].find("\"time\":\"") + 8 + 24 + 2 && got[0].substr(got[0].size() - 3) == "Z\"}");
    assert(hooks.head().find("POST /hook/s3cr3t-token HTTP/1.1") == 0);
    assert(hooks.head().find("Content-Type: application/json") != std::string::npos);
    assert(log_has(kPod, "webhook: rfid.tag delivered (200, "));
    assert(worker.stats().forwarded.load() == 1);

    // Payload bytes JSON cannot carry as they are
    event(kPod, "ir.received", std::string("a\"b\\c\n\xff", 7));
    host.tick(kPod, now += 1000);
    got = hooks.hooks();
    assert(got.size() == 2 && got[1].find("\"payload\":\"a\\\"b\\\\c\\u000a\\u00ff\"") != std::string::npos);

    // The host fails twice: the event stays queued, attempts 1 s and 2 s apart
    hooks.hook_status = {503, 503};
    event(kPod, "rfid.tag", "0011223344");
    host.tick(kPod, now += 1000);
    assert(hooks.hooks().size() == 3 && log_has(kPod, "webhook: rfid.tag not delivered (503), retry in 1 s"));
    host.tick(kPod, now += 500);   // too early
    assert(hooks.hooks().size() == 3);
    host.tick(kPod, now += 500);
    assert(hooks.hooks().size() == 4 && log_has(kPod, "retry in 2 s"));
    host.tick(kPod, now += 1000);   // too early
    assert(hooks.hooks().size() == 4);
    host.tick(kPod, now += 1000);
    got = hooks.hooks();
    assert(got.size() == 5 && got[4] == got[2]);
    std::cout << "[webhook status] " << status(kPod);
    assert(status(kPod).find("\"queued\":0,\"delivered\":3,\"rejected\":0,\"dropped\":0,\"retrying\":false,\"last_result\":200") != std::string::npos);

    // An error page larger than the pod's buffer is still an error, a large
    // success still a success (k32_http_status)
    hooks.hook_body_bytes = 3000;
    hooks.hook_status = {502};
    event(kPod, "rfid.tag", "big");
    host.tick(kPod, now += 1000);
    assert(log_has(kPod, "webhook: rfid.tag not delivered (502), retry in 1 s"));
    host.tick(kPod, now += 1000);
    assert(hooks.hooks().size() == 7 && status(kPod).find("\"queued\":0,\"delivered\":4,") != std::string::npos);
    hooks.hook_body_bytes = 0;

    // A 4xx answer drops the event; the next one goes out on the next tick
    hooks.hook_status = {404};
    event(kPod, "rfid.tag", "aa");
    event(kPod, "rfid.tag", "bb");
    assert(host.tick(kPod, now += 1000) == 1);   // more queued: tick again soon
    assert(log_has(kPod, "webhook: rfid.tag rejected (404), dropped"));
    assert(host.tick(kPod, now += 10) == 0);
    got = hooks.hooks();
    assert(got.size() == 9 && got[8].find("\"payload\":\"bb\"") != std::string::npos);

    // The host down: the queue keeps the six newest events
    hooks.hook_status = std::deque<int>(20, 500);
    for (int i = 0; i < 8; ++i) event(kPod, "rfid.tag", "tag" + std::to_string(i));
    assert(log_has(kPod, "webhook: queue full, dropping the oldest events (rfid.tag first)"));
    host.tick(kPod, now += 1000);
    std::cout << "[webhook full] " << status(kPod);
    assert(status(kPod).find("\"queued\":6,") != std::string::npos && status(kPod).find("\"dropped\":2,") != std::string::npos);
    assert(hooks.hooks().back().find("\"payload\":\"tag2\"") != std::string::npos);
    hooks.hook_status.clear();
    for (int i = 0; i < 8; ++i) host.tick(kPod, now += 60000);
    got = hooks.hooks();
    assert(got.back().find("\"payload\":\"tag7\"") != std::string::npos);
    assert(log_has(kPod, "webhook: 2 event(s) dropped while the queue was full"));
    assert(status(kPod).find("\"queued\":0,") != std::string::npos);

    // The example's NetworkPolicy lets the pod reach TCP 443 only: this host
    // listens elsewhere, so the call is denied, and retried
    api::RouteHandler api(*store);
    assert(policy_request(api, api::HttpMethod::Post, "webhook-egress", R"({"podSelector":{"matchLabels":{"app":"webhook"}},
        "policyTypes":["Egress"],"egress":[{"ports":[{"protocol":"TCP","port":443}]}]})") == 201);
    event(kPod, "rfid.tag", "cc");
    const size_t before = hooks.hooks().size();
    host.tick(kPod, now += 1000);
    assert(hooks.hooks().size() == before && log_has(kPod, "webhook: rfid.tag not delivered (denied (NetworkPolicy, or no SERVICE_CALL capability), -1), retry in 1 s"));
    assert(policy_request(api, api::HttpMethod::Patch, "webhook-egress", R"({"egress":[{"ports":[{"protocol":"TCP","port":443},{"protocol":"TCP","port":)" +
                                                                            std::to_string(hooks.port) + "}]}]}") == 200);
    host.tick(kPod, now += 1000);
    assert(hooks.hooks().size() == before + 1);

    // A one-line text field for chat webhooks (Slack reads "text")
    const std::string chat_pod = "default/webhook-chat-0";
    store->put("/registry/pods/" + chat_pod, pod_json("webhook-chat-0"), true);
    runtime::DeploySpec chat_spec;
    chat_spec.env = {{"WEBHOOK_URL", url}, {"NODE_NAME", "node-a"}, {"WEBHOOK_TEXT_FIELD", "text"}};
    k32test::deploy(*rt, chat_pod, wasm, 8 * 1024, "\"SERVICE_CALL\"", chat_spec);
    event(chat_pod, "ir.received", "nec 0x04 0x08");
    host.tick(chat_pod, now += 1000);
    std::cout << "[webhook chat] " << hooks.hooks().back() << std::endl;
    assert(hooks.hooks().back().find(R"(,"text":"ir.received: nec 0x04 0x08 on node-a"})") != std::string::npos);

    // A host whose certificate the cluster does not trust
    const std::string rogue_pod = "default/webhook-rogue-0";
    runtime::DeploySpec rogue_spec;
    rogue_spec.env = {{"WEBHOOK_URL", "https://hooks.example:" + std::to_string(rogue.port) + "/hook"}};
    k32test::deploy(*rt, rogue_pod, wasm, 8 * 1024, "\"SERVICE_CALL\"", rogue_spec);
    assert(policy_request(api, api::HttpMethod::Delete, "webhook-egress") == 200);
    event(rogue_pod, "ir.received", "nec 0x04 0x08");
    host.tick(rogue_pod, now += 1000);
    assert(log_has(rogue_pod, "webhook: ir.received not delivered (certificate rejected, -129), retry in 1 s"));
    assert(rogue.hooks().empty());

    app::install_service_client(nullptr);
    server->stop();
    hooks.stop();
    rogue.stop();
    std::cout << "[PASS] webhook example: JSON, delivery, retries, queue bound, certificate, NetworkPolicy" << std::endl;
    return 0;
}
