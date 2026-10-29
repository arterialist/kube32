#pragma once

// The Node proxy subresource on the API server:
//
//   <METHOD> /api/v1/nodes/<node>/proxy/<path>  ->  <METHOD> /<path> on the node's kubelet endpoint
//
// The kubelet endpoint is the node's InternalIP and the port in its status
// (daemonEndpoints.kubeletEndpoint.Port), the pod HTTP server. Requests are
// authenticated like every other API call; the API server presents its own
// bearer token to the node. Bodies are streamed both ways, so uploads of any
// size pass through a fixed buffer. For the API server's own node, which runs
// no pod HTTP server, `local` answers in-process.

#include "api/http_server.hpp"
#include "storage/store.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace kube32::api {

class NodeProxyHandler : public HttpHandler {
public:
    // `local(conn, path)` answers requests for `local_node` ("/storage/x" for
    // .../proxy/storage/x); false from it means "no such path" (404).
    NodeProxyHandler(storage::Store& store, RouteHandler& routes, std::string local_node,
                     std::function<bool(HttpConnection&, std::string_view)> local);

    bool handle(HttpConnection& conn) override;
    bool streams_body(const HttpRequest& req) override;
    // Bearer token sent to nodes (the cluster token), empty for none
    void set_node_token(std::string token) { node_token_ = std::move(token); }
    void set_timeout_ms(uint32_t ms) { timeout_ms_ = ms; }

    // "/api/v1/nodes/<node>/proxy[/<rest>]": node name and "/<rest>"
    static bool match(std::string_view path, std::string& out_node, std::string& out_rest);

private:
    bool forward(HttpConnection& conn, const std::string& ip, uint16_t port, const std::string& rest);

    storage::Store& store_;
    RouteHandler& routes_;
    std::string local_node_;
    std::function<bool(HttpConnection&, std::string_view)> local_;
    std::string node_token_;
    uint32_t timeout_ms_{15000};
};

} // namespace kube32::api
