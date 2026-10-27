#pragma once

#include "api/http_types.hpp"
#include "storage/store.hpp"
#include "mem/monotonic_arena.hpp"
#include "scheduler/scheduler.hpp"

#include <string_view>
#include <memory>
#include <functional>

namespace kube32::osal {
class Socket;
}

namespace kube32::api {

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
    WebHardwareWorkbench,
    HardwareApiState,
    HardwareApiDisplay,
    HardwareApiSpeaker,
    HardwareApiKeyboard,
    NotFound
};

struct RouteMatch {
    RouteCategory category{RouteCategory::NotFound};
    std::string_view namespace_name;
    std::string_view resource_name;
    std::string_view subresource;
};

class RouteHandler {
public:
    explicit RouteHandler(storage::Store& store, scheduler::Scheduler* scheduler = nullptr);
    ~RouteHandler() = default;

    void set_scheduler(scheduler::Scheduler* scheduler) { scheduler_ = scheduler; }
    void set_lease_callback(std::function<void(const std::string&)> cb) { on_lease_renewed_ = std::move(cb); }

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

    // Endpoint dispatchers
    HttpResponse handle_discovery(const HttpRequest& req, RouteCategory category);
    HttpResponse handle_nodes(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena, osal::Socket* client_sock = nullptr);
    HttpResponse handle_pods(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena, osal::Socket* client_sock = nullptr);
    HttpResponse handle_deployments(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena, osal::Socket* client_sock = nullptr);
    HttpResponse handle_leases(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena, osal::Socket* client_sock = nullptr);
    HttpResponse handle_services(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena);
    HttpResponse handle_daemonsets(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena);
    HttpResponse handle_jobs(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena);
    HttpResponse handle_web_workbench();
    HttpResponse handle_hardware_state();
    HttpResponse handle_hardware_display(const HttpRequest& req);
    HttpResponse handle_hardware_speaker(const HttpRequest& req);
    HttpResponse handle_hardware_keyboard(const HttpRequest& req);
};

} // namespace kube32::api
