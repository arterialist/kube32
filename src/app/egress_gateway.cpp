#include "app/egress_gateway.hpp"
#include "api/route_handler.hpp"
#include "models/kube_models.hpp"
#include "runtime/k32_app_abi.h"
#include "osal/system.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <new>

namespace kube32::app {

namespace {

// One forwarded call, owned by the TLS session that makes it
struct Forwarded {
    osal::Socket sock;
    std::string pod;
    std::string method;
    std::string url;
    std::string headers;   // "Name: value\r\n" lines
    std::string body;
    uint32_t timeout_ms{5000};
    size_t limit{0};
    uint64_t arrived_ms{0};
};

int status_for(int32_t rc) {
    switch (rc) {
        case K32_ERR_PERM: return 403;
        case K32_ERR_INVAL: return 400;
        case K32_ERR_NOTSUP: return 501;
        case K32_ERR_BUSY:
        case K32_ERR_NOMEM: return 503;
        case K32_ERR_TIMEOUT: return 504;
        default: return 502;
    }
}

const char* reason_for(int32_t rc) {
    switch (rc) {
        case K32_ERR_PERM: return "no NetworkPolicy egress rule of the pod allows the connection";
        case K32_ERR_NOENT: return "the host name does not resolve";
        case K32_ERR_CERT: return "the host's certificate was not accepted";
        case K32_ERR_TLS: return "the TLS handshake failed";
        case K32_ERR_TIMEOUT: return "no complete answer in time";
        case K32_ERR_CONNREFUSED: return "the host refused the connection";
        case K32_ERR_HOSTUNREACH: return "no route to the host";
        case K32_ERR_BUSY: return "every TLS session is in use";
        case K32_ERR_NOMEM: return "out of memory";
        case K32_ERR_INVAL: return "malformed call";
        default: return "the connection to the host failed";
    }
}

// An error answer on a connection the gateway owns
void send_error(osal::Socket& sock, int32_t rc, const std::string& why, const char* extra_headers = "") {
    const int status = status_for(rc);
    const std::string body = why + "\n";
    char head[256];
    const int n = std::snprintf(head, sizeof(head),
                                "HTTP/1.1 %d %s\r\nX-Kube32-Egress-Result: %d\r\nContent-Type: text/plain\r\nContent-Length: %u\r\n%sConnection: close\r\n\r\n",
                                status, api::http_reason_phrase(status), static_cast<int>(rc), static_cast<unsigned>(body.size()), extra_headers);
    if (n > 0 && sock.send_all(head, static_cast<size_t>(n))) sock.send_all(body.data(), body.size());
}

// The host's answer as it arrives: the head once the status is known, then
// the body in chunks, cut after limit + 1 bytes
class RelaySink : public runtime::AppResponseSink {
public:
    RelaySink(osal::Socket& sock, size_t limit) : sock_(sock), limit_(limit) {}
    void begin(int status, std::string_view) override {
        char head[160];
        const int n = std::snprintf(head, sizeof(head),
                                    "HTTP/1.1 %d %s\r\nX-Kube32-Egress-Result: %d\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n",
                                    status, api::http_reason_phrase(status), status);
        began_ = true;
        ok_ = n > 0 && sock_.send_all(head, static_cast<size_t>(n));
    }
    void header(std::string_view, std::string_view) override {}
    bool write(const uint8_t* data, size_t len) override {
        if (!ok_ || len == 0 || sent_ > limit_) return true;
        const size_t take = (len < limit_ + 1 - sent_) ? len : limit_ + 1 - sent_;
        char size_line[20];
        const int n = std::snprintf(size_line, sizeof(size_line), "%zx\r\n", take);
        ok_ = sock_.send_all(size_line, static_cast<size_t>(n)) && sock_.send_all(data, take) && sock_.send_all("\r\n", 2);
        sent_ += take;
        return true;
    }
    void end() override {
        if (ok_) ok_ = sock_.send_all("0\r\n\r\n", 5);
    }
    bool began() const { return began_; }

private:
    osal::Socket& sock_;
    size_t limit_;
    size_t sent_{0};
    bool began_{false};
    bool ok_{false};
};

// Runs on a TLS session
void make_call(ServiceClient& client, Forwarded& call) {
    const uint64_t spent = osal::System::get_uptime_ms() - call.arrived_ms;
    runtime::ServiceCallRequest req;
    req.caller_pod = call.pod;
    req.method = call.method;
    req.url = call.url;
    req.headers = call.headers;
    req.body = call.body;
    req.call_chain = call.pod;
    req.timeout_ms = spent >= call.timeout_ms ? 1 : static_cast<uint32_t>(call.timeout_ms - spent);
    // A node that gave up (its pod timed out or stopped) frees the session
    req.abandoned = [&call]() { return call.sock.peer_closed(); };
    RelaySink sink(call.sock, call.limit);
    int32_t rc;
    try {
        rc = client.call_https_here(req, sink, call.limit);
    } catch (const std::bad_alloc&) {
        rc = K32_ERR_NOMEM;
    } catch (const std::exception&) {
        rc = K32_ERR_IO;
    }
    if (rc < 0 && !sink.began()) send_error(call.sock, rc, reason_for(rc));
    call.sock.close();
}

// Value of `key` in a query string (no decoding: the values a node sends
// need none)
bool query_value(std::string_view qs, std::string_view key, std::string& out) {
    while (!qs.empty()) {
        const size_t amp = qs.find('&');
        const std::string_view kv = qs.substr(0, amp);
        qs = (amp == std::string_view::npos) ? std::string_view() : qs.substr(amp + 1);
        if (kv.size() > key.size() && kv.compare(0, key.size(), key) == 0 && kv[key.size()] == '=') {
            out.assign(kv.substr(key.size() + 1));
            return true;
        }
    }
    return false;
}

std::atomic<uint64_t> s_last_refusal_log_ms{0};

void log_refusal(const std::string& client_ip, const std::string& pod, const std::string& why) {
    const uint64_t now = osal::System::get_uptime_ms() + 1;
    uint64_t last = s_last_refusal_log_ms.load();
    if (last != 0 && now - last < 1000) return;
    if (!s_last_refusal_log_ms.compare_exchange_strong(last, now)) return;
    std::printf("[EGRESS] gateway refused %s (pod %s): %s\n", client_ip.c_str(), pod.empty() ? "-" : pod.c_str(), why.c_str());
}

} // namespace

EgressGateway::EgressGateway(ServiceClient& client, storage::Store& store, api::RouteHandler& routes, api::HttpHandler* next)
    : client_(client), store_(store), routes_(routes), next_(next) {}

bool EgressGateway::streams_body(const api::HttpRequest& req) {
    return req.path != kPath && next_ && next_->streams_body(req);
}

bool EgressGateway::accepts(const std::string& pod, const std::string& client_ip, std::string& why) {
    const size_t slash = pod.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == pod.size() || pod.find('/', slash + 1) != std::string::npos) {
        why = "name the calling pod as <namespace>/<name>";
        return false;
    }
    // A pod that does not exist has no labels for its policies to select
    std::string json, err;
    models::JsonValue doc;
    if (!store_.get("/registry/pods/" + pod, json).ok() || !models::parse_json(json, doc, err)) {
        why = "no pod " + pod;
        return false;
    }
    const models::JsonValue* spec = doc.get("spec");
    const std::string node = spec ? spec->get_string("nodeName", "") : "";
    if (node.empty()) {
        why = "pod " + pod + " is not bound to a node";
        return false;
    }
    if (routes_.authentication_enabled()) return true;
    // Without a token: the call comes from the pod's node
    std::string node_ip;
    if (store_.get("/registry/nodes/" + node, json).ok() && models::parse_json(json, doc, err)) {
        const models::JsonValue* status = doc.get("status");
        const models::JsonValue* addrs = status ? status->get("addresses") : nullptr;
        if (addrs && addrs->is_array()) {
            for (const auto& a : addrs->arr_val) {
                if (a.get_string("type", "") == "InternalIP") { node_ip = a.get_string("address", ""); break; }
            }
        }
    }
    if (!node_ip.empty() && node_ip == client_ip) return true;
    why = "the request does not come from node " + node + (node_ip.empty() ? " (no InternalIP)" : " (" + node_ip + ")");
    return false;
}

bool EgressGateway::handle(api::HttpConnection& conn) {
    const api::HttpRequest& req = conn.request();
    if (req.path != kPath) return next_ && next_->handle(conn);
    auto refuse = [&](int32_t rc, int status, const std::string& why) {
        const std::string extra = "X-Kube32-Egress-Result: " + std::to_string(rc) + "\r\n";
        conn.set_close();
        return conn.send_simple(status, "text/plain", why + "\n", extra);
    };
    std::string pod;
    query_value(req.query_string, "pod", pod);
    if (!routes_.authenticated(req)) {
        log_refusal(req.client_ip, pod, "no valid bearer token");
        return refuse(K32_ERR_PERM, 401, "Unauthorized");
    }
    if (req.method != api::HttpMethod::Post) return refuse(K32_ERR_INVAL, 405, "use POST");

    auto call = std::make_shared<Forwarded>();
    call->arrived_ms = osal::System::get_uptime_ms();
    std::string value;
    if (query_value(req.query_string, "timeout", value)) {
        const long ms = std::strtol(value.c_str(), nullptr, 10);
        call->timeout_ms = ms < 1 ? 1 : (ms > 60000 ? 60000 : static_cast<uint32_t>(ms));
    }
    call->limit = 4096;
    if (query_value(req.query_string, "limit", value)) {
        const long n = std::strtol(value.c_str(), nullptr, 10);
        call->limit = n < 0 ? 0 : (n > (1L << 20) ? (1u << 20) : static_cast<size_t>(n));
    }
    // The call: "<METHOD> <url> HTTP/1.1", header lines, a blank line, the body
    const std::string_view msg = req.body;
    const size_t line_end = msg.find("\r\n");
    const size_t head_end = msg.find("\r\n\r\n");
    const std::string_view line = msg.substr(0, line_end);
    const size_t sp1 = line.find(' ');
    const size_t sp2 = line.rfind(' ');
    if (line_end == std::string_view::npos || head_end == std::string_view::npos || head_end < line_end ||
        sp1 == std::string_view::npos || sp2 <= sp1 || line.substr(sp2 + 1).rfind("HTTP/1.", 0) != 0) {
        return refuse(K32_ERR_INVAL, 400, "the body must be one HTTP request (message/http)");
    }
    call->method.assign(line.substr(0, sp1));
    call->url.assign(line.substr(sp1 + 1, sp2 - sp1 - 1));
    if (call->url.rfind("https://", 0) != 0) return refuse(K32_ERR_INVAL, 400, "the gateway makes https:// calls only");
    if (head_end > line_end) call->headers.assign(msg.substr(line_end + 2, head_end - line_end));
    call->body.assign(msg.substr(head_end + 4));

    std::string why;
    if (!accepts(pod, req.client_ip, why)) {
        log_refusal(req.client_ip, pod, why);
        return refuse(K32_ERR_PERM, 403, why);
    }
    if (!client_.makes_tls()) return refuse(K32_ERR_NOTSUP, 501, "this control plane makes no TLS connections");
    call->pod = pod;

    // The connection goes to a TLS session; this API worker is free again
    call->sock = conn.take_socket();
    ServiceClient* client = &client_;
    if (!client_.start_tls_session([client, call]() { make_call(*client, *call); })) {
        send_error(call->sock, K32_ERR_BUSY, reason_for(K32_ERR_BUSY), "Retry-After: 1\r\n");
        call->sock.close();
    }
    return true;
}

} // namespace kube32::app
