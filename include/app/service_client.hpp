#pragma once

// Pod-to-pod calls through Services: the host side of k32_http_call (see
// include/runtime/k32_app_abi.h).
//
// The Service is resolved to its ready pods as the ingress resolves its
// backends (the Service's selector, the target port by number or container
// port name). A pod on this node is called in-process on the caller's thread
// (AppPodHost::call_nested); any other pod through its node's pod HTTP server
// at the pod's IP and target port, over HTTP/1.1 with a small pool of
// keep-alive connections. Each call carries the chain of pods already serving
// the request (X-Kube32-Call-Chain), and a call that would re-enter one of
// them fails with K32_ERR_LOOP instead of waiting on itself.

#include "app/ingress_routes.hpp"
#include "runtime/app_pod.hpp"
#include "osal/mutex.hpp"
#include "osal/socket.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace kube32::storage { class Store; }

namespace kube32::app {

class ServiceResolver {
public:
    virtual ~ServiceResolver() = default;
    // Ready pods behind Service `ns/name` for its port `port` (0: the first
    // port). False with `why` when there is no such Service, port or pod.
    virtual bool resolve(const std::string& ns, const std::string& name, uint16_t port,
                         std::vector<IngressEndpoint>& out, std::string& why) = 0;
};

// From the object store (control plane); results are kept until a Service or
// Pod changes.
std::unique_ptr<ServiceResolver> make_store_service_resolver(storage::Store& store);
// From the API server (workers): the Service and its Endpoints, kept for
// `ttl_ms`. `api_get(path, body)` returns the HTTP status.
std::unique_ptr<ServiceResolver> make_api_service_resolver(std::function<int(const std::string&, std::string&)> api_get,
                                                           uint32_t ttl_ms = 3000);

struct ServiceClientOptions {
    size_t pool_size{2};          // idle keep-alive connections kept, all targets together
    uint32_t connect_timeout_ms{2000};
};

class ServiceClient {
public:
    explicit ServiceClient(std::unique_ptr<ServiceResolver> resolver, ServiceClientOptions opts = {});
    ~ServiceClient();

    // One k32_http_call. Returns the HTTP status or a negative K32_ERR_*.
    int32_t call(const runtime::ServiceCallRequest& req, runtime::BufferResponseSink& sink);

    struct Stats {
        std::atomic<uint64_t> calls{0};
        std::atomic<uint64_t> local{0};
        std::atomic<uint64_t> remote{0};
        std::atomic<uint64_t> failed{0};
        std::atomic<uint64_t> reused{0};     // remote calls on a pooled connection
    };
    const Stats& stats() const { return stats_; }

    // Splits "http://<service>[.<namespace>[.svc[.cluster.local]]][:<port>]<path>".
    static bool parse_url(std::string_view url, std::string& service, std::string& ns, uint16_t& port, std::string& path);

private:
    int32_t call_remote(const runtime::ServiceCallRequest& req, const IngressEndpoint& ep, const std::string& host,
                        const std::string& path, runtime::BufferResponseSink& sink);
    osal::Socket take_idle(const std::string& ip, uint16_t port, bool& reused);
    void put_idle(osal::Socket sock, const std::string& ip, uint16_t port);

    std::unique_ptr<ServiceResolver> resolver_;
    ServiceClientOptions opts_;
    Stats stats_;
    std::atomic<uint32_t> rr_{0};

    struct Idle {
        osal::Socket sock;
        std::string ip;
        uint16_t port{0};
    };
    osal::Mutex pool_mutex_;
    std::vector<Idle> idle_;
};

// Makes `client` the k32_http_call implementation (nullptr removes it).
void install_service_client(ServiceClient* client);

} // namespace kube32::app
