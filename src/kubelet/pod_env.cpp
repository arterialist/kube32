#include "kubelet/pod_env.hpp"
#include "models/kube_models.hpp"
#include "runtime/image_registry.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace kube32::kubelet {

namespace {

using models::JsonValue;

// [-._a-zA-Z][-._a-zA-Z0-9]*, the names Kubernetes accepts for variables
bool valid_env_name(const std::string& s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const bool ok = std::isalpha(c) || c == '-' || c == '.' || c == '_' || (i > 0 && std::isdigit(c));
        if (!ok) return false;
    }
    return true;
}

void set_var(EnvVars& vars, const std::string& name, const std::string& value) {
    for (auto& kv : vars) {
        if (kv.first == name) { kv.second = value; return; }
    }
    vars.emplace_back(name, value);
}

const std::string* find_var(const EnvVars& vars, const std::string& name) {
    for (const auto& kv : vars) {
        if (kv.first == name) return &kv.second;
    }
    return nullptr;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool labels_match(const JsonValue* labels, const std::vector<std::pair<std::string, std::string>>& selector) {
    for (const auto& kv : selector) {
        const JsonValue* v = (labels && labels->is_object()) ? labels->get(kv.first) : nullptr;
        if (!v || !v->is_string() || v->str_val != kv.second) return false;
    }
    return true;
}

// containerPort of the port named `name` in any container of the pod spec
int64_t named_container_port(const JsonValue* spec, const std::string& name) {
    const JsonValue* cs = spec ? spec->get("containers") : nullptr;
    if (!cs || !cs->is_array()) return 0;
    for (const auto& c : cs->arr_val) {
        const JsonValue* ports = c.get("ports");
        if (!ports || !ports->is_array()) continue;
        for (const auto& p : ports->arr_val) {
            if (p.get_string("name", "") == name) return p.get_int("containerPort", 0);
        }
    }
    return 0;
}

struct ResolvedPort {
    std::string name;
    std::string protocol;   // TCP, UDP, SCTP
    int64_t port{0};        // the target port the backing pod answers on
};

// Target port of a Service port on a pod with spec `pod_spec`; 0 when the
// named port does not exist there.
int64_t target_port(const JsonValue& svc_port, const JsonValue* pod_spec) {
    const JsonValue* tp = svc_port.get("targetPort");
    if (!tp || tp->is_null()) return svc_port.get_int("port", 0);
    if (tp->is_number()) return static_cast<int64_t>(tp->num_val);
    if (tp->is_string()) {
        bool numeric = !tp->str_val.empty();
        for (char c : tp->str_val) numeric = numeric && std::isdigit(static_cast<unsigned char>(c));
        if (numeric) return tp->str_val.size() <= 5 ? std::stoll(tp->str_val) : 0;
        return named_container_port(pod_spec, tp->str_val);
    }
    return 0;
}

void add_service_links(EnvVars& vars, const std::string& service, const std::string& host, const std::vector<ResolvedPort>& ports) {
    if (ports.empty() || host.empty()) return;
    const std::string n = service_env_name(service);
    set_var(vars, n + "_SERVICE_HOST", host);
    set_var(vars, n + "_SERVICE_PORT", std::to_string(ports[0].port));
    for (const auto& p : ports) {
        if (!p.name.empty()) set_var(vars, n + "_SERVICE_PORT_" + service_env_name(p.name), std::to_string(p.port));
    }
    set_var(vars, n + "_PORT", lower(ports[0].protocol) + "://" + host + ":" + std::to_string(ports[0].port));
    for (const auto& p : ports) {
        const std::string proto = lower(p.protocol);
        const std::string prefix = n + "_PORT_" + std::to_string(p.port) + "_" + upper(p.protocol);
        set_var(vars, prefix, proto + "://" + host + ":" + std::to_string(p.port));
        set_var(vars, prefix + "_PROTO", proto);
        set_var(vars, prefix + "_PORT", std::to_string(p.port));
        set_var(vars, prefix + "_ADDR", host);
    }
}

class EnvBuilder {
public:
    EnvBuilder(const PodEnvContext& ctx, std::string ns) : ctx_(ctx), ns_(std::move(ns)) {}

    // The data of a ConfigMap or Secret (Secret values base64-decoded).
    // Returns 1 found, 0 not found, -1 unreadable (error set).
    int object_data(bool secret, const std::string& name, const std::map<std::string, std::string>*& out, std::string& err) {
        auto& cache = secret ? secrets_ : configmaps_;
        auto it = cache.find(name);
        if (it == cache.end()) {
            Fetched f;
            std::string body;
            const std::string kind = secret ? "secrets" : "configmaps";
            int status = ctx_.api_get ? ctx_.api_get("/api/v1/namespaces/" + ns_ + "/" + kind + "/" + name, body) : 0;
            if (status == 404) {
                f.found = false;
            } else if (status != 200) {
                err = "cannot read " + std::string(secret ? "secret" : "configmap") + " \"" + name + "\" (" +
                      (status == 0 ? std::string("API server unreachable") : "HTTP " + std::to_string(status)) + ")";
                return -1;
            } else {
                JsonValue doc;
                std::string perr;
                if (!models::parse_json(body, doc, perr)) {
                    err = "cannot parse " + std::string(secret ? "secret" : "configmap") + " \"" + name + "\": " + perr;
                    return -1;
                }
                f.found = true;
                if (const JsonValue* data = doc.get("data"); data && data->is_object()) {
                    for (const auto& kv : data->obj_val) {
                        if (!kv.second.is_string()) continue;
                        if (secret) {
                            std::vector<uint8_t> raw;
                            if (!runtime::base64_decode(kv.second.str_val, raw)) {
                                err = "secret \"" + name + "\" key " + kv.first + " is not valid base64";
                                return -1;
                            }
                            f.data[kv.first].assign(raw.begin(), raw.end());
                        } else {
                            f.data[kv.first] = kv.second.str_val;
                        }
                    }
                }
                // stringData as written, for Secrets stored without conversion
                if (const JsonValue* sd = secret ? doc.get("stringData") : nullptr; sd && sd->is_object()) {
                    for (const auto& kv : sd->obj_val) {
                        if (kv.second.is_string() && !f.data.count(kv.first)) f.data[kv.first] = kv.second.str_val;
                    }
                }
            }
            it = cache.emplace(name, std::move(f)).first;
        }
        if (!it->second.found) return 0;
        out = &it->second.data;
        return 1;
    }

private:
    struct Fetched {
        bool found{false};
        std::map<std::string, std::string> data;
    };
    const PodEnvContext& ctx_;
    std::string ns_;
    std::map<std::string, Fetched> configmaps_;
    std::map<std::string, Fetched> secrets_;
};

// Calls `fn` with each item of the list at `path`. A context that streams
// lists hands the items over one at a time, so only one item is ever parsed;
// otherwise the list is read whole. False when the list cannot be read.
bool for_each_item(const PodEnvContext& ctx, const std::string& path, const std::function<void(const JsonValue&)>& fn) {
    if (ctx.api_list) {
        return ctx.api_list(path, [&](std::string&& text) {
            JsonValue item;
            std::string err;
            const bool parsed = models::parse_json(text, item, err);
            std::string().swap(text);
            if (parsed && item.is_object()) fn(item);
        }) == 200;
    }
    if (!ctx.api_get) return false;
    std::string body;
    if (ctx.api_get(path, body) != 200) return false;
    JsonValue list;
    std::string err;
    if (!models::parse_json(body, list, err)) return false;
    const JsonValue* items = list.get("items");
    if (items && items->is_array()) {
        for (const auto& item : items->arr_val) fn(item);
    }
    return true;
}

// What a Service's link variables are made of
struct LinkedService {
    std::string name;
    bool selects_pod{false};          // the starting pod is one of its backends
    std::string host;
    std::vector<ResolvedPort> ports;  // in the Service's order; port 0 until resolved
};

// Service link variables for the namespace's Services. A Service selecting
// the starting pod points at this node; any other at the first ready address
// of its Endpoints, with the target ports the Endpoints resolved.
void collect_service_links(const PodEnvContext& ctx, const std::string& ns, const JsonValue& pod, EnvVars& out) {
    const JsonValue* own_meta = pod.get("metadata");
    const JsonValue* own_labels = own_meta ? own_meta->get("labels") : nullptr;
    const JsonValue* own_spec = pod.get("spec");

    std::vector<LinkedService> services;
    bool others = false;
    if (!for_each_item(ctx, "/api/v1/namespaces/" + ns + "/services", [&](const JsonValue& svc) {
        const JsonValue* meta = svc.get("metadata");
        const JsonValue* spec = svc.get("spec");
        if (!meta || !spec) return;
        LinkedService ls;
        ls.name = meta->get_string("name", "");
        // Headless and ExternalName Services get no variables, as in Kubernetes
        if (ls.name.empty() || spec->get_string("clusterIP", "") == "None" || spec->get_string("type", "") == "ExternalName") return;
        const JsonValue* sel = spec->get("selector");
        const JsonValue* ports = spec->get("ports");
        if (!sel || !sel->is_object() || sel->obj_val.empty() || !ports || !ports->is_array() || ports->arr_val.empty()) return;
        std::vector<std::pair<std::string, std::string>> selector;
        for (const auto& kv : sel->obj_val) {
            if (kv.second.is_string()) selector.emplace_back(kv.first, kv.second.str_val);
        }
        ls.selects_pod = labels_match(own_labels, selector);
        if (ls.selects_pod) ls.host = ctx.node_ip;
        for (const auto& p : ports->arr_val) {
            ResolvedPort rp;
            rp.name = p.get_string("name", "");
            rp.protocol = upper(p.get_string("protocol", "TCP"));
            if (ls.selects_pod) rp.port = target_port(p, own_spec);
            ls.ports.push_back(std::move(rp));
        }
        others = others || !ls.selects_pod;
        services.push_back(std::move(ls));
    })) return;

    if (others) {
        for_each_item(ctx, "/api/v1/namespaces/" + ns + "/endpoints", [&](const JsonValue& ep) {
            const JsonValue* meta = ep.get("metadata");
            const std::string name = meta ? meta->get_string("name", "") : "";
            auto it = std::find_if(services.begin(), services.end(), [&](const LinkedService& s) { return s.name == name; });
            if (it == services.end() || it->selects_pod) return;
            const JsonValue* subsets = ep.get("subsets");
            if (!subsets || !subsets->is_array()) return;
            for (const auto& sub : subsets->arr_val) {
                const JsonValue* addrs = sub.get("addresses");
                if (!addrs || !addrs->is_array() || addrs->arr_val.empty()) continue;
                const std::string ip = addrs->arr_val[0].get_string("ip", "");
                if (ip.empty()) continue;
                it->host = ip;
                const JsonValue* eports = sub.get("ports");
                for (auto& rp : it->ports) {
                    if (!eports || !eports->is_array()) break;
                    for (const auto& e : eports->arr_val) {
                        if (e.get_string("name", "") == rp.name && upper(e.get_string("protocol", "TCP")) == rp.protocol) {
                            rp.port = e.get_int("port", 0);
                            break;
                        }
                    }
                }
                break;
            }
        });
    }

    for (const auto& ls : services) {
        if (ls.host.empty()) continue;
        std::vector<ResolvedPort> resolved;
        for (const auto& rp : ls.ports) {
            if (rp.port > 0 && rp.port <= 65535) resolved.push_back(rp);
        }
        add_service_links(out, ls.name, ls.host, resolved);
    }
}

// valueFrom.fieldRef paths the downward API offers.
bool field_ref_value(const JsonValue& pod, const PodEnvContext& ctx, const std::string& path, std::string& out) {
    const JsonValue* meta = pod.get("metadata");
    const JsonValue* spec = pod.get("spec");
    auto from_map = [&](const char* field, size_t prefix_len) -> bool {
        // metadata.labels['key'] / metadata.annotations['key']
        if (path.size() < prefix_len + 4 || path.compare(path.size() - 2, 2, "']") != 0) return false;
        const std::string key = path.substr(prefix_len + 2, path.size() - prefix_len - 4);
        const JsonValue* m = meta ? meta->get(field) : nullptr;
        out = m ? m->get_string(key, "") : "";
        return true;
    };
    if (path == "metadata.name") { out = meta ? meta->get_string("name", "") : ""; return true; }
    if (path == "metadata.namespace") { out = meta ? meta->get_string("namespace", "default") : "default"; return true; }
    if (path == "metadata.uid") { out = meta ? meta->get_string("uid", "") : ""; return true; }
    if (path == "spec.nodeName") { out = spec ? spec->get_string("nodeName", ctx.node_name) : ctx.node_name; return true; }
    if (path == "spec.serviceAccountName") { out = spec ? spec->get_string("serviceAccountName", "default") : "default"; return true; }
    // A pod's IP is its node's address
    if (path == "status.podIP" || path == "status.podIPs" || path == "status.hostIP" || path == "status.hostIPs") { out = ctx.node_ip; return true; }
    if (path.rfind("metadata.labels[", 0) == 0) return from_map("labels", 15);
    if (path.rfind("metadata.annotations[", 0) == 0) return from_map("annotations", 20);
    return false;
}

} // namespace

std::string service_env_name(const std::string& name) {
    std::string out = upper(name);
    for (auto& c : out) {
        if (c == '-') c = '_';
    }
    return out;
}

std::string expand_env_refs(const std::string& in, const std::function<const std::string*(const std::string&)>& lookup) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] != '$' || i + 1 >= in.size()) {
            out.push_back(in[i++]);
            continue;
        }
        const char next = in[i + 1];
        if (next == '$') {
            out.push_back('$');
            i += 2;
        } else if (next == '(') {
            size_t close = in.find(')', i + 2);
            if (close == std::string::npos) {
                out += "$(";
                i += 2;
                continue;
            }
            const std::string name = in.substr(i + 2, close - i - 2);
            const std::string* v = lookup(name);
            out += v ? *v : "$(" + name + ")";
            i = close + 1;
        } else {
            out.push_back('$');
            out.push_back(next);
            i += 2;
        }
    }
    return out;
}

bool build_container_env(const std::string& pod_manifest_json, const PodEnvContext& ctx, EnvVars& out, std::string& out_error) {
    out.clear();
    out_error.clear();
    JsonValue pod;
    std::string err;
    if (!models::parse_json(pod_manifest_json, pod, err) || !pod.is_object()) {
        out_error = "cannot parse pod manifest: " + err;
        return false;
    }
    const JsonValue* meta = pod.get("metadata");
    const JsonValue* spec = pod.get("spec");
    const std::string ns = meta ? meta->get_string("namespace", "default") : "default";
    const JsonValue* containers = spec ? spec->get("containers") : nullptr;
    const JsonValue* c = (containers && containers->is_array() && !containers->arr_val.empty()) ? &containers->arr_val[0] : nullptr;

    EnvVars services;
    // The API server, as Kubernetes injects for its "kubernetes" Service into
    // every pod (also with enableServiceLinks: false)
    if (!ctx.api_host.empty()) add_service_links(services, "kubernetes", ctx.api_host, {ResolvedPort{"", "TCP", ctx.api_port}});
    if (!spec || spec->get_bool("enableServiceLinks", true)) collect_service_links(ctx, ns, pod, services);

    EnvBuilder builder(ctx, ns);
    EnvVars vars;

    if (const JsonValue* env_from = c ? c->get("envFrom") : nullptr; env_from && env_from->is_array()) {
        for (const auto& src : env_from->arr_val) {
            const std::string prefix = src.get_string("prefix", "");
            const JsonValue* ref = src.get("configMapRef");
            bool secret = false;
            if (!ref) { ref = src.get("secretRef"); secret = true; }
            if (!ref || !ref->is_object()) continue;
            const std::string name = ref->get_string("name", "");
            const std::map<std::string, std::string>* data = nullptr;
            int found = builder.object_data(secret, name, data, out_error);
            if (found < 0) return false;
            if (found == 0) {
                if (ref->get_bool("optional", false)) continue;
                out_error = std::string(secret ? "secret" : "configmap") + " \"" + name + "\" not found";
                return false;
            }
            // Keys that are not valid variable names are skipped, as in Kubernetes
            for (const auto& kv : *data) {
                if (valid_env_name(prefix + kv.first)) set_var(vars, prefix + kv.first, kv.second);
            }
        }
    }

    if (const JsonValue* env = c ? c->get("env") : nullptr; env && env->is_array()) {
        auto lookup = [&](const std::string& name) -> const std::string* {
            const std::string* v = find_var(vars, name);
            return v ? v : find_var(services, name);
        };
        for (const auto& e : env->arr_val) {
            const std::string name = e.get_string("name", "");
            if (name.empty()) continue;
            const JsonValue* from = e.get("valueFrom");
            if (!from || !from->is_object()) {
                const JsonValue* v = e.get("value");
                std::string value;
                if (v && v->is_string()) value = v->str_val;
                else if (v && v->is_number()) value = std::to_string(static_cast<long long>(v->num_val));
                else if (v && v->is_bool()) value = v->bool_val ? "true" : "false";
                set_var(vars, name, expand_env_refs(value, lookup));
                continue;
            }
            const JsonValue* cm = from->get("configMapKeyRef");
            const JsonValue* sk = from->get("secretKeyRef");
            const JsonValue* fr = from->get("fieldRef");
            if ((cm && cm->is_object()) || (sk && sk->is_object())) {
                const bool secret = !(cm && cm->is_object());
                const JsonValue* ref = secret ? sk : cm;
                const std::string obj = ref->get_string("name", "");
                const std::string key = ref->get_string("key", "");
                const bool optional = ref->get_bool("optional", false);
                const std::map<std::string, std::string>* data = nullptr;
                int found = builder.object_data(secret, obj, data, out_error);
                if (found < 0) return false;
                if (found == 0) {
                    if (optional) continue;
                    out_error = std::string(secret ? "secret" : "configmap") + " \"" + obj + "\" not found";
                    return false;
                }
                auto it = data->find(key);
                if (it == data->end()) {
                    if (optional) continue;
                    out_error = "couldn't find key " + key + " in " + (secret ? "Secret " : "ConfigMap ") + ns + "/" + obj;
                    return false;
                }
                set_var(vars, name, it->second);
            } else if (fr && fr->is_object()) {
                std::string value;
                const std::string path = fr->get_string("fieldPath", "");
                if (!field_ref_value(pod, ctx, path, value)) {
                    out_error = "env " + name + ": unsupported fieldRef fieldPath \"" + path + "\"";
                    return false;
                }
                set_var(vars, name, value);
            } else {
                out_error = "env " + name + ": unsupported valueFrom source (configMapKeyRef, secretKeyRef and fieldRef are supported)";
                return false;
            }
        }
    }

    out = std::move(vars);
    for (auto& kv : services) {
        if (!find_var(out, kv.first)) out.push_back(std::move(kv));
    }
    return true;
}

} // namespace kube32::kubelet
