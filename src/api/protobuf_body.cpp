// Request bodies in the Kubernetes protobuf encoding, converted to JSON.
//
// Typed clients send them for built-in kinds: kubectl create configmap,
// create secret and create namespace post application/vnd.kubernetes.protobuf
// and do not retry as JSON. The encoding is the "k8s\0" magic followed by a
// runtime.Unknown message (typeMeta = 1, raw = 2) whose raw bytes are the
// object (k8s.io/api/core/v1/generated.proto field numbers below).

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
    bool field(uint32_t& number, std::string_view& out, uint64_t& value) {
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
    if (api_version != "v1" || (kind != "ConfigMap" && kind != "Secret" && kind != "Namespace")) {
        out_error = api_version + " " + kind + " in protobuf is not supported (ConfigMap, Secret and Namespace are), send application/json";
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
