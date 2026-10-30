#include "api/route_handler.hpp"
#include "osal/sha256.hpp"
#include <iostream>
#include "api/table_serializer.hpp"
#include "api/selectors.hpp"
#include "api/watch_hub.hpp"
#include "api/version_info.hpp"
#include "models/kube_models.hpp"
#include "models/kube_lease.hpp"
#include "models/tolerations.hpp"
#include "net/network_policy.hpp"
#include "runtime/pod_log.hpp"
#include "api/protobuf_body.hpp"
#include "osal/socket.hpp"
#include "osal/system.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace kube32::api {

static models::JsonValue jstr(const std::string& v) { return models::JsonValue{models::JsonType::String, false, 0, v, {}, {}}; }
static models::JsonValue jnum(double v) { return models::JsonValue{models::JsonType::Number, false, v, "", {}, {}}; }

namespace {

// Per-request state shared by the handlers of one request on one worker
// thread: the dry-run switch and the continue token a paginated list sets.
struct RequestState {
    bool dry_run{false};
    std::string continue_token;
    uint64_t remaining{0};
};
thread_local RequestState* t_request = nullptr;

struct RequestScope {
    RequestState state;
    RequestState* previous;
    RequestScope() : previous(t_request) { t_request = &state; }
    ~RequestScope() { t_request = previous; }
};

bool dry_run() { return t_request && t_request->dry_run; }

// A store write that did not happen, as kube-apiserver reports it: a full
// object store is a 500 with the reason, a lost race on create a 409.
HttpResponse store_failure(const storage::Status& s, const std::string& plural, const std::string& name) {
    if (s.code == storage::StatusCode::AlreadyExists) {
        return RouteHandler::make_status(409, "AlreadyExists", plural + " \"" + name + "\" already exists", name, plural);
    }
    if (s.code == storage::StatusCode::VersionConflict) {
        return RouteHandler::make_status(409, "Conflict", "Operation cannot be fulfilled on " + plural + " \"" + name +
                                         "\": the object has been modified; please apply your changes to the latest version and try again",
                                         name, plural);
    }
    return RouteHandler::make_status(500, "InternalError", s.message.empty() ? "the object store refused the write" : s.message, name, plural);
}

// Server-owned fields across an update, as kube-apiserver keeps them: uid
// and creationTimestamp stay, metadata.generation counts spec changes, and
// a main-resource PUT that leaves out status keeps the stored one.
void carry_server_fields(const models::JsonValue& old_doc, models::JsonValue& doc, bool keep_status) {
    const models::JsonValue* om = old_doc.get("metadata");
    models::JsonValue* nm = doc.get_mut("metadata");
    if (!nm || !nm->is_object()) {
        models::JsonValue m;
        m.type = models::JsonType::Object;
        doc.set("metadata", std::move(m));
        nm = doc.get_mut("metadata");
    }
    if (om) {
        if (nm->get_string("uid", "").empty() && !om->get_string("uid", "").empty()) nm->set("uid", jstr(om->get_string("uid", "")));
        if (nm->get_string("creationTimestamp", "").empty() && !om->get_string("creationTimestamp", "").empty())
            nm->set("creationTimestamp", jstr(om->get_string("creationTimestamp", "")));
    }
    const models::JsonValue* os = old_doc.get("spec");
    const models::JsonValue* ns = doc.get("spec");
    if (os || ns) {
        int64_t gen = om ? om->get_int("generation", 1) : 1;
        if (gen < 1) gen = 1;
        if ((os ? os->to_string() : std::string()) != (ns ? ns->to_string() : std::string())) ++gen;
        nm->set("generation", jnum(static_cast<double>(gen)));
    }
    if (keep_status && !doc.get("status")) {
        if (const models::JsonValue* st = old_doc.get("status")) doc.set("status", *st);
    }
}
bool page_truncated() { return t_request && !t_request->continue_token.empty(); }

// Lists are streamed straight to the socket (no full copy in RAM) on plain
// connections when the page is complete. A TLS connection is kept alive
// between requests, so it gets a Content-Length response instead.
bool stream_list(osal::Socket* sock) { return sock && !sock->is_tls() && !page_truncated(); }

// The store as the handlers see it. With ?dryRun=All a write is checked
// (existence, resourceVersion preconditions) and then dropped: nothing is
// stored, no watch event fires, nothing reaches flash.
class DryRunView : public storage::Store {
public:
    explicit DryRunView(storage::Store& real) : real_(real) {}
    storage::Status get(const std::string& key, std::string& out) override { return real_.get(key, out); }
    storage::Status put(const std::string& key, const std::string& json, bool overwrite) override {
        if (dry_run()) return check_put(key, overwrite, 0, nullptr);
        return real_.put(key, json, overwrite);
    }
    storage::Status remove(const std::string& key) override {
        if (dry_run()) {
            std::string existing;
            return real_.get(key, existing).ok() ? storage::Status::Ok() : storage::Status::NotFound();
        }
        return real_.remove(key);
    }
    storage::Status list(const std::string& prefix, std::vector<std::string>& out) override { return real_.list(prefix, out); }
    storage::Status persist() override { return real_.persist(); }
    storage::Status recover() override { return real_.recover(); }
    storage::Status get_extended(const std::string& key, std::string& out, uint64_t* version) override {
        return real_.get_extended(key, out, version);
    }
    storage::Status put_extended(const std::string& key, const std::string& json, bool overwrite, bool persist_to_flash,
                                 uint64_t expected_version, uint64_t* out_version) override {
        if (dry_run()) return check_put(key, overwrite, expected_version, out_version);
        return real_.put_extended(key, json, overwrite, persist_to_flash, expected_version, out_version);
    }
    storage::Status list_extended(const std::string& prefix, std::vector<std::string>& out, std::vector<std::string>* keys) override {
        return real_.list_extended(prefix, out, keys);
    }
    uint32_t watch(const std::string& prefix, storage::WatchCallback cb) override { return real_.watch(prefix, std::move(cb)); }
    void unwatch(uint32_t id) override { real_.unwatch(id); }
    uint64_t current_version() const override { return real_.current_version(); }
    storage::Status flush(bool all) override { return real_.flush(all); }
    storage::Status commit() override { return real_.commit(); }
    bool persistence_stats(storage::PersistenceStats& out) const override { return real_.persistence_stats(out); }

private:
    storage::Store& real_;
    storage::Status check_put(const std::string& key, bool overwrite, uint64_t expected, uint64_t* out_version) {
        std::string existing;
        uint64_t version = 0;
        bool exists = real_.get_extended(key, existing, &version).ok();
        if (expected && (!exists || version != expected)) return storage::Status::VersionConflict();
        if (exists && !overwrite) return storage::Status::AlreadyExists();
        if (out_version) *out_version = real_.current_version() + 1;
        return storage::Status::Ok();
    }
};

// Continue tokens are opaque to clients: base64url (no padding) of
// {"rv":<list resourceVersion>,"start":"<namespace>/<name>"}. A continued list
// starts after `start` in the current state of the store.
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64url_encode(const std::string& in) {
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8) | static_cast<uint8_t>(in[i + 2]);
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += kB64[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        uint32_t v = static_cast<uint8_t>(in[i]) << 16;
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63];
    } else if (i + 2 == in.size()) {
        uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8);
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63];
    }
    return out;
}

bool base64url_decode(const std::string& in, std::string& out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const char* p = std::strchr(kB64, c == '+' ? '-' : (c == '/' ? '_' : c));
        if (!p || !*p) return false;
        acc = (acc << 6) | static_cast<uint32_t>(p - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return true;
}

// Sort key of a listed object: "<namespace>/<name>", the order of the keys
// under one store prefix.
std::string list_order_key(const std::string& json) {
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) return std::string();
    const models::JsonValue* meta = doc.get("metadata");
    if (!meta) return std::string();
    return meta->get_string("namespace", "") + "/" + meta->get_string("name", "");
}

} // namespace

RouteHandler::RouteHandler(storage::Store& store, scheduler::Scheduler* scheduler)
    : dry_run_view_(std::make_unique<DryRunView>(store)), store_(*dry_run_view_), scheduler_(scheduler),
      events_(*dry_run_view_, 64), metrics_(30000, 32) {
    seed_namespaces();
}

RouteMatch RouteHandler::match_route(std::string_view path) {
    RouteMatch match;

    if (path == "/healthz" || path == "/livez" || path == "/readyz" || path.rfind("/healthz/", 0) == 0 ||
        path.rfind("/livez/", 0) == 0 || path.rfind("/readyz/", 0) == 0) {
        match.category = RouteCategory::Health;
        return match;
    }

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

    // Endpoints (core/v1) and EndpointSlices (discovery.k8s.io/v1)
    if (path == "/api/v1/endpoints") {
        match.category = RouteCategory::EndpointsCollection;
        return match;
    }
    if (path == "/apis/discovery.k8s.io") {
        match.category = RouteCategory::DiscoveryApisDiscoveryGroup;
        return match;
    }
    if (path == "/apis/discovery.k8s.io/v1") {
        match.category = RouteCategory::DiscoveryApisDiscoveryV1;
        return match;
    }
    if (path == "/apis/discovery.k8s.io/v1/endpointslices") {
        match.category = RouteCategory::EndpointSlicesCollection;
        match.api_group = "discovery.k8s.io";
        return match;
    }
    static constexpr std::string_view kSlicesNsPrefix = "/apis/discovery.k8s.io/v1/namespaces/";
    if (path.rfind(kSlicesNsPrefix, 0) == 0) {
        std::string_view sub = path.substr(kSlicesNsPrefix.size());
        size_t sl = sub.find('/');
        if (sl != std::string_view::npos) {
            match.namespace_name = sub.substr(0, sl);
            match.api_group = "discovery.k8s.io";
            std::string_view rest = sub.substr(sl + 1);
            if (rest == "endpointslices") { match.category = RouteCategory::EndpointSlicesCollection; return match; }
            if (rest.rfind("endpointslices/", 0) == 0 && rest.size() > 15 && rest.find('/', 15) == std::string_view::npos) {
                match.category = RouteCategory::EndpointSliceItem;
                match.resource_name = rest.substr(15);
                return match;
            }
        }
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
            } else if (rest == "endpoints") {
                match.category = RouteCategory::EndpointsCollection;
                return match;
            } else if (rest.rfind("endpoints/", 0) == 0 && rest.size() > 10 && rest.find('/', 10) == std::string_view::npos) {
                match.category = RouteCategory::EndpointsItem;
                match.resource_name = rest.substr(10);
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

    // 7. kube32.io FirmwareRollouts (cluster-scoped)
    if (path == "/apis/kube32.io") {
        match.category = RouteCategory::DiscoveryApisKube32Group;
        return match;
    }
    if (path == "/apis/kube32.io/v1") {
        match.category = RouteCategory::DiscoveryApisKube32V1;
        return match;
    }
    static constexpr std::string_view kRolloutsPrefix = "/apis/kube32.io/v1/firmwarerollouts";
    if (path.rfind(kRolloutsPrefix, 0) == 0) {
        std::string_view rest = path.substr(kRolloutsPrefix.size());
        match.api_group = "kube32.io";
        if (rest.empty()) {
            match.category = RouteCategory::FirmwareRolloutsCollection;
            return match;
        }
        if (rest.size() > 1 && rest[0] == '/' && rest.find('/', 1) == std::string_view::npos) {
            match.category = RouteCategory::FirmwareRolloutItem;
            match.resource_name = rest.substr(1);
            return match;
        }
    }

    // 8. networking.k8s.io Ingresses and NetworkPolicies
    if (path == "/apis/networking.k8s.io") {
        match.category = RouteCategory::DiscoveryApisNetworkingGroup;
        return match;
    }
    if (path == "/apis/networking.k8s.io/v1") {
        match.category = RouteCategory::DiscoveryApisNetworkingV1;
        return match;
    }
    if (path == "/apis/networking.k8s.io/v1/ingresses") {
        match.category = RouteCategory::IngressesCollection;
        match.api_group = "networking.k8s.io";
        return match;
    }
    if (path == "/apis/networking.k8s.io/v1/networkpolicies") {
        match.category = RouteCategory::NetworkPoliciesCollection;
        match.api_group = "networking.k8s.io";
        return match;
    }
    static constexpr std::string_view kNetNsPrefix = "/apis/networking.k8s.io/v1/namespaces/";
    if (path.rfind(kNetNsPrefix, 0) == 0) {
        std::string_view sub = path.substr(kNetNsPrefix.size());
        size_t sl = sub.find('/');
        if (sl != std::string_view::npos) {
            match.namespace_name = sub.substr(0, sl);
            match.api_group = "networking.k8s.io";
            std::string_view rest = sub.substr(sl + 1);
            if (rest == "ingresses") { match.category = RouteCategory::IngressesCollection; return match; }
            if (rest.rfind("ingresses/", 0) == 0 && rest.find('/', 10) == std::string_view::npos && rest.size() > 10) {
                match.category = RouteCategory::IngressItem;
                match.resource_name = rest.substr(10);
                return match;
            }
            if (rest == "networkpolicies") { match.category = RouteCategory::NetworkPoliciesCollection; return match; }
            if (rest.rfind("networkpolicies/", 0) == 0 && rest.find('/', 16) == std::string_view::npos && rest.size() > 16) {
                match.category = RouteCategory::NetworkPolicyItem;
                match.resource_name = rest.substr(16);
                return match;
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

bool RouteHandler::authenticated(const HttpRequest& req) const {
    if (bearer_token_.empty()) return true;
    std::string_view h = req.authorization;
    while (!h.empty() && h.front() == ' ') h.remove_prefix(1);
    if (h.size() < 7) return false;
    static const char kScheme[] = "bearer ";
    for (size_t i = 0; i < 7; ++i) {
        if (std::tolower(static_cast<unsigned char>(h[i])) != kScheme[i]) return false;
    }
    h.remove_prefix(7);
    while (!h.empty() && (h.front() == ' ' || h.back() == ' ')) {
        if (h.front() == ' ') h.remove_prefix(1); else h.remove_suffix(1);
    }
    // Constant time in the token's length
    unsigned diff = static_cast<unsigned>(h.size() ^ bearer_token_.size());
    for (size_t i = 0; i < bearer_token_.size(); ++i) {
        diff |= static_cast<unsigned>(static_cast<unsigned char>(bearer_token_[i]) ^ (i < h.size() ? static_cast<unsigned char>(h[i]) : 0));
    }
    return diff == 0;
}

HttpResponse RouteHandler::handle_request(const HttpRequest& req, mem::MonotonicArena& arena, osal::Socket* client_sock) {
    RouteMatch match = match_route(req.path);

    // Probes answer without credentials, as kube-apiserver's do
    if (match.category == RouteCategory::Health) {
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "text/plain";
        resp.body = (req.method == HttpMethod::Head) ? "" : "ok";
        return resp;
    }
    if (!authenticated(req) && match.category != RouteCategory::DiscoveryVersion) {
        return make_status(401, "Unauthorized", "Unauthorized");
    }

    RequestScope scope;
    std::string dry;
    if (query_param(req.query_string, "dryRun", dry)) {
        if (dry != "All") {
            return make_status(400, "BadRequest", "dryRun: Unsupported value: \"" + dry + "\": supported values: \"All\"");
        }
        scope.state.dry_run = (req.method == HttpMethod::Post || req.method == HttpMethod::Put ||
                               req.method == HttpMethod::Patch || req.method == HttpMethod::Delete);
    } else if (req.method == HttpMethod::Delete && !req.body.empty()) {
        // kubectl delete --dry-run=server sends DeleteOptions {"dryRun":["All"]}
        models::JsonValue opts;
        std::string err;
        if (models::parse_json(req.body, opts, err) && opts.is_object()) {
            if (const models::JsonValue* d = opts.get("dryRun"); d && d->is_array()) {
                for (const auto& v : d->arr_val) {
                    if (!v.is_string() || v.str_val != "All") {
                        return make_status(400, "BadRequest", "dryRun: Unsupported value: supported values: \"All\"");
                    }
                    scope.state.dry_run = true;
                }
            }
        }
    }

    // Typed clients send ConfigMaps, Secrets, Namespaces, Services,
    // Deployments and Jobs as protobuf (kubectl create ...) and do not retry
    // as JSON
    HttpRequest converted;
    const HttpRequest* effective = &req;
    if (!req.body.empty() && req.content_type.rfind("application/vnd.kubernetes.protobuf", 0) == 0) {
        std::string json, why;
        if (protobuf_body_to_json(req.body, json, why)) {
            converted = req;
            converted.body = std::move(json);
            converted.content_type = "application/json";
            effective = &converted;
        }
    }

    HttpResponse resp = dispatch(*effective, match, arena, client_sock);

    // A change is in the journal before the client hears it was made, so a
    // crash right after the answer keeps it. Requests that end at the same
    // time share one journal write.
    const bool mutation = req.method == HttpMethod::Post || req.method == HttpMethod::Put ||
                          req.method == HttpMethod::Patch || req.method == HttpMethod::Delete;
    if (mutation && !scope.state.dry_run && resp.status_code >= 200 && resp.status_code < 300) {
        storage::Status durable = store_.commit();
        if (!durable.ok()) {
            return make_status(500, "InternalError", "the change could not be written to the object store journal: " + durable.message);
        }
    }

    // A list cut short by ?limit carries the token for the next page
    if (!scope.state.continue_token.empty() && resp.status_code == 200 && !resp.is_chunked && !resp.socket_taken) {
        size_t meta = resp.body.find("\"metadata\":{");
        if (meta != std::string::npos) {
            size_t at = meta + 12;
            std::string field = "\"continue\":\"" + scope.state.continue_token + "\",\"remainingItemCount\":" +
                                std::to_string(scope.state.remaining);
            if (at < resp.body.size() && resp.body[at] != '}') field += ",";
            resp.body.insert(at, field);
        }
    }
    return resp;
}

HttpResponse RouteHandler::dispatch(const HttpRequest& req, const RouteMatch& match, mem::MonotonicArena& arena, osal::Socket* client_sock) {
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

    // Bodies are JSON; protobuf ones of the kinds handle_request converts
    // arrive here as JSON already. Access reviews are the exception: k9s posts
    // them as protobuf on every view and does not retry, and the answer does
    // not depend on the body (always allowed).
    bool protobuf_body = !req.body.empty() && req.content_type.rfind("application/vnd.kubernetes.protobuf", 0) == 0;
    bool body_free_review = (match.category == RouteCategory::SelfSubjectAccessReviews || match.category == RouteCategory::SelfSubjectRulesReviews);
    if (protobuf_body && !body_free_review) {
        std::string json, why;
        protobuf_body_to_json(req.body, json, why);
        HttpResponse resp = make_status(415, "UnsupportedMediaType", "the body could not be decoded: " + why);
        return resp;
    }

    // Patches are JSON merge patches or strategic merge patches
    // (models::apply_patch). Server-side apply needs field ownership tracking
    // and JSON patch an RFC 6902 engine; both are refused with the media
    // types that are accepted, as kube-apiserver answers an unknown patch type.
    if (req.method == HttpMethod::Patch) {
        const std::string& ct = req.content_type;
        if (ct.rfind("application/apply-patch", 0) == 0 || ct.rfind("application/json-patch+json", 0) == 0) {
            const bool apply = ct.rfind("application/apply-patch", 0) == 0;
            return make_status(415, "UnsupportedMediaType",
                               std::string(apply ? "server-side apply is not supported, use client-side apply (kubectl apply without --server-side)"
                                                 : "JSON patch is not supported") +
                               ": the body of the request was in an unknown format - accepted media types include: "
                               "application/merge-patch+json, application/strategic-merge-patch+json");
        }
    }

    // fieldValidation=Strict (what kubectl apply/create send by default, see
    // openapi_v3_document): reject unknown top-level and metadata fields, as
    // kube-apiserver does. Nested fields are not checked.
    if ((req.method == HttpMethod::Post || req.method == HttpMethod::Put || req.method == HttpMethod::Patch) && !req.body.empty()) {
        std::string fv;
        if (query_param(req.query_string, "fieldValidation", fv) && fv == "Strict") {
            std::string unknown;
            if (!strict_fields_ok(match.category, match.api_group, req.body, unknown)) {
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
        case RouteCategory::DiscoveryApisNetworkingGroup:
        case RouteCategory::DiscoveryApisNetworkingV1:
        case RouteCategory::DiscoveryApisDiscoveryGroup:
        case RouteCategory::DiscoveryApisDiscoveryV1:
        case RouteCategory::DiscoveryApisKube32Group:
        case RouteCategory::DiscoveryApisKube32V1:
        case RouteCategory::SelfSubjectAccessReviews:
        case RouteCategory::SelfSubjectRulesReviews:
        case RouteCategory::DiscoveryOpenApiV3:
            return handle_discovery(req, match.category);

        case RouteCategory::NamespacesCollection:
        case RouteCategory::NamespaceItem:
        case RouteCategory::ConfigMapsCollection:
        case RouteCategory::ConfigMapItem:
        case RouteCategory::SecretsCollection:
        case RouteCategory::SecretItem:
        case RouteCategory::ServicesNsCollection:
        case RouteCategory::ServiceItem:
        case RouteCategory::IngressesCollection:
        case RouteCategory::IngressItem:
        case RouteCategory::NetworkPoliciesCollection:
        case RouteCategory::NetworkPolicyItem: {
            ResourceInfo info;
            resource_info(match.category, info);
            return handle_generic(req, match, info);
        }

        // Created by kube32-cli firmware rollout (or kubectl); the firmware
        // rollout controller writes their status
        case RouteCategory::FirmwareRolloutsCollection:
        case RouteCategory::FirmwareRolloutItem: {
            ResourceInfo info;
            resource_info(match.category, info);
            if ((req.method == HttpMethod::Post || req.method == HttpMethod::Put) && !req.body.empty()) {
                models::JsonValue doc;
                std::string err;
                const models::JsonValue* spec = models::parse_json(req.body, doc, err) ? doc.get("spec") : nullptr;
                if (!osal::Sha256::valid_hex(spec ? spec->get_string("sha256", "") : "")) {
                    return make_status(422, "Invalid", "FirmwareRollout.kube32.io is invalid: spec.sha256: "
                                                       "the SHA-256 of the staged firmware image (64 hex digits) is required");
                }
            }
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

        // Stored like any object; the workload controller creates their pods
        case RouteCategory::DaemonSetsNsCollection:
        case RouteCategory::DaemonSetItem:
        case RouteCategory::JobsNsCollection:
        case RouteCategory::JobItem: {
            ResourceInfo info;
            resource_info(match.category, info);
            if ((req.method == HttpMethod::Post || req.method == HttpMethod::Put) && !req.body.empty()) {
                std::string why;
                if (!workload_template_ok(match.category, req.body, why)) {
                    return make_status(422, "Invalid", why);
                }
            }
            return handle_generic(req, match, info);
        }

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

        case RouteCategory::EndpointsCollection:
        case RouteCategory::EndpointsItem:
        case RouteCategory::EndpointSlicesCollection:
        case RouteCategory::EndpointSliceItem:
            return handle_endpoints(req, match);

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
            resp.body = version_json();
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
                        "{\"name\":\"networking.k8s.io\",\"versions\":[{\"groupVersion\":\"networking.k8s.io/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"networking.k8s.io/v1\",\"version\":\"v1\"}},"
                        "{\"name\":\"discovery.k8s.io\",\"versions\":[{\"groupVersion\":\"discovery.k8s.io/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"discovery.k8s.io/v1\",\"version\":\"v1\"}},"
                        "{\"name\":\"metrics.k8s.io\",\"versions\":[{\"groupVersion\":\"metrics.k8s.io/v1beta1\",\"version\":\"v1beta1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"metrics.k8s.io/v1beta1\",\"version\":\"v1beta1\"}},"
                        "{\"name\":\"kube32.io\",\"versions\":[{\"groupVersion\":\"kube32.io/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"kube32.io/v1\",\"version\":\"v1\"}}"
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
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"update\",\"patch\",\"delete\"]},"
                        "{\"name\":\"endpoints\",\"singularName\":\"endpoints\",\"namespaced\":true,\"kind\":\"Endpoints\","
                        "\"verbs\":[\"get\",\"list\"],\"shortNames\":[\"ep\"]}"
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

        case RouteCategory::DiscoveryApisNetworkingGroup:
            resp.body = "{\"kind\":\"APIGroup\",\"apiVersion\":\"v1\",\"name\":\"networking.k8s.io\","
                        "\"versions\":[{\"groupVersion\":\"networking.k8s.io/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"networking.k8s.io/v1\",\"version\":\"v1\"}}";
            break;

        case RouteCategory::DiscoveryApisNetworkingV1:
            resp.body = "{\"kind\":\"APIResourceList\",\"apiVersion\":\"v1\",\"groupVersion\":\"networking.k8s.io/v1\",\"resources\":["
                        "{\"name\":\"ingresses\",\"singularName\":\"ingress\",\"namespaced\":true,\"kind\":\"Ingress\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"update\",\"patch\",\"delete\"],\"shortNames\":[\"ing\"]},"
                        "{\"name\":\"networkpolicies\",\"singularName\":\"networkpolicy\",\"namespaced\":true,\"kind\":\"NetworkPolicy\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"update\",\"patch\",\"delete\"],\"shortNames\":[\"netpol\"]}"
                        "]}";
            break;

        case RouteCategory::DiscoveryApisDiscoveryGroup:
            resp.body = "{\"kind\":\"APIGroup\",\"apiVersion\":\"v1\",\"name\":\"discovery.k8s.io\","
                        "\"versions\":[{\"groupVersion\":\"discovery.k8s.io/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"discovery.k8s.io/v1\",\"version\":\"v1\"}}";
            break;

        case RouteCategory::DiscoveryApisDiscoveryV1:
            resp.body = "{\"kind\":\"APIResourceList\",\"apiVersion\":\"v1\",\"groupVersion\":\"discovery.k8s.io/v1\",\"resources\":["
                        "{\"name\":\"endpointslices\",\"singularName\":\"endpointslice\",\"namespaced\":true,\"kind\":\"EndpointSlice\","
                        "\"verbs\":[\"get\",\"list\"]}"
                        "]}";
            break;

        case RouteCategory::DiscoveryApisKube32Group:
            resp.body = "{\"kind\":\"APIGroup\",\"apiVersion\":\"v1\",\"name\":\"kube32.io\","
                        "\"versions\":[{\"groupVersion\":\"kube32.io/v1\",\"version\":\"v1\"}],"
                        "\"preferredVersion\":{\"groupVersion\":\"kube32.io/v1\",\"version\":\"v1\"}}";
            break;

        case RouteCategory::DiscoveryApisKube32V1:
            resp.body = "{\"kind\":\"APIResourceList\",\"apiVersion\":\"v1\",\"groupVersion\":\"kube32.io/v1\",\"resources\":["
                        "{\"name\":\"firmwarerollouts\",\"singularName\":\"firmwarerollout\",\"namespaced\":false,\"kind\":\"FirmwareRollout\","
                        "\"verbs\":[\"get\",\"list\",\"watch\",\"create\",\"update\",\"patch\",\"delete\"],\"shortNames\":[\"fwr\"]}"
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
            // Callers that reach this point are authenticated (or authentication
            // is off); kube32 has one role, which may do everything
            resp.body = std::string("{\"kind\":\"SelfSubjectAccessReview\",\"apiVersion\":\"authorization.k8s.io/v1\",\"status\":{\"allowed\":true,\"reason\":\"") +
                        (bearer_token_.empty() ? "authentication is off: every caller may do everything"
                                               : "the bearer token grants full access") + "\"}}";
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
            if (stream_list(client_sock)) {
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
            if (storage::Status ps = store_.put(key, node_json, false); !ps.ok()) return store_failure(ps, "nodes", node_name);
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
        models::JsonValue old_doc;
        if (models::parse_json(existing_json, old_doc, err)) carry_server_fields(old_doc, doc, true);
        const std::string updated = doc.to_string();
        if (storage::Status ps = store_.put(key, updated, true); !ps.ok()) return store_failure(ps, "nodes", name);
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = updated;
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
        models::apply_patch(target_doc, patch_doc, req.content_type);
        std::string updated_json = target_doc.to_string();
        if (storage::Status ps = store_.put(key, updated_json, true); !ps.ok()) return store_failure(ps, "nodes", name);

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

// ?inlineImages=omit (kubelets): the value of each kube32.io/wasm-base64
// annotation is left empty, so a list stays small on nodes with little RAM;
// the kubelet reads the module with the pod when it starts it.
static void omit_inline_images(const HttpRequest& req, std::vector<std::string>& pod_jsons) {
    std::string v;
    if (!query_param(req.query_string, "inlineImages", v) || v != "omit") return;
    static const std::string kKey = "\"kube32.io/wasm-base64\":\"";
    for (auto& j : pod_jsons) {
        size_t at = j.find(kKey);
        if (at == std::string::npos) continue;
        size_t start = at + kKey.size();
        size_t end = start;
        while (end < j.size() && j[end] != '"') end += (j[end] == '\\') ? 2 : 1;
        if (end < j.size()) j.erase(start, end - start);
    }
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
            omit_inline_images(req, pod_jsons);
            const uint64_t rv = store_.current_version();
            if (stream_list(client_sock)) {
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
            omit_inline_images(req, pod_jsons);
            const uint64_t rv = store_.current_version();
            if (stream_list(client_sock)) {
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

            // Read before stamping: adding top-level fields can move doc's
            // members and leave meta, spec and containers dangling
            const std::string explicit_node = spec->get_string("nodeName", "");

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
            // DefaultTolerationSeconds admission: 300 s on a not-ready or
            // unreachable node unless the pod sets its own toleration
            models::add_default_tolerations(*doc.get_mut("spec"));

            std::string pod_json = doc.to_string();

            // Run autonomous scheduler if pod does not already have an assigned node
            if (scheduler_ && !dry_run()) {
                if (explicit_node.empty() || explicit_node == "<none>") {
                    scheduler_->schedule_pod_manifest(pod_json);
                }
            }

            if (storage::Status ps = store_.put(key, pod_json, false); !ps.ok()) return store_failure(ps, "pods", pod_name);

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
            if (storage::Status ps = store_.put(key, updated, true); !ps.ok()) return store_failure(ps, "pods", name);
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = "application/json";
            resp.body = updated;
            return resp;
        }

        {
            models::JsonValue old_doc;
            if (models::parse_json(existing_json, old_doc, err)) carry_server_fields(old_doc, doc, true);
        }
        // Admission runs on updates too: a replaced spec keeps the default tolerations
        if (models::JsonValue* pspec = doc.get_mut("spec"); pspec && pspec->is_object()) models::add_default_tolerations(*pspec);
        const std::string updated_pod = doc.to_string();
        if (storage::Status ps = store_.put(key, updated_pod, true); !ps.ok()) return store_failure(ps, "pods", name);
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = updated_pod;
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
        models::apply_patch(target_doc, patch_doc, req.content_type);
        if (match.category != RouteCategory::PodStatus) {
            if (models::JsonValue* pspec = target_doc.get_mut("spec"); pspec && pspec->is_object()) models::add_default_tolerations(*pspec);
        }
        std::string updated_json = target_doc.to_string();
        if (storage::Status ps = store_.put(key, updated_json, true); !ps.ok()) return store_failure(ps, "pods", name);

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
            if (storage::Status ps = store_.put(key, updated, true); !ps.ok()) return store_failure(ps, "pods", name);
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = "application/json";
            resp.body = updated;
            return resp;
        }
        store_.remove(key);
        if (!dry_run()) metrics_.forget_pod(ns, name);
        // The log mirror stays readable for followers draining the last lines;
        // the store evicts it by LRU. Its files go with the pod.
        if (!dry_run()) runtime::PodLogStore::apiserver().remove_persisted(ns + "/" + name);
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
            if (stream_list(client_sock)) {
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
            if (storage::Status ps = store_.put(key, dep_json, false); !ps.ok()) return store_failure(ps, "deployments.apps", dep_name);

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
            const models::JsonValue old_doc = doc;
            dspec->set("replicas", jnum(static_cast<double>(replicas)));
            carry_server_fields(old_doc, doc, false);
            if (storage::Status ps = store_.put(key, doc.to_string(), true); !ps.ok()) return store_failure(ps, "deployments.apps", name);
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
        models::JsonValue old_doc;
        if (models::parse_json(existing_json, old_doc, err)) carry_server_fields(old_doc, doc, true);
        const std::string updated = doc.to_string();
        if (storage::Status ps = store_.put(key, updated, true); !ps.ok()) return store_failure(ps, "deployments.apps", name);
        HttpResponse resp;
        resp.status_code = 200;
        resp.content_type = "application/json";
        resp.body = updated;
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
        const models::JsonValue old_doc = target_doc;
        models::apply_patch(target_doc, patch_doc, req.content_type);
        if (match.category != RouteCategory::DeploymentStatus) carry_server_fields(old_doc, target_doc, false);
        std::string updated_json = target_doc.to_string();
        if (storage::Status ps = store_.put(key, updated_json, true); !ps.ok()) return store_failure(ps, "deployments.apps", name);

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

// A kubelet without a wall clock renews its lease without renewTime; the
// control plane stamps the renewal on arrival when its own clock is set.
static void fill_renew_time(models::JsonValue& lease) {
    models::JsonValue* spec = lease.get_mut("spec");
    if (!spec || !spec->is_object() || !spec->get_string("renewTime", "").empty()) return;
    std::string now = models::now_micro_time();
    if (!now.empty()) spec->set("renewTime", jstr(now));
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
            fill_renew_time(doc);
            std::string lease_json = doc.to_string();
            store_.put(key, lease_json, true);


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
            fill_renew_time(doc);
            std::string lease_json = doc.to_string();
            store_.put(key, lease_json, true);
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
            if (!store_.get(key, existing_json).ok()) {
                return make_status(404, "NotFound", "leases.coordination.k8s.io \"" + name + "\" not found", name, "leases");
            }
            models::JsonValue orig_doc, patch_doc;
            std::string err;
            if (!models::parse_json(req.body, patch_doc, err) || !patch_doc.is_object()) {
                return make_status(400, "BadRequest", "Failed to parse JSON: " + err);
            }
            if (!models::parse_json(existing_json, orig_doc, err) || !orig_doc.is_object()) {
                return make_status(500, "InternalError", "Failed to parse existing resource");
            }
            models::apply_patch(orig_doc, patch_doc, req.content_type);
            fill_renew_time(orig_doc);
            std::string final_json = orig_doc.to_string();
            store_.put(key, final_json, true);
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
        case RouteCategory::IngressesCollection: case RouteCategory::IngressItem:
            out = {"Ingress", "networking.k8s.io/v1", "IngressList", "ingresses", "/registry/ingresses/", true}; return true;
        case RouteCategory::NetworkPoliciesCollection: case RouteCategory::NetworkPolicyItem:
            out = {"NetworkPolicy", "networking.k8s.io/v1", "NetworkPolicyList", "networkpolicies", "/registry/networkpolicies/", true}; return true;
        case RouteCategory::FirmwareRolloutsCollection: case RouteCategory::FirmwareRolloutItem:
            out = {"FirmwareRollout", "kube32.io/v1", "FirmwareRolloutList", "firmwarerollouts", "/registry/firmwarerollouts/", false}; return true;
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
        {"apis/networking.k8s.io/v1", "networking.k8s.io", "v1", "Ingress", "ingresses", true},
        {"apis/networking.k8s.io/v1", "networking.k8s.io", "v1", "NetworkPolicy", "networkpolicies", true},
        {"apis/kube32.io/v1", "kube32.io", "v1", "FirmwareRollout", "firmwarerollouts", false},
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

bool RouteHandler::strict_fields_ok(RouteCategory category, std::string_view api_group, const std::string& body, std::string& out_unknown) const {
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
    static const char* const kEventV1[] = {"regarding", "related", "note", "reason", "type", "action", "reportingController",
                                           "reportingInstance", "eventTime", "series", "deprecatedSource",
                                           "deprecatedFirstTimestamp", "deprecatedLastTimestamp", "deprecatedCount"};
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
            else if (kind == "Event" && api_group == "events.k8s.io") ok = in(key, kEventV1, sizeof(kEventV1) / sizeof(kEventV1[0]));
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

    // limit / continue
    uint64_t limit = 0;
    query_uint(req.query_string, "limit", limit);
    std::string cont;
    query_param(req.query_string, "continue", cont);
    if (cont.empty() && (limit == 0 || jsons.size() <= limit)) return true;
    std::string start;
    if (!cont.empty()) {
        std::string raw;
        models::JsonValue tok;
        std::string terr;
        if (!base64url_decode(cont, raw) || !models::parse_json(raw, tok, terr) || !tok.is_object() || !tok.get("start")) {
            err_out = make_status(400, "BadRequest", "continue key is not valid");
            return false;
        }
        start = tok.get_string("start", "");
    }
    std::vector<std::pair<std::string, size_t>> order;
    order.reserve(jsons.size());
    for (size_t i = 0; i < jsons.size(); ++i) order.emplace_back(list_order_key(jsons[i]), i);
    std::sort(order.begin(), order.end());
    std::vector<std::string> page;
    size_t taken = 0, after = 0;
    std::string last;
    for (const auto& o : order) {
        if (!start.empty() && o.first <= start) continue;
        if (limit > 0 && taken >= limit) { ++after; continue; }
        page.push_back(std::move(jsons[o.second]));
        last = o.first;
        ++taken;
    }
    jsons.swap(page);
    if (after > 0 && t_request) {
        std::string raw = "{\"rv\":" + std::to_string(store_.current_version()) + ",\"start\":\"" + escape_json(last) + "\"}";
        t_request->continue_token = base64url_encode(raw);
        t_request->remaining = after;
    }
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
    if (doc.get("spec") && meta->get_int("generation", 0) < 1) meta->set("generation", jnum(1));
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
// Generic store-backed resources: namespaces, configmaps, secrets, services,
// ingresses, networkpolicies, daemonsets, jobs
// =============================================================================
// Pod templates of Jobs need restartPolicy Never or OnFailure, those of
// DaemonSets Always (or none), as kube-apiserver validates them.
bool RouteHandler::workload_template_ok(RouteCategory category, const std::string& body, std::string& out_why) const {
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(body, doc, err) || !doc.is_object()) return true;   // handle_generic reports the parse error
    const models::JsonValue* meta = doc.get("metadata");
    const std::string name = meta ? meta->get_string("name", "") : "";
    const models::JsonValue* spec = doc.get("spec");
    const models::JsonValue* tmpl = spec ? spec->get("template") : nullptr;
    const models::JsonValue* tspec = tmpl ? tmpl->get("spec") : nullptr;
    const std::string policy = tspec ? tspec->get_string("restartPolicy", "") : "";
    const bool job = (category == RouteCategory::JobsNsCollection || category == RouteCategory::JobItem);
    if (job && policy != "Never" && policy != "OnFailure") {
        out_why = "Job.batch \"" + name + "\" is invalid: spec.template.spec.restartPolicy: " +
                  (policy.empty() ? std::string("Required value") : "Unsupported value: \"" + policy + "\"") +
                  ": supported values: \"OnFailure\", \"Never\"";
        return false;
    }
    if (!job && !policy.empty() && policy != "Always") {
        out_why = "DaemonSet.apps \"" + name + "\" is invalid: spec.template.spec.restartPolicy: Unsupported value: \"" + policy +
                  "\": supported values: \"Always\"";
        return false;
    }
    return true;
}

// Service defaults kube-apiserver applies: type ClusterIP, sessionAffinity
// None, single-stack IPv4, and per port protocol TCP and targetPort = port.
// kube32 assigns no cluster IPs: clients reach pods through Ingress.
static void default_service(models::JsonValue& svc) {
    models::JsonValue* spec = svc.get_mut("spec");
    if (!spec || !spec->is_object()) return;
    if (spec->get_string("type", "").empty()) spec->set("type", jstr("ClusterIP"));
    if (spec->get_string("sessionAffinity", "").empty()) spec->set("sessionAffinity", jstr("None"));
    if (spec->get_string("internalTrafficPolicy", "").empty()) spec->set("internalTrafficPolicy", jstr("Cluster"));
    if (spec->get_string("ipFamilyPolicy", "").empty()) spec->set("ipFamilyPolicy", jstr("SingleStack"));
    if (!spec->get("ipFamilies")) {
        models::JsonValue fam;
        fam.type = models::JsonType::Array;
        fam.arr_val.push_back(jstr("IPv4"));
        spec->set("ipFamilies", std::move(fam));
    }
    if (models::JsonValue* ports = spec->get_mut("ports"); ports && ports->is_array()) {
        for (auto& p : ports->arr_val) {
            if (!p.is_object()) continue;
            if (p.get_string("protocol", "").empty()) p.set("protocol", jstr("TCP"));
            if (!p.get("targetPort") && p.get("port")) p.set("targetPort", *p.get("port"));
        }
    }
}

HttpResponse RouteHandler::handle_generic(const HttpRequest& req, const RouteMatch& match, const ResourceInfo& info) {
    const bool is_item = (match.category == RouteCategory::NamespaceItem ||
                          match.category == RouteCategory::ConfigMapItem ||
                          match.category == RouteCategory::SecretItem ||
                          match.category == RouteCategory::DaemonSetItem ||
                          match.category == RouteCategory::JobItem ||
                          match.category == RouteCategory::ServiceItem ||
                          match.category == RouteCategory::IngressItem ||
                          match.category == RouteCategory::NetworkPolicyItem ||
                          match.category == RouteCategory::FirmwareRolloutItem);
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
            if (match.category == RouteCategory::ServicesNsCollection) default_service(doc);
            if (match.category == RouteCategory::NetworkPoliciesCollection) {
                std::string why;
                if (!net::validate_network_policy(doc, why)) {
                    return make_status(422, "Invalid", "NetworkPolicy.networking.k8s.io \"" + name + "\" is invalid: " + why);
                }
                net::default_network_policy(doc);
            }
            if (match.category == RouteCategory::NamespacesCollection && !doc.get("status")) {
                models::JsonValue st;
                st.type = models::JsonType::Object;
                st.set("phase", jstr("Active"));
                doc.set("status", std::move(st));
            }
            std::string json = doc.to_string();
            storage::Status ps = store_.put(key, json, false);
            if (!ps.ok()) return store_failure(ps, plural, name);
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
        if (req.as_table && TableSerializer::has_generic_table(info.kind)) {
            // kubectl get <kind> <name> prints the same columns as a list
            return list_response(req, info, {existing}, std::string(), store_.current_version());
        }
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
        const models::JsonValue old_doc = target;
        if (req.method == HttpMethod::Patch) {
            models::apply_patch(target, in, req.content_type);
            carry_server_fields(old_doc, target, false);
        } else {
            target = std::move(in);
            carry_server_fields(old_doc, target, true);
        }
        if (match.category == RouteCategory::ServiceItem) default_service(target);
        if (match.category == RouteCategory::NetworkPolicyItem) {
            std::string why;
            if (!net::validate_network_policy(target, why)) {
                return make_status(422, "Invalid", "NetworkPolicy.networking.k8s.io \"" + name + "\" is invalid: " + why);
            }
            net::default_network_policy(target);
        }
        std::string updated = target.to_string();
        if (storage::Status ps = store_.put(key, updated, true); !ps.ok()) return store_failure(ps, plural, name);
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
// events.k8s.io/v1 -> core/v1, field for field (see EventRecorder::to_events_v1
// for the way back).
static std::string events_v1_to_core(const models::JsonValue& in) {
    models::JsonValue out;
    out.type = models::JsonType::Object;
    out.set("kind", jstr("Event"));
    out.set("apiVersion", jstr("v1"));
    if (const auto* m = in.get("metadata")) out.set("metadata", *m);
    if (const auto* r = in.get("regarding")) out.set("involvedObject", *r);
    if (const auto* r = in.get("related")) out.set("related", *r);
    out.set("reason", jstr(in.get_string("reason", "")));
    out.set("message", jstr(in.get_string("note", "")));
    out.set("type", jstr(in.get_string("type", "Normal")));
    if (!in.get_string("action", "").empty()) out.set("action", jstr(in.get_string("action", "")));
    out.set("reportingComponent", jstr(in.get_string("reportingController", "")));
    out.set("reportingInstance", jstr(in.get_string("reportingInstance", "")));
    const std::string event_time = in.get_string("eventTime", "");
    if (!event_time.empty()) out.set("eventTime", jstr(event_time));
    std::string first = in.get_string("deprecatedFirstTimestamp", event_time);
    std::string last = in.get_string("deprecatedLastTimestamp", "");
    int64_t count = in.get_int("deprecatedCount", 0);
    if (const auto* series = in.get("series"); series && series->is_object()) {
        out.set("series", *series);
        if (count == 0) count = series->get_int("count", 0);
        if (last.empty()) last = series->get_string("lastObservedTime", "");
    }
    if (!first.empty()) out.set("firstTimestamp", jstr(first));
    if (last.empty()) last = first;
    if (!last.empty()) out.set("lastTimestamp", jstr(last));
    if (count > 0) out.set("count", jnum(static_cast<double>(count)));
    if (const auto* src = in.get("deprecatedSource")) {
        out.set("source", *src);
    } else if (!in.get_string("reportingController", "").empty()) {
        models::JsonValue src_obj;
        src_obj.type = models::JsonType::Object;
        src_obj.set("component", jstr(in.get_string("reportingController", "")));
        out.set("source", std::move(src_obj));
    }
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
        if (req.method == HttpMethod::Patch) models::apply_patch(target, in, req.content_type); else target = std::move(in);
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
        // {"epoch":E,"seq":S,"t":<unix ms or 0>,"u":<uptime ms>,"r":<container instance>,"m":"text"}
        // Lines are kept per pod object (metadata.uid), so a pod recreated
        // under the same name starts with an empty log.
        std::string pod_uid, container_name;
        {
            models::JsonValue pd;
            std::string perr;
            if (models::parse_json(pod_json, pd, perr)) {
                const models::JsonValue* meta = pd.get("metadata");
                if (meta) pod_uid = meta->get_string("uid", "");
                const models::JsonValue* spec = pd.get("spec");
                const models::JsonValue* cs = spec ? spec->get("containers") : nullptr;
                if (cs && cs->is_array() && !cs->arr_val.empty()) container_name = cs->arr_val[0].get_string("name", "");
            }
        }
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
            if (logs.accept_shipped(pod_key, static_cast<uint64_t>(rec.get_int("epoch", 0)), r,
                                    static_cast<uint32_t>(rec.get_int("r", 0)), pod_uid, container_name)) ++accepted;
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
    // ?previous=true: the log of the container instance before the last
    // restart; it has ended, so it is never followed.
    const bool previous = query_flag(req.query_string, "previous");

    std::string cname = container;
    if (cname.empty()) {
        models::JsonValue pd;
        std::string perr;
        if (models::parse_json(pod_json, pd, perr)) {
            const models::JsonValue* spec = pd.get("spec");
            const models::JsonValue* cs = spec ? spec->get("containers") : nullptr;
            if (cs && cs->is_array() && !cs->arr_val.empty()) cname = cs->arr_val[0].get_string("name", "");
        }
    }
    std::vector<runtime::PodLogRecord> recs;
    bool have_ring = false;
    const size_t tail_count = has_tail ? static_cast<size_t>(tail) : 0;
    if (previous) {
        // The rotated file when the node keeps logs on disk, else the tail
        // kept in RAM
        if (!logs.read_persisted(pod_key, cname, true, tail_count, recs) && !logs.tail_previous(pod_key, tail_count, recs)) {
            return make_status(400, "BadRequest", "previous terminated container \"" + (cname.empty() ? name : cname) +
                               "\" in pod \"" + name + "\" not found");
        }
        have_ring = true;
    } else {
        have_ring = logs.tail(pod_key, tail_count, recs);
        // Nothing in RAM (the API server restarted): the file, when there is one
        if (!have_ring) have_ring = logs.read_persisted(pod_key, cname, false, tail_count, recs);
    }
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

    if (!follow || previous || req.method == HttpMethod::Head) {
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
// Endpoints (core/v1) and EndpointSlices (discovery.k8s.io/v1), computed from
// Services and their pods on every read: always current, nothing stored.
// =============================================================================
namespace {

struct EndpointPod {
    std::string ns, name, uid, ip, node;
    std::map<std::string, std::string> labels;
    std::map<std::string, int64_t> named_ports;
    bool ready{false};
    bool terminating{false};
};

struct ResolvedPort {
    std::string name;
    int64_t port{0};
    std::string protocol;
    bool operator<(const ResolvedPort& o) const {
        return name != o.name ? name < o.name : (port != o.port ? port < o.port : protocol < o.protocol);
    }
};

bool parse_endpoint_pod(const std::string& json, EndpointPod& out) {
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) return false;
    const models::JsonValue* meta = doc.get("metadata");
    const models::JsonValue* spec = doc.get("spec");
    const models::JsonValue* st = doc.get("status");
    if (!meta || !st) return false;
    out.ip = st->get_string("podIP", "");
    if (out.ip.empty()) return false;
    out.ns = meta->get_string("namespace", "default");
    out.name = meta->get_string("name", "");
    out.uid = meta->get_string("uid", "");
    out.terminating = !meta->get_string("deletionTimestamp", "").empty();
    if (const models::JsonValue* l = meta->get("labels"); l && l->is_object()) {
        for (const auto& kv : l->obj_val) if (kv.second.is_string()) out.labels[kv.first] = kv.second.str_val;
    }
    if (spec) {
        out.node = spec->get_string("nodeName", "");
        if (const models::JsonValue* cs = spec->get("containers"); cs && cs->is_array()) {
            for (const auto& c : cs->arr_val) {
                const models::JsonValue* ports = c.get("ports");
                if (!ports || !ports->is_array()) continue;
                for (const auto& p : ports->arr_val) {
                    std::string n = p.get_string("name", "");
                    if (!n.empty()) out.named_ports[n] = p.get_int("containerPort", 0);
                }
            }
        }
    }
    if (st->get_string("phase", "") == "Running") {
        if (const models::JsonValue* conds = st->get("conditions"); conds && conds->is_array()) {
            for (const auto& c : conds->arr_val) {
                if (c.get_string("type", "") == "Ready") out.ready = c.get_string("status", "") == "True";
            }
        }
    }
    return true;
}

models::JsonValue jobject() { models::JsonValue o; o.type = models::JsonType::Object; return o; }
models::JsonValue jarray() { models::JsonValue a; a.type = models::JsonType::Array; return a; }
models::JsonValue jboolean(bool v) { return models::JsonValue{models::JsonType::Boolean, v, 0, "", {}, {}}; }

models::JsonValue target_ref(const EndpointPod* p) {
    models::JsonValue ref = jobject();
    ref.set("kind", jstr("Pod"));
    ref.set("namespace", jstr(p->ns));
    ref.set("name", jstr(p->name));
    if (!p->uid.empty()) ref.set("uid", jstr(p->uid));
    return ref;
}

// One Endpoints object, or the EndpointSlices, of a Service. Pods whose
// named target ports resolve differently land in separate subsets/slices.
void build_service_endpoints(const models::JsonValue& svc, const std::vector<EndpointPod>& pods, bool slices,
                             const std::string& rv, std::vector<std::string>& out) {
    const models::JsonValue* meta = svc.get("metadata");
    const models::JsonValue* spec = svc.get("spec");
    if (!meta || !spec) return;
    const models::JsonValue* selector = spec->get("selector");
    if (!selector || !selector->is_object() || selector->obj_val.empty()) return;   // selectorless: managed by the user
    const std::string ns = meta->get_string("namespace", "default");
    const std::string name = meta->get_string("name", "");

    std::map<std::vector<ResolvedPort>, std::vector<const EndpointPod*>> groups;
    for (const auto& pod : pods) {
        if (pod.ns != ns) continue;
        bool selected = true;
        for (const auto& kv : selector->obj_val) {
            auto it = pod.labels.find(kv.first);
            if (it == pod.labels.end() || !kv.second.is_string() || it->second != kv.second.str_val) { selected = false; break; }
        }
        if (!selected) continue;
        std::vector<ResolvedPort> ports;
        if (const models::JsonValue* sp = spec->get("ports"); sp && sp->is_array()) {
            for (const auto& p : sp->arr_val) {
                ResolvedPort rp;
                rp.name = p.get_string("name", "");
                rp.protocol = p.get_string("protocol", "TCP");
                const models::JsonValue* tp = p.get("targetPort");
                if (tp && tp->is_string()) {
                    auto it = pod.named_ports.find(tp->str_val);
                    if (it == pod.named_ports.end()) continue;
                    rp.port = it->second;
                } else {
                    rp.port = tp ? p.get_int("targetPort", 0) : p.get_int("port", 0);
                }
                if (rp.port > 0) ports.push_back(rp);
            }
        }
        std::sort(ports.begin(), ports.end());
        groups[ports].push_back(&pod);
    }

    models::JsonValue labels = jobject();
    if (const models::JsonValue* l = meta->get("labels"); l && l->is_object()) labels = *l;
    auto base_meta = [&](const std::string& object_name) {
        models::JsonValue m = jobject();
        m.set("name", jstr(object_name));
        m.set("namespace", jstr(ns));
        m.set("resourceVersion", jstr(rv));
        std::string created = meta->get_string("creationTimestamp", "");
        if (!created.empty()) m.set("creationTimestamp", jstr(created));
        return m;
    };
    auto ports_json = [](const std::vector<ResolvedPort>& ports) {
        models::JsonValue arr = jarray();
        for (const auto& p : ports) {
            models::JsonValue o = jobject();
            if (!p.name.empty()) o.set("name", jstr(p.name));
            o.set("port", jnum(static_cast<double>(p.port)));
            o.set("protocol", jstr(p.protocol));
            arr.arr_val.push_back(std::move(o));
        }
        return arr;
    };

    if (!slices) {
        models::JsonValue ep = jobject();
        ep.set("kind", jstr("Endpoints"));
        ep.set("apiVersion", jstr("v1"));
        models::JsonValue m = base_meta(name);
        if (!labels.obj_val.empty()) m.set("labels", labels);
        ep.set("metadata", std::move(m));
        models::JsonValue subsets = jarray();
        for (const auto& g : groups) {
            models::JsonValue ready = jarray(), not_ready = jarray();
            for (const EndpointPod* p : g.second) {
                if (p->terminating) continue;
                models::JsonValue a = jobject();
                a.set("ip", jstr(p->ip));
                if (!p->node.empty()) a.set("nodeName", jstr(p->node));
                a.set("targetRef", target_ref(p));
                (p->ready ? ready : not_ready).arr_val.push_back(std::move(a));
            }
            if (ready.arr_val.empty() && not_ready.arr_val.empty()) continue;
            models::JsonValue sub = jobject();
            if (!ready.arr_val.empty()) sub.set("addresses", std::move(ready));
            if (!not_ready.arr_val.empty()) sub.set("notReadyAddresses", std::move(not_ready));
            sub.set("ports", ports_json(g.first));
            subsets.arr_val.push_back(std::move(sub));
        }
        if (!subsets.arr_val.empty()) ep.set("subsets", std::move(subsets));
        out.push_back(ep.to_string());
        return;
    }

    size_t index = 0;
    if (groups.empty()) groups[{}];   // a selected-but-empty Service still has a slice
    for (const auto& g : groups) {
        models::JsonValue sl = jobject();
        sl.set("kind", jstr("EndpointSlice"));
        sl.set("apiVersion", jstr("discovery.k8s.io/v1"));
        models::JsonValue m = base_meta(index == 0 ? name : name + "-" + std::to_string(index));
        models::JsonValue l = labels;
        l.set("kubernetes.io/service-name", jstr(name));
        l.set("endpointslice.kubernetes.io/managed-by", jstr("endpointslice-controller.k8s.io"));
        m.set("labels", std::move(l));
        std::string uid = meta->get_string("uid", "");
        if (!uid.empty()) {
            models::JsonValue refs = jarray();
            models::JsonValue ref = jobject();
            ref.set("apiVersion", jstr("v1"));
            ref.set("kind", jstr("Service"));
            ref.set("name", jstr(name));
            ref.set("uid", jstr(uid));
            ref.set("controller", jboolean(true));
            ref.set("blockOwnerDeletion", jboolean(true));
            refs.arr_val.push_back(std::move(ref));
            m.set("ownerReferences", std::move(refs));
        }
        sl.set("metadata", std::move(m));
        sl.set("addressType", jstr("IPv4"));
        models::JsonValue eps = jarray();
        for (const EndpointPod* p : g.second) {
            models::JsonValue e = jobject();
            models::JsonValue addrs = jarray();
            addrs.arr_val.push_back(jstr(p->ip));
            e.set("addresses", std::move(addrs));
            models::JsonValue c = jobject();
            c.set("ready", jboolean(p->ready && !p->terminating));
            c.set("serving", jboolean(p->ready));
            c.set("terminating", jboolean(p->terminating));
            e.set("conditions", std::move(c));
            if (!p->node.empty()) e.set("nodeName", jstr(p->node));
            e.set("targetRef", target_ref(p));
            eps.arr_val.push_back(std::move(e));
        }
        sl.set("endpoints", std::move(eps));
        sl.set("ports", ports_json(g.first));
        out.push_back(sl.to_string());
        ++index;
    }
}

} // namespace

HttpResponse RouteHandler::handle_endpoints(const HttpRequest& req, const RouteMatch& match) {
    const bool slices = (match.category == RouteCategory::EndpointSlicesCollection || match.category == RouteCategory::EndpointSliceItem);
    const bool item = (match.category == RouteCategory::EndpointsItem || match.category == RouteCategory::EndpointSliceItem);
    ResourceInfo info = slices ? ResourceInfo{"EndpointSlice", "discovery.k8s.io/v1", "EndpointSliceList", "endpointslices", "", true}
                               : ResourceInfo{"Endpoints", "v1", "EndpointsList", "endpoints", "", true};
    const std::string plural = slices ? "endpointslices.discovery.k8s.io" : "endpoints";
    if (req.method != HttpMethod::Get && req.method != HttpMethod::Head) {
        return make_status(405, "MethodNotAllowed", plural + " are derived from Services and their pods and are read-only");
    }
    if (query_flag(req.query_string, "watch")) {
        return make_status(405, "MethodNotAllowed", "watch is not supported for " + plural);
    }
    const std::string ns(match.namespace_name);
    const std::string scope = ns.empty() ? std::string() : ns + "/";
    std::vector<std::string> svc_jsons, pod_jsons;
    store_.list("/registry/services/" + scope, svc_jsons);
    store_.list("/registry/pods/" + scope, pod_jsons);
    std::vector<EndpointPod> pods;
    pods.reserve(pod_jsons.size());
    for (const auto& j : pod_jsons) {
        EndpointPod p;
        if (parse_endpoint_pod(j, p)) pods.push_back(std::move(p));
    }
    std::sort(pods.begin(), pods.end(), [](const EndpointPod& a, const EndpointPod& b) {
        return a.ns != b.ns ? a.ns < b.ns : a.name < b.name;
    });
    const std::string rv = std::to_string(store_.current_version());
    std::vector<std::string> objects;
    for (const auto& sj : svc_jsons) {
        models::JsonValue svc;
        std::string err;
        if (!models::parse_json(sj, svc, err) || !svc.is_object()) continue;
        if (item) {
            const models::JsonValue* m = svc.get("metadata");
            const std::string sname = m ? m->get_string("name", "") : "";
            const std::string want(match.resource_name);
            // A slice is named after its Service, with "-<n>" for extra slices
            if (!(sname == want || (slices && want.rfind(sname + "-", 0) == 0))) continue;
        }
        build_service_endpoints(svc, pods, slices, rv, objects);
    }

    if (item) {
        const std::string want(match.resource_name);
        for (const auto& o : objects) {
            models::JsonValue doc;
            std::string err;
            if (!models::parse_json(o, doc, err)) continue;
            const models::JsonValue* m = doc.get("metadata");
            if (!m || m->get_string("name", "") != want) continue;
            if (req.as_table && TableSerializer::has_generic_table(info.kind)) {
                return list_response(req, info, {o}, std::string(), store_.current_version());
            }
            HttpResponse resp;
            resp.status_code = 200;
            resp.content_type = "application/json";
            resp.body = (req.method == HttpMethod::Head) ? "" : o;
            return resp;
        }
        return make_status(404, "NotFound", plural + " \"" + want + "\" not found", want, slices ? "endpointslices" : "endpoints");
    }
    HttpResponse sel_err;
    if (!filter_list(req, objects, sel_err)) return sel_err;
    return list_response(req, info, objects, list_json(info, objects), store_.current_version());
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
