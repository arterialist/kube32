#include "api/event_recorder.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#include <cstdio>

namespace kube32::api {

static models::JsonValue jstr(const std::string& s) { return models::JsonValue{models::JsonType::String, false, 0, s, {}, {}}; }
static models::JsonValue jnum(double n) { return models::JsonValue{models::JsonType::Number, false, n, "", {}, {}}; }

EventRecorder::EventRecorder(storage::Store& store, size_t max_events)
    : store_(store), max_(max_events == 0 ? 1 : max_events) {}

size_t EventRecorder::count() const {
    std::vector<std::string> jsons;
    store_.list("/registry/events/", jsons);
    return jsons.size();
}

static uint64_t rv_of(const std::string& json) {
    size_t p = json.find("\"resourceVersion\":\"");
    if (p == std::string::npos) return 0;
    uint64_t v = 0;
    for (size_t i = p + 19; i < json.size() && json[i] >= '0' && json[i] <= '9'; ++i) v = v * 10 + static_cast<uint64_t>(json[i] - '0');
    return v;
}

bool EventRecorder::find_duplicate(const std::string& ns, const EventSpec& spec, std::string& out_key, std::string& out_json) const {
    std::vector<std::string> jsons;
    std::vector<std::string> keys;
    store_.list_extended("/registry/events/" + ns + "/", jsons, &keys);
    const std::string reason_needle = "\"reason\":\"" + spec.reason + "\"";
    const std::string name_needle = "\"name\":\"" + spec.involved.name + "\"";
    for (size_t i = 0; i < jsons.size(); ++i) {
        const std::string& j = jsons[i];
        if (j.find(reason_needle) == std::string::npos || j.find(name_needle) == std::string::npos) continue;
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(j, doc, err) || !doc.is_object()) continue;
        const models::JsonValue* inv = doc.get("involvedObject");
        if (!inv || inv->get_string("kind", "") != spec.involved.kind || inv->get_string("name", "") != spec.involved.name) continue;
        if (doc.get_string("reason", "") != spec.reason || doc.get_string("message", "") != spec.message) continue;
        if (doc.get_string("reportingComponent", "") != spec.component) continue;
        out_key = keys[i];
        out_json = j;
        return true;
    }
    return false;
}

void EventRecorder::enforce_bound() {
    std::vector<std::string> jsons;
    std::vector<std::string> keys;
    store_.list_extended("/registry/events/", jsons, &keys);
    while (keys.size() > max_) {
        size_t oldest = 0;
        uint64_t oldest_rv = ~0ULL;
        for (size_t i = 0; i < jsons.size(); ++i) {
            uint64_t rv = rv_of(jsons[i]);
            if (rv < oldest_rv) { oldest_rv = rv; oldest = i; }
        }
        store_.remove(keys[oldest]);
        keys.erase(keys.begin() + static_cast<long>(oldest));
        jsons.erase(jsons.begin() + static_cast<long>(oldest));
    }
}

static std::string event_name_for(const std::string& involved_name) {
    // kube-apiserver names events "<object>.<hex nanotime>"; hex of the wall
    // clock (or uptime before it is set) keeps them unique and sortable.
    uint64_t stamp = osal::System::get_unix_time_ms();
    if (stamp == 0) stamp = osal::System::get_uptime_ms();
    char buf[24];
    snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(stamp * 1000ULL + (osal::Thread::get_time_us() % 1000ULL)));
    return involved_name + "." + buf;
}

std::string EventRecorder::record(const EventSpec& spec) {
    std::string ns = spec.involved.namespace_name.empty() ? "default" : spec.involved.namespace_name;
    std::string now = models::now_rfc3339();

    osal::LockGuard guard(mutex_);
    std::string dup_key, existing;
    if (find_duplicate(ns, spec, dup_key, existing)) {
        models::JsonValue doc;
        std::string err;
        if (models::parse_json(existing, doc, err) && doc.is_object()) {
            doc.set("count", jnum(static_cast<double>(doc.get_int("count", 1) + 1)));
            if (!now.empty()) doc.set("lastTimestamp", jstr(now));
            std::string merged = doc.to_string();
            store_.put_extended(dup_key, merged, true, false);
            return merged;
        }
    }

    std::string name = event_name_for(spec.involved.name);
    std::string key = "/registry/events/" + ns + "/" + name;

    models::JsonValue doc;
    doc.type = models::JsonType::Object;
    doc.set("kind", jstr("Event"));
    doc.set("apiVersion", jstr("v1"));
    models::JsonValue meta;
    meta.type = models::JsonType::Object;
    meta.set("name", jstr(name));
    meta.set("namespace", jstr(ns));
    meta.set("uid", jstr(models::generate_uid()));
    if (!now.empty()) meta.set("creationTimestamp", jstr(now));
    doc.set("metadata", std::move(meta));
    models::JsonValue inv;
    inv.type = models::JsonType::Object;
    inv.set("kind", jstr(spec.involved.kind));
    if (!spec.involved.namespace_name.empty()) inv.set("namespace", jstr(spec.involved.namespace_name));
    inv.set("name", jstr(spec.involved.name));
    if (!spec.involved.uid.empty()) inv.set("uid", jstr(spec.involved.uid));
    inv.set("apiVersion", jstr(spec.involved.api_version));
    doc.set("involvedObject", std::move(inv));
    doc.set("reason", jstr(spec.reason));
    doc.set("message", jstr(spec.message));
    models::JsonValue src;
    src.type = models::JsonType::Object;
    src.set("component", jstr(spec.component));
    if (!spec.host.empty()) src.set("host", jstr(spec.host));
    doc.set("source", std::move(src));
    if (!now.empty()) {
        doc.set("firstTimestamp", jstr(now));
        doc.set("lastTimestamp", jstr(now));
    }
    doc.set("count", jnum(1));
    doc.set("type", jstr(spec.warning ? "Warning" : "Normal"));
    doc.set("reportingComponent", jstr(spec.component));
    if (!spec.host.empty()) doc.set("reportingInstance", jstr(spec.host));
    if (spec.passthrough.is_object()) {
        for (const auto& kv : spec.passthrough.obj_val) doc.set(kv.first, kv.second);
    }

    std::string json = doc.to_string();
    store_.put_extended(key, json, true, false);
    enforce_bound();
    return json;
}

bool EventRecorder::admit(const std::string& event_json, std::string& out_stored, std::string& err) {
    models::JsonValue doc;
    if (!models::parse_json(event_json, doc, err) || !doc.is_object()) return false;
    const models::JsonValue* inv = doc.get("involvedObject");
    const models::JsonValue* meta = doc.get("metadata");
    if (!inv || !inv->is_object()) { err = "involvedObject is required"; return false; }
    EventSpec spec;
    spec.involved.kind = inv->get_string("kind", "");
    spec.involved.name = inv->get_string("name", "");
    spec.involved.namespace_name = inv->get_string("namespace", "");
    spec.involved.uid = inv->get_string("uid", "");
    spec.involved.api_version = inv->get_string("apiVersion", "v1");
    if (spec.involved.name.empty()) { err = "involvedObject.name is required"; return false; }
    spec.reason = doc.get_string("reason", "");
    spec.message = doc.get_string("message", "");
    spec.warning = doc.get_string("type", "Normal") == "Warning";
    const models::JsonValue* src = doc.get("source");
    spec.component = doc.get_string("reportingComponent", src ? src->get_string("component", "") : "");
    spec.host = doc.get_string("reportingInstance", src ? src->get_string("host", "") : "");
    spec.passthrough.type = models::JsonType::Object;
    for (const char* field : {"action", "related", "eventTime", "series"}) {
        if (const models::JsonValue* v = doc.get(field)) spec.passthrough.set(field, *v);
    }
    std::string ns = meta ? meta->get_string("namespace", "") : "";
    if (ns.empty()) ns = spec.involved.namespace_name;
    if (!ns.empty()) spec.involved.namespace_name = ns;
    out_stored = record(spec);
    return true;
}

std::string EventRecorder::to_events_v1(const std::string& core_json) {
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(core_json, doc, err) || !doc.is_object()) return core_json;
    models::JsonValue out;
    out.type = models::JsonType::Object;
    out.set("kind", jstr("Event"));
    out.set("apiVersion", jstr("events.k8s.io/v1"));
    if (const auto* m = doc.get("metadata")) out.set("metadata", *m);
    if (const auto* inv = doc.get("involvedObject")) out.set("regarding", *inv);
    if (const auto* rel = doc.get("related")) out.set("related", *rel);
    if (!doc.get_string("action", "").empty()) out.set("action", jstr(doc.get_string("action", "")));
    out.set("reason", jstr(doc.get_string("reason", "")));
    out.set("note", jstr(doc.get_string("message", "")));
    out.set("type", jstr(doc.get_string("type", "Normal")));
    out.set("reportingController", jstr(doc.get_string("reportingComponent", "")));
    out.set("reportingInstance", jstr(doc.get_string("reportingInstance", "")));
    std::string first = doc.get_string("firstTimestamp", "");
    std::string last = doc.get_string("lastTimestamp", "");
    int64_t count = doc.get_int("count", 1);
    const std::string event_time = doc.get_string("eventTime", first);
    if (!event_time.empty()) out.set("eventTime", jstr(event_time));
    if (count > 1) {
        models::JsonValue series;
        series.type = models::JsonType::Object;
        series.set("count", jnum(static_cast<double>(count)));
        if (!last.empty()) series.set("lastObservedTime", jstr(last));
        out.set("series", std::move(series));
    }
    if (!first.empty()) out.set("deprecatedFirstTimestamp", jstr(first));
    if (!last.empty()) out.set("deprecatedLastTimestamp", jstr(last));
    out.set("deprecatedCount", jnum(static_cast<double>(count)));
    if (const auto* src = doc.get("source")) out.set("deprecatedSource", *src);
    return out.to_string();
}

} // namespace kube32::api
