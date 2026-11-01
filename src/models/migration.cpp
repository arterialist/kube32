#include "models/migration.hpp"

#include <cstdlib>

namespace kube32::models {

namespace {

std::string annotation(const JsonValue& obj, const char* key) {
    const JsonValue* meta = obj.get("metadata");
    const JsonValue* ann = meta ? meta->get("annotations") : nullptr;
    return ann ? ann->get_string(key, "") : std::string();
}

} // namespace

MigrationPolicy migration_policy(const JsonValue& pod) {
    const std::string p = annotation(pod, kAnnotationMigration);
    if (p == "Never") return MigrationPolicy::Never;
    if (p == "Auto") return MigrationPolicy::Auto;
    return MigrationPolicy::OnDrain;
}

uint64_t parse_duration_ms(const std::string& text) {
    char* end = nullptr;
    const unsigned long long n = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str()) return 0;
    const std::string unit(end);
    if (unit.empty() || unit == "ms") return n;
    if (unit == "s") return n * 1000ULL;
    if (unit == "m") return n * 60000ULL;
    if (unit == "h") return n * 3600000ULL;
    return 0;
}

std::string checkpoint_json(const std::string& ns, const std::string& name, const std::string& pod, const std::string& action,
                            const std::string& target_node, const std::string& trigger) {
    // Names are DNS labels: nothing in them needs escaping
    const std::string now = now_rfc3339();
    return str_cat({R"({"apiVersion":"kube32.io/v1","kind":"Checkpoint","metadata":{"name":")", name, R"(","namespace":")", ns, R"(","uid":")",
                    generate_uid(), "\"", now.empty() ? "" : R"(,"creationTimestamp":")", now, now.empty() ? "" : "\"",
                    trigger.empty() ? "" : R"(,"labels":{"kube32.io/trigger":")", trigger, trigger.empty() ? "" : "\"}",
                    R"(},"spec":{"podName":")", pod, R"(","action":")", action, "\"", target_node.empty() ? "" : R"(,"target":{"nodeName":")",
                    target_node, target_node.empty() ? "" : "\"}", R"(},"status":{"phase":"Pending"}})"});
}

} // namespace kube32::models
