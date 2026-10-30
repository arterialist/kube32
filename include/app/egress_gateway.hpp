#pragma once

// The egress gateway: https calls of pods on workers, made by the control
// plane for them. A worker makes no TLS connections; its ServiceClient sends
// the call here instead (app/service_client.hpp). On the API server:
//
//   POST /apis/kube32.io/v1/egress?pod=<namespace>/<name>&timeout=<ms>&limit=<bytes>
//   Content-Type: message/http
//
//   <METHOD> https://<host>[:<port>]<path> HTTP/1.1
//   <the pod's request headers>
//
//   <the pod's request body>
//
// The answer is the host's: its status and body, the body chunked as it
// arrives and cut after `limit` + 1 bytes (the extra byte tells the caller
// it did not fit), with X-Kube32-Egress-Result: <status>. A call the gateway
// does not make, or that fails before the host answers, gets an HTTP error
// with X-Kube32-Egress-Result: <K32_ERR_*> and a one-line reason. A call
// that fails once the host's answer has begun ends the connection before the
// last chunk. The call ends `timeout` ms after it arrived.
//
// Who may call. The gateway connects only for pods, on behalf of nodes:
//  - With a bearer token on the API server the request must carry it, as
//    kubelets do. A holder of the token may name any pod; the token already
//    grants the whole API.
//  - Without one, the request must come from the InternalIP of the node the
//    named pod is bound to. That keeps out devices that are not nodes; the
//    API server itself accepts anyone then.
// Either way the pod must exist and be bound to a node (a pod the store
// does not know has no labels for its policies to select), and the call is
// held to that pod's NetworkPolicies as on the control plane's own pods.
// The worker's request crosses the cluster network as plain HTTP, as the
// kubelet's API requests do; TLS covers the connection to the host.
//
// The API worker hands the connection to one of the client's TLS session
// threads (ServiceClient::start_tls_session) and goes back to the API: a
// slow host holds a TLS session, not the API server. With every session in
// use the answer is 503 with K32_ERR_BUSY, at once. A worker that closes the
// connection ends the call and frees the session.

#include "api/http_server.hpp"
#include "app/service_client.hpp"
#include "storage/store.hpp"

#include <string>

namespace kube32::app {

class EgressGateway : public api::HttpHandler {
public:
    static constexpr const char* kPath = "/apis/kube32.io/v1/egress";

    // Other requests go to `next`
    EgressGateway(ServiceClient& client, storage::Store& store, api::RouteHandler& routes, api::HttpHandler* next = nullptr);

    bool handle(api::HttpConnection& conn) override;
    bool streams_body(const api::HttpRequest& req) override;

private:
    // The pod exists, is bound to a node, and (without a token) the request
    // comes from that node; false with `why`
    bool accepts(const std::string& pod, const std::string& client_ip, std::string& why);

    ServiceClient& client_;
    storage::Store& store_;
    api::RouteHandler& routes_;
    api::HttpHandler* next_;
};

} // namespace kube32::app
