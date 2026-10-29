#include "app/ingress_routes.hpp"
#include "ipc/channel.hpp"
#include "models/kube_models.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

namespace kube32::app {

std::string normalize_host(std::string_view host_header) {
    std::string_view h = host_header;
    while (!h.empty() && h.front() == ' ') h.remove_prefix(1);
    while (!h.empty() && h.back() == ' ') h.remove_suffix(1);
    if (!h.empty() && h.front() == '[') {
        size_t close = h.find(']');
        h = (close == std::string_view::npos) ? h : h.substr(0, close + 1);
    } else {
        size_t colon = h.rfind(':');
        if (colon != std::string_view::npos) h = h.substr(0, colon);
    }
    if (!h.empty() && h.back() == '.') h.remove_suffix(1);
    std::string out(h);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool path_matches(PathMatch match, std::string_view rule_path, std::string_view path) {
    if (match == PathMatch::Exact) return path == rule_path;
    if (rule_path == "/") return !path.empty() && path.front() == '/';
    if (path.size() < rule_path.size() || path.compare(0, rule_path.size(), rule_path) != 0) return false;
    return path.size() == rule_path.size() || path[rule_path.size()] == '/';
}

static bool host_matches_wildcard(std::string_view pattern, std::string_view host) {
    // "*.example.com" matches "a.example.com", not "example.com" or "a.b.example.com"
    std::string_view suffix = pattern.substr(1);   // ".example.com"
    if (host.size() <= suffix.size()) return false;
    if (host.compare(host.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    std::string_view label = host.substr(0, host.size() - suffix.size());
    return !label.empty() && label.find('.') == std::string_view::npos;
}

const IngressRoute* RouteTable::match(std::string_view host_header, std::string_view path) const {
    const std::string host = normalize_host(host_header);
    // 0: exact host, 1: wildcard host, 2: any host
    auto group_of = [&](const IngressRoute& r) -> int {
        if (r.host.empty()) return 2;
        if (r.host[0] == '*') return host_matches_wildcard(r.host, host) ? 1 : -1;
        return r.host == host ? 0 : -1;
    };
    int best_group = 3;
    for (const auto& r : routes) {
        int g = group_of(r);
        if (g >= 0 && g < best_group) best_group = g;
    }
    const IngressRoute* best = nullptr;
    for (const auto& r : routes) {
        if (group_of(r) != best_group) continue;
        if (!path_matches(r.match, r.path, path)) continue;
        if (!best || r.path.size() > best->path.size() ||
            (r.path.size() == best->path.size() && r.match == PathMatch::Exact && best->match == PathMatch::Prefix)) {
            best = &r;
        }
    }
    if (best) return best;
    return has_default ? &default_backend : nullptr;
}

std::string bundle_relative_path(const IngressRoute& route, std::string_view request_path) {
    std::string_view rest = request_path;
    if (route.match == PathMatch::Prefix && route.path != "/" && rest.size() >= route.path.size()) {
        rest = rest.substr(route.path.size());
    }
    if (rest.empty() || rest.front() != '/') return "/" + std::string(rest);
    return std::string(rest);
}

// ---------------------------------------------------------------------------
// Table construction
// ---------------------------------------------------------------------------
namespace {

struct ServiceInfo {
    std::string ns;
    std::string name;
    std::vector<std::pair<std::string, std::string>> selector;
    struct Port {
        std::string name;
        int64_t port{0};
        int64_t target_number{0};     // 0 when targetPort is a name or absent
        std::string target_name;
    };
    std::vector<Port> ports;
};

struct PodInfo {
    std::string ns;
    std::string name;
    std::string ip;
    std::map<std::string, std::string> labels;
    std::vector<std::pair<std::string, int64_t>> named_ports;   // containerPort names
};

std::string trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
    return std::string(s);
}

std::string annotation(const models::JsonValue* meta, const char* key) {
    if (!meta) return {};
    const models::JsonValue* ann = meta->get("annotations");
    if (!ann || !ann->is_object()) return {};
    const models::JsonValue* v = ann->get(key);
    return (v && v->is_string()) ? v->str_val : std::string();
}

bool parse_service(const std::string& json, ServiceInfo& out) {
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) return false;
    const models::JsonValue* meta = doc.get("metadata");
    if (!meta) return false;
    out.ns = meta->get_string("namespace", "default");
    out.name = meta->get_string("name", "");
    const models::JsonValue* spec = doc.get("spec");
    if (!spec) return !out.name.empty();
    if (const models::JsonValue* sel = spec->get("selector"); sel && sel->is_object()) {
        for (const auto& kv : sel->obj_val) {
            if (kv.second.is_string()) out.selector.emplace_back(kv.first, kv.second.str_val);
        }
    }
    if (const models::JsonValue* ports = spec->get("ports"); ports && ports->is_array()) {
        for (const auto& p : ports->arr_val) {
            ServiceInfo::Port sp;
            sp.name = p.get_string("name", "");
            sp.port = p.get_int("port", 0);
            const models::JsonValue* tp = p.get("targetPort");
            if (tp && tp->is_number()) sp.target_number = static_cast<int64_t>(tp->num_val);
            else if (tp && tp->is_string()) {
                char* end = nullptr;
                long v = std::strtol(tp->str_val.c_str(), &end, 10);
                if (end && *end == '\0' && !tp->str_val.empty()) sp.target_number = v;
                else sp.target_name = tp->str_val;
            }
            if (sp.target_number == 0 && sp.target_name.empty()) sp.target_number = sp.port;
            out.ports.push_back(std::move(sp));
        }
    }
    return !out.name.empty();
}

// Running pods only: a pod that is not Running (or is terminating) gets no traffic.
bool parse_running_pod(const std::string& json, PodInfo& out) {
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) return false;
    const models::JsonValue* meta = doc.get("metadata");
    const models::JsonValue* status = doc.get("status");
    if (!meta || !status) return false;
    if (!meta->get_string("deletionTimestamp", "").empty()) return false;
    if (status->get_string("phase", "") != "Running") return false;
    out.ns = meta->get_string("namespace", "default");
    out.name = meta->get_string("name", "");
    out.ip = status->get_string("podIP", "");
    if (const models::JsonValue* labels = meta->get("labels"); labels && labels->is_object()) {
        for (const auto& kv : labels->obj_val) {
            if (kv.second.is_string()) out.labels[kv.first] = kv.second.str_val;
        }
    }
    if (const models::JsonValue* spec = doc.get("spec")) {
        if (const models::JsonValue* cs = spec->get("containers"); cs && cs->is_array()) {
            for (const auto& c : cs->arr_val) {
                const models::JsonValue* ports = c.get("ports");
                if (!ports || !ports->is_array()) continue;
                for (const auto& p : ports->arr_val) {
                    std::string pname = p.get_string("name", "");
                    if (!pname.empty()) out.named_ports.emplace_back(pname, p.get_int("containerPort", 0));
                }
            }
        }
    }
    return !out.name.empty();
}

struct BackendRef {
    bool is_service{false};
    std::string service_name;
    int64_t port_number{0};
    std::string port_name;
    bool is_bundle{false};
    std::string bundle;
    std::string unsupported;   // non-empty: a resource kind this controller does not serve
};

bool parse_backend(const models::JsonValue* b, BackendRef& out) {
    if (!b || !b->is_object()) return false;
    if (const models::JsonValue* svc = b->get("service"); svc && svc->is_object()) {
        out.is_service = true;
        out.service_name = svc->get_string("name", "");
        if (const models::JsonValue* port = svc->get("port"); port && port->is_object()) {
            out.port_number = port->get_int("number", 0);
            out.port_name = port->get_string("name", "");
        }
        return !out.service_name.empty();
    }
    if (const models::JsonValue* res = b->get("resource"); res && res->is_object()) {
        std::string group = res->get_string("apiGroup", "");
        std::string kind = res->get_string("kind", "");
        if (group == kStaticBundleGroup && kind == kStaticBundleKind) {
            out.is_bundle = true;
            out.bundle = res->get_string("name", "");
            return !out.bundle.empty();
        }
        out.unsupported = group + "/" + kind;
        return false;
    }
    return false;
}

void resolve_service(IngressRoute& route, const std::string& ns, const BackendRef& ref,
                     const std::vector<ServiceInfo>& services, const std::vector<PodInfo>& pods) {
    route.kind = BackendKind::Service;
    route.service = ns + "/" + ref.service_name;
    const ServiceInfo* svc = nullptr;
    for (const auto& s : services) {
        if (s.ns == ns && s.name == ref.service_name) { svc = &s; break; }
    }
    if (!svc) { route.unavailable = "service " + route.service + " not found"; return; }
    const ServiceInfo::Port* port = nullptr;
    for (const auto& p : svc->ports) {
        if ((ref.port_number && p.port == ref.port_number) || (!ref.port_name.empty() && p.name == ref.port_name)) { port = &p; break; }
    }
    if (!port && !ref.port_number && ref.port_name.empty() && svc->ports.size() == 1) port = &svc->ports[0];
    if (!port) { route.unavailable = "service " + route.service + " has no matching port"; return; }
    if (svc->selector.empty()) { route.unavailable = "service " + route.service + " has no selector"; return; }
    for (const auto& pod : pods) {
        if (pod.ns != ns) continue;
        bool selected = true;
        for (const auto& kv : svc->selector) {
            auto it = pod.labels.find(kv.first);
            if (it == pod.labels.end() || it->second != kv.second) { selected = false; break; }
        }
        if (!selected) continue;
        int64_t target = port->target_number;
        if (!port->target_name.empty()) {
            for (const auto& np : pod.named_ports) {
                if (np.first == port->target_name) { target = np.second; break; }
            }
        }
        if (target <= 0 || target > 65535) continue;
        IngressEndpoint ep;
        ep.pod = pod.ns + "/" + pod.name;
        ep.ip = pod.ip;
        ep.port = static_cast<uint16_t>(target);
        route.endpoints.push_back(std::move(ep));
    }
    if (route.endpoints.empty()) route.unavailable = "no running pod selected by service " + route.service;
}

// "path=topic,topic;path2=topic" (';' or newlines between streams)
std::vector<std::pair<std::string, std::vector<std::string>>> parse_event_streams(const std::string& value) {
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find_first_of(";\n", start);
        if (end == std::string::npos) end = value.size();
        std::string item = trim(std::string_view(value).substr(start, end - start));
        start = end + 1;
        size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        std::string path = trim(std::string_view(item).substr(0, eq));
        if (path.empty() || path[0] != '/') continue;
        std::vector<std::string> topics;
        std::string_view rest = std::string_view(item).substr(eq + 1);
        size_t ts = 0;
        while (ts <= rest.size()) {
            size_t te = rest.find(',', ts);
            if (te == std::string_view::npos) te = rest.size();
            std::string t = trim(rest.substr(ts, te - ts));
            if (!t.empty()) topics.push_back(t);
            ts = te + 1;
        }
        if (!topics.empty()) out.emplace_back(std::move(path), std::move(topics));
    }
    return out;
}

std::string clean_prefix(std::string path) {
    if (path.empty() || path[0] != '/') path.insert(path.begin(), '/');
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

} // namespace

bool resolve_service_endpoints(const std::string& ns, const std::string& name, uint16_t port,
                               const std::vector<std::string>& service_jsons, const std::vector<std::string>& pod_jsons,
                               std::vector<IngressEndpoint>& out, std::string& why) {
    std::vector<ServiceInfo> services;
    for (const auto& j : service_jsons) {
        ServiceInfo s;
        if (parse_service(j, s) && s.ns == ns && s.name == name) { services.push_back(std::move(s)); break; }
    }
    BackendRef ref;
    ref.is_service = true;
    ref.service_name = name;
    ref.port_number = port;
    if (port == 0 && !services.empty() && !services[0].ports.empty()) ref.port_number = services[0].ports[0].port;
    std::vector<PodInfo> pods;
    for (const auto& j : pod_jsons) {
        PodInfo p;
        if (parse_running_pod(j, p) && p.ns == ns) pods.push_back(std::move(p));
    }
    std::sort(pods.begin(), pods.end(), [](const PodInfo& a, const PodInfo& b) { return a.name < b.name; });
    IngressRoute route;
    resolve_service(route, ns, ref, services, pods);
    out = std::move(route.endpoints);
    why = route.unavailable;
    return !out.empty();
}

std::shared_ptr<RouteTable> build_route_table(const std::vector<std::string>& ingress_jsons,
                                              const std::vector<std::string>& service_jsons,
                                              const std::vector<std::string>& pod_jsons,
                                              const RouteTable* previous) {
    auto table = std::make_shared<RouteTable>();

    std::vector<ServiceInfo> services;
    services.reserve(service_jsons.size());
    for (const auto& j : service_jsons) {
        ServiceInfo s;
        if (parse_service(j, s)) services.push_back(std::move(s));
    }
    std::vector<PodInfo> pods;
    pods.reserve(pod_jsons.size());
    for (const auto& j : pod_jsons) {
        PodInfo p;
        if (parse_running_pod(j, p)) pods.push_back(std::move(p));
    }
    // Endpoint order does not depend on the store's iteration order
    std::sort(pods.begin(), pods.end(), [](const PodInfo& a, const PodInfo& b) {
        return a.ns != b.ns ? a.ns < b.ns : a.name < b.name;
    });

    // Ingresses in namespace/name order: on identical host+path the first wins
    struct Parsed { std::string key; models::JsonValue doc; };
    std::vector<Parsed> ingresses;
    for (const auto& j : ingress_jsons) {
        Parsed p;
        std::string err;
        if (!models::parse_json(j, p.doc, err) || !p.doc.is_object()) continue;
        const models::JsonValue* meta = p.doc.get("metadata");
        if (!meta) continue;
        p.key = meta->get_string("namespace", "default") + "/" + meta->get_string("name", "");
        ingresses.push_back(std::move(p));
    }
    std::sort(ingresses.begin(), ingresses.end(), [](const Parsed& a, const Parsed& b) { return a.key < b.key; });

    for (const auto& ing : ingresses) {
        const models::JsonValue* meta = ing.doc.get("metadata");
        const models::JsonValue* spec = ing.doc.get("spec");
        std::string cls = spec ? spec->get_string("ingressClassName", "") : "";
        if (!cls.empty() && cls != kIngressClassName) continue;
        const std::string ns = meta->get_string("namespace", "default");

        IngressInfo info;
        info.key = ing.key;
        info.stats_topic = trim(annotation(meta, "kube32.io/stats-topic"));
        if (const models::JsonValue* st = ing.doc.get("status")) {
            if (const models::JsonValue* lb = st->get("loadBalancer")) {
                const models::JsonValue* list = lb->get("ingress");
                if (list && list->is_array() && !list->arr_val.empty()) info.address_ip = list->arr_val[0].get_string("ip", "");
            }
        }
        if (previous) {
            for (const auto& old : previous->ingresses) {
                if (old.key == info.key) { info.counters = old.counters; break; }
            }
        }
        if (!info.counters) info.counters = std::make_shared<IngressCounters>();
        const uint16_t index = static_cast<uint16_t>(table->ingresses.size());
        table->ingresses.push_back(std::move(info));

        std::vector<std::string> hosts;
        bool any_host = false;
        auto backend_route = [&](const std::string& host, const std::string& path, PathMatch match, const BackendRef& ref) {
            IngressRoute r;
            r.host = host;
            r.path = path;
            r.match = match;
            r.ingress = index;
            if (ref.is_bundle) {
                r.kind = BackendKind::StaticBundle;
                r.bundle = ref.bundle;
            } else {
                resolve_service(r, ns, ref, services, pods);
            }
            return r;
        };

        const models::JsonValue* rules = spec ? spec->get("rules") : nullptr;
        if (rules && rules->is_array()) {
            for (const auto& rule : rules->arr_val) {
                std::string host = normalize_host(rule.get_string("host", ""));
                if (host.empty()) any_host = true;
                else if (std::find(hosts.begin(), hosts.end(), host) == hosts.end()) hosts.push_back(host);
                const models::JsonValue* http = rule.get("http");
                const models::JsonValue* paths = http ? http->get("paths") : nullptr;
                if (!paths || !paths->is_array()) continue;
                for (const auto& p : paths->arr_val) {
                    BackendRef ref;
                    if (!parse_backend(p.get("backend"), ref)) continue;
                    std::string type = p.get_string("pathType", "Prefix");
                    PathMatch match = (type == "Exact") ? PathMatch::Exact : PathMatch::Prefix;
                    std::string path = p.get_string("path", "/");
                    if (match == PathMatch::Prefix) path = clean_prefix(path);
                    else if (path.empty() || path[0] != '/') continue;
                    table->routes.push_back(backend_route(host, path, match, ref));
                }
            }
        }
        if (hosts.empty()) any_host = true;

        if (spec && !table->has_default) {
            BackendRef ref;
            if (parse_backend(spec->get("defaultBackend"), ref)) {
                table->default_backend = backend_route("", "/", PathMatch::Prefix, ref);
                table->has_default = true;
            }
        }

        std::vector<std::string> route_hosts = hosts;
        if (any_host) route_hosts.push_back(std::string());
        auto add_annotation_route = [&](const std::string& path, BackendKind kind, const std::vector<std::string>& topics) {
            uint32_t stream_id = 0;
            if (kind == BackendKind::EventStream) {
                std::string ident = ing.key + "|" + path;
                for (const auto& t : topics) ident += "|" + t;
                stream_id = ipc::fnv1a_32(ident);
                if (stream_id == 0) stream_id = 1;
            }
            for (const auto& h : route_hosts) {
                IngressRoute r;
                r.host = h;
                r.path = path;
                r.match = PathMatch::Exact;
                r.kind = kind;
                r.ingress = index;
                r.topics = topics;
                r.stream_id = stream_id;
                table->routes.push_back(std::move(r));
            }
        };
        for (auto& stream : parse_event_streams(annotation(meta, "kube32.io/event-stream"))) {
            for (const auto& t : stream.second) {
                if (t.rfind("kube32.", 0) == 0) continue;
                if (std::find(table->channel_topics.begin(), table->channel_topics.end(), t) == table->channel_topics.end()) {
                    table->channel_topics.push_back(t);
                }
            }
            add_annotation_route(stream.first, BackendKind::EventStream, stream.second);
        }
        std::string telemetry = trim(annotation(meta, "kube32.io/cluster-telemetry"));
        if (!telemetry.empty() && telemetry[0] == '/') add_annotation_route(telemetry, BackendKind::Telemetry, {});
    }

    // Identical host+path+type in two Ingresses: the first (namespace/name order) wins
    std::vector<IngressRoute> unique;
    unique.reserve(table->routes.size());
    for (auto& r : table->routes) {
        bool dup = false;
        for (const auto& u : unique) {
            if (u.host == r.host && u.path == r.path && u.match == r.match) { dup = true; break; }
        }
        if (!dup) unique.push_back(std::move(r));
    }
    table->routes = std::move(unique);
    return table;
}

} // namespace kube32::app
