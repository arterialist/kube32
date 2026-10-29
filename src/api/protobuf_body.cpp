// Request bodies in the Kubernetes protobuf encoding, converted to JSON.
//
// Typed clients send them for built-in kinds: kubectl create configmap,
// secret, namespace, deployment, job and service post
// application/vnd.kubernetes.protobuf and do not retry as JSON. The encoding
// is the "k8s\0" magic followed by a runtime.Unknown message (typeMeta = 1,
// raw = 2) whose raw bytes are the object (field numbers from the
// generated.proto files of k8s.io/api and k8s.io/apimachinery).

#include "api/protobuf_body.hpp"
#include "models/kube_models.hpp"

#include <cstdint>

namespace kube32::api {

namespace {

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok{true};

    bool more() const { return ok && p < end; }
    uint64_t varint() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p >= end) { ok = false; return 0; }
            uint8_t b = *p++;
            v |= static_cast<uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    // Next field: number and wire type; length-delimited payload in `out`
    // (null data for the other wire types), varint in `value`
    bool field(uint32_t& number, std::string_view& out, uint64_t& value) {
        out = {};
        value = 0;
        uint64_t key = varint();
        if (!ok) return false;
        number = static_cast<uint32_t>(key >> 3);
        switch (key & 7) {
            case 0: value = varint(); out = {}; break;
            case 1: if (end - p < 8) { ok = false; break; } p += 8; break;
            case 5: if (end - p < 4) { ok = false; break; } p += 4; break;
            case 2: {
                uint64_t len = varint();
                if (!ok || len > static_cast<uint64_t>(end - p)) { ok = false; break; }
                out = std::string_view(reinterpret_cast<const char*>(p), static_cast<size_t>(len));
                p += len;
                break;
            }
            default: ok = false;
        }
        return ok;
    }
};

Reader reader(std::string_view s) {
    return Reader{reinterpret_cast<const uint8_t*>(s.data()), reinterpret_cast<const uint8_t*>(s.data()) + s.size()};
}

models::JsonValue jstring(std::string_view s) {
    models::JsonValue v;
    v.type = models::JsonType::String;
    v.str_val.assign(s.data(), s.size());
    return v;
}

models::JsonValue jobject() {
    models::JsonValue v;
    v.type = models::JsonType::Object;
    return v;
}

std::string base64(std::string_view in) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t n = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8) | static_cast<uint8_t>(in[i + 2]);
        out += kAlphabet[n >> 18]; out += kAlphabet[(n >> 12) & 63]; out += kAlphabet[(n >> 6) & 63]; out += kAlphabet[n & 63];
    }
    if (i < in.size()) {
        uint32_t n = static_cast<uint8_t>(in[i]) << 16;
        if (i + 1 < in.size()) n |= static_cast<uint8_t>(in[i + 1]) << 8;
        out += kAlphabet[n >> 18];
        out += kAlphabet[(n >> 12) & 63];
        out += (i + 1 < in.size()) ? kAlphabet[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// map<string, string|bytes> entry: key = 1, value = 2
bool map_entry(std::string_view entry, models::JsonValue& map, bool bytes) {
    Reader r = reader(entry);
    std::string_view key, value, payload;
    uint32_t n;
    uint64_t v;
    while (r.more() && r.field(n, payload, v)) {
        if (n == 1) key = payload;
        else if (n == 2) value = payload;
    }
    if (!r.ok) return false;
    if (map.type != models::JsonType::Object) map = jobject();
    map.set(std::string(key), jstring(bytes ? std::string_view(base64(value)) : value));
    return true;
}

// ObjectMeta: the fields a client sets on create or update
bool object_meta(std::string_view in, models::JsonValue& out) {
    out = jobject();
    Reader r = reader(in);
    std::string_view s;
    uint32_t n;
    uint64_t v;
    models::JsonValue labels, annotations, finalizers;
    while (r.more() && r.field(n, s, v)) {
        switch (n) {
            case 1: if (!s.empty()) out.set("name", jstring(s)); break;
            case 2: if (!s.empty()) out.set("generateName", jstring(s)); break;
            case 3: if (!s.empty()) out.set("namespace", jstring(s)); break;
            case 5: if (!s.empty()) out.set("uid", jstring(s)); break;
            case 6: if (!s.empty()) out.set("resourceVersion", jstring(s)); break;
            case 11: if (!map_entry(s, labels, false)) return false; break;
            case 12: if (!map_entry(s, annotations, false)) return false; break;
            case 14:
                finalizers.type = models::JsonType::Array;
                finalizers.arr_val.push_back(jstring(s));
                break;
            default: break;   // selfLink, generation, timestamps, owners, managedFields
        }
    }
    if (labels.is_object()) out.set("labels", std::move(labels));
    if (annotations.is_object()) out.set("annotations", std::move(annotations));
    if (finalizers.is_array()) out.set("finalizers", std::move(finalizers));
    return r.ok;
}

models::JsonValue jbool(bool b) {
    models::JsonValue v;
    v.type = models::JsonType::Boolean;
    v.bool_val = b;
    return v;
}

models::JsonValue jnumber(int64_t n) {
    models::JsonValue v;
    v.type = models::JsonType::Number;
    v.num_val = static_cast<double>(n);
    return v;
}

models::JsonValue jarray() {
    models::JsonValue v;
    v.type = models::JsonType::Array;
    return v;
}

// int32 and int64 fields travel as the varint of the sign-extended value
int64_t as_int(uint64_t v) { return static_cast<int64_t>(v); }

// An integer field of a message by number and JSON name. Fields that are not
// pointers in the Go types travel as 0 when unset and are left out then.
struct IntField {
    uint32_t number;
    const char* name;
    bool zero_unset{false};
};

// Decodes the varint fields listed in `ints` and hands every other field to
// `other`; a field of another wire type under a listed number is skipped.
template <size_t N, typename F>
bool message(std::string_view in, models::JsonValue& out, const IntField (&ints)[N], F other) {
    out = jobject();
    Reader r = reader(in);
    std::string_view s;
    uint32_t n;
    uint64_t v;
    while (r.more() && r.field(n, s, v)) {
        bool handled = false;
        for (const IntField& f : ints) {
            if (f.number == n && s.data() == nullptr) {
                if (v != 0 || !f.zero_unset) out.set(f.name, jnumber(as_int(v)));
                handled = true;
            }
        }
        if (!handled && !other(n, s, v, out)) return false;
    }
    return r.ok;
}

bool string_field(std::string_view s, const char* name, models::JsonValue& out) {
    if (!s.empty()) out.set(name, jstring(s));
    return true;
}

bool append_string(std::string_view s, const char* name, models::JsonValue& out) {
    models::JsonValue* arr = out.get_mut(name);
    if (!arr) {
        out.set(name, jarray());
        arr = out.get_mut(name);
    }
    arr->arr_val.push_back(jstring(s));
    return true;
}

// map<string, string> field `n` of a message: merged into out[name]
bool string_map_field(std::string_view s, const char* name, models::JsonValue& out) {
    models::JsonValue* m = out.get_mut(name);
    models::JsonValue fresh;
    if (!map_entry(s, m ? *m : fresh, false)) return false;
    if (!m) out.set(name, std::move(fresh));
    return true;
}

// IntOrString: type = 1 (0 int, 1 string), intVal = 2, strVal = 3
bool int_or_string(std::string_view in, models::JsonValue& out) {
    Reader r = reader(in);
    std::string_view s, str;
    uint32_t n;
    uint64_t v, type = 0, num = 0;
    while (r.more() && r.field(n, s, v)) {
        if (n == 1 && !s.data()) type = v;
        else if (n == 2 && !s.data()) num = v;
        else if (n == 3) str = s;
    }
    if (!r.ok) return false;
    out = type == 1 ? jstring(str) : jnumber(static_cast<int32_t>(num));
    return true;
}

// map<string, Quantity> entry: key = 1, value = 2 (Quantity: string = 1)
bool quantity_entry(std::string_view entry, models::JsonValue& map) {
    Reader r = reader(entry);
    std::string_view key, value, s;
    uint32_t n;
    uint64_t v;
    while (r.more() && r.field(n, s, v)) {
        if (n == 1) key = s;
        else if (n == 2) value = s;
    }
    Reader q = reader(value);
    std::string_view text;
    while (r.ok && q.more() && q.field(n, s, v)) if (n == 1) text = s;
    if (!r.ok || !q.ok) return false;
    if (!map.is_object()) map = jobject();
    map.set(std::string(key), jstring(text));
    return true;
}

// LabelSelector: matchLabels = 1, matchExpressions = 2
bool label_selector(std::string_view in, models::JsonValue& out) {
    static const IntField none[1] = {{0, ""}};
    return message(in, out, none, [](uint32_t n, std::string_view s, uint64_t, models::JsonValue& o) {
        if (n == 1) return string_map_field(s, "matchLabels", o);
        if (n != 2) return true;
        models::JsonValue req;
        bool ok = message(s, req, none, [](uint32_t rn, std::string_view rs, uint64_t, models::JsonValue& ro) {
            if (rn == 1) return string_field(rs, "key", ro);
            if (rn == 2) return string_field(rs, "operator", ro);
            if (rn == 3) return append_string(rs, "values", ro);
            return true;
        });
        if (!ok) return false;
        models::JsonValue* arr = o.get_mut("matchExpressions");
        if (!arr) {
            o.set("matchExpressions", jarray());
            arr = o.get_mut("matchExpressions");
        }
        arr->arr_val.push_back(std::move(req));
        return true;
    });
}

// ResourceRequirements: limits = 1, requests = 2
bool resource_requirements(std::string_view in, models::JsonValue& out) {
    static const IntField none[1] = {{0, ""}};
    return message(in, out, none, [](uint32_t n, std::string_view s, uint64_t, models::JsonValue& o) {
        if (n != 1 && n != 2) return true;
        const char* name = n == 1 ? "limits" : "requests";
        models::JsonValue* m = o.get_mut(name);
        models::JsonValue fresh;
        if (!quantity_entry(s, m ? *m : fresh)) return false;
        if (!m) o.set(name, std::move(fresh));
        return true;
    });
}

// Appends the decoded element of repeated message field `name`
template <typename Decode>
bool append_message(std::string_view s, const char* name, models::JsonValue& out, Decode decode) {
    models::JsonValue item;
    if (!decode(s, item)) return false;
    models::JsonValue* arr = out.get_mut(name);
    if (!arr) {
        out.set(name, jarray());
        arr = out.get_mut(name);
    }
    arr->arr_val.push_back(std::move(item));
    return true;
}

// ContainerPort: name = 1, hostPort = 2, containerPort = 3, protocol = 4
bool container_port(std::string_view in, models::JsonValue& out) {
    static const IntField ints[] = {{2, "hostPort", true}, {3, "containerPort"}};
    return message(in, out, ints, [](uint32_t n, std::string_view s, uint64_t, models::JsonValue& o) {
        if (n == 1) return string_field(s, "name", o);
        if (n == 4) return string_field(s, "protocol", o);
        return true;
    });
}

// Container: name = 1, image = 2, command = 3, args = 4, ports = 6,
// resources = 8
bool container(std::string_view in, models::JsonValue& out) {
    static const IntField none[1] = {{0, ""}};
    return message(in, out, none, [](uint32_t n, std::string_view s, uint64_t, models::JsonValue& o) {
        switch (n) {
            case 1: return string_field(s, "name", o);
            case 2: return string_field(s, "image", o);
            case 3: return append_string(s, "command", o);
            case 4: return append_string(s, "args", o);
            case 6: return append_message(s, "ports", o, container_port);
            case 8: {
                models::JsonValue res;
                if (!resource_requirements(s, res)) return false;
                o.set("resources", std::move(res));
                return true;
            }
            default: return true;
        }
    });
}

// PodSpec: containers = 2, restartPolicy = 3
bool pod_spec(std::string_view in, models::JsonValue& out) {
    static const IntField none[1] = {{0, ""}};
    return message(in, out, none, [](uint32_t n, std::string_view s, uint64_t, models::JsonValue& o) {
        if (n == 2) return append_message(s, "containers", o, container);
        if (n == 3) return string_field(s, "restartPolicy", o);
        return true;
    });
}

// PodTemplateSpec: metadata = 1, spec = 2
bool pod_template(std::string_view in, models::JsonValue& out) {
    static const IntField none[1] = {{0, ""}};
    return message(in, out, none, [](uint32_t n, std::string_view s, uint64_t, models::JsonValue& o) {
        models::JsonValue sub;
        if (n == 1) {
            if (!object_meta(s, sub)) return false;
            o.set("metadata", std::move(sub));
        } else if (n == 2) {
            if (!pod_spec(s, sub)) return false;
            o.set("spec", std::move(sub));
        }
        return true;
    });
}

// DeploymentStrategy: type = 1, rollingUpdate = 2
bool deployment_strategy(std::string_view in, models::JsonValue& out) {
    static const IntField none[1] = {{0, ""}};
    return message(in, out, none, [](uint32_t n, std::string_view s, uint64_t, models::JsonValue& o) {
        if (n == 1) return string_field(s, "type", o);
        if (n != 2) return true;
        models::JsonValue ru;
        bool ok = message(s, ru, none, [](uint32_t rn, std::string_view rs, uint64_t, models::JsonValue& ro) {
            if (rn != 1 && rn != 2) return true;
            models::JsonValue val;
            if (!int_or_string(rs, val)) return false;
            ro.set(rn == 1 ? "maxUnavailable" : "maxSurge", std::move(val));
            return true;
        });
        if (!ok) return false;
        o.set("rollingUpdate", std::move(ru));
        return true;
    });
}

// DeploymentSpec: replicas = 1, selector = 2, template = 3, strategy = 4,
// minReadySeconds = 5, revisionHistoryLimit = 6, paused = 7,
// progressDeadlineSeconds = 9
bool deployment_spec(std::string_view in, models::JsonValue& out) {
    static const IntField ints[] = {{1, "replicas"}, {5, "minReadySeconds", true}, {6, "revisionHistoryLimit"}, {9, "progressDeadlineSeconds"}};
    return message(in, out, ints, [](uint32_t n, std::string_view s, uint64_t v, models::JsonValue& o) {
        models::JsonValue sub;
        switch (n) {
            case 2: if (!label_selector(s, sub)) return false; o.set("selector", std::move(sub)); break;
            case 3: if (!pod_template(s, sub)) return false; o.set("template", std::move(sub)); break;
            case 4: if (!deployment_strategy(s, sub)) return false; o.set("strategy", std::move(sub)); break;
            case 7: if (v) o.set("paused", jbool(true)); break;
            default: break;
        }
        return true;
    });
}

// JobSpec: parallelism = 1, completions = 2, activeDeadlineSeconds = 3,
// selector = 4, template = 6, backoffLimit = 7, ttlSecondsAfterFinished = 8,
// completionMode = 9, suspend = 10
bool job_spec(std::string_view in, models::JsonValue& out) {
    static const IntField ints[] = {{1, "parallelism"}, {2, "completions"}, {3, "activeDeadlineSeconds"}, {7, "backoffLimit"}, {8, "ttlSecondsAfterFinished"}};
    return message(in, out, ints, [](uint32_t n, std::string_view s, uint64_t v, models::JsonValue& o) {
        models::JsonValue sub;
        switch (n) {
            case 4: if (!label_selector(s, sub)) return false; o.set("selector", std::move(sub)); break;
            case 6: if (!pod_template(s, sub)) return false; o.set("template", std::move(sub)); break;
            case 9: return string_field(s, "completionMode", o);
            case 10: o.set("suspend", jbool(v != 0)); break;
            default: break;
        }
        return true;
    });
}

// ServicePort: name = 1, protocol = 2, port = 3, targetPort = 4,
// nodePort = 5
bool service_port(std::string_view in, models::JsonValue& out) {
    static const IntField ints[] = {{3, "port"}, {5, "nodePort", true}};
    return message(in, out, ints, [](uint32_t n, std::string_view s, uint64_t, models::JsonValue& o) {
        if (n == 1) return string_field(s, "name", o);
        if (n == 2) return string_field(s, "protocol", o);
        if (n != 4) return true;
        models::JsonValue target;
        if (!int_or_string(s, target)) return false;
        o.set("targetPort", std::move(target));
        return true;
    });
}

// ServiceSpec: ports = 1, selector = 2, clusterIP = 3, type = 4,
// sessionAffinity = 7, externalName = 10
bool service_spec(std::string_view in, models::JsonValue& out) {
    static const IntField none[1] = {{0, ""}};
    return message(in, out, none, [](uint32_t n, std::string_view s, uint64_t, models::JsonValue& o) {
        switch (n) {
            case 1: return append_message(s, "ports", o, service_port);
            case 2: return string_map_field(s, "selector", o);
            case 3: return string_field(s, "clusterIP", o);
            case 4: return string_field(s, "type", o);
            case 7: return string_field(s, "sessionAffinity", o);
            case 10: return string_field(s, "externalName", o);
            default: return true;
        }
    });
}

} // namespace

bool protobuf_body_to_json(std::string_view body, std::string& out_json, std::string& out_error) {
    static const char kMagic[4] = {'k', '8', 's', '\0'};
    if (body.size() < 4 || body.compare(0, 4, std::string_view(kMagic, 4)) != 0) {
        out_error = "not a Kubernetes protobuf message";
        return false;
    }
    Reader env = reader(body.substr(4));
    std::string_view s, type_meta, raw;
    uint32_t n;
    uint64_t v;
    while (env.more() && env.field(n, s, v)) {
        if (n == 1) type_meta = s;
        else if (n == 2) raw = s;
    }
    std::string api_version, kind;
    Reader tm = reader(type_meta);
    while (tm.more() && tm.field(n, s, v)) {
        if (n == 1) api_version.assign(s.data(), s.size());
        else if (n == 2) kind.assign(s.data(), s.size());
    }
    if (!env.ok || !tm.ok) {
        out_error = "malformed protobuf envelope";
        return false;
    }
    const bool core = (api_version == "v1" && (kind == "ConfigMap" || kind == "Secret" || kind == "Namespace" || kind == "Service"));
    const bool workload = (api_version == "apps/v1" && kind == "Deployment") || (api_version == "batch/v1" && kind == "Job");
    if (!core && !workload) {
        out_error = api_version + " " + kind + " in protobuf is not supported (ConfigMap, Secret, Namespace, Service, Deployment and Job are), send application/json";
        return false;
    }

    models::JsonValue obj = jobject();
    obj.set("apiVersion", jstring(api_version));
    obj.set("kind", jstring(kind));
    models::JsonValue data, binary, string_data, spec;
    Reader r = reader(raw);
    const bool secret = (kind == "Secret");
    while (r.more() && r.field(n, s, v)) {
        if (n == 1) {
            models::JsonValue meta;
            if (!object_meta(s, meta)) { r.ok = false; break; }
            obj.set("metadata", std::move(meta));
            continue;
        }
        if (workload || kind == "Service") {   // metadata = 1, spec = 2
            if (n == 2) {
                bool ok = kind == "Deployment" ? deployment_spec(s, spec) : kind == "Job" ? job_spec(s, spec) : service_spec(s, spec);
                if (!ok) r.ok = false;
            }
            continue;
        }
        if (kind == "Namespace") {
            if (n == 2) {   // NamespaceSpec: finalizers = 1
                Reader sr = reader(s);
                std::string_view f;
                uint32_t fn;
                uint64_t fv;
                models::JsonValue fin;
                fin.type = models::JsonType::Array;
                while (sr.more() && sr.field(fn, f, fv)) if (fn == 1) fin.arr_val.push_back(jstring(f));
                spec = jobject();
                if (!fin.arr_val.empty()) spec.set("finalizers", std::move(fin));
            }
            continue;
        }
        // ConfigMap: data = 2, binaryData = 3, immutable = 4
        // Secret:    data = 2 (bytes), type = 3, stringData = 4, immutable = 5
        if (n == 2) { if (!map_entry(s, data, secret)) r.ok = false; }
        else if (n == 3 && !secret) { if (!map_entry(s, binary, true)) r.ok = false; }
        else if (n == 3 && secret) { if (!s.empty()) obj.set("type", jstring(s)); }
        else if (n == 4 && secret) { if (!map_entry(s, string_data, false)) r.ok = false; }
        else if ((n == 4 && !secret) || (n == 5 && secret)) obj.set("immutable", jbool(v != 0));
    }
    if (!r.ok) {
        out_error = "malformed protobuf " + kind;
        return false;
    }
    if (data.is_object()) obj.set("data", std::move(data));
    if (binary.is_object()) obj.set("binaryData", std::move(binary));
    if (string_data.is_object()) obj.set("stringData", std::move(string_data));
    if (spec.is_object()) obj.set("spec", std::move(spec));
    out_json = obj.to_string();
    return true;
}

} // namespace kube32::api
