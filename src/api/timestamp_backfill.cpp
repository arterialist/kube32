#include "api/timestamp_backfill.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"

#include <vector>

namespace kube32::api {

size_t backfill_timestamps(storage::Store& store) {
    const uint64_t now = osal::System::get_unix_time_ms();
    if (now == 0) return 0;
    const uint64_t uptime = osal::System::get_uptime_ms();
    const std::string boot = models::format_rfc3339(now > uptime ? now - uptime : now);

    std::vector<std::string> jsons, keys;
    store.list_extended("/registry/", jsons, &keys);
    size_t updated = 0;
    for (size_t i = 0; i < jsons.size(); ++i) {
        // Cheap test first: most objects have their timestamp
        const bool is_event = keys[i].rfind("/registry/events/", 0) == 0;
        if (jsons[i].find("\"creationTimestamp\"") != std::string::npos &&
            (!is_event || jsons[i].find("\"lastTimestamp\"") != std::string::npos)) {
            continue;
        }
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(jsons[i], doc, err) || !doc.is_object()) continue;
        models::JsonValue* meta = doc.get_mut("metadata");
        if (!meta || !meta->is_object()) continue;
        bool changed = false;
        auto stamp = [&](models::JsonValue& obj, const char* field) {
            if (!obj.get_string(field, "").empty()) return;
            obj.set(field, models::JsonValue{models::JsonType::String, false, 0, boot, {}, {}});
            changed = true;
        };
        stamp(*meta, "creationTimestamp");
        if (is_event) {
            stamp(doc, "firstTimestamp");
            stamp(doc, "lastTimestamp");
        }
        if (!changed) continue;
        // Events are kept off flash, everything else is written as usual
        storage::Status s = is_event ? store.put_extended(keys[i], doc.to_string(), true, false)
                                     : store.put(keys[i], doc.to_string(), true);
        if (s.ok()) ++updated;
    }
    return updated;
}

} // namespace kube32::api
