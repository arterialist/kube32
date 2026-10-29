#include "models/kube_lease.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"
#include <cstdio>
#include <sstream>

namespace kube32::models {

std::string KubeLease::to_json() const {
    JsonValue root;
    root.type = JsonType::Object;
    root.set("apiVersion", JsonValue{JsonType::String, false, 0, "coordination.k8s.io/v1", {}, {}});
    root.set("kind", JsonValue{JsonType::String, false, 0, "Lease", {}, {}});

    JsonValue meta;
    meta.type = JsonType::Object;
    meta.set("name", JsonValue{JsonType::String, false, 0, name, {}, {}});
    meta.set("namespace", JsonValue{JsonType::String, false, 0, namespace_name.empty() ? "kube-node-lease" : namespace_name, {}, {}});
    if (resource_version > 0) {
        meta.set("resourceVersion", JsonValue{JsonType::String, false, 0, std::to_string(resource_version), {}, {}});
    }
    root.set("metadata", std::move(meta));

    JsonValue spec;
    spec.type = JsonType::Object;
    spec.set("holderIdentity", JsonValue{JsonType::String, false, 0, holder_identity.empty() ? name : holder_identity, {}, {}});
    spec.set("leaseDurationSeconds", JsonValue{JsonType::Number, false, static_cast<double>(lease_duration_seconds), "", {}, {}});
    spec.set("renewTime", JsonValue{JsonType::String, false, 0, renew_time, {}, {}});
    root.set("spec", std::move(spec));

    return root.to_string();
}

KubeLease KubeLease::from_json(const std::string& json_str) {
    KubeLease lease;
    JsonValue doc;
    std::string err;
    if (!parse_json(json_str, doc, err) || !doc.is_object()) {
        return lease;
    }

    const JsonValue* meta = doc.get("metadata");
    if (meta && meta->is_object()) {
        lease.name = meta->get_string("name", "");
        lease.namespace_name = meta->get_string("namespace", "kube-node-lease");
        std::string rv_str = meta->get_string("resourceVersion", "0");
        try {
            lease.resource_version = std::stoull(rv_str);
        } catch (...) {
            lease.resource_version = 0;
        }
    }

    const JsonValue* spec = doc.get("spec");
    if (spec && spec->is_object()) {
        lease.holder_identity = spec->get_string("holderIdentity", lease.name);
        lease.lease_duration_seconds = static_cast<uint32_t>(spec->get_int("leaseDurationSeconds", 40));
        lease.renew_time = spec->get_string("renewTime", "");
    }

    return lease;
}

std::string now_micro_time() {
    uint64_t ms = osal::System::get_unix_time_ms();
    if (ms == 0) return std::string();
    std::string t = format_rfc3339(ms);
    char frac[16];
    snprintf(frac, sizeof(frac), ".%03u000Z", static_cast<unsigned>(ms % 1000));
    if (!t.empty() && t.back() == 'Z') t.pop_back();
    return t + frac;
}

} // namespace kube32::models
