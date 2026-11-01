#include "models/migration.hpp"

#include <algorithm>
#include <cstdlib>

namespace kube32::models {

namespace {

JsonValue jstr(const std::string& v) { return JsonValue{JsonType::String, false, 0, v, {}, {}}; }

std::string annotation(const JsonValue& obj, const char* key) {
    const JsonValue* meta = obj.get("metadata");
    const JsonValue* ann = meta ? meta->get("annotations") : nullptr;
    return ann ? ann->get_string(key, "") : std::string();
}

bool starts(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

void add(std::vector<std::string>& out, std::string line) {
    if (std::find(out.begin(), out.end(), line) == out.end()) out.push_back(std::move(line));
}

} // namespace

MigrationPolicy migration_policy(const JsonValue& pod) {
    const std::string p = annotation(pod, kAnnotationMigration);
    if (p == "Never") return MigrationPolicy::Never;
    if (p == "Auto") return MigrationPolicy::Auto;
    return MigrationPolicy::OnDrain;
}

std::vector<std::string> migration_blockers(const JsonValue& pod, bool for_move) {
    std::vector<std::string> out;
    if (for_move && migration_policy(pod) == MigrationPolicy::Never) add(out, std::string("annotation ") + kAnnotationMigration + ": Never");
    if (!annotation(pod, "kube32.io/i2c-device").empty()) add(out, "grant kube32.io/i2c-device");
    if (has_owner(pod, "DaemonSet")) add(out, "DaemonSet pod: it belongs to its node");
    const JsonValue* spec = pod.get("spec");
    if (!spec) return out;
    if (const JsonValue* cs = spec->get("containers"); cs && cs->is_array()) {
        for (const auto& c : cs->arr_val) {
            const JsonValue* res = c.get("resources");
            for (const char* part : {"requests", "limits"}) {
                const JsonValue* m = res ? res->get(part) : nullptr;
                if (!m || !m->is_object()) continue;
                for (const auto& kv : m->obj_val) {
                    const std::string& k = kv.first;
                    const bool hw = (starts(k, "hardware.kube32.io/") && k != "hardware.kube32.io/ipc") ||
                                    ((starts(k, "esp32.io/") || starts(k, "esp.io/")) && k.find("/psram") == std::string::npos);
                    if (hw) add(out, "grant " + k);
                }
            }
            const JsonValue* sc = c.get("securityContext");
            const JsonValue* caps = sc ? sc->get("capabilities") : nullptr;
            const JsonValue* adds = caps ? caps->get("add") : nullptr;
            if (adds && adds->is_array()) {
                for (const auto& a : adds->arr_val) {
                    if (a.is_string() && a.str_val != "CHANNEL_IPC" && a.str_val != "IPC" && a.str_val != "SERVICE_CALL") {
                        add(out, "capability " + a.str_val);
                    }
                }
            }
        }
    }
    if (const JsonValue* vols = spec->get("volumes"); vols && vols->is_array()) {
        for (const auto& v : vols->arr_val) {
            if (v.get("persistentVolumeClaim") || v.get("kube32.io/sd")) add(out, "volume " + v.get_string("name", "") + " on node storage");
        }
    }
    return out;
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
    JsonValue meta;
    meta.type = JsonType::Object;
    meta.set("name", jstr(name));
    meta.set("namespace", jstr(ns));
    meta.set("uid", jstr(generate_uid()));
    const std::string now = now_rfc3339();
    if (!now.empty()) meta.set("creationTimestamp", jstr(now));
    if (!trigger.empty()) {
        JsonValue labels;
        labels.type = JsonType::Object;
        labels.set(kLabelTrigger, jstr(trigger));
        meta.set("labels", std::move(labels));
    }
    JsonValue spec;
    spec.type = JsonType::Object;
    spec.set("podName", jstr(pod));
    spec.set("action", jstr(action));
    if (!target_node.empty()) {
        JsonValue target;
        target.type = JsonType::Object;
        target.set("nodeName", jstr(target_node));
        spec.set("target", std::move(target));
    }
    JsonValue status;
    status.type = JsonType::Object;
    status.set("phase", jstr("Pending"));
    JsonValue doc;
    doc.type = JsonType::Object;
    doc.set("apiVersion", jstr("kube32.io/v1"));
    doc.set("kind", jstr("Checkpoint"));
    doc.set("metadata", std::move(meta));
    doc.set("spec", std::move(spec));
    doc.set("status", std::move(status));
    return doc.to_string();
}

} // namespace kube32::models
