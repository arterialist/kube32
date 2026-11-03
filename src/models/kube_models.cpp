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
    if (!v || !v->is_number() || std::isnan(v->num_val)) return def;
    // Numbers outside int64_t clamp to its range: casting them is undefined.
    if (v->num_val >= 9223372036854775808.0) return INT64_MAX;
    if (v->num_val < -9223372036854775808.0) return INT64_MIN;
    return static_cast<int64_t>(v->num_val);
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

// Bytes escape_json_string() appends for `s`
static size_t escaped_size(std::string_view s) {
    size_t n = 2;
    for (char c : s) {
        switch (c) {
            case '"': case '\\': case '\b': case '\f': case '\n': case '\r': case '\t': n += 2; break;
            default: n += static_cast<unsigned char>(c) < 0x20 ? 6 : 1; break;
        }
    }
    return n;
}

static size_t format_number(double v, char* buf, size_t cap) {
    int n;
    if (v == static_cast<int64_t>(v)) {
        n = snprintf(buf, cap, "%lld", static_cast<long long>(v));
    } else {
        n = snprintf(buf, cap, "%.6g", v);
    }
    return n > 0 ? static_cast<size_t>(n) : 0;
}

// The text is written into one string, sized first: a document costs one
// allocation, not one per member and level
static size_t serialized_size(const JsonValue& v, bool pretty, int level) {
    switch (v.type) {
        case JsonType::Null: return 4;
        case JsonType::Boolean: return v.bool_val ? 4 : 5;
        case JsonType::Number: {
            char buf[64];
            return format_number(v.num_val, buf, sizeof(buf));
        }
        case JsonType::String: return escaped_size(v.str_val);
        case JsonType::Array: {
            if (v.arr_val.empty()) return 2;
            size_t n = 2 + (v.arr_val.size() - 1);
            if (pretty) n += 1 + v.arr_val.size() * (1 + (level + 1) * 2) + level * 2;
            for (const auto& item : v.arr_val) n += serialized_size(item, pretty, level + 1);
            return n;
        }
        case JsonType::Object: {
            if (v.obj_val.empty()) return 2;
            size_t n = 2 + (v.obj_val.size() - 1) + v.obj_val.size();
            if (pretty) n += 1 + v.obj_val.size() * (2 + (level + 1) * 2) + level * 2;
            for (const auto& kv : v.obj_val) n += escaped_size(kv.first) + serialized_size(kv.second, pretty, level + 1);
            return n;
        }
    }
    return 0;
}

static void write_json(const JsonValue& v, std::string& out, bool pretty, int level) {
    auto indent = [&](int l) { if (pretty) out.append(static_cast<size_t>(l) * 2, ' '); };
    switch (v.type) {
        case JsonType::Null:
            out += "null";
            break;
        case JsonType::Boolean:
            out += v.bool_val ? "true" : "false";
            break;
        case JsonType::Number: {
            char buf[64];
            out.append(buf, format_number(v.num_val, buf, sizeof(buf)));
            break;
        }
        case JsonType::String:
            escape_json_string(v.str_val, out);
            break;
        case JsonType::Array:
            if (v.arr_val.empty()) {
                out += "[]";
                break;
            }
            out += '[';
            if (pretty) out += '\n';
            for (size_t i = 0; i < v.arr_val.size(); ++i) {
                indent(level + 1);
                write_json(v.arr_val[i], out, pretty, level + 1);
                if (i + 1 < v.arr_val.size()) out += ',';
                if (pretty) out += '\n';
            }
            indent(level);
            out += ']';
            break;
        case JsonType::Object:
            if (v.obj_val.empty()) {
                out += "{}";
                break;
            }
            out += '{';
            if (pretty) out += '\n';
            for (size_t i = 0; i < v.obj_val.size(); ++i) {
                indent(level + 1);
                escape_json_string(v.obj_val[i].first, out);
                out += ':';
                if (pretty) out += ' ';
                write_json(v.obj_val[i].second, out, pretty, level + 1);
                if (i + 1 < v.obj_val.size()) out += ',';
                if (pretty) out += '\n';
            }
            indent(level);
            out += '}';
            break;
    }
}

std::string JsonValue::to_string(bool pretty, int indent_level) const {
    std::string out;
    out.reserve(serialized_size(*this, pretty, indent_level));
    write_json(*this, out, pretty, indent_level);
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

        // The text decodes to at most its raw length: the string is
        // allocated once, at that size, not grown a character at a time
        size_t end = pos_;
        bool escapes = false;
        bool control = false;
        while (end < input_.size() && input_[end] != '"') {
            if (input_[end] == '\\') {
                escapes = true;
                end += 2;
                continue;
            }
            if (static_cast<unsigned char>(input_[end]) < 0x20) control = true;
            ++end;
        }
        if (end < input_.size() && !control) {
            if (!escapes) {
                val.str_val.assign(input_.data() + pos_, end - pos_);
                pos_ = end + 1;
                return true;
            }
            val.str_val.reserve(end - pos_);
        }

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
        target = JsonValue();
        target.type = JsonType::Object;
    }

    for (const auto& kv : patch.obj_val) {
        if (kv.second.is_null()) {
            target.remove(kv.first);
            continue;
        }
        JsonValue* existing = target.get_mut(kv.first);
        if (existing) {
            if (existing->type == JsonType::Object && kv.second.type == JsonType::Object) {
                merge_json(*existing, kv.second);
            } else {
                *existing = kv.second;
            }
        } else {
            JsonValue fresh;
            merge_json(fresh, kv.second);   // drops nulls inside a new member
            target.set(kv.first, std::move(fresh));
        }
    }
}

namespace {

constexpr std::string_view kSetElementOrder = "$setElementOrder/";
constexpr std::string_view kDeleteFromPrimitiveList = "$deleteFromPrimitiveList/";

bool starts_with(const std::string& s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool is_directive(const std::string& key) {
    return key == "$patch" || key == "$retainKeys" || starts_with(key, kSetElementOrder) || starts_with(key, kDeleteFromPrimitiveList);
}

// A patch value without its directives, for members the target does not have
JsonValue strip_directives(const JsonValue& v) {
    if (v.is_object()) {
        JsonValue out;
        out.type = JsonType::Object;
        for (const auto& kv : v.obj_val) {
            if (is_directive(kv.first) || kv.second.is_null()) continue;
            out.obj_val.emplace_back(kv.first, strip_directives(kv.second));
        }
        return out;
    }
    if (v.is_array()) {
        JsonValue out;
        out.type = JsonType::Array;
        for (const auto& e : v.arr_val) {
            if (e.is_object() && e.get("$patch")) continue;
            out.arr_val.push_back(strip_directives(e));
        }
        return out;
    }
    return v;
}

// The merge key of a list field (patchMergeKey in the Kubernetes API types),
// or nullptr for a list that a patch replaces whole. Fields are matched by
// name: the names below mean the same list wherever they appear.
const char* list_merge_key(const std::string& field, const JsonValue& patch_list) {
    if (field == "containers" || field == "initContainers" || field == "ephemeralContainers" || field == "env" ||
        field == "volumes" || field == "imagePullSecrets") {
        return "name";
    }
    if (field == "ports") {
        // Container ports merge by containerPort, Service ports by port
        for (const auto& e : patch_list.arr_val) {
            if (e.is_object() && e.get("containerPort")) return "containerPort";
        }
        return "port";
    }
    if (field == "volumeMounts") return "mountPath";
    if (field == "volumeDevices") return "devicePath";
    if (field == "hostAliases") return "ip";
    if (field == "conditions") return "type";
    if (field == "ownerReferences") return "uid";
    if (field == "topologySpreadConstraints") return "topologyKey";
    return nullptr;
}

bool same_value(const JsonValue& a, const JsonValue& b) {
    return a.type == b.type && a.to_string() == b.to_string();
}

void merge_list(const std::string& field, JsonValue& target, const JsonValue& patch) {
    for (const auto& e : patch.arr_val) {
        if (e.is_object() && e.get_string("$patch") == "replace") {
            target = strip_directives(patch);
            return;
        }
    }
    const char* key = list_merge_key(field, patch);
    if (!key) {
        if (field == "finalizers") {
            // A list of strings that merges: what the patch adds is kept with the rest
            for (const auto& e : patch.arr_val) {
                bool have = false;
                for (const auto& t : target.arr_val) have = have || same_value(t, e);
                if (!have) target.arr_val.push_back(e);
            }
        } else {
            target = strip_directives(patch);
        }
        return;
    }
    for (const auto& e : patch.arr_val) {
        const JsonValue* k = e.is_object() ? e.get(key) : nullptr;
        if (!k) {
            if (!(e.is_object() && e.get("$patch"))) target.arr_val.push_back(strip_directives(e));
            continue;
        }
        auto it = target.arr_val.begin();
        for (; it != target.arr_val.end(); ++it) {
            const JsonValue* tk = it->is_object() ? it->get(key) : nullptr;
            if (tk && same_value(*tk, *k)) break;
        }
        if (e.get_string("$patch") == "delete") {
            if (it != target.arr_val.end()) target.arr_val.erase(it);
        } else if (it != target.arr_val.end()) {
            strategic_merge_json(*it, e);
        } else {
            target.arr_val.push_back(strip_directives(e));
        }
    }
}

// $setElementOrder/<field>: the list in the order the patch gives, members
// it does not name after them
void order_list(JsonValue& list, const JsonValue& order, const char* key) {
    if (!list.is_array() || !order.is_array()) return;
    std::vector<JsonValue> rest = std::move(list.arr_val);
    list.arr_val.clear();
    for (const auto& o : order.arr_val) {
        const JsonValue* want = (key && o.is_object()) ? o.get(key) : &o;
        if (!want) continue;
        for (auto it = rest.begin(); it != rest.end(); ++it) {
            const JsonValue* have = (key && it->is_object()) ? it->get(key) : &*it;
            if (have && same_value(*have, *want)) {
                list.arr_val.push_back(std::move(*it));
                rest.erase(it);
                break;
            }
        }
    }
    for (auto& r : rest) list.arr_val.push_back(std::move(r));
}

} // namespace

void strategic_merge_json(JsonValue& target, const JsonValue& patch) {
    if (!patch.is_object()) {
        target = strip_directives(patch);
        return;
    }
    if (patch.get_string("$patch") == "replace" || !target.is_object()) {
        target = strip_directives(patch);
        return;
    }
    for (const auto& kv : patch.obj_val) {
        if (is_directive(kv.first)) continue;
        if (kv.second.is_null() || (kv.second.is_object() && kv.second.get_string("$patch") == "delete")) {
            target.remove(kv.first);
            continue;
        }
        JsonValue* existing = target.get_mut(kv.first);
        if (!existing) {
            target.set(kv.first, strip_directives(kv.second));
        } else if (existing->is_object() && kv.second.is_object()) {
            strategic_merge_json(*existing, kv.second);
        } else if (existing->is_array() && kv.second.is_array()) {
            merge_list(kv.first, *existing, kv.second);
        } else {
            *existing = strip_directives(kv.second);
        }
    }
    for (const auto& kv : patch.obj_val) {
        if (starts_with(kv.first, kDeleteFromPrimitiveList)) {
            JsonValue* list = target.get_mut(kv.first.substr(kDeleteFromPrimitiveList.size()));
            if (!list || !list->is_array() || !kv.second.is_array()) continue;
            auto& a = list->arr_val;
            for (const auto& gone : kv.second.arr_val) {
                for (auto it = a.begin(); it != a.end();) it = same_value(*it, gone) ? a.erase(it) : it + 1;
            }
        } else if (starts_with(kv.first, kSetElementOrder)) {
            const std::string field = kv.first.substr(kSetElementOrder.size());
            JsonValue* list = target.get_mut(field);
            if (list) order_list(*list, kv.second, list_merge_key(field, kv.second));
        }
    }
    if (const JsonValue* retain = patch.get("$retainKeys")) {
        if (retain->is_array()) {
            auto& members = target.obj_val;
            for (auto it = members.begin(); it != members.end();) {
                bool keep = false;
                for (const auto& r : retain->arr_val) keep = keep || (r.is_string() && r.str_val == it->first);
                it = keep ? it + 1 : members.erase(it);
            }
        }
    }
}

int apply_patch(JsonValue& target, const JsonValue& patch, std::string_view content_type, std::string* error) {
    if (is_json_patch(content_type)) {
        std::string why;
        const int status = apply_json_patch(target, patch, why);
        if (error) *error = std::move(why);
        return status;
    }
    if (content_type.rfind("application/strategic-merge-patch+json", 0) == 0) strategic_merge_json(target, patch);
    else merge_json(target, patch);
    return 0;
}

static const JsonValue* condition_of_type(const JsonValue* conditions, const std::string& type) {
    if (!conditions || !conditions->is_array()) return nullptr;
    for (const auto& c : conditions->arr_val) {
        if (c.is_object() && c.get_string("type", "") == type) return &c;
    }
    return nullptr;
}

const JsonValue* node_conditions(const JsonValue& node) {
    const JsonValue* status = node.get("status");
    const JsonValue* conditions = status ? status->get("conditions") : nullptr;
    return conditions && conditions->is_array() ? conditions : nullptr;
}

void settle_node_condition_times(JsonValue& node, const JsonValue* stored, const JsonValue* sent, uint64_t now_ms) {
    JsonValue* status = node.get_mut("status");
    JsonValue* conditions = status ? status->get_mut("conditions") : nullptr;
    if (!conditions || !conditions->is_array() || !sent || !sent->is_array()) return;
    const std::string now = now_ms ? format_rfc3339(now_ms) : std::string();
    auto set_time = [&](JsonValue& c, const char* field, const std::string& value) {
        if (value.empty()) c.remove(field);
        else c.set(field, JsonValue{JsonType::String, false, 0, value, {}, {}});
    };
    for (auto& c : conditions->arr_val) {
        if (!c.is_object()) continue;
        const std::string type = c.get_string("type", "");
        const JsonValue* written = condition_of_type(sent, type);
        if (!written) continue;   // not part of this write: as it was
        if (written->get_string("lastHeartbeatTime", "").empty()) set_time(c, "lastHeartbeatTime", now);
        const JsonValue* old = condition_of_type(stored, type);
        const std::string old_since = old ? old->get_string("lastTransitionTime", "") : std::string();
        if (old && old->get_string("status", "") == c.get_string("status", "")) {
            if (!old_since.empty()) set_time(c, "lastTransitionTime", old_since);
            continue;
        }
        // The status changed: the writer's time when it saw the change, which
        // is after the stored one; otherwise the change is happening now
        const std::string since = written->get_string("lastTransitionTime", "");
        uint64_t since_ms = 0, old_ms = 0;
        const bool newer = !since.empty() && parse_rfc3339(since, since_ms) &&
                           (old_since.empty() || !parse_rfc3339(old_since, old_ms) || since_ms > old_ms);
        set_time(c, "lastTransitionTime", newer ? since : now);
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

uint16_t node_pod_port(const JsonValue& node) {
    const JsonValue* st = node.get("status");
    const JsonValue* de = st ? st->get("daemonEndpoints") : nullptr;
    const JsonValue* ke = de ? de->get("kubeletEndpoint") : nullptr;
    const int64_t port = ke ? ke->get_int("Port", 0) : 0;
    return port > 0 && port <= 65535 ? static_cast<uint16_t>(port) : 0;
}

std::map<std::string, uint16_t> node_pod_ports(const std::vector<std::string>& node_jsons) {
    std::map<std::string, uint16_t> out;
    for (const auto& json : node_jsons) {
        JsonValue doc;
        std::string err;
        if (!parse_json(json, doc, err) || !doc.is_object()) continue;
        const JsonValue* meta = doc.get("metadata");
        const uint16_t port = node_pod_port(doc);
        if (meta && port) out[meta->get_string("name", "")] = port;
    }
    return out;
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
        pod.start_time = status->get_string("startTime", "");
        const JsonValue* first = (cstatuses && cstatuses->is_array() && !cstatuses->arr_val.empty() && cstatuses->arr_val[0].is_object())
                                     ? &cstatuses->arr_val[0] : nullptr;
        if (first) {
            pod.first_restart_count = static_cast<uint32_t>(first->get_int("restartCount", 0));
            const JsonValue* state = first->get("state");
            const JsonValue* last = first->get("lastState");
            const JsonValue* running = state ? state->get("running") : nullptr;
            const JsonValue* waiting = state ? state->get("waiting") : nullptr;
            const JsonValue* ended = state ? state->get("terminated") : nullptr;
            if (!ended && last) ended = last->get("terminated");
            if (running && running->is_object()) {
                pod.container_running = true;
                pod.container_started_at = running->get_string("startedAt", "");
            }
            if (ended && ended->is_object()) {
                pod.has_last_state = true;
                pod.last_exit_code = static_cast<int32_t>(ended->get_int("exitCode", 0));
                pod.last_reason = ended->get_string("reason", "");
                pod.last_message = ended->get_string("message", "");
                pod.last_started_at = ended->get_string("startedAt", "");
                pod.last_finished_at = ended->get_string("finishedAt", "");
            }
            pod.container_ran = pod.container_running || pod.has_last_state || pod.first_restart_count > 0 ||
                                (waiting && waiting->get_string("reason", "") == "CrashLoopBackOff");
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

size_t format_rfc3339(uint64_t unix_ms, char* out, size_t cap) {
    int64_t secs = static_cast<int64_t>(unix_ms / 1000ULL);
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;
    int y; unsigned m, d;
    civil_from_days(days, y, m, d);
    int n = snprintf(out, cap, "%04d-%02u-%02uT%02lld:%02lld:%02lldZ", y, m, d,
                     static_cast<long long>(rem / 3600), static_cast<long long>((rem % 3600) / 60),
                     static_cast<long long>(rem % 60));
    return (n < 0 || static_cast<size_t>(n) >= cap) ? 0 : static_cast<size_t>(n);
}

std::string format_rfc3339(uint64_t unix_ms) {
    char buf[32];
    return std::string(buf, format_rfc3339(unix_ms, buf, sizeof(buf)));
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

std::string str_cat(std::initializer_list<std::string_view> parts) {
    size_t n = 0;
    for (const auto& p : parts) n += p.size();
    std::string out;
    out.reserve(n);
    for (const auto& p : parts) out.append(p.data(), p.size());
    return out;
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
