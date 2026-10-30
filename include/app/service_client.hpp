#pragma once

// Calls from pods: the host side of k32_http_call (see
// include/runtime/k32_app_abi.h), to Services in the cluster and to hosts
// outside it.
//
// The URL's host resolves as a Kubernetes DNS name does in a pod: <service>
// and <service>.<namespace>.svc[.cluster.local] name a Service;
// <service>.<namespace> names that Service when it exists and a host on the
// network otherwise (the search path ends with the name itself); an IPv4
// address and any other name are hosts outside the cluster.
//
// The Service is resolved to its ready pods as the ingress resolves its
// backends (the Service's selector, the target port by number or container
// port name). A pod on this node is called in-process on the caller's thread
// (AppPodHost::call_nested); any other pod through its node's pod HTTP server
// at the pod's IP and target port, over HTTP/1.1 with a small pool of
// keep-alive connections. Each call carries the chain of pods already serving
// the request (X-Kube32-Call-Chain), and a call that would re-enter one of
// them fails with K32_ERR_LOOP instead of waiting on itself.
//
// A host outside the cluster is looked up with the node's resolver and gets
// a connection of its own (Connection: close), read as the answer arrives
// and closed once it is complete or no longer fits the caller's buffer. The
// lookup, the connect and every read end at the call's deadline and when
// the calling pod is being stopped.
//
// With an EgressPolicySource every connection is checked against the
// NetworkPolicies that restrict the calling pod (app/egress_policy.hpp); a
// call none of them allows fails with K32_ERR_PERM. Name lookups are not
// subject to policy.

#include "app/egress_policy.hpp"
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
    // port). False with `why` when there is no such Service, port or pod;
    // `no_service` (optional) is set when there is no such Service.
    virtual bool resolve(const std::string& ns, const std::string& name, uint16_t port,
                         std::vector<IngressEndpoint>& out, std::string& why, bool* no_service = nullptr) = 0;
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
    // Looks up a host outside the cluster: 0 with its IPv4 address (host
    // byte order), or an errno. Unset: osal::Socket::resolve_ipv4.
    std::function<int(const std::string& host, uint32_t timeout_ms, uint32_t& out)> resolve_host;
};

class ServiceClient {
public:
    explicit ServiceClient(std::unique_ptr<ServiceResolver> resolver, ServiceClientOptions opts = {},
                           std::unique_ptr<EgressPolicySource> egress = nullptr);
    ~ServiceClient();

    // One k32_http_call. Returns the HTTP status or a negative K32_ERR_*.
    int32_t call(const runtime::ServiceCallRequest& req, runtime::BufferResponseSink& sink);

    struct Stats {
        std::atomic<uint64_t> calls{0};
        std::atomic<uint64_t> local{0};
        std::atomic<uint64_t> remote{0};
        std::atomic<uint64_t> external{0};   // calls to hosts outside the cluster
        std::atomic<uint64_t> denied{0};     // by NetworkPolicy
        std::atomic<uint64_t> failed{0};
        std::atomic<uint64_t> reused{0};     // remote calls on a pooled connection
    };
    const Stats& stats() const { return stats_; }

    // A k32_http_call URL taken apart (see the resolution rules above)
    struct Target {
        bool https{false};
        std::string host;             // as written
        uint16_t port{0};             // 0: not given
        std::string path;             // "/...", with the query
        std::string service, ns;      // a Service the host may name (ns empty: the caller's)
        bool external{false};         // a host outside the cluster
        bool or_external{false};      // <service>.<namespace>: outside the cluster when there is no such Service
        uint32_t ip{0};               // the host as an IPv4 address (host byte order), else 0
    };
    static bool parse_target(std::string_view url, Target& out);

private:
    int32_t call_service(const runtime::ServiceCallRequest& req, const Target& t, const std::string& ns,
                         std::vector<IngressEndpoint>& eps, runtime::BufferResponseSink& sink);
    int32_t call_external(const runtime::ServiceCallRequest& req, const Target& t, runtime::BufferResponseSink& sink);
    int32_t call_remote(const runtime::ServiceCallRequest& req, const IngressEndpoint& ep, const std::string& host,
                        const std::string& path, runtime::BufferResponseSink& sink);
    // The caller's NetworkPolicy scope; false: denied before any connection
    bool egress_scope(const runtime::ServiceCallRequest& req, EgressScope& out, const std::string& target);
    void log_denied(std::string_view caller, const std::string& target, const std::string& why);
    osal::Socket take_idle(const std::string& ip, uint16_t port, bool& reused);
    void put_idle(osal::Socket sock, const std::string& ip, uint16_t port);

    std::unique_ptr<ServiceResolver> resolver_;
    ServiceClientOptions opts_;
    std::unique_ptr<EgressPolicySource> egress_;
    Stats stats_;
    std::atomic<uint32_t> rr_{0};
    std::atomic<uint64_t> last_denial_log_ms_{0};

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
