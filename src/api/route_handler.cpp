#include "api/route_handler.hpp"
#include "api/table_serializer.hpp"
#include "models/kube_models.hpp"

#include <sstream>

namespace kube32::api {

RouteHandler::RouteHandler(storage::Store& store, scheduler::Scheduler* scheduler)
    : store_(store), scheduler_(scheduler) {}

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
    if (path == "/apis/coordination.k8s.io" || path == "/apis/coordination.k8s.io/v1") {
        match.category = RouteCategory::DiscoveryApisCoordinationV1;
        return match;
    }
    if (path == "/openapi/v3") {
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
        if (slash1 != std::string_view::npos) {
            match.namespace_name = sub.substr(0, slash1);
            std::string_view rest = sub.substr(slash1 + 1);

            if (rest == "pods") {
                match.category = RouteCategory::PodsNsCollection;
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
        std::string_view sub = path.substr(26); // Skip "/apis/apps/v1/namespaces/"
        size_t slash1 = sub.find('/');
        if (slash1 != std::string_view::npos) {
            match.namespace_name = sub.substr(0, slash1);
            std::string_view rest = sub.substr(slash1 + 1);

            if (rest == "deployments") {
                match.category = RouteCategory::DeploymentsNsCollection;
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

    switch (match.category) {
        case RouteCategory::DiscoveryVersion:
        case RouteCategory::DiscoveryApi:
        case RouteCategory::DiscoveryApis:
        case RouteCategory::DiscoveryApiV1:
        case RouteCategory::DiscoveryApisAppsV1:
        case RouteCategory::DiscoveryApisEventsV1:
        case RouteCategory::DiscoveryApisCoordinationV1:
        case RouteCategory::DiscoveryOpenApiV3:
            return handle_discovery(req, match.category);

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
    if (req.method != HttpMethod::Get && req.method != HttpMethod::Head) {
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
                        "{\"name\":\"apps\",\"versions\":[{\"groupVersion\":\"apps/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"apps/v1\",\"version\":\"v1\"}},"
                        "{\"name\":\"events.k8s.io\",\"versions\":[{\"groupVersion\":\"events.k8s.io/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"events.k8s.io/v1\",\"version\":\"v1\"}},"
                        "{\"name\":\"coordination.k8s.io\",\"versions\":[{\"groupVersion\":\"coordination.k8s.io/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"coordination.k8s.io/v1\",\"version\":\"v1\"}}"
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
                        "{\"name\":\"namespaces\",\"singularName\":\"namespace\",\"namespaced\":false,\"kind\":\"Namespace\","
                        "\"verbs\":[\"get\",\"list\",\"create\",\"delete\"],\"shortNames\":[\"ns\"]},"
                        "{\"name\":\"events\",\"singularName\":\"event\",\"namespaced\":true,\"kind\":\"Event\","
                        "\"verbs\":[\"get\",\"list\"]}"
                        "]}";
            break;

        case RouteCategory::DiscoveryApisAppsV1:
            resp.body = "{\"kind\":\"APIResourceList\",\"apiVersion\":\"v1\",\"groupVersion\":\"apps/v1\",\"resources\":["
                        "{\"name\":\"deployments\",\"singularName\":\"deployment\",\"namespaced\":true,\"kind\":\"Deployment\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"update\",\"patch\",\"delete\"],\"shortNames\":[\"deploy\"],\"categories\":[\"all\"]},"
                        "{\"name\":\"deployments/status\",\"singularName\":\"deployment\",\"namespaced\":true,\"kind\":\"Deployment\","
                        "\"verbs\":[\"get\",\"patch\",\"update\"]},"
                        "{\"name\":\"deployments/scale\",\"singularName\":\"deployment\",\"namespaced\":true,\"group\":\"autoscaling\",\"version\":\"v1\",\"kind\":\"Scale\","
                        "\"verbs\":[\"get\",\"patch\",\"update\"]}"
                        "]}";
            break;

        case RouteCategory::DiscoveryApisEventsV1:
            resp.body = "{\"kind\":\"APIResourceList\",\"apiVersion\":\"v1\",\"groupVersion\":\"events.k8s.io/v1\",\"resources\":[]}";
            break;

        case RouteCategory::DiscoveryApisCoordinationV1:
            resp.body = "{\"kind\":\"APIResourceList\",\"apiVersion\":\"v1\",\"groupVersion\":\"coordination.k8s.io/v1\",\"resources\":["
                        "{\"name\":\"leases\",\"singularName\":\"lease\",\"namespaced\":true,\"kind\":\"Lease\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"update\",\"patch\",\"delete\"]}"
                        "]}";
            break;

        case RouteCategory::DiscoveryOpenApiV3:
            resp.body = "{\"paths\":{}}";
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
            if (client_sock != nullptr) {
                ChunkedStreamWriter writer(*client_sock);
                if (req.as_table) {
                    TableSerializer::serialize_nodes_table_from_json(writer, node_jsons);
                } else {
                    TableSerializer::serialize_nodes_list_from_json(writer, node_jsons);
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
                    resp.body = TableSerializer::build_nodes_table_json(node_jsons);
                } else {
                    resp.content_type = "application/json";
                    resp.body = TableSerializer::build_nodes_list_json(node_jsons);
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

            store_.put(key, req.body, false);
            if (on_lease_renewed_) {
                on_lease_renewed_(node_name);
            }
            HttpResponse resp;
            resp.status_code = 201;
            resp.content_type = "application/json";
            resp.body = req.body;
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
        resp.content_type = "application/json";
        resp.body = existing_json;
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
            if (client_sock != nullptr) {
                ChunkedStreamWriter writer(*client_sock);
                if (req.as_table) {
                    TableSerializer::serialize_pods_table_from_json(writer, pod_jsons);
                } else {
                    TableSerializer::serialize_pods_list_from_json(writer, pod_jsons);
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
                    resp.body = TableSerializer::build_pods_table_json(pod_jsons);
                } else {
                    resp.content_type = "application/json";
                    resp.body = TableSerializer::build_pods_list_json(pod_jsons);
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
            if (client_sock != nullptr) {
                ChunkedStreamWriter writer(*client_sock);
                if (req.as_table) {
                    TableSerializer::serialize_pods_table_from_json(writer, pod_jsons);
                } else {
                    TableSerializer::serialize_pods_list_from_json(writer, pod_jsons);
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
                    resp.body = TableSerializer::build_pods_table_json(pod_jsons);
                } else {
                    resp.content_type = "application/json";
                    resp.body = TableSerializer::build_pods_list_json(pod_jsons);
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

            // Ensure namespace is set
            models::JsonValue* meta_mut = doc.get_mut("metadata");
            if (meta_mut) {
                meta_mut->set("namespace", models::JsonValue{models::JsonType::String, false, 0, ns, {}, {}});
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
        resp.content_type = "application/json";
        resp.body = existing_json;
        return resp;
    } else if (req.method == HttpMethod::Delete) {
        store_.remove(key);
        return make_status(200, "Success", "pods \"" + name + "\" deleted");
    }

    return make_status(405, "MethodNotAllowed", std::string(http_method_str(req.method)) + " not allowed on " + req.path);
}

HttpResponse RouteHandler::handle_deployments(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena, osal::Socket* client_sock) {
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
            store_.list("/registry/deployments/" + ns + "/", dep_jsons);
            if (client_sock != nullptr) {
                ChunkedStreamWriter writer(*client_sock);
                if (req.as_table) {
                    TableSerializer::serialize_deployments_table_from_json(writer, dep_jsons);
                } else {
                    TableSerializer::serialize_deployments_list_from_json(writer, dep_jsons);
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
                    resp.body = TableSerializer::build_deployments_table_json(dep_jsons);
                } else {
                    resp.content_type = "application/json";
                    resp.body = TableSerializer::build_deployments_list_json(dep_jsons);
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

            // Ensure namespace and status replicas
            models::JsonValue* meta_mut = doc.get_mut("metadata");
            if (meta_mut) {
                meta_mut->set("namespace", models::JsonValue{models::JsonType::String, false, 0, ns, {}, {}});
            }

            int64_t replicas = 1;
            const models::JsonValue* spec = doc.get("spec");
            if (spec && spec->is_object()) {
                replicas = spec->get_int("replicas", 1);
            }

            if (!doc.get("status")) {
                models::JsonValue st;
                st.type = models::JsonType::Object;
                st.set("replicas", models::JsonValue{models::JsonType::Number, false, static_cast<double>(replicas), "", {}, {}});
                st.set("readyReplicas", models::JsonValue{models::JsonType::Number, false, static_cast<double>(replicas), "", {}, {}});
                st.set("availableReplicas", models::JsonValue{models::JsonType::Number, false, static_cast<double>(replicas), "", {}, {}});
                st.set("updatedReplicas", models::JsonValue{models::JsonType::Number, false, static_cast<double>(replicas), "", {}, {}});
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

    if (match.category == RouteCategory::DeploymentScale) {
        models::JsonValue doc;
        std::string err;
        models::parse_json(existing_json, doc, err);
        int64_t replicas = 1;
        const models::JsonValue* spec = doc.get("spec");
        if (spec && spec->is_object()) replicas = spec->get_int("replicas", 1);

        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        if (req.method == HttpMethod::Head) {
            resp.body = "";
        } else {
            resp.body = "{\"kind\":\"Scale\",\"apiVersion\":\"autoscaling/v1\","
                        "\"metadata\":{\"name\":\"" + escape_json(name) + "\",\"namespace\":\"" + escape_json(ns) + "\"},"
                        "\"spec\":{\"replicas\":" + std::to_string(replicas) + "},"
                        "\"status\":{\"replicas\":" + std::to_string(replicas) + "}}";
        }
        return resp;
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
        resp.content_type = "application/json";
        resp.body = existing_json;
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
    std::string ns = match.namespace_name.empty() ? "kube-node-lease" : std::string(match.namespace_name);
    std::string name(match.resource_name);

    if (match.category == RouteCategory::LeasesNsCollection) {
        if (req.method == HttpMethod::Get || req.method == HttpMethod::Head) {
            std::vector<std::string> lease_jsons;
            std::string prefix = "/registry/leases/" + ns + "/";
            store_.list(prefix, lease_jsons);

            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = "application/json";
            if (req.method == HttpMethod::Head) {
                resp.body = "";
            } else {
                std::string list_json = "{\"kind\":\"LeaseList\",\"apiVersion\":\"coordination.k8s.io/v1\",\"metadata\":{\"resourceVersion\":\"1\"},\"items\":[";
                for (size_t i = 0; i < lease_jsons.size(); ++i) {
                    if (i > 0) list_json += ",";
                    list_json += lease_jsons[i];
                }
                list_json += "]}";
                resp.body = std::move(list_json);
            }
            return resp;
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
            store_.put(key, req.body, true);

            if (on_lease_renewed_) {
                on_lease_renewed_(lease_name);
            }

            HttpResponse resp;
            resp.status_code = 201;
            resp.content_type = "application/json";
            resp.body = req.body;
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

} // namespace kube32::api
