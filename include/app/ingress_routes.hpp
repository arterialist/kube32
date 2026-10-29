#pragma once

// Routing table of the ingress controller, built from the networking.k8s.io/v1
// Ingress objects in the store plus the Services and Pods they point at.
//
// Matching follows the Ingress specification:
//   * a rule's host is matched against the request's Host header (port
//     removed, case-insensitive); "*.example.com" matches exactly one extra
//     label. Rules for the exact host win over wildcard rules, which win over
//     rules without a host; the first group that has rules for the request's
//     host is the only one consulted.
//   * pathType Exact compares the whole path; Prefix (and ImplementationSpecific)
//     matches element by element ("/api" matches "/api" and "/api/x", not
//     "/apix"), a trailing slash in the rule is ignored.
//   * the longest matching path wins, Exact before Prefix at equal length.
//   * spec.defaultBackend answers what no rule matches; without one the
//     controller answers 404.
//
// Backends:
//   * service {name, port.number | port.name}: the Service in the Ingress's
//     namespace, its selector, the Running pods it selects and the target port
//     (a number, or a containerPort name looked up in each pod).
//   * resource {apiGroup: kube32.io, kind: StaticBundle, name}: files of a
//     bundle compiled into the firmware (images/web/<name>/).
//
// Annotations on an Ingress add routes that need no Service:
//   kube32.io/event-stream: "<path>=<topic>[,<topic>...][;<path>=...]"
//       serves <path> (Exact) as Server-Sent Events: every message published
//       on a listed channel topic becomes one event. Two names are produced
//       by the controller itself rather than by a channel: kube32.cluster
//       (cluster telemetry every 2 s, event "cluster") and kube32.presence
//       (event "presence" with the stream's viewer count when it changes).
//   kube32.io/cluster-telemetry: "<path>"
//       answers GET <path> (Exact) with the cluster telemetry JSON.
//   kube32.io/stats-topic: "<topic>"
//       publishes the Ingress's traffic once a second on a channel topic:
//       {"t":"stats","ingress":"<ns>/<name>","rps":N,"viewers":M}.
// The extra routes apply to the hosts of the Ingress's rules (any host when a
// rule has none or the Ingress has no rules).
//
// Ingresses whose spec.ingressClassName is set to something other than
// "kube32" belong to another controller and are ignored.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kube32::app {

constexpr const char* kIngressClassName = "kube32";
constexpr const char* kStaticBundleGroup = "kube32.io";
constexpr const char* kStaticBundleKind = "StaticBundle";
constexpr const char* kTopicClusterTelemetry = "kube32.cluster";
constexpr const char* kTopicPresence = "kube32.presence";

enum class PathMatch : uint8_t { Exact, Prefix };
enum class BackendKind : uint8_t { Service, StaticBundle, EventStream, Telemetry };

// One pod behind a service backend.
struct IngressEndpoint {
    std::string pod;          // "<namespace>/<name>", the AppPodHost and X-Kube32-Pod key
    std::string ip;           // status.podIP
    uint16_t port{0};         // resolved target port
};

struct IngressRoute {
    std::string host;          // "" = any host, "*.example.com" = wildcard
    std::string path;          // Exact: as written; Prefix: without trailing '/' ("/" stays)
    PathMatch match{PathMatch::Prefix};
    BackendKind kind{BackendKind::Service};
    uint16_t ingress{0};       // index into RouteTable::ingresses

    // Service backend
    std::string service;                   // "<namespace>/<name>" (error messages)
    std::string unavailable;               // why there is no endpoint (empty when endpoints exist)
    std::vector<IngressEndpoint> endpoints;

    // StaticBundle backend
    std::string bundle;

    // EventStream
    std::vector<std::string> topics;
    uint32_t stream_id{0};     // changes whenever the stream's definition changes
};

struct IngressCounters {
    std::atomic<uint64_t> requests{0};
    uint64_t requests_at_last_tick{0};   // touched by the ticking thread only
};

struct IngressInfo {
    std::string key;           // "<namespace>/<name>"
    std::string stats_topic;   // kube32.io/stats-topic, empty when unset
    std::string address_ip;    // status.loadBalancer.ingress[0].ip as stored
    std::shared_ptr<IngressCounters> counters;   // carried across rebuilds
};

class RouteTable {
public:
    std::vector<IngressRoute> routes;
    std::vector<IngressInfo> ingresses;
    bool has_default{false};               // spec.defaultBackend of the first Ingress that has one
    IngressRoute default_backend;
    std::vector<std::string> channel_topics;   // channel topics used by event streams (kube32.* names excluded)

    // Best route for a request, nullptr when nothing matches. `host` is the
    // raw Host header, `path` the request target without the query.
    const IngressRoute* match(std::string_view host, std::string_view path) const;
};

// Builds a table from stored JSON objects. Counters of Ingresses present in
// `previous` are carried over.
std::shared_ptr<RouteTable> build_route_table(const std::vector<std::string>& ingress_jsons,
                                              const std::vector<std::string>& service_jsons,
                                              const std::vector<std::string>& pod_jsons,
                                              const RouteTable* previous);

// Endpoints of Service `ns/name` for its port `port` (0: the Service's first
// port): the Running pods it selects, the target port resolved per pod. False
// with `why` when the Service, the port or a selected pod is missing.
bool resolve_service_endpoints(const std::string& ns, const std::string& name, uint16_t port,
                               const std::vector<std::string>& service_jsons, const std::vector<std::string>& pod_jsons,
                               std::vector<IngressEndpoint>& out, std::string& why);

// Path of a request inside a bundle mounted by `route`: the matched prefix is
// removed for Prefix routes below "/". Always starts with '/'.
std::string bundle_relative_path(const IngressRoute& route, std::string_view request_path);

// "Host" header value -> lowercase host name without the port.
std::string normalize_host(std::string_view host_header);

// True when `path` matches `rule_path` under `match` (see the header comment).
bool path_matches(PathMatch match, std::string_view rule_path, std::string_view path);

} // namespace kube32::app
