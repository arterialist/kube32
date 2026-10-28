#include "storage/store.hpp"
#include "storage/radix_tree.hpp"
#include "storage/littlefs_journal.hpp"

#include <mutex>
#include <mutex>
#include <algorithm>
#include <cctype>

namespace kube32::storage {

// Writes the store's version into metadata.resourceVersion so every object a
// client reads carries the version the store assigned (watch bookmarks and
// resourceVersion-based watches depend on it). Pure string surgery: scans the
// metadata object at depth 1, replaces an existing resourceVersion value or
// inserts one. Values without a metadata object are stored verbatim.
std::string inject_resource_version(const std::string& json, uint64_t version) {
    size_t meta = json.find("\"metadata\"");
    if (meta == std::string::npos) return json;
    size_t p = meta + 10;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) ++p;
    if (p >= json.size() || json[p] != ':') return json;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) ++p;
    if (p >= json.size() || json[p] != '{') return json;
    const size_t open = p;
    std::string ver = "\"" + std::to_string(version) + "\"";

    int depth = 0;
    bool in_str = false;
    size_t i = open;
    for (; i < json.size(); ++i) {
        char c = json[i];
        if (in_str) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') {
            if (depth == 1 && json.compare(i, 17, "\"resourceVersion\"") == 0) {
                size_t q = i + 17;
                while (q < json.size() && (json[q] == ' ' || json[q] == ':')) ++q;
                size_t vstart = q;
                size_t vend = q;
                if (q < json.size() && json[q] == '"') {
                    vend = q + 1;
                    while (vend < json.size() && json[vend] != '"') { if (json[vend] == '\\') ++vend; ++vend; }
                    if (vend < json.size()) ++vend;
                } else {
                    while (vend < json.size() && json[vend] != ',' && json[vend] != '}') ++vend;
                }
                std::string out;
                out.reserve(json.size() + 8);
                out.append(json, 0, vstart);
                out += ver;
                out.append(json, vend, std::string::npos);
                return out;
            }
            in_str = true;
            continue;
        }
        if (c == '{' || c == '[') { ++depth; continue; }
        if (c == '}' || c == ']') {
            --depth;
            if (depth == 0) break;
        }
    }
    // No resourceVersion in metadata: insert right after the opening brace
    size_t after = open + 1;
    while (after < json.size() && (json[after] == ' ' || json[after] == '\n' || json[after] == '\r' || json[after] == '\t')) ++after;
    bool empty_obj = (after < json.size() && json[after] == '}');
    std::string out;
    out.reserve(json.size() + 32);
    out.append(json, 0, open + 1);
    out += "\"resourceVersion\":" + ver;
    if (!empty_obj) out += ",";
    out.append(json, open + 1, std::string::npos);
    return out;
}

class RadixStore : public Store {
public:
    explicit RadixStore(StoreOptions options = {})
        : options_(std::move(options)),
          journal_(options_.base_path) {}

    Status get(const std::string& key, std::string& out_json) override {
        return get_extended(key, out_json, nullptr);
    }

    Status put(const std::string& key, const std::string& json, bool overwrite) override {
        bool is_ephemeral = is_key_ephemeral(key);
        return put_extended(key, json, overwrite, !is_ephemeral, 0, nullptr);
    }

    Status remove(const std::string& key) override {
        if (key.empty()) {
            return Status::InvalidArgument("Storage key cannot be empty");
        }

        std::vector<WatchCallback> pending_callbacks;
        WatchEvent pending_event;
        bool has_event = false;

        {
            std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
            std::string deleted_val;
            uint64_t deleted_ver = 0;
            if (!tree_.remove(key, &deleted_val, &deleted_ver)) {
                return Status::NotFound();
            }

            if (options_.enable_flash_persistence && !is_key_ephemeral(key)) {
                journal_.remove_file(key);
            }

            uint64_t new_version = ++current_version_;
            pending_callbacks = collect_matching_watchers(key);
            if (!pending_callbacks.empty()) {
                pending_event = WatchEvent{WatchEventType::Deleted, key, deleted_val, new_version};
                has_event = true;
            }
        }

        if (has_event) {
            notify_watchers(pending_callbacks, pending_event);
        }
        return Status::Ok();
    }

    Status list(const std::string& prefix, std::vector<std::string>& out_jsons) override {
        std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
        tree_.list(prefix, out_jsons, nullptr);
        return Status::Ok();
    }

    Status persist() override {
        if (!options_.enable_flash_persistence) return Status::Ok();
        std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
        Status result = Status::Ok();
        tree_.iterate_all([this, &result](const std::string& key, const std::string& value, 
                                          uint64_t /*ver*/, bool ephemeral) {
            if (!ephemeral) {
                if (!journal_.atomic_write(key, value)) {
                    result = Status::InternalError("Failed writing key during persist: " + key);
                }
            }
        });
        return result;
    }

    Status recover() override {
        std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
        tree_.clear();

        if (!options_.enable_flash_persistence) return Status::Ok();

        std::vector<std::pair<std::string, std::string>> recovered;
        journal_.scan_and_recover(recovered);

        uint64_t max_rev = 0;
        for (const auto& item : recovered) {
            uint64_t rev = extract_resource_version(item.second);
            if (rev > max_rev) max_rev = rev;
            tree_.put(item.first, item.second, rev, false, true);
        }

        current_version_ = max_rev + 1;
        return Status::Ok();
    }

    Status get_extended(const std::string& key, std::string& out_json, uint64_t* out_version) override {
        if (key.empty()) {
            return Status::InvalidArgument("Storage key cannot be empty");
        }
        std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
        if (tree_.get(key, out_json, out_version)) {
            return Status::Ok();
        }
        return Status::NotFound();
    }

    Status put_extended(const std::string& key, const std::string& json, 
                        bool overwrite, bool persist_to_flash, 
                        uint64_t expected_version, uint64_t* out_version) override {
        if (key.empty()) {
            return Status::InvalidArgument("Storage key cannot be empty");
        }

        std::vector<WatchCallback> pending_callbacks;
        WatchEvent pending_event;
        bool has_event = false;

        {
            std::lock_guard<std::recursive_mutex> lock(rw_mutex_);

            // OCC CAS check
            std::string existing_val;
            uint64_t existing_ver = 0;
            bool exists = tree_.get(key, existing_val, &existing_ver);

            if (expected_version > 0) {
                if (!exists) {
                    return Status::VersionConflict("ResourceVersion mismatch: key does not exist for expected version " + 
                                                  std::to_string(expected_version));
                }
                if (existing_ver != expected_version) {
                    return Status::VersionConflict("ResourceVersion mismatch: expected " + 
                                                  std::to_string(expected_version) + " but found " + 
                                                  std::to_string(existing_ver));
                }
            }

            if (exists && !overwrite) {
                return Status::AlreadyExists();
            }

            uint64_t new_ver = current_version_ + 1;
            const std::string stored = inject_resource_version(json, new_ver);

            // Two-phase atomic write to LittleFS
            if (options_.enable_flash_persistence && persist_to_flash) {
                if (!journal_.atomic_write(key, stored)) {
                    return Status::StorageFull("Failed writing to persistent flash journal");
                }
            }

            bool is_new = false;
            if (!tree_.put(key, stored, new_ver, !persist_to_flash, overwrite, &is_new)) {
                return Status::InternalError("Failed inserting key into state store index");
            }
            current_version_ = new_ver;

            if (out_version) *out_version = new_ver;

            pending_callbacks = collect_matching_watchers(key);
            if (!pending_callbacks.empty()) {
                pending_event = WatchEvent{
                    is_new ? WatchEventType::Added : WatchEventType::Modified, 
                    key, stored, new_ver
                };
                has_event = true;
            }
        }

        if (has_event) {
            notify_watchers(pending_callbacks, pending_event);
        }
        return Status::Ok();
    }

    Status list_extended(const std::string& prefix, std::vector<std::string>& out_jsons, 
                         std::vector<std::string>* out_keys) override {
        std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
        tree_.list(prefix, out_jsons, out_keys);
        return Status::Ok();
    }

    uint32_t watch(const std::string& prefix, WatchCallback cb) override {
        std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
        uint32_t id = ++next_watch_id_;
        watchers_.emplace_back(Watcher{id, prefix, std::move(cb)});
        return id;
    }

    void unwatch(uint32_t watch_id) override {
        std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
        watchers_.erase(std::remove_if(watchers_.begin(), watchers_.end(),
            [watch_id](const Watcher& w) { return w.id == watch_id; }), watchers_.end());
    }

    uint64_t current_version() const override {
        return current_version_;
    }

private:
    struct Watcher {
        uint32_t id;
        std::string prefix;
        WatchCallback callback;
    };

    StoreOptions options_;
    mutable std::recursive_mutex rw_mutex_;
    RadixTree tree_;
    LittleFSJournal journal_;
    uint64_t current_version_{0};
    uint32_t next_watch_id_{0};
    std::vector<Watcher> watchers_;

    static bool is_key_ephemeral(const std::string& key) {
        return key.find("/registry/leases/") != std::string::npos || 
               key.find("/registry/events/") != std::string::npos ||
               key.find("/heartbeat") != std::string::npos ||
               key.find("/telemetry") != std::string::npos;
    }

    static uint64_t extract_resource_version(const std::string& json) {
        size_t pos = json.find("\"resourceVersion\":");
        if (pos == std::string::npos) pos = json.find("\"resourceVersion\" :");
        if (pos == std::string::npos) return 1;

        pos += 18;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\"' || json[pos] == ':')) {
            ++pos;
        }
        uint64_t val = 0;
        while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
            val = val * 10 + (json[pos] - '0');
            ++pos;
        }
        return val == 0 ? 1 : val;
    }

    std::vector<WatchCallback> collect_matching_watchers(const std::string& key) const {
        std::vector<WatchCallback> callbacks;
        for (const auto& w : watchers_) {
            if (key.rfind(w.prefix, 0) == 0) {
                callbacks.push_back(w.callback);
            }
        }
        return callbacks;
    }

    static void notify_watchers(const std::vector<WatchCallback>& callbacks, 
                                const WatchEvent& event) {
        for (const auto& cb : callbacks) {
            if (cb) {
                cb(event);
            }
        }
    }
};

std::unique_ptr<Store> create_radix_store(StoreOptions options) {
    return std::make_unique<RadixStore>(std::move(options));
}

} // namespace kube32::storage
