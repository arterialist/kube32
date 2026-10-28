#include "api/selectors.hpp"

#include <cctype>
#include <cstdlib>

namespace kube32::api {

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < s.size() && hex_val(s[i + 1]) >= 0 && hex_val(s[i + 2]) >= 0) {
            out += static_cast<char>(hex_val(s[i + 1]) * 16 + hex_val(s[i + 2]));
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

bool query_param(std::string_view query, std::string_view key, std::string& out) {
    while (!query.empty()) {
        size_t amp = query.find('&');
        std::string_view token = (amp == std::string_view::npos) ? query : query.substr(0, amp);
        size_t eq = token.find('=');
        std::string_view k = (eq == std::string_view::npos) ? token : token.substr(0, eq);
        if (k == key) {
            out = (eq == std::string_view::npos) ? std::string() : url_decode(token.substr(eq + 1));
            return true;
        }
        if (amp == std::string_view::npos) break;
        query = query.substr(amp + 1);
    }
    return false;
}

bool query_flag(std::string_view query, std::string_view key) {
    std::string v;
    return query_param(query, key, v) && (v == "true" || v == "1");
}

bool query_uint(std::string_view query, std::string_view key, uint64_t& out) {
    std::string v;
    if (!query_param(query, key, v) || v.empty()) return false;
    for (char c : v) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    out = std::strtoull(v.c_str(), nullptr, 10);
    return true;
}

static std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// Splits on commas that are not inside parentheses (label "in (a,b)" sets).
static std::vector<std::string_view> split_top_level(std::string_view s) {
    std::vector<std::string_view> parts;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') { if (depth > 0) --depth; }
        else if (s[i] == ',' && depth == 0) {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(s.substr(start));
    return parts;
}

static bool parse_field_selector(std::string_view fs, std::vector<FieldRequirement>& out, std::string& err) {
    for (std::string_view part : split_top_level(fs)) {
        part = trim(part);
        if (part.empty()) continue;
        size_t ne = part.find("!=");
        size_t eq = part.find('=');
        FieldRequirement r;
        if (ne != std::string_view::npos && (eq == std::string_view::npos || ne <= eq)) {
            r.path = std::string(trim(part.substr(0, ne)));
            r.equals = false;
            r.value = std::string(trim(part.substr(ne + 2)));
        } else if (eq != std::string_view::npos) {
            size_t vstart = (eq + 1 < part.size() && part[eq + 1] == '=') ? eq + 2 : eq + 1;
            r.path = std::string(trim(part.substr(0, eq)));
            r.equals = true;
            r.value = std::string(trim(part.substr(vstart)));
        } else {
            err = "invalid field selector: " + std::string(part);
            return false;
        }
        if (r.path.empty()) { err = "invalid field selector: empty field"; return false; }
        out.push_back(std::move(r));
    }
    return true;
}

static bool parse_label_selector(std::string_view ls, std::vector<LabelRequirement>& out, std::string& err) {
    for (std::string_view part : split_top_level(ls)) {
        part = trim(part);
        if (part.empty()) continue;
        LabelRequirement r;
        size_t ne = part.find("!=");
        size_t eq = part.find('=');
        size_t paren = part.find('(');
        if (paren != std::string_view::npos) {
            size_t close = part.find(')', paren);
            if (close == std::string_view::npos) { err = "invalid label selector: " + std::string(part); return false; }
            std::string_view head = trim(part.substr(0, paren));
            std::string_view set = part.substr(paren + 1, close - paren - 1);
            size_t sp = head.find_last_of(" \t");
            if (sp == std::string_view::npos) { err = "invalid label selector: " + std::string(part); return false; }
            std::string_view op = trim(head.substr(sp + 1));
            r.key = std::string(trim(head.substr(0, sp)));
            if (op == "in") r.op = LabelRequirement::Op::In;
            else if (op == "notin") r.op = LabelRequirement::Op::NotIn;
            else { err = "invalid label selector operator: " + std::string(op); return false; }
            for (std::string_view v : split_top_level(set)) {
                v = trim(v);
                if (!v.empty()) r.values.emplace_back(v);
            }
        } else if (ne != std::string_view::npos) {
            r.op = LabelRequirement::Op::NotEquals;
            r.key = std::string(trim(part.substr(0, ne)));
            r.values.emplace_back(trim(part.substr(ne + 2)));
        } else if (eq != std::string_view::npos) {
            size_t vstart = (eq + 1 < part.size() && part[eq + 1] == '=') ? eq + 2 : eq + 1;
            r.op = LabelRequirement::Op::Equals;
            r.key = std::string(trim(part.substr(0, eq)));
            r.values.emplace_back(trim(part.substr(vstart)));
        } else if (part.front() == '!') {
            r.op = LabelRequirement::Op::DoesNotExist;
            r.key = std::string(trim(part.substr(1)));
        } else {
            r.op = LabelRequirement::Op::Exists;
            r.key = std::string(part);
        }
        if (r.key.empty()) { err = "invalid label selector: empty key"; return false; }
        out.push_back(std::move(r));
    }
    return true;
}

bool ListSelector::parse(std::string_view field_selector, std::string_view label_selector,
                         ListSelector& out, std::string& err) {
    out.fields_.clear();
    out.labels_.clear();
    if (!parse_field_selector(field_selector, out.fields_, err)) return false;
    if (!parse_label_selector(label_selector, out.labels_, err)) return false;
    return true;
}

bool ListSelector::from_query(std::string_view query, ListSelector& out, std::string& err) {
    std::string fs, ls;
    query_param(query, "fieldSelector", fs);
    query_param(query, "labelSelector", ls);
    return parse(fs, ls, out, err);
}

std::string ListSelector::field_value(const models::JsonValue& obj, std::string_view path) {
    const models::JsonValue* cur = &obj;
    while (!path.empty()) {
        size_t dot = path.find('.');
        std::string_view seg = (dot == std::string_view::npos) ? path : path.substr(0, dot);
        if (!cur->is_object()) return "";
        cur = cur->get(std::string(seg));
        if (!cur) return "";
        if (dot == std::string_view::npos) break;
        path = path.substr(dot + 1);
    }
    switch (cur->type) {
        case models::JsonType::String: return cur->str_val;
        case models::JsonType::Boolean: return cur->bool_val ? "true" : "false";
        case models::JsonType::Number: {
            char buf[32];
            if (cur->num_val == static_cast<double>(static_cast<long long>(cur->num_val))) {
                snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(cur->num_val));
            } else {
                snprintf(buf, sizeof(buf), "%g", cur->num_val);
            }
            return buf;
        }
        default: return "";
    }
}

bool ListSelector::matches(const models::JsonValue& obj) const {
    for (const auto& f : fields_) {
        bool eq = (field_value(obj, f.path) == f.value);
        if (eq != f.equals) return false;
    }
    if (!labels_.empty()) {
        const models::JsonValue* meta = obj.get("metadata");
        const models::JsonValue* labels = (meta && meta->is_object()) ? meta->get("labels") : nullptr;
        for (const auto& r : labels_) {
            const models::JsonValue* v = (labels && labels->is_object()) ? labels->get(r.key) : nullptr;
            bool present = (v != nullptr);
            std::string val = (present && v->is_string()) ? v->str_val : "";
            auto in_set = [&]() {
                for (const auto& s : r.values) if (s == val) return true;
                return false;
            };
            switch (r.op) {
                case LabelRequirement::Op::Equals: if (!present || !in_set()) return false; break;
                case LabelRequirement::Op::NotEquals: if (present && in_set()) return false; break;
                case LabelRequirement::Op::Exists: if (!present) return false; break;
                case LabelRequirement::Op::DoesNotExist: if (present) return false; break;
                case LabelRequirement::Op::In: if (!present || !in_set()) return false; break;
                case LabelRequirement::Op::NotIn: if (present && in_set()) return false; break;
            }
        }
    }
    return true;
}

bool ListSelector::matches_json(std::string_view json) const {
    if (empty()) return true;
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) return false;
    return matches(doc);
}

void filter_jsons(const ListSelector& sel, std::vector<std::string>& jsons) {
    if (sel.empty()) return;
    std::vector<std::string> kept;
    kept.reserve(jsons.size());
    for (auto& j : jsons) {
        if (sel.matches_json(j)) kept.push_back(std::move(j));
    }
    jsons.swap(kept);
}

} // namespace kube32::api
