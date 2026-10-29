#include "models/kube_models.hpp"
#include "osal/system.hpp"
#include <random>
#include <cstdio>
#if defined(ESP_PLATFORM)
#include "esp_random.h"
#endif

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>

namespace kube32::models {

uint64_t parse_quantity(const std::string& val) {
    // <number><suffix> as k8s.io/apimachinery/pkg/api/resource parses it. The
    // number is digits with an optional fraction (a leading '+' is allowed,
    // negatives are not resources). Suffixes: binary Ki Mi Gi Ti Pi Ei, decimal
    // n u m k M G T P E, or an exponent e<int>/E<int>. Like Quantity.Value(),
    // the result is rounded up to a whole unit, so "500m" of memory is 1
    // byte. Anything else is not a quantity and yields 0.
    size_t i = 0;
    if (i < val.size() && val[i] == '+') ++i;
    const size_t num_start = i;
    bool digits = false;
    while (i < val.size() && std::isdigit(static_cast<unsigned char>(val[i]))) { ++i; digits = true; }
    if (i < val.size() && val[i] == '.') {
        ++i;
        while (i < val.size() && std::isdigit(static_cast<unsigned char>(val[i]))) { ++i; digits = true; }
    }
    if (!digits) return 0;
    double num = 0.0;
    try { num = std::stod(val.substr(num_start, i - num_start)); } catch (...) { return 0; }
    const std::string unit = val.substr(i);
    double scale = 1.0;
    if (unit.empty()) scale = 1.0;
    else if (unit == "Ki") scale = 1024.0;
    else if (unit == "Mi") scale = 1024.0 * 1024.0;
    else if (unit == "Gi") scale = 1024.0 * 1024.0 * 1024.0;
    else if (unit == "Ti") scale = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    else if (unit == "Pi") scale = 1024.0 * 1024.0 * 1024.0 * 1024.0 * 1024.0;
    else if (unit == "Ei") scale = 1024.0 * 1024.0 * 1024.0 * 1024.0 * 1024.0 * 1024.0;
    else if (unit == "n") scale = 1e-9;
    else if (unit == "u") scale = 1e-6;
    else if (unit == "m") scale = 1e-3;
    else if (unit == "k") scale = 1e3;
    else if (unit == "M") scale = 1e6;
    else if (unit == "G") scale = 1e9;
    else if (unit == "T") scale = 1e12;
    else if (unit == "P") scale = 1e15;
    else if (unit == "E") scale = 1e18;
    else if ((unit[0] == 'e' || unit[0] == 'E') && unit.size() > 1) {
        size_t j = 1;
        bool neg = false;
        if (unit[j] == '+' || unit[j] == '-') { neg = (unit[j] == '-'); ++j; }
        if (j >= unit.size() || j + 3 < unit.size()) return 0;
        int exp = 0;
        for (; j < unit.size(); ++j) {
            if (!std::isdigit(static_cast<unsigned char>(unit[j]))) return 0;
            exp = exp * 10 + (unit[j] - '0');
        }
        scale = std::pow(10.0, neg ? -exp : exp);
    } else {
        return 0;
    }
    const double v = num * scale;
    if (v >= 1.8e19) return UINT64_MAX;
    // Round up, but let a product that lands a hair above an integer through
    // floating point (1.1 * 1000) stay that integer.
    const double whole = std::floor(v);
    if (v - whole < 1e-9 * (whole > 1.0 ? whole : 1.0)) return static_cast<uint64_t>(whole);
    return static_cast<uint64_t>(whole) + 1;
}

// =============================================================================
// JsonValue Methods
// =============================================================================

const JsonValue* JsonValue::get(const std::string& key) const {
    if (type != JsonType::Object) return nullptr;
    for (const auto& kv : obj_val) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

JsonValue* JsonValue::get_mut(const std::string& key) {
    if (type != JsonType::Object) return nullptr;
    for (auto& kv : obj_val) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

void JsonValue::set(const std::string& key, JsonValue val) {
    if (type != JsonType::Object) {
        type = JsonType::Object;
        obj_val.clear();
    }
    for (auto& kv : obj_val) {
        if (kv.first == key) {
            kv.second = std::move(val);
            return;
        }
    }
    obj_val.emplace_back(key, std::move(val));
}

bool JsonValue::remove(const std::string& key) {
    if (type != JsonType::Object) return false;
    for (auto it = obj_val.begin(); it != obj_val.end(); ++it) {
        if (it->first == key) {
            obj_val.erase(it);
            return true;
        }
    }
    return false;
}

std::string JsonValue::get_string(const std::string& key, const std::string& def) const {
    const JsonValue* v = get(key);
    if (v && v->is_string()) return v->str_val;
    return def;
}

int64_t JsonValue::get_int(const std::string& key, int64_t def) const {
    const JsonValue* v = get(key);
    if (v && v->is_number()) return static_cast<int64_t>(v->num_val);
    return def;
}

bool JsonValue::get_bool(const std::string& key, bool def) const {
    const JsonValue* v = get(key);
    if (v && v->is_bool()) return v->bool_val;
    return def;
}

static void escape_json_string(std::string_view s, std::string& out) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out.append(buf);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
}

std::string JsonValue::to_string(bool pretty, int indent_level) const {
    std::string out;
    std::string indent = pretty ? std::string(indent_level * 2, ' ') : "";
    std::string next_indent = pretty ? std::string((indent_level + 1) * 2, ' ') : "";
    std::string nl = pretty ? "\n" : "";
    std::string space = pretty ? " " : "";

    switch (type) {
        case JsonType::Null:
            out = "null";
            break;
        case JsonType::Boolean:
            out = bool_val ? "true" : "false";
            break;
        case JsonType::Number: {
            char buf[64];
            if (num_val == static_cast<int64_t>(num_val)) {
                snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(num_val));
            } else {
                snprintf(buf, sizeof(buf), "%.6g", num_val);
            }
            out = buf;
            break;
        }
        case JsonType::String:
            escape_json_string(str_val, out);
            break;
        case JsonType::Array: {
            if (arr_val.empty()) {
                out = "[]";
            } else {
                out = "[" + nl;
                for (size_t i = 0; i < arr_val.size(); ++i) {
                    out += next_indent + arr_val[i].to_string(pretty, indent_level + 1);
                    if (i + 1 < arr_val.size()) out += ",";
                    out += nl;
                }
                out += indent + "]";
            }
            break;
        }
        case JsonType::Object: {
            if (obj_val.empty()) {
                out = "{}";
            } else {
                out = "{" + nl;
                for (size_t i = 0; i < obj_val.size(); ++i) {
                    out += next_indent;
                    escape_json_string(obj_val[i].first, out);
                    out += ":" + space + obj_val[i].second.to_string(pretty, indent_level + 1);
                    if (i + 1 < obj_val.size()) out += ",";
                    out += nl;
                }
                out += indent + "}";
            }
            break;
        }
    }
    return out;
}

// =============================================================================
// RFC 8259 Strict JSON Parser
// =============================================================================

class StrictJsonParser {
public:
    static constexpr size_t MAX_RECURSION_DEPTH = 32;

    explicit StrictJsonParser(std::string_view input) : input_(input), pos_(0), depth_(0) {}

    bool parse(JsonValue& out, std::string& err) {
        skip_whitespace();
        if (pos_ >= input_.size()) {
            err = "empty body";
            return false;
        }

        if (!parse_value(out, err)) {
            return false;
        }

        skip_whitespace();
        if (pos_ < input_.size()) {
            err = "unexpected trailing characters";
            return false;
        }
        return true;
    }

private:
    struct DepthGuard {
        size_t& d_;
        explicit DepthGuard(size_t& d) : d_(d) { ++d_; }
        ~DepthGuard() { --d_; }
    };

    void skip_whitespace() {
        while (pos_ < input_.size()) {
            char c = input_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                pos_++;
            } else {
                break;
            }
        }
    }

    char peek() const {
        if (pos_ < input_.size()) return input_[pos_];
        return '\0';
    }

    char get() {
        if (pos_ < input_.size()) return input_[pos_++];
        return '\0';
    }

    bool parse_value(JsonValue& val, std::string& err) {
        if (depth_ >= MAX_RECURSION_DEPTH) {
            err = "maximum JSON nesting depth exceeded";
            return false;
        }
        DepthGuard guard(depth_);

        skip_whitespace();
        if (pos_ >= input_.size()) {
            err = "unexpected end of input";
            return false;
        }

        char c = peek();
        if (c == '{') return parse_object(val, err);
        if (c == '[') return parse_array(val, err);
        if (c == '"') return parse_string(val, err);
        if (c == 't' || c == 'f') return parse_bool(val, err);
        if (c == 'n') return parse_null(val, err);
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number(val, err);

        err = std::string("invalid character: '") + c + "'";
        return false;
    }

    bool parse_object(JsonValue& val, std::string& err) {
        get(); // consume '{'
        val.type = JsonType::Object;
        val.obj_val.clear();

        skip_whitespace();
        if (peek() == '}') {
            get();
            return true;
        }

        while (pos_ < input_.size()) {
            skip_whitespace();
            if (peek() != '"') {
                err = "expected string key in object";
                return false;
            }

            JsonValue key_val;
            if (!parse_string(key_val, err)) return false;

            skip_whitespace();
            if (peek() != ':') {
                err = "expected ':' after key in object";
                return false;
            }
            get(); // consume ':'

            JsonValue item_val;
            if (!parse_value(item_val, err)) return false;

            val.obj_val.emplace_back(std::move(key_val.str_val), std::move(item_val));

            skip_whitespace();
            if (peek() == '}') {
                get();
                return true;
            } else if (peek() == ',') {
                get(); // consume ','
                skip_whitespace();
                // Prohibit trailing comma: {"a": "b",}
                if (peek() == '}') {
                    err = "trailing comma in object";
                    return false;
                }
            } else {
                err = "expected ',' or '}' in object";
                return false;
            }
        }

        err = "unclosed object brace";
        return false;
    }

    bool parse_array(JsonValue& val, std::string& err) {
        get(); // consume '['
        val.type = JsonType::Array;
        val.arr_val.clear();

        skip_whitespace();
        if (peek() == ']') {
            get();
            return true;
        }

        while (pos_ < input_.size()) {
            JsonValue item_val;
            if (!parse_value(item_val, err)) return false;

            val.arr_val.push_back(std::move(item_val));

            skip_whitespace();
            if (peek() == ']') {
                get();
                return true;
            } else if (peek() == ',') {
                get(); // consume ','
                skip_whitespace();
                // Prohibit trailing comma: [1, 2,]
                if (peek() == ']') {
                    err = "trailing comma in array";
                    return false;
                }
            } else {
                err = "expected ',' or ']' in array";
                return false;
            }
        }

        err = "unclosed array bracket";
        return false;
    }

    bool parse_string(JsonValue& val, std::string& err) {
        get(); // consume opening '"'
        val.type = JsonType::String;
        val.str_val.clear();

        while (pos_ < input_.size()) {
            char c = get();
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (pos_ >= input_.size()) {
                    err = "unterminated escape sequence in string";
                    return false;
                }
                char esc = get();
                switch (esc) {
                    case '"': val.str_val.push_back('"'); break;
                    case '\\': val.str_val.push_back('\\'); break;
                    case '/': val.str_val.push_back('/'); break;
                    case 'b': val.str_val.push_back('\b'); break;
                    case 'f': val.str_val.push_back('\f'); break;
                    case 'n': val.str_val.push_back('\n'); break;
                    case 'r': val.str_val.push_back('\r'); break;
                    case 't': val.str_val.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > input_.size()) {
                            err = "unterminated unicode escape";
                            return false;
                        }
                        char hex_buf[5] = { input_[pos_], input_[pos_+1], input_[pos_+2], input_[pos_+3], '\0' };
                        pos_ += 4;
                        char* end_p = nullptr;
                        long uval = std::strtol(hex_buf, &end_p, 16);
                        if (end_p != hex_buf + 4) {
                            err = "invalid unicode escape";
                            return false;
                        }
                        // Output UTF-8
                        if (uval <= 0x7F) {
                            val.str_val.push_back(static_cast<char>(uval));
                        } else if (uval <= 0x7FF) {
                            val.str_val.push_back(static_cast<char>(0xC0 | ((uval >> 6) & 0x1F)));
                            val.str_val.push_back(static_cast<char>(0x80 | (uval & 0x3F)));
                        } else {
                            val.str_val.push_back(static_cast<char>(0xE0 | ((uval >> 12) & 0x0F)));
                            val.str_val.push_back(static_cast<char>(0x80 | ((uval >> 6) & 0x3F)));
                            val.str_val.push_back(static_cast<char>(0x80 | (uval & 0x3F)));
                        }
                        break;
                    }
                    default:
                        err = std::string("invalid escape character '\\") + esc + "'";
                        return false;
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                err = "control character inside unescaped string";
                return false;
            } else {
                val.str_val.push_back(c);
            }
        }

        err = "unterminated string";
        return false;
    }

    bool parse_number(JsonValue& val, std::string& err) {
        size_t start = pos_;
        if (peek() == '-') pos_++;

        if (pos_ >= input_.size()) {
            err = "invalid number format";
            return false;
        }

        if (peek() == '0') {
            pos_++;
        } else if (std::isdigit(static_cast<unsigned char>(peek()))) {
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(peek()))) {
                pos_++;
            }
        } else {
            err = "invalid number format";
            return false;
        }

        if (peek() == '.') {
            pos_++;
            if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                err = "invalid fraction in number";
                return false;
            }
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(peek()))) {
                pos_++;
            }
        }

        if (peek() == 'e' || peek() == 'E') {
            pos_++;
            if (peek() == '+' || peek() == '-') pos_++;
            if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                err = "invalid exponent in number";
                return false;
            }
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(peek()))) {
                pos_++;
            }
        }

        std::string num_str(input_.substr(start, pos_ - start));
        char* end_p = nullptr;
        double d = std::strtod(num_str.c_str(), &end_p);
        val.type = JsonType::Number;
        val.num_val = d;
        return true;
    }

    bool parse_bool(JsonValue& val, std::string& err) {
        if (input_.substr(pos_).rfind("true", 0) == 0) {
            pos_ += 4;
            val.type = JsonType::Boolean;
            val.bool_val = true;
            return true;
        }
        if (input_.substr(pos_).rfind("false", 0) == 0) {
            pos_ += 5;
            val.type = JsonType::Boolean;
            val.bool_val = false;
            return true;
        }
        err = "invalid boolean literal";
        return false;
    }

    bool parse_null(JsonValue& val, std::string& err) {
        if (input_.substr(pos_).rfind("null", 0) == 0) {
            pos_ += 4;
            val.type = JsonType::Null;
            return true;
        }
        err = "invalid null literal";
        return false;
    }

    std::string_view input_;
    size_t pos_{0};
    size_t depth_{0};
};

bool parse_json(std::string_view input, JsonValue& out_value, std::string& out_error) {
    StrictJsonParser parser(input);
    return parser.parse(out_value, out_error);
}

void merge_json(JsonValue& target, const JsonValue& patch) {
    if (patch.type != JsonType::Object) {
        target = patch;
        return;
    }
    if (target.type != JsonType::Object) {
        target = patch;
        return;
    }

    for (const auto& kv : patch.obj_val) {
        JsonValue* existing = target.get_mut(kv.first);
        if (existing) {
            if (existing->type == JsonType::Object && kv.second.type == JsonType::Object) {
                merge_json(*existing, kv.second);
            } else {
                *existing = kv.second;
            }
        } else {
            target.set(kv.first, kv.second);
        }
    }
}

// =============================================================================
// Model Manifest Parsers
// =============================================================================

KubeNode parse_node_manifest(const std::string& json_str) {
    KubeNode node;
    JsonValue doc;
    std::string err;
    if (!parse_json(json_str, doc, err) || !doc.is_object()) {
        return node;
    }

    const JsonValue* meta = doc.get("metadata");
    if (meta && meta->is_object()) {
        node.name = meta->get_string("name", "");
        node.age_seconds = age_seconds_since(meta->get_string("creationTimestamp", ""));
        const JsonValue* labels = meta->get("labels");
        if (labels && labels->is_object()) {
            for (const auto& kv : labels->obj_val) {
                if (kv.second.is_string()) {
                    node.labels[kv.first] = kv.second.str_val;
                }
            }
            if (node.labels.count("node-role.kubernetes.io/control-plane") ||
                node.labels.count("node-role.kubernetes.io/master")) {
                node.roles = "control-plane";
            } else if (node.labels.count("node-role.kubernetes.io/worker")) {
                node.roles = "worker";
            } else {
                node.roles = "worker";
            }
        }
    }

    const JsonValue* status = doc.get("status");
    if (status && status->is_object()) {
        const JsonValue* conds = status->get("conditions");
        if (conds && conds->is_array()) {
            node.is_ready = false;
            for (const auto& c : conds->arr_val) {
                if (c.is_object() && c.get_string("type") == "Ready" && c.get_string("status") == "True") {
                    node.is_ready = true;
                    break;
                }
            }
        }

        const JsonValue* addrs = status->get("addresses");
        if (addrs && addrs->is_array()) {
            for (const auto& a : addrs->arr_val) {
                if (a.is_object()) {
                    std::string atype = a.get_string("type");
                    std::string aaddr = a.get_string("address");
                    if (atype == "InternalIP") node.internal_ip = aaddr;
                    else if (atype == "ExternalIP") node.external_ip = aaddr;
                }
            }
        }

        const JsonValue* ninfo = status->get("nodeInfo");
        if (ninfo && ninfo->is_object()) {
            node.version = ninfo->get_string("kubeletVersion", "");
            node.os_image = ninfo->get_string("osImage", "");
            node.kernel_version = ninfo->get_string("kernelVersion", "");
            node.container_runtime = ninfo->get_string("containerRuntimeVersion", "");
        }
    }

    return node;
}

bool has_owner(const JsonValue& obj, const std::string& kind, const std::string& uid) {
    const JsonValue* meta = obj.get("metadata");
    const JsonValue* refs = meta ? meta->get("ownerReferences") : nullptr;
    if (!refs || !refs->is_array()) return false;
    for (const auto& r : refs->arr_val) {
        if (r.get_string("kind", "") == kind && (uid.empty() || r.get_string("uid", "") == uid)) return true;
    }
    return false;
}

KubePod parse_pod_manifest(const std::string& json_str) {
    KubePod pod;
    JsonValue doc;
    std::string err;
    if (!parse_json(json_str, doc, err) || !doc.is_object()) {
        return pod;
    }

    const JsonValue* meta = doc.get("metadata");
    if (meta && meta->is_object()) {
        pod.name = meta->get_string("name", "");
        pod.namespace_name = meta->get_string("namespace", "default");
        pod.uid = meta->get_string("uid", "");
        pod.deletion_timestamp = meta->get_string("deletionTimestamp", "");
        pod.age_seconds = age_seconds_since(meta->get_string("creationTimestamp", ""));
    }

    if (meta && meta->is_object()) {
        const JsonValue* labels = meta->get("labels");
        if (labels && labels->is_object()) {
            for (const auto& kv : labels->obj_val) {
                if (kv.second.is_string()) pod.labels[kv.first] = kv.second.str_val;
            }
        }
        const JsonValue* ann = meta->get("annotations");
        if (ann && ann->is_object()) {
            for (const auto& kv : ann->obj_val) {
                if (kv.second.is_string()) pod.annotations[kv.first] = kv.second.str_val;
            }
        }
    }

    const JsonValue* spec = doc.get("spec");
    if (spec && spec->is_object()) {
        pod.node_name = spec->get_string("nodeName", "<none>");
        pod.restart_policy = spec->get_string("restartPolicy", "Always");
        const JsonValue* sel = spec->get("nodeSelector");
        if (sel && sel->is_object()) {
            for (const auto& kv : sel->obj_val) {
                if (kv.second.is_string()) pod.node_selector[kv.first] = kv.second.str_val;
            }
        }
        const JsonValue* containers = spec->get("containers");
        if (containers && containers->is_array()) {
            pod.total_containers = static_cast<uint32_t>(containers->arr_val.size());
            uint64_t total_req = 0;
            uint64_t total_lim = 0;
            for (size_t ci = 0; ci < containers->arr_val.size(); ++ci) {
                const JsonValue& c = containers->arr_val[ci];
                if (!c.is_object()) continue;
                if (ci == 0) {
                    pod.image = c.get_string("image", "");
                    pod.container_name = c.get_string("name", "");
                    const JsonValue* env = c.get("env");
                    if (env && env->is_array()) {
                        for (const auto& e : env->arr_val) {
                            if (!e.is_object()) continue;
                            std::string name = e.get_string("name", "");
                            if (name.empty()) continue;
                            const JsonValue* v = e.get("value");
                            std::string value;
                            if (v) {
                                if (v->is_string()) value = v->str_val;
                                else if (v->is_number()) value = std::to_string(static_cast<long long>(v->num_val));
                                else if (v->is_bool()) value = v->bool_val ? "true" : "false";
                            }
                            pod.env.emplace_back(name, value);
                        }
                    }
                }
                const JsonValue* res = c.get("resources");
                if (!res || !res->is_object()) continue;
                const JsonValue* req = res->get("requests");
                const JsonValue* lim = res->get("limits");
                if (req && req->is_object()) {
                    total_req += parse_quantity(req->get_string("memory", ""));
                    pod.psram_request_bytes += parse_quantity(req->get_string("esp32.io/psram", req->get_string("esp.io/psram", "")));
                    pod.gpio_request += static_cast<uint32_t>(parse_quantity(req->get_string("esp32.io/gpio", req->get_string("esp.io/gpio", ""))));
                }
                if (lim && lim->is_object()) {
                    total_lim += parse_quantity(lim->get_string("memory", ""));
                }
            }
            if (total_req > 0) {
                pod.memory_request_bytes = total_req;
                pod.memory_request_set = true;
            }
            pod.memory_limit_bytes = total_lim;
            if (pod.memory_request_bytes == 16 * 1024 && total_req == 0 && total_lim > 0) {
                pod.memory_request_bytes = total_lim;
            }
        }
    }

    const JsonValue* status = doc.get("status");
    if (status && status->is_object()) {
        pod.status = status->get_string("phase", "Pending");
        pod.phase = pod.status;
        // kubectl's pod printer: a waiting/terminated container reason wins
        // over the phase, and a deletionTimestamp shows as Terminating.
        const JsonValue* cst = status->get("containerStatuses");
        if (cst && cst->is_array()) {
            for (const auto& cs : cst->arr_val) {
                const JsonValue* state = cs.get("state");
                if (!state) continue;
                if (const JsonValue* w = state->get("waiting")) {
                    std::string r = w->get_string("reason", "");
                    if (!r.empty()) pod.status = r;
                } else if (const JsonValue* t = state->get("terminated")) {
                    std::string r = t->get_string("reason", "");
                    if (!r.empty() && pod.status != "Running") pod.status = r;
                }
            }
        }
        if (!pod.deletion_timestamp.empty() && pod.status != "Succeeded" && pod.status != "Failed") pod.status = "Terminating";
        pod.ip = status->get_string("podIP", "<none>");
        const JsonValue* cstatuses = status->get("containerStatuses");
        if (cstatuses && cstatuses->is_array()) {
            uint32_t ready_c = 0;
            uint32_t restarts = 0;
            for (const auto& cs : cstatuses->arr_val) {
                if (cs.is_object()) {
                    if (cs.get_bool("ready", false)) ready_c++;
                    restarts += static_cast<uint32_t>(cs.get_int("restartCount", 0));
                }
            }
            pod.ready_containers = ready_c;
            pod.restart_count = restarts;
        } else {
            pod.ready_containers = (pod.status == "Running") ? pod.total_containers : 0;
        }
    }

    return pod;
}

KubeDeployment parse_deployment_manifest(const std::string& json_str) {
    KubeDeployment deploy;
    JsonValue doc;
    std::string err;
    if (!parse_json(json_str, doc, err) || !doc.is_object()) {
        return deploy;
    }

    const JsonValue* meta = doc.get("metadata");
    if (meta && meta->is_object()) {
        deploy.name = meta->get_string("name", "");
        deploy.namespace_name = meta->get_string("namespace", "default");
        deploy.age_seconds = age_seconds_since(meta->get_string("creationTimestamp", ""));
    }

    const JsonValue* spec = doc.get("spec");
    if (spec && spec->is_object()) {
        deploy.replicas = static_cast<uint32_t>(spec->get_int("replicas", 1));
        const JsonValue* tmpl = spec->get("template");
        if (tmpl && tmpl->is_object()) {
            const JsonValue* tspec = tmpl->get("spec");
            if (tspec && tspec->is_object()) {
                const JsonValue* containers = tspec->get("containers");
                if (containers && containers->is_array() && !containers->arr_val.empty()) {
                    const auto& c0 = containers->arr_val[0];
                    if (c0.is_object()) {
                        deploy.containers = c0.get_string("name", "app");
                        deploy.images = c0.get_string("image", "wasm://app");
                    }
                }
            }
        }
        const JsonValue* sel = spec->get("selector");
        if (sel && sel->is_object()) {
            const JsonValue* ml = sel->get("matchLabels");
            if (ml && ml->is_object() && !ml->obj_val.empty()) {
                deploy.selector = ml->obj_val[0].first + "=" + ml->obj_val[0].second.str_val;
            }
        }
    }

    const JsonValue* status = doc.get("status");
    if (status && status->is_object()) {
        deploy.ready_replicas = static_cast<uint32_t>(status->get_int("readyReplicas", 0));
        deploy.updated_replicas = static_cast<uint32_t>(status->get_int("updatedReplicas", 0));
        deploy.available_replicas = static_cast<uint32_t>(status->get_int("availableReplicas", 0));
    }

    return deploy;
}


// =============================================================================
// Time and identity helpers
// =============================================================================

static void civil_from_days(int64_t z, int& y, unsigned& m, unsigned& d) {
    // Howard Hinnant's days-from-civil inverse (proleptic Gregorian, UTC)
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2 ? 1 : 0));
}

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

std::string format_rfc3339(uint64_t unix_ms) {
    int64_t secs = static_cast<int64_t>(unix_ms / 1000ULL);
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    int y; unsigned m, d;
    civil_from_days(days, y, m, d);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02lld:%02lld:%02lldZ", y, m, d,
             static_cast<long long>(rem / 3600), static_cast<long long>((rem % 3600) / 60),
             static_cast<long long>(rem % 60));
    return buf;
}

std::string now_rfc3339() {
    uint64_t now = osal::System::get_unix_time_ms();
    return now == 0 ? std::string() : format_rfc3339(now);
}

bool parse_rfc3339(std::string_view t, uint64_t& out_unix_ms) {
    // Minimal strict parser: YYYY-MM-DDTHH:MM:SS(.fraction)?(Z|+00:00|-00:00)
    if (t.size() < 20) return false;
    auto num = [&](size_t off, size_t len, int& out) {
        int v = 0;
        for (size_t i = off; i < off + len; ++i) {
            if (i >= t.size() || t[i] < '0' || t[i] > '9') return false;
            v = v * 10 + (t[i] - '0');
        }
        out = v;
        return true;
    };
    int y, mo, d, h, mi, s;
    if (!num(0, 4, y) || t[4] != '-' || !num(5, 2, mo) || t[7] != '-' || !num(8, 2, d) ||
        (t[10] != 'T' && t[10] != 't' && t[10] != ' ') || !num(11, 2, h) || t[13] != ':' ||
        !num(14, 2, mi) || t[16] != ':' || !num(17, 2, s)) return false;
    size_t pos = 19;
    uint64_t frac_ms = 0;
    if (pos < t.size() && t[pos] == '.') {
        ++pos;
        unsigned digits = 0;
        while (pos < t.size() && t[pos] >= '0' && t[pos] <= '9') {
            if (digits < 3) { frac_ms = frac_ms * 10 + static_cast<uint64_t>(t[pos] - '0'); ++digits; }
            ++pos;
        }
        while (digits < 3) { frac_ms *= 10; ++digits; }
    }
    int64_t offset_s = 0;
    if (pos < t.size() && (t[pos] == 'Z' || t[pos] == 'z')) {
        ++pos;
    } else if (pos + 6 <= t.size() && (t[pos] == '+' || t[pos] == '-')) {
        int oh, om;
        if (!num(pos + 1, 2, oh) || t[pos + 3] != ':' || !num(pos + 4, 2, om)) return false;
        offset_s = (oh * 3600 + om * 60) * (t[pos] == '+' ? 1 : -1);
        pos += 6;
    } else {
        return false;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return false;
    int64_t days = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    int64_t secs = days * 86400 + h * 3600 + mi * 60 + s - offset_s;
    if (secs < 0) return false;
    out_unix_ms = static_cast<uint64_t>(secs) * 1000ULL + frac_ms;
    return true;
}

uint64_t age_seconds_since(std::string_view ts) {
    uint64_t now = osal::System::get_unix_time_ms();
    uint64_t created = 0;
    if (now == 0 || ts.empty() || !parse_rfc3339(ts, created)) return AGE_UNKNOWN;
    return now >= created ? (now - created) / 1000ULL : 0;
}

std::string generate_uid() {
    uint32_t words[4];
#if defined(ESP_PLATFORM)
    for (auto& w : words) w = esp_random();
#else
    std::random_device rd;
    for (auto& w : words) w = rd();
#endif
    unsigned char b[16];
    for (int i = 0; i < 4; ++i) {
        b[i * 4 + 0] = static_cast<unsigned char>(words[i] >> 24);
        b[i * 4 + 1] = static_cast<unsigned char>(words[i] >> 16);
        b[i * 4 + 2] = static_cast<unsigned char>(words[i] >> 8);
        b[i * 4 + 3] = static_cast<unsigned char>(words[i]);
    }
    b[6] = static_cast<unsigned char>((b[6] & 0x0f) | 0x40); // version 4
    b[8] = static_cast<unsigned char>((b[8] & 0x3f) | 0x80); // RFC 4122 variant
    char out[37];
    snprintf(out, sizeof(out), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return out;
}

} // namespace kube32::models
