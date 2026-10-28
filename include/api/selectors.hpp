#pragma once

// Query string helpers and Kubernetes list selectors (fieldSelector,
// labelSelector) evaluated against stored JSON objects.

#include "models/kube_models.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace kube32::api {

// Percent-decoding with '+' treated as a space.
std::string url_decode(std::string_view s);
// Looks a key up in "a=1&b=2"; the value is returned decoded. False when absent.
bool query_param(std::string_view query, std::string_view key, std::string& out);
// True when the parameter is present and equals "true" or "1".
bool query_flag(std::string_view query, std::string_view key);
// Parses an unsigned integer parameter; false when absent or malformed.
bool query_uint(std::string_view query, std::string_view key, uint64_t& out);

struct FieldRequirement {
    std::string path;   // dotted, e.g. "spec.nodeName", "involvedObject.name"
    bool equals{true};  // "=" / "==" vs "!="
    std::string value;
};

struct LabelRequirement {
    enum class Op { Equals, NotEquals, Exists, DoesNotExist, In, NotIn };
    Op op{Op::Equals};
    std::string key;
    std::vector<std::string> values;
};

class ListSelector {
public:
    // Both inputs are the raw (decoded) selector strings; either may be empty.
    static bool parse(std::string_view field_selector, std::string_view label_selector,
                      ListSelector& out, std::string& err);
    // Builds from a query string ("fieldSelector=...&labelSelector=...").
    static bool from_query(std::string_view query, ListSelector& out, std::string& err);

    bool empty() const { return fields_.empty() && labels_.empty(); }
    bool matches(const models::JsonValue& obj) const;
    // Parses `json` only when the selector is not empty.
    bool matches_json(std::string_view json) const;

    const std::vector<FieldRequirement>& fields() const { return fields_; }
    const std::vector<LabelRequirement>& labels() const { return labels_; }

    // Resolves a dotted path to its scalar string form ("" when absent).
    static std::string field_value(const models::JsonValue& obj, std::string_view path);

private:
    std::vector<FieldRequirement> fields_;
    std::vector<LabelRequirement> labels_;
};

// Applies the selector to a list of JSON objects in place.
void filter_jsons(const ListSelector& sel, std::vector<std::string>& jsons);

} // namespace kube32::api
