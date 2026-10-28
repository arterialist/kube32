#pragma once

#include "api/http_types.hpp"
#include "api/event_recorder.hpp"
#include "api/metrics_registry.hpp"
#include "storage/store.hpp"
#include "mem/monotonic_arena.hpp"
#include "scheduler/scheduler.hpp"

#include <string_view>
#include <memory>
#include <functional>
#include <vector>
#include "models/kube_models.hpp"

namespace kube32::osal {
class Socket;
}

namespace kube32::api {

class WatchHub;

enum class RouteCategory {
    DiscoveryVersion,
    DiscoveryApi,
    DiscoveryApis,
    DiscoveryApiV1,
    DiscoveryApisAppsV1,
    DiscoveryApisEventsV1,
    DiscoveryOpenApiV3,
    NodesCollection,
    NodeItem,
    NodeStatus,
    PodsClusterCollection,
    PodsNsCollection,
    PodItem,
    PodStatus,
    DeploymentsNsCollection,
    DeploymentItem,
    DeploymentStatus,
    DeploymentScale,
    DiscoveryApisCoordinationV1,
    DiscoveryApisAuthorizationV1,
    DiscoveryApisBatchV1,
    SelfSubjectAccessReviews,
    SelfSubjectRulesReviews,
    NamespacesCollection,
    LeasesNsCollection,
    LeaseItem,
    ServicesNsCollection,
    ServiceItem,
    DaemonSetsNsCollection,
    DaemonSetItem,
    JobsNsCollection,
    JobItem,
    // Added for kubectl/k9s parity
    DiscoveryApisAppsGroup,
    DiscoveryApisEventsGroup,
    DiscoveryApisMetricsGroup,
    DiscoveryApisMetricsV1beta1,
    NamespaceItem,
    EventsCollection,        // core/v1 or events.k8s.io/v1 (see RouteMatch::api_group)
    EventItem,
    ConfigMapsCollection,
    ConfigMapItem,
    SecretsCollection,
    SecretItem,
    PodLog,
    MetricsNodes,
    MetricsNodeItem,
    MetricsPods,
    MetricsPodItem,
    NotFound
};

struct RouteMatch {
    RouteCategory category{RouteCategory::NotFound};
    std::string_view namespace_name;
    std::string_view resource_name;
    std::string_view subresource;
    std::string_view api_group;   // "" for core, else e.g. "events.k8s.io"
};

// Static description of a stored resource kind used by the generic list,
// watch and CRUD paths.
struct ResourceInfo {
    const char* kind;
    const char* api_version;
    const char* list_kind;
    const char* plural;
    const char* root;        // store prefix, e.g. "/registry/pods/"
    bool namespaced;
};

class RouteHandler {
public:
    explicit RouteHandler(storage::Store& store, scheduler::Scheduler* scheduler = nullptr);
    ~RouteHandler() = default;

    void set_scheduler(scheduler::Scheduler* scheduler) { scheduler_ = scheduler; }
    void set_lease_callback(std::function<void(const std::string&)> cb) { on_lease_renewed_ = std::move(cb); }
    // Streams (?watch=true, pod log follow) are handed to this hub; without one
    // the endpoints answer 501.
    void set_watch_hub(WatchHub* hub) { watch_hub_ = hub; }
    WatchHub* watch_hub() const { return watch_hub_; }
    EventRecorder& events() { return events_; }
    MetricsRegistry& metrics() { return metrics_; }

    // Resource description for a route category; false for discovery/subresources.
    static bool resource_info(RouteCategory category, ResourceInfo& out);

    // Fast route pattern matcher
    static RouteMatch match_route(std::string_view path);

    // Primary request handling entry point
    HttpResponse handle_request(const HttpRequest& req, mem::MonotonicArena& arena, osal::Socket* client_sock = nullptr);

    // Standard Kubernetes Status object helper
    static HttpResponse make_status(int status_code, 
                                    std::string_view reason, 
                                    std::string_view message, 
                                    std::string_view name = "", 
                                    std::string_view kind = "");

private:
    storage::Store& store_;
    scheduler::Scheduler* scheduler_{nullptr};
    std::function<void(const std::string&)> on_lease_renewed_;
    WatchHub* watch_hub_{nullptr};
    EventRecorder events_;
    MetricsRegistry metrics_;

    void seed_namespaces();
    // Shared helpers
    std::string list_json(const ResourceInfo& info, const std::vector<std::string>& jsons) const;
    bool filter_list(const HttpRequest& req, std::vector<std::string>& jsons, HttpResponse& err_out) const;
    void stamp_new_object(models::JsonValue& doc, const std::string& ns, const ResourceInfo& info) const;
    HttpResponse handle_watch(const HttpRequest& req, const RouteMatch& match, osal::Socket* client_sock);
    HttpResponse handle_generic(const HttpRequest& req, const RouteMatch& match, const ResourceInfo& info);
    HttpResponse handle_events(const HttpRequest& req, const RouteMatch& match);
    HttpResponse handle_pod_log(const HttpRequest& req, const RouteMatch& match, osal::Socket* client_sock);
    HttpResponse handle_metrics(const HttpRequest& req, const RouteMatch& match);

    // Endpoint dispatchers
    HttpResponse handle_discovery(const HttpRequest& req, RouteCategory category);
    HttpResponse handle_nodes(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena, osal::Socket* client_sock = nullptr);
    HttpResponse handle_pods(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena, osal::Socket* client_sock = nullptr);
    HttpResponse handle_deployments(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena, osal::Socket* client_sock = nullptr);
    HttpResponse handle_leases(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena, osal::Socket* client_sock = nullptr);
    HttpResponse handle_services(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena);
    HttpResponse handle_daemonsets(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena);
    HttpResponse handle_jobs(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena);
};

} // namespace kube32::api
