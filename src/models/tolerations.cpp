#include "models/tolerations.hpp"

namespace kube32::models {

static JsonValue jstr(const std::string& v) { return JsonValue{JsonType::String, false, 0, v, {}, {}}; }
static JsonValue jnum(double v) { return JsonValue{JsonType::Number, false, v, "", {}, {}}; }

static const char* const kNoExecute = "NoExecute";

bool tolerates_taint(const JsonValue& toleration, const JsonValue& taint) {
    if (!toleration.is_object() || !taint.is_object()) return false;
    const std::string effect = toleration.get_string("effect", "");
    if (!effect.empty() && effect != taint.get_string("effect", "")) return false;
    const std::string key = toleration.get_string("key", "");
    if (!key.empty() && key != taint.get_string("key", "")) return false;
    const std::string op = toleration.get_string("operator", "");
    if (op == "Exists") return true;
    if (op.empty() || op == "Equal") return toleration.get_string("value", "") == taint.get_string("value", "");
    return false;
}

int64_t no_execute_toleration_seconds(const JsonValue* tolerations, const JsonValue& taint) {
    if (!tolerations || !tolerations->is_array()) return 0;
    for (const auto& t : tolerations->arr_val) {
        if (!tolerates_taint(t, taint)) continue;
        const JsonValue* seconds = t.get("tolerationSeconds");
        if (!seconds || !seconds->is_number()) return kTolerateForever;
        return seconds->num_val > 0 ? static_cast<int64_t>(seconds->num_val) : 0;
    }
    return 0;
}

static JsonValue* tolerations_of(JsonValue& pod_spec) {
    JsonValue* tolerations = pod_spec.get_mut("tolerations");
    if (!tolerations || !tolerations->is_array()) {
        JsonValue arr;
        arr.type = JsonType::Array;
        pod_spec.set("tolerations", std::move(arr));
        tolerations = pod_spec.get_mut("tolerations");
    }
    return tolerations;
}

static JsonValue exists_toleration(const char* key, int64_t seconds) {
    JsonValue t;
    t.type = JsonType::Object;
    t.set("key", jstr(key));
    t.set("operator", jstr("Exists"));
    t.set("effect", jstr(kNoExecute));
    if (seconds != kTolerateForever) t.set("tolerationSeconds", jnum(static_cast<double>(seconds)));
    return t;
}

void add_default_tolerations(JsonValue& pod_spec) {
    if (!pod_spec.is_object()) return;
    bool not_ready = false, unreachable = false;
    if (const JsonValue* existing = pod_spec.get("tolerations"); existing && existing->is_array()) {
        for (const auto& t : existing->arr_val) {
            if (!t.is_object()) continue;
            const std::string key = t.get_string("key", "");
            const std::string effect = t.get_string("effect", "");
            if (!effect.empty() && effect != kNoExecute) continue;
            if (key.empty() || key == kTaintNodeNotReady) not_ready = true;
            if (key.empty() || key == kTaintNodeUnreachable) unreachable = true;
        }
    }
    if (not_ready && unreachable) return;
    JsonValue* tolerations = tolerations_of(pod_spec);
    if (!not_ready) tolerations->arr_val.push_back(exists_toleration(kTaintNodeNotReady, kDefaultTolerationSeconds));
    if (!unreachable) tolerations->arr_val.push_back(exists_toleration(kTaintNodeUnreachable, kDefaultTolerationSeconds));
}

void add_daemon_pod_tolerations(JsonValue& pod_spec) {
    if (!pod_spec.is_object()) return;
    JsonValue* tolerations = tolerations_of(pod_spec);
    for (const char* key : {kTaintNodeNotReady, kTaintNodeUnreachable}) {
        JsonValue wanted = exists_toleration(key, kTolerateForever);
        bool replaced = false;
        for (auto& t : tolerations->arr_val) {
            if (t.is_object() && t.get_string("key", "") == key && t.get_string("operator", "") == "Exists" &&
                t.get_string("value", "").empty() && t.get_string("effect", "") == kNoExecute) {
                t = wanted;
                replaced = true;
            }
        }
        if (!replaced) tolerations->arr_val.push_back(std::move(wanted));
    }
}

} // namespace kube32::models
