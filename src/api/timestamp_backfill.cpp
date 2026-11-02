#include "api/timestamp_backfill.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace kube32::api {

static const char* const kRegistry = "/registry/";
static const char* const kEvents = "/registry/events/";

static uint64_t key_hash(const std::string& key) {
    uint64_t h = 14695981039346656037ULL;   // FNV-1a
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

TimestampBackfill::TimestampBackfill(storage::Store& store, size_t max_objects, Clock unix_ms, Clock uptime_ms)
    : store_(store), max_objects_(max_objects), unix_ms_(std::move(unix_ms)), uptime_ms_(std::move(uptime_ms)) {
    if (!unix_ms_) unix_ms_ = [] { return osal::System::get_unix_time_ms(); };
    if (!uptime_ms_) uptime_ms_ = [] { return osal::System::get_uptime_ms(); };
    watch_id_ = store_.watch(kRegistry, [this](const storage::WatchEvent& ev) { on_change(ev); });
}

TimestampBackfill::~TimestampBackfill() {
    store_.unwatch(watch_id_);
}

// On the writer's thread, after the write: notes only, never the store
void TimestampBackfill::on_change(const storage::WatchEvent& ev) {
    if (ev.type == storage::WatchEventType::Added && ev.json.find("\"creationTimestamp\"") != std::string::npos) return;
    // Once the clock is set, writers stamp their own updates
    if (ev.type == storage::WatchEventType::Modified && unix_ms_() != 0) return;
    const uint64_t h = key_hash(ev.key);
    const uint64_t now = uptime_ms_();
    osal::LockGuard guard(mutex_);
    auto it = std::find_if(notes_.begin(), notes_.end(), [h](const Note& n) { return n.key_hash == h; });
    if (ev.type == storage::WatchEventType::Deleted) {
        if (it != notes_.end()) notes_.erase(it);
    } else if (ev.type == storage::WatchEventType::Added) {
        if (it != notes_.end()) *it = Note{h, now, now};   // deleted and created again
        else if (notes_.size() < max_objects_) notes_.push_back(Note{h, now, now});
    } else if (it != notes_.end()) {
        it->updated_ms = now;
    }
}

size_t TimestampBackfill::pending() const {
    osal::LockGuard guard(mutex_);
    return notes_.size();
}

size_t TimestampBackfill::run() {
    const uint64_t unix_now = unix_ms_();
    if (unix_now == 0) return 0;
    std::vector<Note> notes;
    {
        osal::LockGuard guard(mutex_);
        if (notes_.empty()) return 0;
        notes = notes_;
    }
    const uint64_t up = uptime_ms_();
    auto wall = [&](uint64_t at) { return models::format_rfc3339(unix_now - std::min(unix_now, up > at ? up - at : 0)); };

    std::vector<std::string> jsons, keys;
    store_.list_extended(kRegistry, jsons, &keys);
    size_t updated = 0;
    std::vector<uint64_t> stored(keys.size());
    std::vector<uint64_t> done;   // stamped, or with nothing left to stamp
    for (size_t i = 0; i < jsons.size(); ++i) {
        const uint64_t h = stored[i] = key_hash(keys[i]);
        auto note = std::find_if(notes.begin(), notes.end(), [h](const Note& n) { return n.key_hash == h; });
        if (note == notes.end()) continue;
        models::JsonValue doc;
        std::string err;
        models::JsonValue* meta = nullptr;
        if (models::parse_json(jsons[i], doc, err) && doc.is_object()) meta = doc.get_mut("metadata");
        if (!meta || !meta->is_object()) {
            done.push_back(h);
            continue;
        }
        const bool is_event = keys[i].rfind(kEvents, 0) == 0;
        bool changed = false;
        auto stamp = [&](models::JsonValue& obj, const char* field, uint64_t at) {
            if (!obj.get_string(field, "").empty()) return;
            obj.set(field, models::JsonValue{models::JsonType::String, false, 0, wall(at), {}, {}});
            changed = true;
        };
        stamp(*meta, "creationTimestamp", note->created_ms);
        if (is_event) {
            stamp(doc, "firstTimestamp", note->created_ms);
            stamp(doc, "lastTimestamp", note->updated_ms);
        }
        if (!changed) {
            done.push_back(h);
            continue;
        }
        // Events are kept off flash, everything else is written as usual
        storage::Status s = is_event ? store_.put_extended(keys[i], doc.to_string(), true, false)
                                     : store_.put(keys[i], doc.to_string(), true);
        if (s.ok()) {
            ++updated;
            done.push_back(h);
        }
    }
    // Notes of objects no longer stored go too; one created meanwhile stays
    osal::LockGuard guard(mutex_);
    notes_.erase(std::remove_if(notes_.begin(), notes_.end(), [&](const Note& n) {
        if (std::find(done.begin(), done.end(), n.key_hash) != done.end()) return true;
        const bool listed = std::find(stored.begin(), stored.end(), n.key_hash) != stored.end();
        const bool noted_before = std::find_if(notes.begin(), notes.end(), [&](const Note& o) { return o.key_hash == n.key_hash; }) != notes.end();
        return noted_before && !listed;
    }), notes_.end());
    return updated;
}

} // namespace kube32::api
