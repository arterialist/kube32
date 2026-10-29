#include "storage/persist_policy.hpp"
#include "models/kube_models.hpp"

namespace kube32::storage {

PersistClass persist_class(const std::string& key) {
    if (key.find("/registry/leases/") != std::string::npos || key.find("/registry/events/") != std::string::npos ||
        key.find("/heartbeat") != std::string::npos || key.find("/telemetry") != std::string::npos) {
        return PersistClass::Ephemeral;
    }
    if (key.rfind("/registry/nodes/", 0) == 0) return PersistClass::Deferred;
    return PersistClass::Immediate;
}

static uint64_t fnv(uint64_t h, const std::string& s) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    h ^= 0xFF;
    h *= 1099511628211ull;
    return h;
}

uint64_t durable_fingerprint(const std::string& key, const std::string& json) {
    (void)key;
    models::JsonValue doc;
    std::string err;
    uint64_t h = 1469598103934665603ull;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) return fnv(h, json);
    if (const models::JsonValue* meta = doc.get("metadata")) {
        if (const models::JsonValue* l = meta->get("labels")) h = fnv(h, l->to_string());
        if (const models::JsonValue* a = meta->get("annotations")) h = fnv(h, a->to_string());
        h = fnv(h, meta->get_string("uid", ""));
        h = fnv(h, meta->get_string("deletionTimestamp", ""));
    }
    if (const models::JsonValue* spec = doc.get("spec")) h = fnv(h, spec->to_string());
    const models::JsonValue* st = doc.get("status");
    const models::JsonValue* conds = st ? st->get("conditions") : nullptr;
    if (conds && conds->is_array()) {
        for (const auto& c : conds->arr_val) {
            if (c.is_object() && c.get_string("type", "") == "Ready") h = fnv(h, c.get_string("status", ""));
        }
    }
    return h;
}

} // namespace kube32::storage
