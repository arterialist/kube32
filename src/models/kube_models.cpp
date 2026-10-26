#include "models/kube_models.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>

namespace kube32::models {

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
            node.version = ninfo->get_string("kubeletVersion", "v1.33.0-kube32");
            node.os_image = ninfo->get_string("osImage", "ESP-IDF v5.2");
            node.kernel_version = ninfo->get_string("kernelVersion", "FreeRTOS-v10.4");
            node.container_runtime = ninfo->get_string("containerRuntimeVersion", "kube32-runtime://0.1.0");
        }
    }

    return node;
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
    }

    const JsonValue* spec = doc.get("spec");
    if (spec && spec->is_object()) {
        pod.node_name = spec->get_string("nodeName", "<none>");
        const JsonValue* containers = spec->get("containers");
        if (containers && containers->is_array()) {
            pod.total_containers = static_cast<uint32_t>(containers->arr_val.size());
        }
    }

    const JsonValue* status = doc.get("status");
    if (status && status->is_object()) {
        pod.status = status->get_string("phase", "Pending");
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

} // namespace kube32::models
