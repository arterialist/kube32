#include "api/route_handler.hpp"
#include <iostream>
#include "api/table_serializer.hpp"
#include "api/selectors.hpp"
#include "api/watch_hub.hpp"
#include "models/kube_models.hpp"
#include "runtime/pod_log.hpp"
#include "osal/socket.hpp"
#include "osal/system.hpp"

#include <sstream>

namespace kube32::api {

static models::JsonValue jstr(const std::string& v) { return models::JsonValue{models::JsonType::String, false, 0, v, {}, {}}; }
static models::JsonValue jnum(double v) { return models::JsonValue{models::JsonType::Number, false, v, "", {}, {}}; }

RouteHandler::RouteHandler(storage::Store& store, scheduler::Scheduler* scheduler)
    : store_(store), scheduler_(scheduler), events_(store, 64), metrics_(30000, 32) {
    seed_namespaces();
}

RouteMatch RouteHandler::match_route(std::string_view path) {
    RouteMatch match;

    // 1. Discovery Endpoints
    if (path == "/version") {
        match.category = RouteCategory::DiscoveryVersion;
        return match;
    }
    if (path == "/api") {
        match.category = RouteCategory::DiscoveryApi;
        return match;
    }
    if (path == "/apis") {
        match.category = RouteCategory::DiscoveryApis;
        return match;
    }
    if (path == "/api/v1") {
        match.category = RouteCategory::DiscoveryApiV1;
        return match;
    }
    if (path == "/apis/apps/v1") {
        match.category = RouteCategory::DiscoveryApisAppsV1;
        return match;
    }
    if (path == "/apis/events.k8s.io/v1") {
        match.category = RouteCategory::DiscoveryApisEventsV1;
        return match;
    }
    if (path == "/apis/apps") {
        match.category = RouteCategory::DiscoveryApisAppsGroup;
        return match;
    }
    if (path == "/apis/events.k8s.io") {
        match.category = RouteCategory::DiscoveryApisEventsGroup;
        return match;
    }
    if (path == "/apis/metrics.k8s.io") {
        match.category = RouteCategory::DiscoveryApisMetricsGroup;
        return match;
    }
    if (path == "/apis/metrics.k8s.io/v1beta1") {
        match.category = RouteCategory::DiscoveryApisMetricsV1beta1;
        return match;
    }
    static constexpr std::string_view kMetricsPrefix = "/apis/metrics.k8s.io/v1beta1/";
    if (path.rfind(kMetricsPrefix, 0) == 0) {
        std::string_view rest = path.substr(kMetricsPrefix.size());
        match.api_group = "metrics.k8s.io";
        if (rest == "nodes") { match.category = RouteCategory::MetricsNodes; return match; }
        if (rest.rfind("nodes/", 0) == 0) {
            match.category = RouteCategory::MetricsNodeItem;
            match.resource_name = rest.substr(6);
            return match;
        }
        if (rest == "pods") { match.category = RouteCategory::MetricsPods; return match; }
        if (rest.rfind("namespaces/", 0) == 0) {
            std::string_view sub = rest.substr(11);
            size_t sl = sub.find('/');
            if (sl != std::string_view::npos) {
                match.namespace_name = sub.substr(0, sl);
                std::string_view r2 = sub.substr(sl + 1);
                if (r2 == "pods") { match.category = RouteCategory::MetricsPods; return match; }
                if (r2.rfind("pods/", 0) == 0) {
                    match.category = RouteCategory::MetricsPodItem;
                    match.resource_name = r2.substr(5);
                    return match;
                }
            }
        }
    }
    if (path == "/api/v1/events") {
        match.category = RouteCategory::EventsCollection;
        return match;
    }
    if (path == "/api/v1/configmaps") {
        match.category = RouteCategory::ConfigMapsCollection;
        return match;
    }
    if (path == "/api/v1/secrets") {
        match.category = RouteCategory::SecretsCollection;
        return match;
    }
    if (path == "/apis/events.k8s.io/v1/events") {
        match.category = RouteCategory::EventsCollection;
        match.api_group = "events.k8s.io";
        return match;
    }
    static constexpr std::string_view kEventsNsPrefix = "/apis/events.k8s.io/v1/namespaces/";
    if (path.rfind(kEventsNsPrefix, 0) == 0) {
        std::string_view sub = path.substr(kEventsNsPrefix.size());
        size_t sl = sub.find('/');
        if (sl != std::string_view::npos) {
            match.namespace_name = sub.substr(0, sl);
            match.api_group = "events.k8s.io";
            std::string_view rest = sub.substr(sl + 1);
            if (rest == "events") { match.category = RouteCategory::EventsCollection; return match; }
            if (rest.rfind("events/", 0) == 0) {
                match.category = RouteCategory::EventItem;
                match.resource_name = rest.substr(7);
                return match;
            }
        }
    }
    if (path == "/apis/apps/v1/deployments") {
        match.category = RouteCategory::DeploymentsNsCollection;
        return match;
    }
    if (path == "/apis/coordination.k8s.io" || path == "/apis/coordination.k8s.io/v1") {
        match.category = RouteCategory::DiscoveryApisCoordinationV1;
        return match;
    }
    if (path == "/apis/authorization.k8s.io" || path == "/apis/authorization.k8s.io/v1") {
        match.category = RouteCategory::DiscoveryApisAuthorizationV1;
        return match;
    }
    if (path == "/apis/batch" || path == "/apis/batch/v1") {
        match.category = RouteCategory::DiscoveryApisBatchV1;
        return match;
    }
    if (path.find("/selfsubjectaccessreviews") != std::string_view::npos) {
        match.category = RouteCategory::SelfSubjectAccessReviews;
        return match;
    }
    if (path.find("/selfsubjectrulesreviews") != std::string_view::npos) {
        match.category = RouteCategory::SelfSubjectRulesReviews;
        return match;
    }
    if (path == "/api/v1/namespaces") {
        match.category = RouteCategory::NamespacesCollection;
        return match;
    }
    if (path == "/openapi/v3" || path.rfind("/openapi/v3/", 0) == 0) {
        match.category = RouteCategory::DiscoveryOpenApiV3;
        return match;
    }

    // 2. Nodes Endpoints: /api/v1/nodes, /api/v1/nodes/{name}, /api/v1/nodes/{name}/status
    if (path == "/api/v1/nodes") {
        match.category = RouteCategory::NodesCollection;
        return match;
    }
    if (path.rfind("/api/v1/nodes/", 0) == 0) {
        std::string_view sub = path.substr(14); // Skip "/api/v1/nodes/"
        if (!sub.empty()) {
            size_t slash = sub.find('/');
            if (slash == std::string_view::npos) {
                match.category = RouteCategory::NodeItem;
                match.resource_name = sub;
                return match;
            } else if (sub.substr(slash + 1) == "status") {
                match.category = RouteCategory::NodeStatus;
                match.resource_name = sub.substr(0, slash);
                match.subresource = "status";
                return match;
            }
        }
    }

    // 3. Pods Endpoints:
    // /api/v1/pods
    if (path == "/api/v1/pods") {
        match.category = RouteCategory::PodsClusterCollection;
        return match;
    }

    // Namespaced Pods: /api/v1/namespaces/{ns}/pods, /api/v1/namespaces/{ns}/pods/{name}, /api/v1/namespaces/{ns}/pods/{name}/status
    if (path.rfind("/api/v1/namespaces/", 0) == 0) {
        std::string_view sub = path.substr(19); // Skip "/api/v1/namespaces/"
        size_t slash1 = sub.find('/');
        if (slash1 == std::string_view::npos) {
            match.category = RouteCategory::NamespaceItem;
            match.resource_name = sub;
            return match;
        }
        if (slash1 != std::string_view::npos) {
            match.namespace_name = sub.substr(0, slash1);
            std::string_view rest = sub.substr(slash1 + 1);

            if (rest == "pods") {
                match.category = RouteCategory::PodsNsCollection;
                return match;
            } else if (rest == "events") {
                match.category = RouteCategory::EventsCollection;
                return match;
            } else if (rest.rfind("events/", 0) == 0) {
                match.category = RouteCategory::EventItem;
                match.resource_name = rest.substr(7);
                return match;
            } else if (rest == "configmaps") {
                match.category = RouteCategory::ConfigMapsCollection;
                return match;
            } else if (rest.rfind("configmaps/", 0) == 0) {
                match.category = RouteCategory::ConfigMapItem;
                match.resource_name = rest.substr(11);
                return match;
            } else if (rest == "secrets") {
                match.category = RouteCategory::SecretsCollection;
                return match;
            } else if (rest.rfind("secrets/", 0) == 0) {
                match.category = RouteCategory::SecretItem;
                match.resource_name = rest.substr(8);
                return match;
            } else if (rest == "services") {
                match.category = RouteCategory::ServicesNsCollection;
                return match;
            } else if (rest.rfind("services/", 0) == 0) {
                match.category = RouteCategory::ServiceItem;
                match.resource_name = rest.substr(9);
                return match;
            } else if (rest.rfind("pods/", 0) == 0) {
                std::string_view pod_rest = rest.substr(5);
                size_t slash2 = pod_rest.find('/');
                if (slash2 == std::string_view::npos) {
                    match.category = RouteCategory::PodItem;
                    match.resource_name = pod_rest;
                    return match;
                } else if (pod_rest.substr(slash2 + 1) == "status") {
                    match.category = RouteCategory::PodStatus;
                    match.resource_name = pod_rest.substr(0, slash2);
                    match.subresource = "status";
                    return match;
                } else if (pod_rest.substr(slash2 + 1) == "log") {
                    match.category = RouteCategory::PodLog;
                    match.resource_name = pod_rest.substr(0, slash2);
                    match.subresource = "log";
                    return match;
                }
            }
        }
    }

    // 4. Deployments Endpoints:
    // /apis/apps/v1/namespaces/{ns}/deployments
    // /apis/apps/v1/namespaces/{ns}/deployments/{name}
    // /apis/apps/v1/namespaces/{ns}/deployments/{name}/status
    // /apis/apps/v1/namespaces/{ns}/deployments/{name}/scale
    if (path.rfind("/apis/apps/v1/namespaces/", 0) == 0) {
        std::string_view sub = path.substr(25); // Skip "/apis/apps/v1/namespaces/" (25 chars)
        size_t slash1 = sub.find('/');
        if (slash1 != std::string_view::npos) {
            match.namespace_name = sub.substr(0, slash1);
            std::string_view rest = sub.substr(slash1 + 1);

            if (rest == "deployments") {
                match.category = RouteCategory::DeploymentsNsCollection;
                return match;
            } else if (rest == "daemonsets") {
                match.category = RouteCategory::DaemonSetsNsCollection;
                return match;
            } else if (rest.rfind("daemonsets/", 0) == 0) {
                match.category = RouteCategory::DaemonSetItem;
                match.resource_name = rest.substr(11);
                return match;
            } else if (rest.rfind("deployments/", 0) == 0) {
                std::string_view dep_rest = rest.substr(12);
                size_t slash2 = dep_rest.find('/');
                if (slash2 == std::string_view::npos) {
                    match.category = RouteCategory::DeploymentItem;
                    match.resource_name = dep_rest;
                    return match;
                } else {
                    std::string_view subres = dep_rest.substr(slash2 + 1);
                    if (subres == "status") {
                        match.category = RouteCategory::DeploymentStatus;
                        match.resource_name = dep_rest.substr(0, slash2);
                        match.subresource = "status";
                        return match;
                    } else if (subres == "scale") {
                        match.category = RouteCategory::DeploymentScale;
                        match.resource_name = dep_rest.substr(0, slash2);
                        match.subresource = "scale";
                        return match;
                    }
                }
            }
        }
    }

    // 5. Coordination Leases Endpoints:
    // /apis/coordination.k8s.io/v1/namespaces/{ns}/leases
    // /apis/coordination.k8s.io/v1/namespaces/{ns}/leases/{name}
    // /apis/coordination.k8s.io/v1/leases
    if (path == "/apis/coordination.k8s.io/v1/leases") {
        match.category = RouteCategory::LeasesNsCollection;
        return match;
    }
    static constexpr std::string_view kCoordNsPrefix = "/apis/coordination.k8s.io/v1/namespaces/";
    if (path.rfind(kCoordNsPrefix, 0) == 0) {
        std::string_view sub = path.substr(kCoordNsPrefix.length());
        size_t slash1 = sub.find('/');
        if (slash1 != std::string_view::npos) {
            match.namespace_name = sub.substr(0, slash1);
            std::string_view rest = sub.substr(slash1 + 1);

            if (rest == "leases") {
                match.category = RouteCategory::LeasesNsCollection;
                return match;
            } else if (rest.rfind("leases/", 0) == 0) {
                std::string_view lease_rest = rest.substr(7);
                size_t slash2 = lease_rest.find('/');
                if (slash2 == std::string_view::npos) {
                    match.category = RouteCategory::LeaseItem;
                    match.resource_name = lease_rest;
                    return match;
                }
            }
        }
    }

    // 6. Batch Jobs Endpoints
    static constexpr std::string_view kBatchNsPrefix = "/apis/batch/v1/namespaces/";
    if (path.rfind(kBatchNsPrefix, 0) == 0) {
        std::string_view sub = path.substr(kBatchNsPrefix.length());
        size_t slash1 = sub.find('/');
        if (slash1 != std::string_view::npos) {
            match.namespace_name = sub.substr(0, slash1);
            std::string_view rest = sub.substr(slash1 + 1);

            if (rest == "jobs") {
                match.category = RouteCategory::JobsNsCollection;
                return match;
            } else if (rest.rfind("jobs/", 0) == 0) {
                std::string_view job_rest = rest.substr(5);
                size_t slash2 = job_rest.find('/');
                if (slash2 == std::string_view::npos) {
                    match.category = RouteCategory::JobItem;
                    match.resource_name = job_rest;
                    return match;
                }
            }
        }
    }

    // Root collections across namespaces
    if (path == "/api/v1/services") {
        match.category = RouteCategory::ServicesNsCollection;
        return match;
    }
    if (path == "/apis/apps/v1/daemonsets") {
        match.category = RouteCategory::DaemonSetsNsCollection;
        return match;
    }
    if (path == "/apis/batch/v1/jobs") {
        match.category = RouteCategory::JobsNsCollection;
        return match;
    }

    match.category = RouteCategory::NotFound;
    return match;
}

static std::string escape_json(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

static HttpResponse list_response(const HttpRequest& req, const ResourceInfo& info, const std::vector<std::string>& jsons,
                                  const std::string& plain_list_json, uint64_t rv);

HttpResponse RouteHandler::make_status(int status_code, 
                                      std::string_view reason, 
                                      std::string_view message, 
                                      std::string_view name, 
                                      std::string_view kind) {
    HttpResponse resp;
    resp.status_code = status_code;
    resp.content_type = "application/json";

    std::string is_failure = (status_code >= 400) ? "Failure" : "Success";
    std::string details_str = "{}";
    if (!name.empty() || !kind.empty()) {
        details_str = "{\"name\":\"" + escape_json(name) + "\",\"kind\":\"" + escape_json(kind) + "\"}";
    }

    resp.body = "{\"kind\":\"Status\",\"apiVersion\":\"v1\",\"metadata\":{},"
                "\"status\":\"" + is_failure + "\","
                "\"message\":\"" + escape_json(message) + "\","
                "\"reason\":\"" + escape_json(reason) + "\","
                "\"details\":" + details_str + ","
                "\"code\":" + std::to_string(status_code) + "}";
    return resp;
}

HttpResponse RouteHandler::handle_request(const HttpRequest& req, mem::MonotonicArena& arena, osal::Socket* client_sock) {
    // Arena Scratchpad Allocation 1: Normalized URI & Path Tokens
    size_t uri_len = req.raw_uri.empty() ? req.path.size() : req.raw_uri.size();
    char* uri_scratch = static_cast<char*>(arena.allocate(uri_len + 16, 4));
    if (uri_scratch) {
        std::memcpy(uri_scratch, req.path.data(), req.path.size());
        uri_scratch[req.path.size()] = '\0';
    }

    // Arena Scratchpad Allocation 2: Query String Parameter Parsing
    struct QueryParamView {
        std::string_view key;
        std::string_view value;
    };
    constexpr size_t MAX_PARAMS = 16;
    auto* params = static_cast<QueryParamView*>(arena.allocate(sizeof(QueryParamView) * MAX_PARAMS, alignof(QueryParamView)));
    size_t param_count = 0;
    if (params && !req.query_string.empty()) {
        std::string_view qs = req.query_string;
        while (!qs.empty() && param_count < MAX_PARAMS) {
            size_t ampersand = qs.find('&');
            std::string_view token = (ampersand == std::string_view::npos) ? qs : qs.substr(0, ampersand);
            size_t eq = token.find('=');
            if (eq != std::string_view::npos) {
                params[param_count++] = {token.substr(0, eq), token.substr(eq + 1)};
            } else {
                params[param_count++] = {token, ""};
            }
            if (ampersand == std::string_view::npos) break;
            qs = qs.substr(ampersand + 1);
        }
    }

    RouteMatch match = match_route(req.path);

    // This server speaks JSON only. Typed kubectl clients may send protobuf
    // first; 415 makes client-go retry the request as JSON. Access reviews
    // are the exception: k9s posts them as protobuf on every view and does
    // not retry, and the answer does not depend on the body (always allowed).
    bool protobuf_body = !req.body.empty() && req.content_type.rfind("application/vnd.kubernetes.protobuf", 0) == 0;
    bool body_free_review = (match.category == RouteCategory::SelfSubjectAccessReviews || match.category == RouteCategory::SelfSubjectRulesReviews);
    if (protobuf_body && !body_free_review) {
        HttpResponse resp = make_status(415, "UnsupportedMediaType",
                                        "the body could not be decoded: application/vnd.kubernetes.protobuf is not supported, send application/json");
        return resp;
    }

    // fieldValidation=Strict (what kubectl apply/create send by default, see
    // openapi_v3_document): reject unknown top-level and metadata fields, as
    // kube-apiserver does. Nested fields are not checked.
    if ((req.method == HttpMethod::Post || req.method == HttpMethod::Put || req.method == HttpMethod::Patch) && !req.body.empty()) {
        std::string fv;
        if (query_param(req.query_string, "fieldValidation", fv) && fv == "Strict") {
            std::string unknown;
            if (!strict_fields_ok(match.category, req.body, unknown)) {
                return make_status(400, "BadRequest", "strict decoding error: unknown field \"" + unknown + "\"");
            }
        }
    }

    // ?watch=true on any collection becomes a long-lived stream owned by the hub
    if (req.method == HttpMethod::Get && (query_flag(req.query_string, "watch"))) {
        ResourceInfo info;
        if (resource_info(match.category, info)) {
            return handle_watch(req, match, client_sock);
        }
    }

    switch (match.category) {
        case RouteCategory::DiscoveryApisAppsGroup:
        case RouteCategory::DiscoveryApisEventsGroup:
        case RouteCategory::DiscoveryApisMetricsGroup:
        case RouteCategory::DiscoveryApisMetricsV1beta1:
        case RouteCategory::DiscoveryVersion:
        case RouteCategory::DiscoveryApi:
        case RouteCategory::DiscoveryApis:
        case RouteCategory::DiscoveryApiV1:
        case RouteCategory::DiscoveryApisAppsV1:
        case RouteCategory::DiscoveryApisEventsV1:
        case RouteCategory::DiscoveryApisCoordinationV1:
        case RouteCategory::DiscoveryApisAuthorizationV1:
        case RouteCategory::DiscoveryApisBatchV1:
        case RouteCategory::SelfSubjectAccessReviews:
        case RouteCategory::SelfSubjectRulesReviews:
        case RouteCategory::DiscoveryOpenApiV3:
            return handle_discovery(req, match.category);

        case RouteCategory::NamespacesCollection:
        case RouteCategory::NamespaceItem:
        case RouteCategory::ConfigMapsCollection:
        case RouteCategory::ConfigMapItem:
        case RouteCategory::SecretsCollection:
        case RouteCategory::SecretItem: {
            ResourceInfo info;
            resource_info(match.category, info);
            return handle_generic(req, match, info);
        }

        case RouteCategory::EventsCollection:
        case RouteCategory::EventItem:
            return handle_events(req, match);

        case RouteCategory::PodLog:
            return handle_pod_log(req, match, client_sock);

        case RouteCategory::MetricsNodes:
        case RouteCategory::MetricsNodeItem:
        case RouteCategory::MetricsPods:
        case RouteCategory::MetricsPodItem:
            return handle_metrics(req, match);

        case RouteCategory::ServicesNsCollection:
        case RouteCategory::ServiceItem:
            return handle_services(req, match, arena);

        case RouteCategory::DaemonSetsNsCollection:
        case RouteCategory::DaemonSetItem:
            return handle_daemonsets(req, match, arena);

        case RouteCategory::JobsNsCollection:
        case RouteCategory::JobItem:
            return handle_jobs(req, match, arena);

        case RouteCategory::NodesCollection:
        case RouteCategory::NodeItem:
        case RouteCategory::NodeStatus:
            return handle_nodes(req, match, arena, client_sock);

        case RouteCategory::PodsClusterCollection:
        case RouteCategory::PodsNsCollection:
        case RouteCategory::PodItem:
        case RouteCategory::PodStatus:
            return handle_pods(req, match, arena, client_sock);

        case RouteCategory::DeploymentsNsCollection:
        case RouteCategory::DeploymentItem:
        case RouteCategory::DeploymentStatus:
        case RouteCategory::DeploymentScale:
            return handle_deployments(req, match, arena, client_sock);

        case RouteCategory::LeasesNsCollection:
        case RouteCategory::LeaseItem:
            return handle_leases(req, match, arena, client_sock);

        case RouteCategory::NotFound:
        default:
            return make_status(404, "NotFound", "Endpoint not found: " + std::string(req.path));
    }
}

HttpResponse RouteHandler::handle_discovery(const HttpRequest& req, RouteCategory category) {
    // SelfSubjectAccessReview / SelfSubjectRulesReview are created with POST (kubectl auth can-i, k9s)
    bool is_review = (category == RouteCategory::SelfSubjectAccessReviews || category == RouteCategory::SelfSubjectRulesReviews);
    if (req.method != HttpMethod::Get && req.method != HttpMethod::Head && !(is_review && req.method == HttpMethod::Post)) {
        if (category == RouteCategory::DiscoveryApis) {
            return make_status(405, "MethodNotAllowed", "DELETE not allowed on root APIGroupList /apis");
        }
        return make_status(405, "MethodNotAllowed", 
                           std::string(http_method_str(req.method)) + " not allowed on discovery endpoint " + req.path);
    }

    HttpResponse resp;
    resp.status_code = 200;
    resp.content_type = "application/json";

    switch (category) {
        case RouteCategory::DiscoveryVersion:
            resp.body = "{\"major\":\"1\",\"minor\":\"33\",\"gitVersion\":\"v1.33.0-kube32\","
                        "\"gitCommit\":\"esp32-kube32-idf\",\"gitTreeState\":\"clean\","
                        "\"buildDate\":\"2026-10-26T00:00:00Z\",\"goVersion\":\"go1.22.0\","
                        "\"compiler\":\"gcc-xtensa\",\"platform\":\"freertos/xtensa-esp32\"}";
            break;

        case RouteCategory::DiscoveryApi:
            resp.body = "{\"kind\":\"APIVersions\",\"versions\":[\"v1\"],"
                        "\"serverAddressByClientCIDRs\":[{\"clientCIDR\":\"0.0.0.0/0\",\"serverAddress\":\"127.0.0.1:6443\"}]}";
            break;

        case RouteCategory::DiscoveryApis:
            resp.body = "{\"kind\":\"APIGroupList\",\"apiVersion\":\"v1\",\"groups\":["
                        "{\"name\":\"authorization.k8s.io\",\"versions\":[{\"groupVersion\":\"authorization.k8s.io/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"authorization.k8s.io/v1\",\"version\":\"v1\"}},"
                        "{\"name\":\"apps\",\"versions\":[{\"groupVersion\":\"apps/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"apps/v1\",\"version\":\"v1\"}},"
                        "{\"name\":\"batch\",\"versions\":[{\"groupVersion\":\"batch/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"batch/v1\",\"version\":\"v1\"}},"
                        "{\"name\":\"events.k8s.io\",\"versions\":[{\"groupVersion\":\"events.k8s.io/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"events.k8s.io/v1\",\"version\":\"v1\"}},"
                        "{\"name\":\"coordination.k8s.io\",\"versions\":[{\"groupVersion\":\"coordination.k8s.io/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"coordination.k8s.io/v1\",\"version\":\"v1\"}},"
                        "{\"name\":\"metrics.k8s.io\",\"versions\":[{\"groupVersion\":\"metrics.k8s.io/v1beta1\",\"version\":\"v1beta1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"metrics.k8s.io/v1beta1\",\"version\":\"v1beta1\"}}"
                        "]}";
            break;

        case RouteCategory::DiscoveryApisAppsGroup:
            resp.body = "{\"kind\":\"APIGroup\",\"apiVersion\":\"v1\",\"name\":\"apps\","
                        "\"versions\":[{\"groupVersion\":\"apps/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"apps/v1\",\"version\":\"v1\"}}";
            break;

        case RouteCategory::DiscoveryApisEventsGroup:
            resp.body = "{\"kind\":\"APIGroup\",\"apiVersion\":\"v1\",\"name\":\"events.k8s.io\","
                        "\"versions\":[{\"groupVersion\":\"events.k8s.io/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"events.k8s.io/v1\",\"version\":\"v1\"}}";
            break;

        case RouteCategory::DiscoveryApisMetricsGroup:
            resp.body = "{\"kind\":\"APIGroup\",\"apiVersion\":\"v1\",\"name\":\"metrics.k8s.io\","
                        "\"versions\":[{\"groupVersion\":\"metrics.k8s.io/v1beta1\",\"version\":\"v1beta1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"metrics.k8s.io/v1beta1\",\"version\":\"v1beta1\"}}";
            break;

        case RouteCategory::DiscoveryApisMetricsV1beta1:
            resp.body = "{\"kind\":\"APIResourceList\",\"apiVersion\":\"v1\",\"groupVersion\":\"metrics.k8s.io/v1beta1\",\"resources\":["
                        "{\"name\":\"nodes\",\"singularName\":\"\",\"namespaced\":false,\"kind\":\"NodeMetrics\",\"verbs\":[\"get\",\"list\"]},"
                        "{\"name\":\"pods\",\"singularName\":\"\",\"namespaced\":true,\"kind\":\"PodMetrics\",\"verbs\":[\"get\",\"list\"]}"
                        "]}";
            break;

        case RouteCategory::DiscoveryApiV1:
            resp.body = "{\"kind\":\"APIResourceList\",\"apiVersion\":\"v1\",\"groupVersion\":\"v1\",\"resources\":["
                        "{\"name\":\"nodes\",\"singularName\":\"node\",\"namespaced\":false,\"kind\":\"Node\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"update\",\"patch\",\"delete\"],\"shortNames\":[\"no\"]},"
                        "{\"name\":\"nodes/status\",\"singularName\":\"node\",\"namespaced\":false,\"kind\":\"Node\","
                        "\"verbs\":[\"get\",\"patch\",\"update\"]},"
                        "{\"name\":\"pods\",\"singularName\":\"pod\",\"namespaced\":true,\"kind\":\"Pod\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"update\",\"patch\",\"delete\"],\"shortNames\":[\"po\"],\"categories\":[\"all\"]},"
                        "{\"name\":\"pods/status\",\"singularName\":\"pod\",\"namespaced\":true,\"kind\":\"Pod\","
                        "\"verbs\":[\"get\",\"patch\",\"update\"]},"
                        "{\"name\":\"services\",\"singularName\":\"service\",\"namespaced\":true,\"kind\":\"Service\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"update\",\"patch\",\"delete\"],\"shortNames\":[\"svc\"],\"categories\":[\"all\"]},"
                        "{\"name\":\"pods/log\",\"singularName\":\"pod\",\"namespaced\":true,\"kind\":\"Pod\","
                        "\"verbs\":[\"get\"]},"
                        "{\"name\":\"namespaces\",\"singularName\":\"namespace\",\"namespaced\":false,\"kind\":\"Namespace\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"delete\"],\"shortNames\":[\"ns\"]},"
                        "{\"name\":\"events\",\"singularName\":\"event\",\"namespaced\":true,\"kind\":\"Event\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"patch\",\"update\",\"delete\"],\"shortNames\":[\"ev\"]},"
                        "{\"name\":\"configmaps\",\"singularName\":\"configmap\",\"namespaced\":true,\"kind\":\"ConfigMap\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"update\",\"patch\",\"delete\"],\"shortNames\":[\"cm\"]},"
                        "{\"name\":\"secrets\",\"singularName\":\"secret\",\"namespaced\":true,\"kind\":\"Secret\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"update\",\"patch\",\"delete\"]}"
                        "]}";
            break;

        case RouteCategory::DiscoveryApisAppsV1:
            resp.body = "{\"kind\":\"APIResourceList\",\"apiVersion\":\"v1\",\"groupVersion\":\"apps/v1\",\"resources\":["
                        "{\"name\":\"deployments\",\"singularName\":\"deployment\",\"namespaced\":true,\"kind\":\"Deployment\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"update\",\"patch\",\"delete\"],\"shortNames\":[\"deploy\"],\"categories\":[\"all\"]},"
                        "{\"name\":\"daemonsets\",\"singularName\":\"daemonset\",\"namespaced\":true,\"kind\":\"DaemonSet\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"update\",\"patch\",\"delete\"],\"shortNames\":[\"ds\"],\"categories\":[\"all\"]},"
                        "{\"name\":\"deployments/status\",\"singularName\":\"deployment\",\"namespaced\":true,\"kind\":\"Deployment\","
                        "\"verbs\":[\"get\",\"patch\",\"update\"]},"
                        "{\"name\":\"deployments/scale\",\"singularName\":\"deployment\",\"namespaced\":true,\"group\":\"autoscaling\",\"version\":\"v1\",\"kind\":\"Scale\","
                        "\"verbs\":[\"get\",\"patch\",\"update\"]}"
                        "]}";
            break;

        case RouteCategory::DiscoveryApisBatchV1:
            resp.body = "{\"kind\":\"APIResourceList\",\"apiVersion\":\"v1\",\"groupVersion\":\"batch/v1\",\"resources\":["
                        "{\"name\":\"jobs\",\"singularName\":\"job\",\"namespaced\":true,\"kind\":\"Job\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"update\",\"patch\",\"delete\"],\"shortNames\":[\"job\"],\"categories\":[\"all\"]}"
                        "]}";
            break;

        case RouteCategory::DiscoveryApisEventsV1:
            resp.body = "{\"kind\":\"APIResourceList\",\"apiVersion\":\"v1\",\"groupVersion\":\"events.k8s.io/v1\",\"resources\":["
                        "{\"name\":\"events\",\"singularName\":\"event\",\"namespaced\":true,\"kind\":\"Event\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"patch\",\"update\",\"delete\"],\"shortNames\":[\"ev\"]}"
                        "]}";
            break;

        case RouteCategory::DiscoveryApisCoordinationV1:
            resp.body = "{\"kind\":\"APIResourceList\",\"apiVersion\":\"v1\",\"groupVersion\":\"coordination.k8s.io/v1\",\"resources\":["
                        "{\"name\":\"leases\",\"singularName\":\"lease\",\"namespaced\":true,\"kind\":\"Lease\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"update\",\"patch\",\"delete\"]}"
                        "]}";
            break;

        case RouteCategory::DiscoveryApisAuthorizationV1:
            resp.body = "{\"kind\":\"APIResourceList\",\"apiVersion\":\"v1\",\"groupVersion\":\"authorization.k8s.io/v1\",\"resources\":["
                        "{\"name\":\"selfsubjectaccessreviews\",\"singularName\":\"selfsubjectaccessreview\",\"namespaced\":false,\"kind\":\"SelfSubjectAccessReview\",\"verbs\":[\"create\"]},"
                        "{\"name\":\"selfsubjectrulesreviews\",\"singularName\":\"selfsubjectrulesreview\",\"namespaced\":false,\"kind\":\"SelfSubjectRulesReview\",\"verbs\":[\"create\"]}"
                        "]}";
            break;

        case RouteCategory::SelfSubjectAccessReviews:
            resp.status_code = 201;
            resp.body = "{\"kind\":\"SelfSubjectAccessReview\",\"apiVersion\":\"authorization.k8s.io/v1\",\"status\":{\"allowed\":true,\"reason\":\"Kube32 cluster authorization is open\"}}";
            break;

        case RouteCategory::SelfSubjectRulesReviews:
            resp.status_code = 201;
            resp.body = "{\"kind\":\"SelfSubjectRulesReview\",\"apiVersion\":\"authorization.k8s.io/v1\",\"status\":{\"resourceRules\":[{\"verbs\":[\"*\"],\"apiGroups\":[\"*\"],\"resources\":[\"*\"]}],\"nonResourceRules\":[{\"verbs\":[\"*\"],\"nonResourceURLs\":[\"*\"]}],\"incomplete\":false}}";
            break;

        case RouteCategory::DiscoveryOpenApiV3:
            resp.body = openapi_v3_document(req.path);
            if (resp.body.empty()) return make_status(404, "NotFound", "no OpenAPI document for " + req.path);
            break;

        default:
            return make_status(404, "NotFound", "Endpoint not found: " + req.path);
    }

    if (req.method == HttpMethod::Head) {
        resp.body = "";
    }

    return resp;
}

HttpResponse RouteHandler::handle_nodes(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena, osal::Socket* client_sock) {
    std::string name(match.resource_name);

    if (match.category == RouteCategory::NodesCollection) {
        if (req.method == HttpMethod::Head) {
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = req.as_table ? "application/json;as=Table;v=v1;g=meta.k8s.io" : "application/json";
            resp.body = "";
            resp.is_chunked = false;
            return resp;
        } else if (req.method == HttpMethod::Get) {
            std::vector<std::string> node_jsons;
            store_.list("/registry/nodes/", node_jsons);
            HttpResponse sel_err;
            if (!filter_list(req, node_jsons, sel_err)) return sel_err;
            const uint64_t rv = store_.current_version();
            if (client_sock != nullptr) {
                ChunkedStreamWriter writer(*client_sock);
                if (req.as_table) {
                    TableSerializer::serialize_nodes_table_from_json(writer, node_jsons, rv);
                } else {
                    TableSerializer::serialize_nodes_list_from_json(writer, node_jsons, rv);
                }
                HttpResponse resp;
                resp.status_code = 200;
                resp.is_chunked = true;
                return resp;
            } else {
                HttpResponse resp;
                resp.status_code = 200;
                if (req.as_table) {
                    resp.content_type = "application/json;as=Table;v=v1;g=meta.k8s.io";
                    resp.body = TableSerializer::build_nodes_table_json(node_jsons, rv);
                } else {
                    resp.content_type = "application/json";
                    resp.body = TableSerializer::build_nodes_list_json(node_jsons, rv);
                }
                resp.is_chunked = false;
                return resp;
            }
        } else if (req.method == HttpMethod::Post) {
            if (req.body.empty()) {
                return make_status(400, "BadRequest", "Failed to parse request body: empty body");
            }
            models::JsonValue doc;
            std::string err;
            if (!models::parse_json(req.body, doc, err) || !doc.is_object()) {
                return make_status(400, "BadRequest", "Failed to parse request body: " + err);
            }
            const models::JsonValue* meta = doc.get("metadata");
            std::string node_name = meta ? meta->get_string("name", "") : "";
            if (node_name.empty()) {
                return make_status(400, "BadRequest", "Node is invalid: metadata.name is required");
            }

            char* nkey_buf = static_cast<char*>(arena.allocate(128, 4));
            int nklen = snprintf(nkey_buf, 128, "/registry/nodes/%.*s", static_cast<int>(node_name.size()), node_name.data());
            std::string key(nkey_buf, nklen > 0 ? static_cast<size_t>(nklen) : 0);

            std::string existing;
            if (store_.get(key, existing).ok()) {
                return make_status(409, "AlreadyExists", "nodes \"" + node_name + "\" already exists", node_name, "nodes");
            }

            ResourceInfo ninfo;
            resource_info(RouteCategory::NodesCollection, ninfo);
            stamp_new_object(doc, "", ninfo);
            std::string node_json = doc.to_string();
            store_.put(key, node_json, false);
            if (on_lease_renewed_) {
                on_lease_renewed_(node_name);
            }
            {
                EventSpec ev;
                ev.involved.kind = "Node";
                ev.involved.name = node_name;
                ev.involved.uid = doc.get("metadata") ? doc.get("metadata")->get_string("uid", "") : "";
                ev.reason = "RegisteredNode";
                ev.message = "Node " + node_name + " event: Registered Node " + node_name + " in Controller";
                ev.component = "node-controller";
                events_.record(ev);
            }
            HttpResponse resp;
            resp.status_code = 201;
            resp.content_type = "application/json";
            resp.body = node_json;
            return resp;
        } else if (req.method == HttpMethod::Put) {
            return make_status(405, "MethodNotAllowed", "PUT not allowed on collection endpoint " + req.path);
        } else {
            return make_status(405, "MethodNotAllowed", std::string(http_method_str(req.method)) + " not allowed on collection endpoint " + req.path);
        }
    }

    // NodeItem or NodeStatus
    if (req.method == HttpMethod::Post) {
        return make_status(405, "MethodNotAllowed", "POST not allowed on resource instance " + req.path);
    }

    char* key_buf = static_cast<char*>(arena.allocate(128, 4));
    int klen = snprintf(key_buf, 128, "/registry/nodes/%.*s", static_cast<int>(match.resource_name.size()), match.resource_name.data());
    std::string key(key_buf, klen > 0 ? static_cast<size_t>(klen) : 0);

    if (req.method == HttpMethod::Put) {
        if (req.body.empty()) {
            return make_status(400, "BadRequest", "Failed to parse request body: empty body");
        }
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(req.body, doc, err) || !doc.is_object()) {
            return make_status(400, "BadRequest", "Failed to parse request body: " + err);
        }
        std::string existing_json;
        if (!store_.get(key, existing_json).ok()) {
            return make_status(404, "NotFound", "nodes \"" + name + "\" not found", name, "nodes");
        }
        store_.put(key, req.body, true);
        if (on_lease_renewed_) {
            on_lease_renewed_(name);
        }
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = req.body;
        return resp;
    } else if (req.method == HttpMethod::Patch) {
        if (req.body.empty()) {
            return make_status(400, "BadRequest", "Failed to parse request body: empty body");
        }
        models::JsonValue patch_doc;
        std::string err;
        if (!models::parse_json(req.body, patch_doc, err)) {
            return make_status(400, "BadRequest", "Failed to parse request body: " + err);
        }
        std::string existing_json;
        if (!store_.get(key, existing_json).ok()) {
            return make_status(404, "NotFound", "nodes \"" + name + "\" not found", name, "nodes");
        }
        models::JsonValue target_doc;
        if (!models::parse_json(existing_json, target_doc, err)) {
            return make_status(500, "InternalError", "Failed to parse existing resource");
        }
        models::merge_json(target_doc, patch_doc);
        std::string updated_json = target_doc.to_string();
        store_.put(key, updated_json, true);
        if (on_lease_renewed_) {
            on_lease_renewed_(name);
        }

        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = updated_json;
        return resp;
    }

    std::string existing_json;
    storage::Status get_s = store_.get(key, existing_json);
    if (!get_s.ok()) {
        return make_status(404, "NotFound", "nodes \"" + name + "\" not found", name, "nodes");
    }

    if (req.method == HttpMethod::Head) {
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = "";
        resp.is_chunked = false;
        return resp;
    } else if (req.method == HttpMethod::Get) {
        HttpResponse resp;
        resp.status_code = 200;
        // kubectl get <kind> <name> asks for a Table; answer with one row
        if (req.as_table) {
            resp.content_type = "application/json;as=Table;v=v1;g=meta.k8s.io";
            resp.body = TableSerializer::build_nodes_table_json(std::vector<std::string>{existing_json}, store_.current_version());
        } else {
            resp.content_type = "application/json";
            resp.body = existing_json;
        }
        return resp;
    } else if (req.method == HttpMethod::Delete) {
        store_.remove(key);
        return make_status(200, "Success", "nodes \"" + name + "\" deleted");
    }

    return make_status(405, "MethodNotAllowed", std::string(http_method_str(req.method)) + " not allowed on " + req.path);
}

HttpResponse RouteHandler::handle_pods(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena, osal::Socket* client_sock) {
    if (match.category == RouteCategory::PodsClusterCollection) {
        if (req.method == HttpMethod::Head) {
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = req.as_table ? "application/json;as=Table;v=v1;g=meta.k8s.io" : "application/json";
            resp.body = "";
            resp.is_chunked = false;
            return resp;
        } else if (req.method == HttpMethod::Get) {
            std::vector<std::string> pod_jsons;
            store_.list("/registry/pods/", pod_jsons);
            // fieldSelector (kubelets: spec.nodeName=<node>) and labelSelector
            HttpResponse sel_err;
            if (!filter_list(req, pod_jsons, sel_err)) return sel_err;
            const uint64_t rv = store_.current_version();
            if (client_sock != nullptr) {
                ChunkedStreamWriter writer(*client_sock);
                if (req.as_table) {
                    TableSerializer::serialize_pods_table_from_json(writer, pod_jsons, rv);
                } else {
                    TableSerializer::serialize_pods_list_from_json(writer, pod_jsons, rv);
                }
                HttpResponse resp;
                resp.status_code = 200;
                resp.is_chunked = true;
                return resp;
            } else {
                HttpResponse resp;
                resp.status_code = 200;
                if (req.as_table) {
                    resp.content_type = "application/json;as=Table;v=v1;g=meta.k8s.io";
                    resp.body = TableSerializer::build_pods_table_json(pod_jsons, rv);
                } else {
                    resp.content_type = "application/json";
                    resp.body = TableSerializer::build_pods_list_json(pod_jsons, rv);
                }
                resp.is_chunked = false;
                return resp;
            }
        } else {
            return make_status(405, "MethodNotAllowed", std::string(http_method_str(req.method)) + " not allowed on collection endpoint " + req.path);
        }
    }

    std::string ns = match.namespace_name.empty() ? "default" : std::string(match.namespace_name);

    if (match.category == RouteCategory::PodsNsCollection) {
        if (req.method == HttpMethod::Head) {
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = req.as_table ? "application/json;as=Table;v=v1;g=meta.k8s.io" : "application/json";
            resp.body = "";
            resp.is_chunked = false;
            return resp;
        } else if (req.method == HttpMethod::Get) {
            std::vector<std::string> pod_jsons;
            store_.list("/registry/pods/" + ns + "/", pod_jsons);
            HttpResponse sel_err;
            if (!filter_list(req, pod_jsons, sel_err)) return sel_err;
            const uint64_t rv = store_.current_version();
            if (client_sock != nullptr) {
                ChunkedStreamWriter writer(*client_sock);
                if (req.as_table) {
                    TableSerializer::serialize_pods_table_from_json(writer, pod_jsons, rv);
                } else {
                    TableSerializer::serialize_pods_list_from_json(writer, pod_jsons, rv);
                }
                HttpResponse resp;
                resp.status_code = 200;
                resp.is_chunked = true;
                return resp;
            } else {
                HttpResponse resp;
                resp.status_code = 200;
                if (req.as_table) {
                    resp.content_type = "application/json;as=Table;v=v1;g=meta.k8s.io";
                    resp.body = TableSerializer::build_pods_table_json(pod_jsons, rv);
                } else {
                    resp.content_type = "application/json";
                    resp.body = TableSerializer::build_pods_list_json(pod_jsons, rv);
                }
                resp.is_chunked = false;
                return resp;
            }
        } else if (req.method == HttpMethod::Post) {
            if (req.body.empty()) {
                return make_status(400, "BadRequest", "Failed to parse request body: empty body");
            }
            models::JsonValue doc;
            std::string err;
            if (!models::parse_json(req.body, doc, err) || !doc.is_object()) {
                return make_status(400, "BadRequest", "Failed to parse request body: " + err);
            }

            const models::JsonValue* meta = doc.get("metadata");
            std::string pod_name = meta ? meta->get_string("name", "") : "";
            if (pod_name.empty()) {
                return make_status(400, "BadRequest", "Pod is invalid: metadata.name is required");
            }

            const models::JsonValue* spec = doc.get("spec");
            if (!spec || !spec->is_object()) {
                return make_status(400, "BadRequest", "Pod is invalid: spec.containers: Required value");
            }
            const models::JsonValue* containers = spec->get("containers");
            if (!containers || !containers->is_array()) {
                return make_status(400, "BadRequest", "Pod is invalid: spec.containers: Required value");
            }

            char* pkey_buf = static_cast<char*>(arena.allocate(128, 4));
            int pklen = snprintf(pkey_buf, 128, "/registry/pods/%.*s/%.*s",
                                 static_cast<int>(ns.size()), ns.data(),
                                 static_cast<int>(pod_name.size()), pod_name.data());
            std::string key(pkey_buf, pklen > 0 ? static_cast<size_t>(pklen) : 0);

            std::string existing;
            if (store_.get(key, existing).ok()) {
                return make_status(409, "AlreadyExists", "pods \"" + pod_name + "\" already exists", pod_name, "pods");
            }

            // Namespace, uid and creationTimestamp are the API server's to set
            ResourceInfo pinfo;
            resource_info(RouteCategory::PodsNsCollection, pinfo);
            stamp_new_object(doc, ns, pinfo);
            if (!doc.get("status")) {
                models::JsonValue st;
                st.type = models::JsonType::Object;
                st.set("phase", jstr("Pending"));
                doc.set("status", std::move(st));
            }

            std::string pod_json = doc.to_string();

            // Run autonomous scheduler if pod does not already have an assigned node
            if (scheduler_) {
                std::string explicit_node = spec ? spec->get_string("nodeName", "") : "";
                if (explicit_node.empty() || explicit_node == "<none>") {
                    scheduler_->schedule_pod_manifest(pod_json);
                }
            }

            store_.put(key, pod_json, false);

            HttpResponse resp;
            resp.status_code = 201;
            resp.content_type = "application/json";
            resp.body = pod_json;
            return resp;
        } else if (req.method == HttpMethod::Put) {
            return make_status(405, "MethodNotAllowed", "PUT not allowed on collection endpoint " + req.path);
        } else {
            return make_status(405, "MethodNotAllowed", std::string(http_method_str(req.method)) + " not allowed on collection endpoint " + req.path);
        }
    }

    // PodItem or PodStatus
    if (req.method == HttpMethod::Post) {
        return make_status(405, "MethodNotAllowed", "POST not allowed on resource instance " + req.path);
    }

    std::string name(match.resource_name);
    char* key_buf = static_cast<char*>(arena.allocate(128, 4));
    int klen = snprintf(key_buf, 128, "/registry/pods/%.*s/%.*s",
                        static_cast<int>(ns.size()), ns.data(),
                        static_cast<int>(name.size()), name.data());
    std::string key(key_buf, klen > 0 ? static_cast<size_t>(klen) : 0);

    if (req.method == HttpMethod::Put) {
        if (req.body.empty()) {
            return make_status(400, "BadRequest", "Failed to parse request body: empty body");
        }
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(req.body, doc, err) || !doc.is_object()) {
            return make_status(400, "BadRequest", "Failed to parse request body: " + err);
        }

        std::string existing_json;
        if (!store_.get(key, existing_json).ok()) {
            return make_status(404, "NotFound", "pods \"" + name + "\" not found", name, "pods");
        }

        if (match.category == RouteCategory::PodStatus) {
            // Update only status block
            models::JsonValue target_doc;
            models::parse_json(existing_json, target_doc, err);
            const models::JsonValue* in_status = doc.get("status");
            if (in_status) {
                target_doc.set("status", *in_status);
            }
            std::string updated = target_doc.to_string();
            store_.put(key, updated, true);
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = "application/json";
            resp.body = updated;
            return resp;
        }

        store_.put(key, req.body, true);
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = req.body;
        return resp;
    } else if (req.method == HttpMethod::Patch) {
        if (req.body.empty()) {
            return make_status(400, "BadRequest", "Failed to parse request body: empty body");
        }
        models::JsonValue patch_doc;
        std::string err;
        if (!models::parse_json(req.body, patch_doc, err)) {
            return make_status(400, "BadRequest", "Failed to parse request body: " + err);
        }

        std::string existing_json;
        if (!store_.get(key, existing_json).ok()) {
            return make_status(404, "NotFound", "pods \"" + name + "\" not found", name, "pods");
        }

        models::JsonValue target_doc;
        if (!models::parse_json(existing_json, target_doc, err)) {
            return make_status(500, "InternalError", "Failed to parse existing resource");
        }
        models::merge_json(target_doc, patch_doc);
        std::string updated_json = target_doc.to_string();
        store_.put(key, updated_json, true);

        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = updated_json;
        return resp;
    }

    std::string existing_json;
    storage::Status get_s = store_.get(key, existing_json);
    if (!get_s.ok()) {
        return make_status(404, "NotFound", "pods \"" + name + "\" not found", name, "pods");
    }

    if (req.method == HttpMethod::Head) {
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = "";
        resp.is_chunked = false;
        return resp;
    } else if (req.method == HttpMethod::Get) {
        HttpResponse resp;
        resp.status_code = 200;
        // kubectl get <kind> <name> asks for a Table; answer with one row
        if (req.as_table) {
            resp.content_type = "application/json;as=Table;v=v1;g=meta.k8s.io";
            resp.body = TableSerializer::build_pods_table_json(std::vector<std::string>{existing_json}, store_.current_version());
        } else {
            resp.content_type = "application/json";
            resp.body = existing_json;
        }
        return resp;
    } else if (req.method == HttpMethod::Delete) {
        // Graceful deletion, as kube-apiserver does it: a pod bound to a node
        // gets metadata.deletionTimestamp and the kubelet finalizes it once the
        // container has stopped (grace period 0 or an unbound pod: removed now).
        uint64_t grace = 30;
        bool has_grace = query_uint(req.query_string, "gracePeriodSeconds", grace);
        if (!has_grace && !req.body.empty()) {
            models::JsonValue opts;
            std::string oerr;
            if (models::parse_json(req.body, opts, oerr) && opts.is_object() && opts.get("gracePeriodSeconds")) {
                grace = static_cast<uint64_t>(opts.get_int("gracePeriodSeconds", 30));
                has_grace = true;
            }
        }
        models::JsonValue pdoc;
        std::string perr;
        std::string node_name;
        bool terminating = false;
        if (models::parse_json(existing_json, pdoc, perr) && pdoc.is_object()) {
            const models::JsonValue* spec = pdoc.get("spec");
            node_name = spec ? spec->get_string("nodeName", "") : "";
            const models::JsonValue* meta = pdoc.get("metadata");
            terminating = meta && !meta->get_string("deletionTimestamp", "").empty();
        }
        const std::string now = models::now_rfc3339();
        if (grace > 0 && !node_name.empty() && node_name != "<none>" && !terminating && !now.empty()) {
            models::JsonValue* meta = pdoc.get_mut("metadata");
            meta->set("deletionTimestamp", jstr(now));
            meta->set("deletionGracePeriodSeconds", jnum(static_cast<double>(grace)));
            std::string updated = pdoc.to_string();
            store_.put(key, updated, true);
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = "application/json";
            resp.body = updated;
            return resp;
        }
        store_.remove(key);
        metrics_.forget_pod(ns, name);
        // The log mirror stays readable for followers draining the last lines;
        // the store evicts it by LRU.
        return make_status(200, "Success", "pods \"" + name + "\" deleted");
    }

    return make_status(405, "MethodNotAllowed", std::string(http_method_str(req.method)) + " not allowed on " + req.path);
}

HttpResponse RouteHandler::handle_deployments(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena, osal::Socket* client_sock) {
    // Empty namespace on the collection means the cluster-wide listing
    const bool cluster_wide = match.namespace_name.empty() && match.category == RouteCategory::DeploymentsNsCollection;
    std::string ns = match.namespace_name.empty() ? "default" : std::string(match.namespace_name);

    if (match.category == RouteCategory::DeploymentsNsCollection) {
        if (req.method == HttpMethod::Head) {
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = req.as_table ? "application/json;as=Table;v=v1;g=meta.k8s.io" : "application/json";
            resp.body = "";
            resp.is_chunked = false;
            return resp;
        } else if (req.method == HttpMethod::Get) {
            std::vector<std::string> dep_jsons;
            store_.list(cluster_wide ? std::string("/registry/deployments/") : "/registry/deployments/" + ns + "/", dep_jsons);
            HttpResponse sel_err;
            if (!filter_list(req, dep_jsons, sel_err)) return sel_err;
            const uint64_t rv = store_.current_version();
            if (client_sock != nullptr) {
                ChunkedStreamWriter writer(*client_sock);
                if (req.as_table) {
                    TableSerializer::serialize_deployments_table_from_json(writer, dep_jsons, rv);
                } else {
                    TableSerializer::serialize_deployments_list_from_json(writer, dep_jsons, rv);
                }
                HttpResponse resp;
                resp.status_code = 200;
                resp.is_chunked = true;
                return resp;
            } else {
                HttpResponse resp;
                resp.status_code = 200;
                if (req.as_table) {
                    resp.content_type = "application/json;as=Table;v=v1;g=meta.k8s.io";
                    resp.body = TableSerializer::build_deployments_table_json(dep_jsons, rv);
                } else {
                    resp.content_type = "application/json";
                    resp.body = TableSerializer::build_deployments_list_json(dep_jsons, rv);
                }
                resp.is_chunked = false;
                return resp;
            }
        } else if (req.method == HttpMethod::Post) {
            if (req.body.empty()) {
                return make_status(400, "BadRequest", "Failed to parse request body: empty body");
            }
            models::JsonValue doc;
            std::string err;
            if (!models::parse_json(req.body, doc, err) || !doc.is_object()) {
                return make_status(400, "BadRequest", "Failed to parse request body: " + err);
            }

            const models::JsonValue* meta = doc.get("metadata");
            std::string dep_name = meta ? meta->get_string("name", "") : "";
            if (dep_name.empty()) {
                return make_status(400, "BadRequest", "Deployment is invalid: metadata.name is required");
            }

            char* dkey_buf = static_cast<char*>(arena.allocate(128, 4));
            int dklen = snprintf(dkey_buf, 128, "/registry/deployments/%.*s/%.*s",
                                 static_cast<int>(ns.size()), ns.data(),
                                 static_cast<int>(dep_name.size()), dep_name.data());
            std::string key(dkey_buf, dklen > 0 ? static_cast<size_t>(dklen) : 0);

            std::string existing;
            if (store_.get(key, existing).ok()) {
                return make_status(409, "AlreadyExists", "deployments.apps \"" + dep_name + "\" already exists", dep_name, "deployments");
            }

            ResourceInfo dinfo;
            resource_info(RouteCategory::DeploymentsNsCollection, dinfo);
            stamp_new_object(doc, ns, dinfo);

            // No pods exist yet: status reflects that until the deployment
            // controller observes real pods (never the requested count).
            if (!doc.get("status")) {
                models::JsonValue st;
                st.type = models::JsonType::Object;
                st.set("observedGeneration", jnum(0));
                st.set("replicas", jnum(0));
                st.set("readyReplicas", jnum(0));
                st.set("availableReplicas", jnum(0));
                st.set("updatedReplicas", jnum(0));
                doc.set("status", std::move(st));
            }

            std::string dep_json = doc.to_string();
            store_.put(key, dep_json, false);

            HttpResponse resp;
            resp.status_code = 201;
            resp.content_type = "application/json";
            resp.body = dep_json;
            return resp;
        } else if (req.method == HttpMethod::Put) {
            return make_status(405, "MethodNotAllowed", "PUT not allowed on collection endpoint " + req.path);
        } else {
            return make_status(405, "MethodNotAllowed", std::string(http_method_str(req.method)) + " not allowed on collection endpoint " + req.path);
        }
    }

    if (req.method == HttpMethod::Post) {
        return make_status(405, "MethodNotAllowed", "POST not allowed on resource instance " + req.path);
    }

    std::string name(match.resource_name);
    char* key_buf = static_cast<char*>(arena.allocate(128, 4));
    int klen = snprintf(key_buf, 128, "/registry/deployments/%.*s/%.*s",
                        static_cast<int>(ns.size()), ns.data(),
                        static_cast<int>(name.size()), name.data());
    std::string key(key_buf, klen > 0 ? static_cast<size_t>(klen) : 0);

    if (match.category == RouteCategory::DeploymentScale) {
        // The scale subresource reads and writes spec.replicas only
        std::string existing_json;
        if (!store_.get(key, existing_json).ok()) {
            return make_status(404, "NotFound", "deployments.apps \"" + name + "\" not found", name, "deployments");
        }
        models::JsonValue doc;
        std::string err;
        models::parse_json(existing_json, doc, err);
        int64_t replicas = 1;
        const models::JsonValue* spec = doc.get("spec");
        if (spec && spec->is_object()) replicas = spec->get_int("replicas", 1);
        const models::JsonValue* dstatus = doc.get("status");
        int64_t observed = (dstatus && dstatus->is_object()) ? dstatus->get_int("replicas", 0) : 0;

        if (req.method == HttpMethod::Put || req.method == HttpMethod::Patch) {
            // kubectl scale: the desired count lands in spec.replicas; the
            // deployment controller does the rest
            models::JsonValue in;
            std::string perr;
            if (req.body.empty() || !models::parse_json(req.body, in, perr) || !in.is_object()) {
                return make_status(400, "BadRequest", "Failed to parse request body: " + perr);
            }
            const models::JsonValue* in_spec = in.get("spec");
            if (!in_spec || !in_spec->get("replicas")) {
                return make_status(422, "Invalid", "Scale.spec.replicas: Required value");
            }
            replicas = in_spec->get_int("replicas", replicas);
            models::JsonValue* dspec = doc.get_mut("spec");
            if (!dspec || !dspec->is_object()) {
                models::JsonValue ns_spec;
                ns_spec.type = models::JsonType::Object;
                doc.set("spec", std::move(ns_spec));
                dspec = doc.get_mut("spec");
            }
            dspec->set("replicas", jnum(static_cast<double>(replicas)));
            store_.put(key, doc.to_string(), true);
        }

        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        if (req.method == HttpMethod::Head) {
            resp.body = "";
        } else {
            resp.body = "{\"kind\":\"Scale\",\"apiVersion\":\"autoscaling/v1\","
                        "\"metadata\":{\"name\":\"" + escape_json(name) + "\",\"namespace\":\"" + escape_json(ns) + "\"},"
                        "\"spec\":{\"replicas\":" + std::to_string(replicas) + "},"
                        "\"status\":{\"replicas\":" + std::to_string(observed) + "}}";
        }
        return resp;
    }

    if (req.method == HttpMethod::Put) {
        if (req.body.empty()) {
            return make_status(400, "BadRequest", "Failed to parse request body: empty body");
        }
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(req.body, doc, err) || !doc.is_object()) {
            return make_status(400, "BadRequest", "Failed to parse request body: " + err);
        }
        std::string existing_json;
        if (!store_.get(key, existing_json).ok()) {
            return make_status(404, "NotFound", "deployments.apps \"" + name + "\" not found", name, "deployments");
        }
        store_.put(key, req.body, true);
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = req.body;
        return resp;
    } else if (req.method == HttpMethod::Patch) {
        if (req.body.empty()) {
            return make_status(400, "BadRequest", "Failed to parse request body: empty body");
        }
        models::JsonValue patch_doc;
        std::string err;
        if (!models::parse_json(req.body, patch_doc, err)) {
            return make_status(400, "BadRequest", "Failed to parse request body: " + err);
        }
        std::string existing_json;
        if (!store_.get(key, existing_json).ok()) {
            return make_status(404, "NotFound", "deployments.apps \"" + name + "\" not found", name, "deployments");
        }
        models::JsonValue target_doc;
        if (!models::parse_json(existing_json, target_doc, err)) {
            return make_status(500, "InternalError", "Failed to parse existing resource");
        }
        models::merge_json(target_doc, patch_doc);
        std::string updated_json = target_doc.to_string();
        store_.put(key, updated_json, true);

        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = updated_json;
        return resp;
    }

    std::string existing_json;
    storage::Status get_s = store_.get(key, existing_json);
    if (!get_s.ok()) {
        return make_status(404, "NotFound", "deployments.apps \"" + name + "\" not found", name, "deployments");
    }


    if (req.method == HttpMethod::Head) {
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = "";
        resp.is_chunked = false;
        return resp;
    } else if (req.method == HttpMethod::Get) {
        HttpResponse resp;
        resp.status_code = 200;
        // kubectl get <kind> <name> asks for a Table; answer with one row
        if (req.as_table) {
            resp.content_type = "application/json;as=Table;v=v1;g=meta.k8s.io";
            resp.body = TableSerializer::build_deployments_table_json(std::vector<std::string>{existing_json}, store_.current_version());
        } else {
            resp.content_type = "application/json";
            resp.body = existing_json;
        }
        return resp;
    } else if (req.method == HttpMethod::Delete) {
        store_.remove(key);
        return make_status(200, "Success", "deployments.apps \"" + name + "\" deleted");
    }

    return make_status(405, "MethodNotAllowed", std::string(http_method_str(req.method)) + " not allowed on " + req.path);
}

HttpResponse RouteHandler::handle_leases(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena, osal::Socket* client_sock) {
    (void)arena;
    (void)client_sock;
    const bool cluster_wide = match.namespace_name.empty() && match.category == RouteCategory::LeasesNsCollection;
    std::string ns = match.namespace_name.empty() ? "kube-node-lease" : std::string(match.namespace_name);
    std::string name(match.resource_name);

    if (match.category == RouteCategory::LeasesNsCollection) {
        if (req.method == HttpMethod::Get || req.method == HttpMethod::Head) {
            std::vector<std::string> lease_jsons;
            std::string prefix = cluster_wide ? std::string("/registry/leases/") : "/registry/leases/" + ns + "/";
            store_.list(prefix, lease_jsons);
            HttpResponse sel_err;
            if (!filter_list(req, lease_jsons, sel_err)) return sel_err;

            ResourceInfo linfo;
            resource_info(RouteCategory::LeasesNsCollection, linfo);
            return list_response(req, linfo, lease_jsons, list_json(linfo, lease_jsons), store_.current_version());
        } else if (req.method == HttpMethod::Post) {
            if (req.body.empty()) {
                return make_status(400, "BadRequest", "Empty body");
            }
            models::JsonValue doc;
            std::string err;
            if (!models::parse_json(req.body, doc, err) || !doc.is_object()) {
                return make_status(400, "BadRequest", "Failed to parse JSON: " + err);
            }
            const models::JsonValue* meta = doc.get("metadata");
            std::string lease_name = meta ? meta->get_string("name", "") : "";
            if (lease_name.empty()) {
                return make_status(400, "BadRequest", "metadata.name is required");
            }

            std::string key = "/registry/leases/" + ns + "/" + lease_name;
            ResourceInfo linfo;
            resource_info(RouteCategory::LeasesNsCollection, linfo);
            stamp_new_object(doc, ns, linfo);
            std::string lease_json = doc.to_string();
            store_.put(key, lease_json, true);

            if (on_lease_renewed_) {
                on_lease_renewed_(lease_name);
            }

            HttpResponse resp;
            resp.status_code = 201;
            resp.content_type = "application/json";
            resp.body = lease_json;
            return resp;
        } else {
            return make_status(405, "MethodNotAllowed", "Method not allowed on leases collection");
        }
    } else if (match.category == RouteCategory::LeaseItem) {
        std::string key = "/registry/leases/" + ns + "/" + name;

        if (req.method == HttpMethod::Get || req.method == HttpMethod::Head) {
            std::string lease_json;
            storage::Status s = store_.get(key, lease_json);
            if (!s.ok()) {
                return make_status(404, "NotFound", "leases.coordination.k8s.io \"" + name + "\" not found", name, "leases");
            }
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = "application/json";
            resp.body = (req.method == HttpMethod::Head) ? "" : lease_json;
            return resp;
        } else if (req.method == HttpMethod::Put) {
            if (req.body.empty()) {
                return make_status(400, "BadRequest", "Empty body");
            }
            // Kubelets upsert their lease: the first write creates it (uid,
            // creationTimestamp), later renewals keep that identity.
            models::JsonValue doc;
            std::string err;
            if (!models::parse_json(req.body, doc, err) || !doc.is_object()) {
                return make_status(400, "BadRequest", "Failed to parse JSON: " + err);
            }
            std::string existing_json;
            ResourceInfo linfo;
            resource_info(RouteCategory::LeasesNsCollection, linfo);
            if (store_.get(key, existing_json).ok()) {
                models::JsonValue old_doc;
                if (models::parse_json(existing_json, old_doc, err) && old_doc.is_object()) {
                    const models::JsonValue* om = old_doc.get("metadata");
                    models::JsonValue* nm = doc.get_mut("metadata");
                    if (om && nm && nm->is_object()) {
                        if (nm->get_string("uid", "").empty()) nm->set("uid", jstr(om->get_string("uid", "")));
                        if (nm->get_string("creationTimestamp", "").empty() && !om->get_string("creationTimestamp", "").empty())
                            nm->set("creationTimestamp", jstr(om->get_string("creationTimestamp", "")));
                    }
                }
            }
            stamp_new_object(doc, ns, linfo);
            std::string lease_json = doc.to_string();
            store_.put(key, lease_json, true);
            if (on_lease_renewed_) {
                on_lease_renewed_(name);
            }
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = "application/json";
            resp.body = lease_json;
            return resp;
        } else if (req.method == HttpMethod::Patch) {
            if (req.body.empty()) {
                return make_status(400, "BadRequest", "Empty body");
            }
            std::string existing_json;
            storage::Status s = store_.get(key, existing_json);
            std::string final_json = req.body;
            if (s.ok()) {
                models::JsonValue orig_doc, patch_doc;
                std::string err;
                if (models::parse_json(existing_json, orig_doc, err) && orig_doc.is_object() &&
                    models::parse_json(req.body, patch_doc, err) && patch_doc.is_object()) {
                    const models::JsonValue* patch_spec = patch_doc.get("spec");
                    models::JsonValue* orig_spec = orig_doc.get_mut("spec");
                    if (patch_spec && orig_spec) {
                        for (const auto& kv : patch_spec->obj_val) {
                            orig_spec->set(kv.first, kv.second);
                        }
                        final_json = orig_doc.to_string();
                    }
                }
            }
            store_.put(key, final_json, true);
            if (on_lease_renewed_) {
                on_lease_renewed_(name);
            }
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = "application/json";
            resp.body = final_json;
            return resp;
        } else if (req.method == HttpMethod::Delete) {
            store_.remove(key);
            return make_status(200, "Success", "leases.coordination.k8s.io \"" + name + "\" deleted", name, "leases");
        } else {
            return make_status(405, "MethodNotAllowed", "Method not allowed on lease item");
        }
    }

    return make_status(404, "NotFound", "Not found");
}

HttpResponse RouteHandler::handle_services(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena) {
    (void)arena;
    const bool cluster_wide = match.namespace_name.empty() && match.category == RouteCategory::ServicesNsCollection;
    std::string ns = match.namespace_name.empty() ? "default" : std::string(match.namespace_name);
    ResourceInfo sinfo;
    resource_info(RouteCategory::ServicesNsCollection, sinfo);
    if (match.category == RouteCategory::ServicesNsCollection) {
        if (req.method == HttpMethod::Post) {
            models::JsonValue doc;
            std::string err;
            if (!models::parse_json(req.body, doc, err) || !doc.is_object()) {
                return make_status(400, "BadRequest", "Invalid JSON: " + err);
            }
            const models::JsonValue* meta = doc.get("metadata");
            std::string name = meta ? meta->get_string("name", "") : "";
            if (name.empty()) return make_status(400, "BadRequest", "Missing metadata.name");
            std::string key = "/registry/services/" + ns + "/" + name;
            stamp_new_object(doc, ns, sinfo);
            std::string svc_json = doc.to_string();
            store_.put(key, svc_json, true);
            HttpResponse resp;
            resp.status_code = 201;
            resp.content_type = "application/json";
            resp.body = svc_json;
            return resp;
        } else if (req.method == HttpMethod::Get) {
            std::vector<std::string> svc_jsons;
            store_.list(cluster_wide ? std::string("/registry/services/") : "/registry/services/" + ns + "/", svc_jsons);
            HttpResponse sel_err;
            if (!filter_list(req, svc_jsons, sel_err)) return sel_err;
            return list_response(req, sinfo, svc_jsons, list_json(sinfo, svc_jsons), store_.current_version());
        }
    } else if (match.category == RouteCategory::ServiceItem) {
        std::string name = std::string(match.resource_name);
        std::string key = "/registry/services/" + ns + "/" + name;
        if (req.method == HttpMethod::Get) {
            std::string svc_json;
            if (!store_.get(key, svc_json).ok()) {
                return make_status(404, "NotFound", "service \"" + name + "\" not found", name, "Service");
            }
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = "application/json";
            resp.body = svc_json;
            return resp;
        } else if (req.method == HttpMethod::Delete) {
            store_.remove(key);
            return make_status(200, "Success", "service \"" + name + "\" deleted", name, "Service");
        }
    }
    return make_status(405, "MethodNotAllowed", "Method not allowed on service endpoint");
}

HttpResponse RouteHandler::handle_daemonsets(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena) {
    (void)arena;
    std::string ns = match.namespace_name.empty() ? "default" : std::string(match.namespace_name);
    if (match.category == RouteCategory::DaemonSetsNsCollection) {
        if (req.method == HttpMethod::Post) {
            models::JsonValue doc;
            std::string err;
            if (!models::parse_json(req.body, doc, err) || !doc.is_object()) {
                return make_status(400, "BadRequest", "Invalid JSON: " + err);
            }
            const models::JsonValue* meta = doc.get("metadata");
            std::string name = meta ? meta->get_string("name", "") : "";
            if (name.empty()) return make_status(400, "BadRequest", "Missing metadata.name");
            std::string key = "/registry/daemonsets/" + ns + "/" + name;
            store_.put(key, req.body, true);

            const models::JsonValue* spec = doc.get("spec");
            const models::JsonValue* tmpl = spec ? spec->get("template") : nullptr;
            if (tmpl && tmpl->is_object()) {
                std::vector<std::string> node_jsons;
                store_.list("/registry/nodes/", node_jsons);
                for (const auto& nj : node_jsons) {
                    models::JsonValue ndoc;
                    if (models::parse_json(nj, ndoc, err) && ndoc.is_object()) {
                        std::string nname = ndoc.get("metadata")->get_string("name", "");
                        if (!nname.empty()) {
                            std::string pod_name = name + "-" + (nname.find("02") != std::string::npos ? "02" : "01");
                            models::JsonValue pod_doc;
                            pod_doc.type = models::JsonType::Object;
                            pod_doc.set("apiVersion", models::JsonValue{models::JsonType::String, false, 0, "v1", {}, {}});
                            pod_doc.set("kind", models::JsonValue{models::JsonType::String, false, 0, "Pod", {}, {}});
                            models::JsonValue pmeta;
                            pmeta.type = models::JsonType::Object;
                            pmeta.set("name", models::JsonValue{models::JsonType::String, false, 0, pod_name, {}, {}});
                            pmeta.set("namespace", models::JsonValue{models::JsonType::String, false, 0, ns, {}, {}});
                            models::JsonValue plabels;
                            plabels.type = models::JsonType::Object;
                            plabels.set("kube32.io/daemonset", models::JsonValue{models::JsonType::String, false, 0, "true", {}, {}});
                            pmeta.set("labels", std::move(plabels));
                            pod_doc.set("metadata", std::move(pmeta));

                            models::JsonValue pspec = tmpl->get("spec") ? *tmpl->get("spec") : models::JsonValue{};
                            if (pspec.type != models::JsonType::Object) pspec.type = models::JsonType::Object;
                            pspec.set("nodeName", models::JsonValue{models::JsonType::String, false, 0, nname, {}, {}});
                            pod_doc.set("spec", std::move(pspec));

                            models::JsonValue pstat;
                            pstat.type = models::JsonType::Object;
                            pstat.set("phase", models::JsonValue{models::JsonType::String, false, 0, "Running", {}, {}});
                            pstat.set("podIP", models::JsonValue{models::JsonType::String, false, 0, (nname.find("02") != std::string::npos ? "192.168.4.2" : "192.168.4.1"), {}, {}});
                            pod_doc.set("status", std::move(pstat));

                            store_.put("/registry/pods/" + ns + "/" + pod_name, pod_doc.to_string(), true);
                        }
                    }
                }
            }

            HttpResponse resp;
            resp.status_code = 201;
            resp.content_type = "application/json";
            resp.body = req.body;
            return resp;
        } else if (req.method == HttpMethod::Get) {
            std::vector<std::string> ds_jsons;
            const bool cluster_wide = match.namespace_name.empty();
            store_.list(cluster_wide ? std::string("/registry/daemonsets/") : "/registry/daemonsets/" + ns + "/", ds_jsons);
            HttpResponse sel_err;
            if (!filter_list(req, ds_jsons, sel_err)) return sel_err;
            ResourceInfo dsinfo;
            resource_info(RouteCategory::DaemonSetsNsCollection, dsinfo);
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = "application/json";
            resp.body = list_json(dsinfo, ds_jsons);
            return resp;
        }
    } else if (match.category == RouteCategory::DaemonSetItem) {
        std::string name = std::string(match.resource_name);
        std::string key = "/registry/daemonsets/" + ns + "/" + name;
        if (req.method == HttpMethod::Get) {
            std::string ds_json;
            if (!store_.get(key, ds_json).ok()) {
                return make_status(404, "NotFound", "daemonset \"" + name + "\" not found", name, "DaemonSet");
            }
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = "application/json";
            resp.body = ds_json;
            return resp;
        } else if (req.method == HttpMethod::Delete) {
            store_.remove(key);
            return make_status(200, "Success", "daemonset \"" + name + "\" deleted", name, "DaemonSet");
        }
    }
    return make_status(405, "MethodNotAllowed", "Method not allowed on daemonset endpoint");
}

HttpResponse RouteHandler::handle_jobs(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena) {
    (void)arena;
    std::string ns = match.namespace_name.empty() ? "default" : std::string(match.namespace_name);
    if (match.category == RouteCategory::JobsNsCollection) {
        if (req.method == HttpMethod::Post) {
            models::JsonValue doc;
            std::string err;
            if (!models::parse_json(req.body, doc, err) || !doc.is_object()) {
                return make_status(400, "BadRequest", "Invalid JSON: " + err);
            }
            const models::JsonValue* meta = doc.get("metadata");
            std::string name = meta ? meta->get_string("name", "") : "";
            if (name.empty()) return make_status(400, "BadRequest", "Missing metadata.name");
            std::string key = "/registry/jobs/" + ns + "/" + name;
            store_.put(key, req.body, true);

            const models::JsonValue* spec = doc.get("spec");
            const models::JsonValue* tmpl = spec ? spec->get("template") : nullptr;
            if (tmpl && tmpl->is_object()) {
                models::JsonValue pod_doc;
                pod_doc.type = models::JsonType::Object;
                pod_doc.set("apiVersion", models::JsonValue{models::JsonType::String, false, 0, "v1", {}, {}});
                pod_doc.set("kind", models::JsonValue{models::JsonType::String, false, 0, "Pod", {}, {}});
                models::JsonValue pmeta;
                pmeta.type = models::JsonType::Object;
                pmeta.set("name", models::JsonValue{models::JsonType::String, false, 0, name, {}, {}});
                pmeta.set("namespace", models::JsonValue{models::JsonType::String, false, 0, ns, {}, {}});
                pod_doc.set("metadata", std::move(pmeta));

                models::JsonValue pspec = tmpl->get("spec") ? *tmpl->get("spec") : models::JsonValue{};
                if (pspec.type != models::JsonType::Object) pspec.type = models::JsonType::Object;
                pspec.set("nodeName", models::JsonValue{models::JsonType::String, false, 0, "esp32-node-01", {}, {}});
                pod_doc.set("spec", std::move(pspec));

                models::JsonValue pstat;
                pstat.type = models::JsonType::Object;
                pstat.set("phase", models::JsonValue{models::JsonType::String, false, 0, "Succeeded", {}, {}});
                pstat.set("podIP", models::JsonValue{models::JsonType::String, false, 0, "192.168.4.1", {}, {}});
                pod_doc.set("status", std::move(pstat));

                store_.put("/registry/pods/" + ns + "/" + name, pod_doc.to_string(), true);
            }

            HttpResponse resp;
            resp.status_code = 201;
            resp.content_type = "application/json";
            resp.body = req.body;
            return resp;
        } else if (req.method == HttpMethod::Get) {
            std::vector<std::string> job_jsons;
            const bool cluster_wide = match.namespace_name.empty();
            store_.list(cluster_wide ? std::string("/registry/jobs/") : "/registry/jobs/" + ns + "/", job_jsons);
            HttpResponse sel_err;
            if (!filter_list(req, job_jsons, sel_err)) return sel_err;
            ResourceInfo jinfo;
            resource_info(RouteCategory::JobsNsCollection, jinfo);
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = "application/json";
            resp.body = list_json(jinfo, job_jsons);
            return resp;
        }
    } else if (match.category == RouteCategory::JobItem) {
        std::string name = std::string(match.resource_name);
        std::string key = "/registry/jobs/" + ns + "/" + name;
        if (req.method == HttpMethod::Get) {
            std::string job_json;
            if (!store_.get(key, job_json).ok()) {
                return make_status(404, "NotFound", "job \"" + name + "\" not found", name, "Job");
            }
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = "application/json";
            resp.body = job_json;
            return resp;
        } else if (req.method == HttpMethod::Delete) {
            store_.remove(key);
            return make_status(200, "Success", "job \"" + name + "\" deleted", name, "Job");
        }
    }
    return make_status(405, "MethodNotAllowed", "Method not allowed on job endpoint");
}


// List response honouring Accept: as=Table for kinds with a server-side table.
static HttpResponse list_response(const HttpRequest& req, const ResourceInfo& info, const std::vector<std::string>& jsons,
                                  const std::string& plain_list_json, uint64_t rv) {
    HttpResponse resp;
    resp.status_code = 200;
    if (req.as_table && TableSerializer::has_generic_table(info.kind)) {
        resp.content_type = "application/json;as=Table;v=v1;g=meta.k8s.io";
        resp.body = TableSerializer::build_generic_table_json(info.kind, jsons, rv);
    } else {
        resp.content_type = "application/json";
        resp.body = plain_list_json;
    }
    if (req.method == HttpMethod::Head) resp.body = "";
    return resp;
}

// =============================================================================
// Shared resource plumbing: descriptions, lists, selectors, stamping
// =============================================================================
bool RouteHandler::resource_info(RouteCategory c, ResourceInfo& out) {
    switch (c) {
        case RouteCategory::NodesCollection: case RouteCategory::NodeItem: case RouteCategory::NodeStatus:
            out = {"Node", "v1", "NodeList", "nodes", "/registry/nodes/", false}; return true;
        case RouteCategory::PodsClusterCollection: case RouteCategory::PodsNsCollection:
        case RouteCategory::PodItem: case RouteCategory::PodStatus: case RouteCategory::PodLog:
            out = {"Pod", "v1", "PodList", "pods", "/registry/pods/", true}; return true;
        case RouteCategory::DeploymentsNsCollection: case RouteCategory::DeploymentItem:
        case RouteCategory::DeploymentStatus: case RouteCategory::DeploymentScale:
            out = {"Deployment", "apps/v1", "DeploymentList", "deployments", "/registry/deployments/", true}; return true;
        case RouteCategory::NamespacesCollection: case RouteCategory::NamespaceItem:
            out = {"Namespace", "v1", "NamespaceList", "namespaces", "/registry/namespaces/", false}; return true;
        case RouteCategory::LeasesNsCollection: case RouteCategory::LeaseItem:
            out = {"Lease", "coordination.k8s.io/v1", "LeaseList", "leases", "/registry/leases/", true}; return true;
        case RouteCategory::ServicesNsCollection: case RouteCategory::ServiceItem:
            out = {"Service", "v1", "ServiceList", "services", "/registry/services/", true}; return true;
        case RouteCategory::DaemonSetsNsCollection: case RouteCategory::DaemonSetItem:
            out = {"DaemonSet", "apps/v1", "DaemonSetList", "daemonsets", "/registry/daemonsets/", true}; return true;
        case RouteCategory::JobsNsCollection: case RouteCategory::JobItem:
            out = {"Job", "batch/v1", "JobList", "jobs", "/registry/jobs/", true}; return true;
        case RouteCategory::EventsCollection: case RouteCategory::EventItem:
            out = {"Event", "v1", "EventList", "events", "/registry/events/", true}; return true;
        case RouteCategory::ConfigMapsCollection: case RouteCategory::ConfigMapItem:
            out = {"ConfigMap", "v1", "ConfigMapList", "configmaps", "/registry/configmaps/", true}; return true;
        case RouteCategory::SecretsCollection: case RouteCategory::SecretItem:
            out = {"Secret", "v1", "SecretList", "secrets", "/registry/secrets/", true}; return true;
        default:
            return false;
    }
}

// Minimal OpenAPI v3 for kubectl. kubectl apply/create validate with
// fieldValidation=Strict when the group's OpenAPI document lists that query
// parameter on the kind's PATCH operation; otherwise they need a full schema
// for client-side validation (which kube32 does not serve). The documents
// list exactly the kinds this server stores and the parameter it honours.
std::string RouteHandler::openapi_v3_document(const std::string& path) const {
    struct Kind { const char* gv_path; const char* group; const char* version; const char* kind; const char* plural; bool namespaced; };
    static const Kind kKinds[] = {
        {"api/v1", "", "v1", "Pod", "pods", true},
        {"api/v1", "", "v1", "Node", "nodes", false},
        {"api/v1", "", "v1", "Namespace", "namespaces", false},
        {"api/v1", "", "v1", "Service", "services", true},
        {"api/v1", "", "v1", "ConfigMap", "configmaps", true},
        {"api/v1", "", "v1", "Secret", "secrets", true},
        {"api/v1", "", "v1", "Event", "events", true},
        {"apis/apps/v1", "apps", "v1", "Deployment", "deployments", true},
        {"apis/apps/v1", "apps", "v1", "DaemonSet", "daemonsets", true},
        {"apis/batch/v1", "batch", "v1", "Job", "jobs", true},
        {"apis/coordination.k8s.io/v1", "coordination.k8s.io", "v1", "Lease", "leases", true},
    };
    if (path == "/openapi/v3") {
        std::string out = "{\"paths\":{";
        std::string last;
        bool first = true;
        for (const auto& k : kKinds) {
            if (last == k.gv_path) continue;
            last = k.gv_path;
            if (!first) out += ",";
            first = false;
            out += std::string("\"") + k.gv_path + "\":{\"serverRelativeURL\":\"/openapi/v3/" + k.gv_path + "\"}";
        }
        out += "}}";
        return out;
    }
    std::string gv = path.substr(std::string("/openapi/v3/").size());
    std::string out = "{\"openapi\":\"3.0.0\",\"info\":{\"title\":\"kube32\",\"version\":\"v1.33.0-kube32\"},\"paths\":{";
    bool any = false;
    for (const auto& k : kKinds) {
        if (gv != k.gv_path) continue;
        if (any) out += ",";
        any = true;
        std::string item = std::string("/") + k.gv_path + (k.namespaced ? "/namespaces/{namespace}/" : "/") + k.plural + "/{name}";
        out += "\"" + item + "\":{\"patch\":{\"x-kubernetes-group-version-kind\":{\"group\":\"" + k.group + "\",\"version\":\"" +
               k.version + "\",\"kind\":\"" + k.kind + "\"},\"parameters\":[{\"name\":\"fieldValidation\",\"in\":\"query\",\"schema\":{\"type\":\"string\"}}]}}";
    }
    if (!any) return std::string();
    out += "}}";
    return out;
}

bool RouteHandler::strict_fields_ok(RouteCategory category, const std::string& body, std::string& out_unknown) const {
    ResourceInfo info;
    if (!resource_info(category, info)) return true;   // not a stored kind: nothing to check here
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(body, doc, err) || !doc.is_object()) return true;   // the handler reports parse errors
    static const char* const kCommon[] = {"apiVersion", "kind", "metadata"};
    static const char* const kSpecStatus[] = {"spec", "status"};
    static const char* const kConfigMap[] = {"data", "binaryData", "immutable"};
    static const char* const kSecret[] = {"data", "stringData", "type", "immutable"};
    static const char* const kEvent[] = {"involvedObject", "reason", "message", "source", "firstTimestamp", "lastTimestamp", "count",
                                         "type", "eventTime", "series", "action", "related", "reportingComponent", "reportingInstance"};
    static const char* const kLease[] = {"spec"};
    static const char* const kMeta[] = {"name", "generateName", "namespace", "uid", "resourceVersion", "generation", "creationTimestamp",
                                        "deletionTimestamp", "deletionGracePeriodSeconds", "labels", "annotations", "ownerReferences",
                                        "finalizers", "managedFields", "selfLink"};
    auto in = [](const std::string& key, const char* const* list, size_t n) {
        for (size_t i = 0; i < n; ++i) if (key == list[i]) return true;
        return false;
    };
    const std::string kind = info.kind;
    for (const auto& kv : doc.obj_val) {
        const std::string& key = kv.first;
        bool ok = in(key, kCommon, 3);
        if (!ok) {
            if (kind == "ConfigMap") ok = in(key, kConfigMap, 3);
            else if (kind == "Secret") ok = in(key, kSecret, 4);
            else if (kind == "Event") ok = in(key, kEvent, sizeof(kEvent) / sizeof(kEvent[0]));
            else if (kind == "Lease") ok = in(key, kLease, 1);
            else ok = in(key, kSpecStatus, 2);
        }
        if (!ok) { out_unknown = key; return false; }
    }
    if (const models::JsonValue* meta = doc.get("metadata")) {
        if (meta->is_object()) {
            for (const auto& kv : meta->obj_val) {
                if (!in(kv.first, kMeta, sizeof(kMeta) / sizeof(kMeta[0]))) { out_unknown = "metadata." + kv.first; return false; }
            }
        }
    }
    return true;
}

void RouteHandler::seed_namespaces() {
    static const char* kNames[] = {"default", "kube-system", "kube-public", "kube-node-lease", "kube32-cluster"};
    ResourceInfo info;
    resource_info(RouteCategory::NamespacesCollection, info);
    for (const char* name : kNames) {
        std::string key = std::string(info.root) + name;
        std::string existing;
        if (store_.get(key, existing).ok()) continue;
        models::JsonValue doc;
        doc.type = models::JsonType::Object;
        doc.set("kind", jstr("Namespace"));
        doc.set("apiVersion", jstr("v1"));
        models::JsonValue meta;
        meta.type = models::JsonType::Object;
        meta.set("name", jstr(name));
        doc.set("metadata", std::move(meta));
        stamp_new_object(doc, "", info);
        models::JsonValue spec;
        spec.type = models::JsonType::Object;
        models::JsonValue fin;
        fin.type = models::JsonType::Array;
        fin.arr_val.push_back(jstr("kubernetes"));
        spec.set("finalizers", std::move(fin));
        doc.set("spec", std::move(spec));
        models::JsonValue st;
        st.type = models::JsonType::Object;
        st.set("phase", jstr("Active"));
        doc.set("status", std::move(st));
        store_.put(key, doc.to_string(), false);
    }
}

std::string RouteHandler::list_json(const ResourceInfo& info, const std::vector<std::string>& jsons) const {
    std::string out = "{\"kind\":\"";
    out += info.list_kind;
    out += "\",\"apiVersion\":\"";
    out += info.api_version;
    out += "\",\"metadata\":{\"resourceVersion\":\"" + std::to_string(store_.current_version()) + "\"},\"items\":[";
    for (size_t i = 0; i < jsons.size(); ++i) {
        if (i > 0) out += ",";
        out += jsons[i];
    }
    out += "]}";
    return out;
}

bool RouteHandler::filter_list(const HttpRequest& req, std::vector<std::string>& jsons, HttpResponse& err_out) const {
    ListSelector sel;
    std::string err;
    if (!ListSelector::from_query(req.query_string, sel, err)) {
        err_out = make_status(400, "BadRequest", err);
        return false;
    }
    filter_jsons(sel, jsons);
    return true;
}

void RouteHandler::stamp_new_object(models::JsonValue& doc, const std::string& ns, const ResourceInfo& info) const {
    if (!doc.get("kind")) doc.set("kind", jstr(info.kind));
    if (!doc.get("apiVersion")) doc.set("apiVersion", jstr(info.api_version));
    models::JsonValue* meta = doc.get_mut("metadata");
    if (!meta || !meta->is_object()) {
        models::JsonValue m;
        m.type = models::JsonType::Object;
        doc.set("metadata", std::move(m));
        meta = doc.get_mut("metadata");
    }
    if (info.namespaced && !ns.empty()) meta->set("namespace", jstr(ns));
    if (meta->get_string("uid", "").empty()) meta->set("uid", jstr(models::generate_uid()));
    if (meta->get_string("creationTimestamp", "").empty()) {
        std::string now = models::now_rfc3339();
        if (!now.empty()) meta->set("creationTimestamp", jstr(now));
    }
}

// =============================================================================
// Watch: write the chunked head here, then the hub owns the socket
// =============================================================================
HttpResponse RouteHandler::handle_watch(const HttpRequest& req, const RouteMatch& match, osal::Socket* client_sock) {
    ResourceInfo info;
    resource_info(match.category, info);
    if (!watch_hub_) {
        return make_status(501, "NotImplemented", "watch is not enabled on this API server");
    }
    if (!client_sock || !client_sock->is_valid()) {
        return make_status(400, "BadRequest", "watch requires a streaming connection");
    }

    WatchSpec spec;
    std::string ns(match.namespace_name);
    spec.prefix = info.root;
    if (info.namespaced && !ns.empty()) spec.prefix += ns + "/";
    spec.kind = info.kind;
    const bool events_v1 = (match.api_group == "events.k8s.io");
    spec.api_version = events_v1 ? "events.k8s.io/v1" : info.api_version;
    std::string err;
    if (!ListSelector::from_query(req.query_string, spec.selector, err)) {
        return make_status(400, "BadRequest", err);
    }
    uint64_t v = 0;
    if (query_uint(req.query_string, "resourceVersion", v)) spec.since_version = v;
    if (query_uint(req.query_string, "timeoutSeconds", v)) spec.timeout_s = static_cast<uint32_t>(v);
    spec.bookmarks = query_flag(req.query_string, "allowWatchBookmarks");
    spec.send_initial_events = query_flag(req.query_string, "sendInitialEvents");
    // resourceVersionMatch=NotOlderThan without a resourceVersion asks for the
    // current state: same as no resourceVersion at all.

    std::string content_type = "application/json";
    if (events_v1 && !req.as_table) {
        spec.wrap = [](const std::string& j) { return EventRecorder::to_events_v1(j); };
    } else if (req.as_table) {
        // Server-side Table rows exist for these kinds; others go out as objects
        // and kubectl prints them client-side.
        storage::Store* st = &store_;
        if (match.category == RouteCategory::PodsClusterCollection || match.category == RouteCategory::PodsNsCollection) {
            spec.wrap = [st](const std::string& j) { return TableSerializer::build_pods_table_json(std::vector<std::string>{j}, st->current_version()); };
        } else if (match.category == RouteCategory::NodesCollection) {
            spec.wrap = [st](const std::string& j) { return TableSerializer::build_nodes_table_json(std::vector<std::string>{j}, st->current_version()); };
        } else if (match.category == RouteCategory::DeploymentsNsCollection) {
            spec.wrap = [st](const std::string& j) { return TableSerializer::build_deployments_table_json(std::vector<std::string>{j}, st->current_version()); };
        } else if (TableSerializer::has_generic_table(info.kind)) {
            std::string kind = info.kind;
            spec.wrap = [st, kind](const std::string& j) { return TableSerializer::build_generic_table_json(kind, std::vector<std::string>{j}, st->current_version()); };
        }
        if (spec.wrap) content_type = "application/json;as=Table;v=v1;g=meta.k8s.io";
    }

    if (!watch_hub_->has_capacity()) {
        std::cout << "[API] watch refused for " << req.path << ": " << watch_hub_->options().max_streams << " streams already open" << std::endl;
        return make_status(429, "TooManyRequests",
                           "watch limit reached (" + std::to_string(watch_hub_->options().max_streams) + " concurrent streams)");
    }

    std::string head = "HTTP/1.1 200 OK\r\nContent-Type: " + content_type +
                       "\r\nTransfer-Encoding: chunked\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
    if (!client_sock->send_all(head.data(), head.size())) {
        return make_status(500, "InternalError", "failed to start watch stream");
    }
    osal::Socket taken = std::move(*client_sock);
    HttpResponse resp;
    resp.socket_taken = true;
    resp.status_code = watch_hub_->add_watch(std::move(taken), std::move(spec)) ? 200 : 429;
    return resp;
}

// =============================================================================
// Generic store-backed resources: namespaces, configmaps, secrets
// =============================================================================
HttpResponse RouteHandler::handle_generic(const HttpRequest& req, const RouteMatch& match, const ResourceInfo& info) {
    const bool is_item = (match.category == RouteCategory::NamespaceItem ||
                          match.category == RouteCategory::ConfigMapItem ||
                          match.category == RouteCategory::SecretItem);
    std::string ns(match.namespace_name);
    std::string root = info.root;
    std::string prefix = root;
    if (info.namespaced && !ns.empty()) prefix += ns + "/";
    const std::string plural = info.plural;

    if (!is_item) {
        if (req.method == HttpMethod::Get || req.method == HttpMethod::Head) {
            std::vector<std::string> jsons;
            store_.list(prefix, jsons);
            HttpResponse sel_err;
            if (!filter_list(req, jsons, sel_err)) return sel_err;
            return list_response(req, info, jsons, list_json(info, jsons), store_.current_version());
        }
        if (req.method == HttpMethod::Post) {
            if (info.namespaced && ns.empty()) {
                return make_status(405, "MethodNotAllowed", "POST not allowed on cluster-wide " + plural + " listing");
            }
            if (req.body.empty()) return make_status(400, "BadRequest", "Failed to parse request body: empty body");
            models::JsonValue doc;
            std::string err;
            if (!models::parse_json(req.body, doc, err) || !doc.is_object()) {
                return make_status(400, "BadRequest", "Failed to parse request body: " + err);
            }
            const models::JsonValue* meta = doc.get("metadata");
            std::string name = meta ? meta->get_string("name", "") : "";
            if (name.empty()) return make_status(400, "BadRequest", std::string(info.kind) + " is invalid: metadata.name is required");
            std::string key = prefix + name;
            std::string existing;
            if (store_.get(key, existing).ok()) {
                return make_status(409, "AlreadyExists", plural + " \"" + name + "\" already exists", name, plural);
            }
            stamp_new_object(doc, ns, info);
            if (match.category == RouteCategory::NamespacesCollection && !doc.get("status")) {
                models::JsonValue st;
                st.type = models::JsonType::Object;
                st.set("phase", jstr("Active"));
                doc.set("status", std::move(st));
            }
            std::string json = doc.to_string();
            storage::Status ps = store_.put(key, json, false);
            if (!ps.ok()) return make_status(500, "InternalError", ps.message);
            HttpResponse resp;
            resp.status_code = 201;
            resp.content_type = "application/json";
            resp.body = json;
            return resp;
        }
        return make_status(405, "MethodNotAllowed", std::string(http_method_str(req.method)) + " not allowed on collection endpoint " + req.path);
    }

    std::string name(match.resource_name);
    std::string key = prefix + name;
    std::string existing;
    bool exists = store_.get(key, existing).ok();

    if (req.method == HttpMethod::Get || req.method == HttpMethod::Head) {
        if (!exists) return make_status(404, "NotFound", plural + " \"" + name + "\" not found", name, plural);
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = (req.method == HttpMethod::Head) ? "" : existing;
        return resp;
    }
    if (req.method == HttpMethod::Delete) {
        if (!exists) return make_status(404, "NotFound", plural + " \"" + name + "\" not found", name, plural);
        store_.remove(key);
        return make_status(200, "Success", plural + " \"" + name + "\" deleted", name, plural);
    }
    if (req.method == HttpMethod::Put || req.method == HttpMethod::Patch) {
        if (req.body.empty()) return make_status(400, "BadRequest", "Failed to parse request body: empty body");
        models::JsonValue in;
        std::string err;
        if (!models::parse_json(req.body, in, err) || !in.is_object()) {
            return make_status(400, "BadRequest", "Failed to parse request body: " + err);
        }
        if (!exists) return make_status(404, "NotFound", plural + " \"" + name + "\" not found", name, plural);
        models::JsonValue target;
        if (!models::parse_json(existing, target, err)) return make_status(500, "InternalError", "Failed to parse existing resource");
        if (req.method == HttpMethod::Patch) {
            models::merge_json(target, in);
        } else {
            // Identity fields are server-owned: carry them over when the client omits them
            const models::JsonValue* old_meta = target.get("metadata");
            std::string uid = old_meta ? old_meta->get_string("uid", "") : "";
            std::string created = old_meta ? old_meta->get_string("creationTimestamp", "") : "";
            target = std::move(in);
            models::JsonValue* nm = target.get_mut("metadata");
            if (nm && nm->is_object()) {
                if (nm->get_string("uid", "").empty() && !uid.empty()) nm->set("uid", jstr(uid));
                if (nm->get_string("creationTimestamp", "").empty() && !created.empty()) nm->set("creationTimestamp", jstr(created));
            }
        }
        std::string updated = target.to_string();
        store_.put(key, updated, true);
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = updated;
        return resp;
    }
    return make_status(405, "MethodNotAllowed", std::string(http_method_str(req.method)) + " not allowed on " + req.path);
}

// =============================================================================
// Events: core/v1 storage with an events.k8s.io/v1 projection
// =============================================================================
static std::string events_v1_to_core(const models::JsonValue& in) {
    models::JsonValue out;
    out.type = models::JsonType::Object;
    out.set("kind", jstr("Event"));
    out.set("apiVersion", jstr("v1"));
    if (const auto* m = in.get("metadata")) out.set("metadata", *m);
    if (const auto* r = in.get("regarding")) out.set("involvedObject", *r);
    out.set("reason", jstr(in.get_string("reason", "")));
    out.set("message", jstr(in.get_string("note", "")));
    out.set("type", jstr(in.get_string("type", "Normal")));
    out.set("reportingComponent", jstr(in.get_string("reportingController", "")));
    out.set("reportingInstance", jstr(in.get_string("reportingInstance", "")));
    return out.to_string();
}

HttpResponse RouteHandler::handle_events(const HttpRequest& req, const RouteMatch& match) {
    ResourceInfo info;
    resource_info(RouteCategory::EventsCollection, info);
    const bool projection = (match.api_group == "events.k8s.io");
    ResourceInfo out_info = info;
    if (projection) out_info.api_version = "events.k8s.io/v1";
    std::string ns(match.namespace_name);
    std::string prefix = std::string(info.root) + (ns.empty() ? "" : ns + "/");

    if (match.category == RouteCategory::EventsCollection) {
        if (req.method == HttpMethod::Get || req.method == HttpMethod::Head) {
            std::vector<std::string> jsons;
            store_.list(prefix, jsons);
            HttpResponse sel_err;
            if (!filter_list(req, jsons, sel_err)) return sel_err;
            if (req.as_table) {
                // Table cells read core fields; the projection only changes the plain list
                return list_response(req, info, jsons, list_json(out_info, jsons), store_.current_version());
            }
            if (projection) for (auto& j : jsons) j = EventRecorder::to_events_v1(j);
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = "application/json";
            resp.body = (req.method == HttpMethod::Head) ? "" : list_json(out_info, jsons);
            return resp;
        }
        if (req.method == HttpMethod::Post) {
            if (req.body.empty()) return make_status(400, "BadRequest", "Failed to parse request body: empty body");
            std::string core = req.body;
            if (projection) {
                models::JsonValue in;
                std::string perr;
                if (!models::parse_json(req.body, in, perr) || !in.is_object()) {
                    return make_status(400, "BadRequest", "Failed to parse request body: " + perr);
                }
                core = events_v1_to_core(in);
            }
            std::string stored, err;
            if (!events_.admit(core, stored, err)) return make_status(400, "BadRequest", "Event is invalid: " + err);
            HttpResponse resp;
            resp.status_code = 201;
            resp.content_type = "application/json";
            resp.body = projection ? EventRecorder::to_events_v1(stored) : stored;
            return resp;
        }
        return make_status(405, "MethodNotAllowed", std::string(http_method_str(req.method)) + " not allowed on collection endpoint " + req.path);
    }

    std::string name(match.resource_name);
    std::string key = prefix + name;
    std::string existing;
    bool exists = store_.get(key, existing).ok();
    if (!exists) return make_status(404, "NotFound", "events \"" + name + "\" not found", name, "events");
    if (req.method == HttpMethod::Get || req.method == HttpMethod::Head) {
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = (req.method == HttpMethod::Head) ? "" : (projection ? EventRecorder::to_events_v1(existing) : existing);
        return resp;
    }
    if (req.method == HttpMethod::Delete) {
        store_.remove(key);
        return make_status(200, "Success", "events \"" + name + "\" deleted", name, "events");
    }
    if (req.method == HttpMethod::Patch || req.method == HttpMethod::Put) {
        models::JsonValue in, target;
        std::string err;
        if (req.body.empty() || !models::parse_json(req.body, in, err) || !in.is_object()) {
            return make_status(400, "BadRequest", "Failed to parse request body: " + err);
        }
        if (projection) {
            std::string core = events_v1_to_core(in);
            models::parse_json(core, in, err);
        }
        if (!models::parse_json(existing, target, err)) return make_status(500, "InternalError", "Failed to parse existing resource");
        if (req.method == HttpMethod::Patch) models::merge_json(target, in); else target = std::move(in);
        std::string updated = target.to_string();
        store_.put_extended(key, updated, true, false);
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = projection ? EventRecorder::to_events_v1(updated) : updated;
        return resp;
    }
    return make_status(405, "MethodNotAllowed", std::string(http_method_str(req.method)) + " not allowed on " + req.path);
}

// =============================================================================
// Pod logs: served from the API server's mirror ring; kubelets ship into it
// =============================================================================
HttpResponse RouteHandler::handle_pod_log(const HttpRequest& req, const RouteMatch& match, osal::Socket* client_sock) {
    std::string ns = match.namespace_name.empty() ? "default" : std::string(match.namespace_name);
    std::string name(match.resource_name);
    std::string pod_key = ns + "/" + name;
    std::string pod_json;
    if (!store_.get("/registry/pods/" + ns + "/" + name, pod_json).ok()) {
        return make_status(404, "NotFound", "pods \"" + name + "\" not found", name, "pods");
    }
    runtime::PodLogStore& logs = runtime::PodLogStore::apiserver();

    if (req.method == HttpMethod::Post) {
        // Kubelet shipping: one JSON record per line
        // {"epoch":E,"seq":S,"t":<unix ms or 0>,"u":<uptime ms>,"m":"text"}
        size_t accepted = 0, seen = 0;
        std::string_view body = req.body;
        while (!body.empty()) {
            size_t nl = body.find('\n');
            std::string_view line = (nl == std::string_view::npos) ? body : body.substr(0, nl);
            body = (nl == std::string_view::npos) ? std::string_view() : body.substr(nl + 1);
            if (line.empty()) continue;
            models::JsonValue rec;
            std::string err;
            if (!models::parse_json(line, rec, err) || !rec.is_object()) continue;
            ++seen;
            runtime::PodLogRecord r;
            r.seq = static_cast<uint64_t>(rec.get_int("seq", 0));
            r.unix_ms = static_cast<uint64_t>(rec.get_int("t", 0));
            r.uptime_ms = static_cast<uint64_t>(rec.get_int("u", 0));
            r.text = rec.get_string("m", "");
            if (logs.accept_shipped(pod_key, static_cast<uint64_t>(rec.get_int("epoch", 0)), r)) ++accepted;
        }
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = "{\"received\":" + std::to_string(seen) + ",\"accepted\":" + std::to_string(accepted) + "}";
        return resp;
    }
    if (req.method != HttpMethod::Get && req.method != HttpMethod::Head) {
        return make_status(405, "MethodNotAllowed", std::string(http_method_str(req.method)) + " not allowed on " + req.path);
    }

    // Validate ?container= against the pod spec
    std::string container;
    if (query_param(req.query_string, "container", container) && !container.empty()) {
        models::JsonValue pd;
        std::string perr;
        bool found = false;
        if (models::parse_json(pod_json, pd, perr)) {
            const models::JsonValue* spec = pd.get("spec");
            const models::JsonValue* cs = spec ? spec->get("containers") : nullptr;
            if (cs && cs->is_array()) {
                for (const auto& c : cs->arr_val) if (c.get_string("name", "") == container) found = true;
            }
        }
        if (!found) return make_status(400, "BadRequest", "container " + container + " is not valid for pod " + name);
    }
    const bool follow = query_flag(req.query_string, "follow");
    const bool timestamps = query_flag(req.query_string, "timestamps");
    uint64_t tail = 0, since_s = 0, limit_bytes = 0;
    bool has_tail = query_uint(req.query_string, "tailLines", tail);
    bool has_since = query_uint(req.query_string, "sinceSeconds", since_s);
    bool has_limit = query_uint(req.query_string, "limitBytes", limit_bytes);

    std::vector<runtime::PodLogRecord> recs;
    bool have_ring = logs.tail(pod_key, has_tail ? static_cast<size_t>(tail) : 0, recs);
    if (!have_ring) {
        // Nothing shipped yet: a pod still waiting to start has no log
        models::JsonValue pd;
        std::string perr;
        std::string phase;
        if (models::parse_json(pod_json, pd, perr)) {
            const models::JsonValue* st = pd.get("status");
            if (st) phase = st->get_string("phase", "");
        }
        if (phase == "Pending" || phase.empty()) {
            return make_status(400, "BadRequest", "container \"" + (container.empty() ? name : container) + "\" in pod \"" +
                               name + "\" is waiting to start: ContainerCreating");
        }
    }
    if (has_since) {
        uint64_t now = osal::System::get_unix_time_ms();
        uint64_t cutoff = (now > since_s * 1000ULL) ? now - since_s * 1000ULL : 0;
        std::vector<runtime::PodLogRecord> kept;
        for (auto& r : recs) if (r.unix_ms == 0 || r.unix_ms >= cutoff) kept.push_back(std::move(r));
        recs.swap(kept);
    }
    std::string body;
    uint64_t last_seq = logs.last_seq(pod_key);
    for (const auto& r : recs) {
        body += runtime::format_log_line(r, timestamps);
        last_seq = r.seq;
    }
    if (has_limit && body.size() > limit_bytes) body.resize(static_cast<size_t>(limit_bytes));

    if (!follow || req.method == HttpMethod::Head) {
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "text/plain";
        resp.body = (req.method == HttpMethod::Head) ? "" : body;
        return resp;
    }

    if (!watch_hub_) return make_status(501, "NotImplemented", "log follow is not enabled on this API server");
    if (!client_sock || !client_sock->is_valid()) return make_status(400, "BadRequest", "follow requires a streaming connection");
    if (!watch_hub_->has_capacity()) {
        return make_status(429, "TooManyRequests",
                           "stream limit reached (" + std::to_string(watch_hub_->options().max_streams) + " concurrent streams)");
    }
    std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\n"
                       "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
    if (!client_sock->send_all(head.data(), head.size())) return make_status(500, "InternalError", "failed to start log stream");
    if (!body.empty()) {
        char hex[16];
        int n = snprintf(hex, sizeof(hex), "%zx\r\n", body.size());
        std::string chunk(hex, static_cast<size_t>(n));
        chunk += body;
        chunk += "\r\n";
        client_sock->send_all(chunk.data(), chunk.size());
    }
    LogFollowSpec spec;
    spec.pod_key = pod_key;
    spec.after_seq = last_seq;
    spec.timestamps = timestamps;
    uint64_t to = 0;
    if (query_uint(req.query_string, "timeoutSeconds", to)) spec.timeout_s = static_cast<uint32_t>(to);
    osal::Socket taken = std::move(*client_sock);
    HttpResponse resp;
    resp.socket_taken = true;
    resp.status_code = watch_hub_->add_log_follow(std::move(taken), std::move(spec)) ? 200 : 429;
    return resp;
}

// =============================================================================
// metrics.k8s.io/v1beta1: latest kubelet-pushed samples
// =============================================================================
HttpResponse RouteHandler::handle_metrics(const HttpRequest& req, const RouteMatch& match) {
    HttpResponse resp;
    resp.status_code = 200;
    resp.content_type = "application/json";
    std::string name(match.resource_name);
    std::string ns(match.namespace_name);

    if (match.category == RouteCategory::MetricsNodeItem && req.method == HttpMethod::Post) {
        // Kubelet push (see kubelet/metrics_provider.hpp for the measurement)
        std::string err;
        if (req.body.empty() || !metrics_.ingest(req.body, err)) {
            return make_status(400, "BadRequest", "metrics sample rejected: " + err);
        }
        return make_status(200, "Success", "metrics for node \"" + name + "\" recorded", name, "nodes");
    }
    if (req.method != HttpMethod::Get && req.method != HttpMethod::Head) {
        return make_status(405, "MethodNotAllowed", std::string(http_method_str(req.method)) + " not allowed on " + req.path);
    }

    switch (match.category) {
        case RouteCategory::MetricsNodes: {
            std::vector<NodeUsageSample> samples = metrics_.nodes();
            std::string body = "{\"kind\":\"NodeMetricsList\",\"apiVersion\":\"metrics.k8s.io/v1beta1\",\"metadata\":{},\"items\":[";
            for (size_t i = 0; i < samples.size(); ++i) {
                if (i) body += ",";
                body += MetricsRegistry::node_metrics_json(samples[i]);
            }
            body += "]}";
            resp.body = (req.method == HttpMethod::Head) ? "" : body;
            return resp;
        }
        case RouteCategory::MetricsNodeItem: {
            NodeUsageSample s;
            if (!metrics_.node(name, s)) {
                return make_status(404, "NotFound", "nodemetrics.metrics.k8s.io \"" + name + "\" not found", name, "nodes");
            }
            resp.body = (req.method == HttpMethod::Head) ? "" : MetricsRegistry::node_metrics_json(s);
            return resp;
        }
        case RouteCategory::MetricsPods: {
            std::vector<PodUsageSample> samples = metrics_.pods(ns);
            // labelSelector applies to the pods themselves
            ListSelector sel;
            std::string err;
            if (!ListSelector::from_query(req.query_string, sel, err)) return make_status(400, "BadRequest", err);
            std::string body = "{\"kind\":\"PodMetricsList\",\"apiVersion\":\"metrics.k8s.io/v1beta1\",\"metadata\":{},\"items\":[";
            bool first = true;
            for (const auto& s : samples) {
                if (!sel.empty()) {
                    std::string pj;
                    if (!store_.get("/registry/pods/" + s.namespace_name + "/" + s.name, pj).ok() || !sel.matches_json(pj)) continue;
                }
                if (!first) body += ",";
                first = false;
                body += MetricsRegistry::pod_metrics_json(s);
            }
            body += "]}";
            resp.body = (req.method == HttpMethod::Head) ? "" : body;
            return resp;
        }
        case RouteCategory::MetricsPodItem: {
            PodUsageSample s;
            if (!metrics_.pod(ns.empty() ? "default" : ns, name, s)) {
                return make_status(404, "NotFound", "podmetrics.metrics.k8s.io \"" + name + "\" not found", name, "pods");
            }
            resp.body = (req.method == HttpMethod::Head) ? "" : MetricsRegistry::pod_metrics_json(s);
            return resp;
        }
        default:
            return make_status(404, "NotFound", "Endpoint not found: " + req.path);
    }
}

} // namespace kube32::api
