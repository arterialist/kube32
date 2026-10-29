#include "api/node_proxy.hpp"
#include "api/route_handler.hpp"
#include "models/kube_models.hpp"
#include "osal/socket.hpp"

#include <cstdio>

namespace kube32::api {

NodeProxyHandler::NodeProxyHandler(storage::Store& store, RouteHandler& routes, std::string local_node,
                                   std::function<bool(HttpConnection&, std::string_view)> local)
    : store_(store), routes_(routes), local_node_(std::move(local_node)), local_(std::move(local)) {}

bool NodeProxyHandler::match(std::string_view path, std::string& out_node, std::string& out_rest) {
    static const std::string_view kPrefix = "/api/v1/nodes/";
    if (path.rfind(kPrefix, 0) != 0) return false;
    std::string_view rest = path.substr(kPrefix.size());
    size_t slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0) return false;
    std::string_view node = rest.substr(0, slash);
    rest.remove_prefix(slash);
    if (rest.rfind("/proxy", 0) != 0) return false;
    rest.remove_prefix(6);
    if (!rest.empty() && rest.front() != '/') return false;
    out_node.assign(node.data(), node.size());
    out_rest = rest.empty() ? std::string("/") : std::string(rest);
    return true;
}

bool NodeProxyHandler::streams_body(const HttpRequest& req) {
    std::string node, rest;
    return match(req.path, node, rest);
}

static bool send_status(HttpConnection& conn, int code, std::string_view reason, const std::string& message) {
    HttpResponse r = RouteHandler::make_status(code, reason, message);
    conn.set_close();
    return conn.send_simple(r.status_code, r.content_type, r.body);
}

bool NodeProxyHandler::handle(HttpConnection& conn) {
    const HttpRequest& req = conn.request();
    std::string node, rest;
    if (!match(req.path, node, rest)) return false;
    if (!routes_.authenticated(req)) return send_status(conn, 401, "Unauthorized", "Unauthorized");

    std::string node_json;
    if (!store_.get("/registry/nodes/" + node, node_json).ok()) {
        return send_status(conn, 404, "NotFound", "nodes \"" + node + "\" not found");
    }
    if (node == local_node_ && local_) {
        if (local_(conn, rest)) return true;
        return send_status(conn, 404, "NotFound", "no such path on node " + node + ": " + rest);
    }
    models::JsonValue doc;
    std::string err;
    std::string ip;
    int64_t port = 0;
    if (models::parse_json(node_json, doc, err)) {
        if (const models::JsonValue* st = doc.get("status")) {
            if (const models::JsonValue* addrs = st->get("addresses"); addrs && addrs->is_array()) {
                for (const auto& a : addrs->arr_val) {
                    if (a.get_string("type", "") == "InternalIP") { ip = a.get_string("address", ""); break; }
                }
            }
            if (const models::JsonValue* de = st->get("daemonEndpoints")) {
                if (const models::JsonValue* ke = de->get("kubeletEndpoint")) port = ke->get_int("Port", 0);
            }
        }
    }
    if (ip.empty() || port <= 0 || port > 65535) {
        return send_status(conn, 503, "ServiceUnavailable", "node " + node + " reports no kubelet endpoint");
    }
    if (!req.query_string.empty()) rest += "?" + req.query_string;
    return forward(conn, ip, static_cast<uint16_t>(port), rest);
}

bool NodeProxyHandler::forward(HttpConnection& conn, const std::string& ip, uint16_t port, const std::string& rest) {
    const HttpRequest& req = conn.request();
    osal::Socket up;
    if (!up.create(osal::SocketType::Tcp) || !up.connect(ip.c_str(), port, 3000)) {
        return send_status(conn, 502, "ServiceUnavailable", "cannot reach the node's kubelet at " + ip + ":" + std::to_string(port));
    }
    up.set_tcp_nodelay(true);
    up.set_timeout(timeout_ms_, timeout_ms_);
    std::string head;
    head.reserve(256 + rest.size());
    head.append(http_method_str(req.method)).append(" ").append(rest).append(" HTTP/1.1\r\n");
    head.append("Host: ").append(ip).append(":").append(std::to_string(port)).append("\r\n");
    const bool chunked = conn.body_chunked();
    if (chunked) head.append("Transfer-Encoding: chunked\r\n");
    else head.append("Content-Length: ").append(std::to_string(req.content_length)).append("\r\n");
    if (!req.content_type.empty()) head.append("Content-Type: ").append(req.content_type).append("\r\n");
    if (!node_token_.empty()) head.append("Authorization: Bearer ").append(node_token_).append("\r\n");
    head.append("Connection: close\r\n\r\n");
    if (!up.send_all(head.data(), head.size())) {
        return send_status(conn, 502, "ServiceUnavailable", "the node's kubelet closed the connection");
    }
    // Body: client -> node through the connection's buffer
    char* buf = conn.scratch();
    const size_t cap = conn.scratch_capacity();
    while (conn.body_remaining() > 0) {
        long n = conn.read_body(buf, cap);
        if (n == 0 && conn.body_remaining() == 0) break;
        if (n <= 0) {
            conn.set_close();
            return send_status(conn, 400, "BadRequest", "request body ended early");
        }
        bool sent = true;
        if (chunked) {
            char size_line[16];
            int sl = snprintf(size_line, sizeof(size_line), "%lx\r\n", n);
            sent = up.send_all(size_line, static_cast<size_t>(sl)) && up.send_all(buf, static_cast<size_t>(n)) && up.send_all("\r\n", 2);
        } else {
            sent = up.send_all(buf, static_cast<size_t>(n));
        }
        if (!sent) break;   // the node answers why (or the relay below fails)
    }
    if (chunked && !up.send_all("0\r\n\r\n", 5)) {
        return send_status(conn, 502, "ServiceUnavailable", "the node's kubelet closed the connection");
    }
    // Response: node -> client as it arrives; the node closes when done
    conn.set_close();
    size_t relayed = 0;
    while (true) {
        ssize_t n = up.recv(buf, cap);
        if (n <= 0) break;
        if (!conn.send_raw(buf, static_cast<size_t>(n))) break;
        relayed += static_cast<size_t>(n);
    }
    if (relayed == 0) return send_status(conn, 504, "Timeout", "no answer from the node's kubelet");
    return true;
}

} // namespace kube32::api
