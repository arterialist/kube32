#include "storage/store.hpp"
#include "storage/radix_tree.hpp"
#include "storage/littlefs_journal.hpp"
#include "storage/flash_log.hpp"
#include "storage/persist_policy.hpp"

#include <mutex>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>

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

// Revisions are reserved on flash in blocks so the counter never goes back
// after a restart, even though Leases and Events (which also take revisions)
// are not persisted.
static constexpr uint64_t kRevisionBlock = 10000;

static uint64_t steady_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

class RadixStore : public Store {
public:
    explicit RadixStore(StoreOptions options = {})
        : options_(std::move(options)),
          journal_(options_.base_path) {
        if (options_.enable_flash_persistence && options_.flash_region) {
            log_ = std::make_unique<FlashLog>(*options_.flash_region, options_.max_value_size_bytes + 1024);
        }
    }

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
            size_t bytes = 0;
            bool on_flash = false;
            if (!tree_.stat(key, &bytes, nullptr, nullptr, &on_flash)) {
                return Status::NotFound();
            }
            const bool persisted = options_.enable_flash_persistence && !is_key_ephemeral(key);

            // What can run out of memory comes before the tree changes: the
            // delete happens with its flush and its watch event, or not at all
            pending_callbacks = collect_matching_watchers(key);
            if (!pending_callbacks.empty()) {
                pending_event.type = WatchEventType::Deleted;
                pending_event.key = key;
                has_event = true;
            }
            auto slot = pending_.end();
            size_t rec = 0;
            if (persisted && log_) {
                slot = reserve_flush_locked(key);
                rec = record_bytes_locked(key, bytes, on_flash);
            }

            std::string deleted_val;
            uint64_t deleted_ver = 0;
            if (!tree_.remove(key, &deleted_val, &deleted_ver)) {
                return Status::NotFound();
            }

            if (persisted) {
                if (log_) {
                    deferred_fp_.erase(key);
                    durable_bytes_ = durable_bytes_ > rec ? durable_bytes_ - rec : 0;
                    set_flush_locked(slot, options_.flush_delay_ms, false);
                } else {
                    journal_.remove_file(key);
                }
            }

            uint64_t new_version = ++current_version_;
            if (has_event) {
                pending_event.json = std::move(deleted_val);
                pending_event.resource_version = new_version;
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
        if (log_) {
            {
                std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
                tree_.iterate_all([this](const std::string& key, const std::string&, uint64_t, bool ephemeral) {
                    if (!ephemeral) schedule_flush_locked(key, 0);
                });
            }
            return flush(true);
        }
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
        std::lock_guard<std::mutex> serial(flush_mutex_);
        std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
        tree_.clear();

        if (!options_.enable_flash_persistence) return Status::Ok();

        if (log_) {
            pending_.clear();
            deferred_fp_.clear();
            uint64_t max_rev = 0;
            bool ok = log_->recover([this](FlashLog::RecordType type, const std::string& key, std::string& value, uint64_t rev) {
                if (type == FlashLog::RecordType::Put) {
                    // FlashOnly values stay in the journal
                    const bool flash_only = persist_class(key) == PersistClass::FlashOnly;
                    tree_.put(key, flash_only ? std::string() : std::move(value), rev, false, true);
                    if (flash_only) tree_.release_value(key, rev);
                } else {
                    tree_.remove(key);
                }
            }, max_rev);
            journal_unreadable_ = !ok;
            if (!ok) return Status::InternalError("flash journal could not be read");
            durable_bytes_ = 0;
            tree_.iterate_all([this](const std::string& key, const std::string& value, uint64_t, bool) {
                const PersistClass pc = persist_class(key);
                if (pc == PersistClass::Deferred) deferred_fp_[key] = durable_fingerprint(key, value);
                durable_bytes_ += pc == PersistClass::FlashOnly ? log_->record_bytes(key) : journal_record_bytes(key, value.size());
            });
            current_version_ = max_rev + 1;
            revision_reserved_ = max_rev;
            std::lock_guard<std::mutex> commit_lock(commit_mutex_);
            committed_version_ = current_version_;
            return Status::Ok();
        }

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
        {
            std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
            bool on_flash = false;
            if (!tree_.get(key, out_json, out_version, nullptr, &on_flash)) return Status::NotFound();
            if (!on_flash) return Status::Ok();
        }
        // A FlashOnly value is read back from the journal, outside the lock
        // (the read may run on the storage task, which takes the lock too)
        bool ok = false;
        auto run = [&]() { ok = log_->read_value(key, out_json); };
        if (options_.io_call) options_.io_call(run);
        else run();
        if (!ok) return Status::InternalError("flash journal could not read " + key + " back");
        return Status::Ok();
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
            size_t existing_bytes = 0;
            uint64_t existing_ver = 0;
            bool existing_ephemeral = false;
            bool existing_on_flash = false;
            bool exists = tree_.stat(key, &existing_bytes, &existing_ver, &existing_ephemeral, &existing_on_flash);

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

            const PersistClass pclass = persist_class(key);
            uint64_t new_ver = current_version_ + 1;
            // FlashOnly values are opaque bytes, stored as given
            std::string stored = pclass == PersistClass::FlashOnly ? json : inject_resource_version(json, new_ver);

            // A key in the journal stays there: a write that skips persistence
            // would leave the key RAM-only, and the next flush would write it
            // to the journal as deleted
            const bool persist = persist_to_flash || (log_ && exists && !existing_ephemeral);

            size_t old_record = 0, new_record = 0;
            if (options_.enable_flash_persistence && persist) {
                if (log_) {
                    // A write that would grow the persisted state past the
                    // journal's capacity is refused here, before anything
                    // changes; writes that keep or shrink it always pass.
                    // (Reclamation inside the journal frees space taken by
                    // old versions, not by live objects, so it cannot help.)
                    old_record = (exists && !existing_ephemeral) ? record_bytes_locked(key, existing_bytes, existing_on_flash) : 0;
                    new_record = journal_record_bytes(key, stored.size());
                    if (new_record > old_record && durable_bytes_ - old_record + new_record > log_->capacity_bytes()) {
                        return Status::StorageFull("object store is full: " + std::to_string(durable_bytes_) + " of " +
                                                   std::to_string(log_->capacity_bytes()) + " bytes in use, " +
                                                   std::to_string(new_record - old_record) + " more needed");
                    }
                } else if (!journal_.atomic_write(key, stored)) {
                    // Two-phase atomic write to the file journal
                    return Status::StorageFull("Failed writing to persistent flash journal");
                }
            }

            // What can run out of memory comes before the tree changes: the
            // write happens with its flush and its watch event, or not at all
            pending_callbacks = collect_matching_watchers(key);
            if (!pending_callbacks.empty()) {
                pending_event = WatchEvent{exists ? WatchEventType::Modified : WatchEventType::Added, key, stored, new_ver};
                has_event = true;
            }
            const bool scheduled = log_ && options_.enable_flash_persistence && persist;
            auto slot = pending_.end();
            auto fp_slot = deferred_fp_.end();
            uint64_t fp = 0;
            bool deferred = false;
            if (scheduled) {
                if (pclass == PersistClass::Deferred) {
                    // Heartbeat churn waits, commit() included; a change that matters does not
                    fp = durable_fingerprint(key, stored);
                    auto ins = deferred_fp_.try_emplace(key, ~fp);
                    fp_slot = ins.first;
                    deferred = !ins.second && fp_slot->second == fp;
                }
                slot = reserve_flush_locked(key);
            }

            if (!tree_.put(key, std::move(stored), new_ver, !persist, overwrite)) {
                return Status::InternalError("Failed inserting key into state store index");
            }
            current_version_ = new_ver;

            if (scheduled) {
                durable_bytes_ = durable_bytes_ - old_record + new_record;
                if (fp_slot != deferred_fp_.end() && !deferred) fp_slot->second = fp;
                set_flush_locked(slot, deferred ? options_.deferred_flush_delay_ms : options_.flush_delay_ms, deferred);
            }

            if (out_version) *out_version = new_ver;
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

    Status visit(const std::string& prefix, Visitor fn) override {
        std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
        tree_.visit(prefix, fn);
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

    Status flush(bool all) override {
        return write_pending(all ? Batch::All : Batch::Due);
    }

    Status commit() override {
        if (!log_) return Status::Ok();
        uint64_t target = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
            target = current_version_;
        }
        std::unique_lock<std::mutex> lock(commit_mutex_);
        while (committed_version_ < target) {
            // One caller writes; the others wait for it, and its batch, or
            // the next one, carries their changes too (group commit)
            if (committing_) {
                commit_cv_.wait(lock);
                continue;
            }
            Status s = Status::OutOfMemory();
            committing_ = true;
            lock.unlock();
            // Whatever happens to the batch, the next caller may write: a
            // writer that gave up would leave every later commit waiting
            try {
                s = write_pending(Batch::Urgent);
            } catch (const std::bad_alloc&) {
            }
            lock.lock();
            committing_ = false;
            commit_cv_.notify_all();
            if (!s.ok()) return s;
        }
        return Status::Ok();
    }

    bool persistence_stats(PersistenceStats& out) const override {
        if (!log_) return false;
        FlashLogStats ls = log_->stats();
        out.flash_log = true;
        out.region_bytes = ls.region_bytes;
        out.capacity_bytes = log_->capacity_bytes();
        out.live_bytes = ls.live_bytes;
        out.live_keys = ls.live_keys;
        out.used_sectors = ls.used_sectors;
        out.sector_count = ls.sector_count;
        out.records_written = ls.records_written;
        out.bytes_written = ls.bytes_written;
        out.records_moved = ls.records_moved;
        out.sectors_erased = ls.sectors_erased;
        out.unchanged_skipped = unchanged_skipped_;
        out.write_errors = write_errors_;
        out.records_through_ram = ls.records_through_ram;
        out.refused_full = ls.refused_full;
        out.refused_no_space = ls.refused_no_space;
        out.refused_io = ls.refused_io;
        out.refused_out_of_memory = ls.refused_out_of_memory;
        out.last_error = FlashLog::error_name(last_refusal_);
        std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
        out.pending_keys = pending_.size();
        return true;
    }

private:
    struct Watcher {
        uint32_t id;
        std::string prefix;
        WatchCallback callback;
    };
    struct Pending {
        uint64_t due;     // steady_ms() by which the change is written
        bool deferred;    // Node heartbeat churn only: commit() leaves it
        bool reserved;    // made for a write that has not happened yet
    };
    // The pending changes a journal write takes: the due ones (the periodic
    // flush), every one but deferred heartbeats (commit), or all
    enum class Batch { Due, Urgent, All };
    // One change a batch writes. The value stays in the tree: it is written
    // from there, so a write needs no second block of the value's size
    struct Item { std::string key; bool present; uint64_t rev; bool flash_only; };

    StoreOptions options_;
    mutable std::recursive_mutex rw_mutex_;
    RadixTree tree_;
    LittleFSJournal journal_;
    std::unique_ptr<FlashLog> log_;
    std::map<std::string, Pending> pending_;        // key -> its change not yet on flash
    std::map<std::string, uint64_t> deferred_fp_;   // Deferred keys: durable fingerprint last scheduled
    std::mutex flush_mutex_;                        // one batch reaches the journal at a time, in order
    std::mutex commit_mutex_;
    std::condition_variable commit_cv_;
    bool committing_{false};                        // a commit() caller is writing a batch
    bool journal_unreadable_{false};                // recover() could not read the journal
    uint64_t committed_version_{0};                 // every change up to this revision is durable
    uint64_t revision_reserved_{0};
    size_t durable_bytes_{0};   // journal bytes the durable keys take once flushed
    uint64_t unchanged_skipped_{0};
    uint64_t write_errors_{0};
    FlashLog::Error last_refusal_{FlashLog::Error::None};
    uint64_t current_version_{0};
    uint32_t next_watch_id_{0};
    std::vector<Watcher> watchers_;

    // Bytes a put record of this key and a value of `value_bytes` takes in the journal
    static size_t journal_record_bytes(const std::string& key, size_t value_bytes) {
        return (24 + key.size() + value_bytes + 3) & ~static_cast<size_t>(3);
    }

    // The same for a key in the tree, whose value may be on flash only
    size_t record_bytes_locked(const std::string& key, size_t value_bytes, bool on_flash) const {
        return on_flash ? log_->record_bytes(key) : journal_record_bytes(key, value_bytes);
    }

    static bool is_key_ephemeral(const std::string& key) {
        return persist_class(key) == PersistClass::Ephemeral;
    }

    void schedule_flush_locked(const std::string& key, uint32_t delay_ms, bool deferred = false) {
        set_flush_locked(reserve_flush_locked(key), delay_ms, deferred);
    }

    // The pending entry of `key`, made (not due yet) when there is none: the
    // half of schedule_flush_locked that allocates
    std::map<std::string, Pending>::iterator reserve_flush_locked(const std::string& key) {
        return pending_.try_emplace(key, Pending{~static_cast<uint64_t>(0), true, true}).first;
    }

    // The half that cannot fail
    void set_flush_locked(std::map<std::string, Pending>::iterator it, uint32_t delay_ms, bool deferred) {
        const uint64_t due = steady_ms() + delay_ms;
        if (it->second.reserved) {
            it->second = Pending{due, deferred, false};
            return;
        }
        if (due < it->second.due) it->second.due = due;
        if (!deferred) it->second.deferred = false;
    }

    Status write_pending(Batch batch) {
        if (!log_) return Status::Ok();
        Status result = Status::InternalError("flash journal write did not run");
        auto run = [&]() { result = write_batch(batch); };
        if (options_.io_call) options_.io_call(run);
        else run();
        return result;
    }

    Status write_batch(Batch batch) {
        std::lock_guard<std::mutex> serial(flush_mutex_);
        // The changes stay pending: a log started now would go over what
        // the journal holds
        if (journal_unreadable_) return Status::InternalError("the flash journal could not be read at boot; nothing is written to it");
        std::vector<Item> items;
        try {
            return write_items(batch, items);
        } catch (const std::bad_alloc&) {
            // Out of memory: what the batch took is pending again, for a later batch
            std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
            for (Item& item : items) {
                try {
                    schedule_flush_locked(item.key, 1000);
                } catch (const std::bad_alloc&) {
                }
            }
            write_errors_++;
            return Status::OutOfMemory("out of memory while writing to the flash journal");
        }
    }

    // The status of an append the journal refused, by its cause
    Status refusal(const std::string& what) {
        const FlashLog::Error e = log_->last_error();
        last_refusal_ = e;
        switch (e) {
            case FlashLog::Error::Full:
                return Status::StorageFull("flash journal is full: " + std::to_string(log_->live_bytes()) + " of " +
                                           std::to_string(log_->capacity_bytes()) + " bytes live, " + what + " does not fit");
            case FlashLog::Error::TooLarge:
                return Status::StorageFull(what + " is larger than a flash journal record");
            case FlashLog::Error::OutOfMemory:
                return Status::OutOfMemory("out of memory while writing " + what + " to the flash journal");
            case FlashLog::Error::NoSpace: {
                const FlashLogStats ls = log_->stats();
                return Status::InternalError("flash journal could not free a sector for " + what + " (" +
                                             std::to_string(ls.used_sectors) + " of " + std::to_string(ls.sector_count) +
                                             " sectors in use)");
            }
            case FlashLog::Error::Unreadable:
                return Status::InternalError("the flash journal could not be read at boot; nothing is written to it");
            default:
                return Status::InternalError("flash I/O error while writing " + what + " to the journal");
        }
    }

    Status write_items(Batch batch, std::vector<Item>& items) {
        uint64_t mark = 0;
        uint64_t covered = 0;   // when the batch takes every urgent change: the revision they reach
        {
            std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
            const uint64_t now = steady_ms();
            bool urgent_left = false;
            for (auto it = pending_.begin(); it != pending_.end();) {
                const bool take = batch == Batch::All || it->second.due <= now || (batch == Batch::Urgent && !it->second.deferred);
                if (!take) {
                    if (!it->second.deferred) urgent_left = true;
                    ++it;
                    continue;
                }
                Item item;
                item.key = it->first;
                bool ephemeral = false;
                bool on_flash = false;
                item.present = tree_.stat(item.key, nullptr, &item.rev, &ephemeral, &on_flash) && !ephemeral;
                // A released value is in the journal already
                if (item.present && on_flash) { it = pending_.erase(it); continue; }
                if (!item.present) item.rev = current_version_;
                item.flash_only = persist_class(item.key) == PersistClass::FlashOnly;
                items.push_back(std::move(item));
                it = pending_.erase(it);
            }
            // Deletes first: the space they free is counted already when a
            // put in the same batch is checked against the journal's capacity
            std::stable_partition(items.begin(), items.end(), [](const Item& i) { return !i.present; });
            if (!urgent_left) covered = current_version_;
            if (current_version_ + kRevisionBlock / 2 > revision_reserved_) mark = current_version_ + kRevisionBlock;
        }
        Status result = Status::Ok();
        bool wrote = false;
        if (mark) {
            if (log_->append_revision_mark(mark)) {
                wrote = true;
                std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
                revision_reserved_ = mark;
            } else {
                write_errors_++;
                result = refusal("the revision mark");
            }
        }
        std::vector<size_t> on_flash;   // FlashOnly values the journal now holds
        on_flash.reserve(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            Item& item = items[i];
            bool ok = true;
            if (item.present) {
                // The append reads the value in the tree, so the tree holds
                // still until it is written
                std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
                bool ephemeral = false;
                bool released = false;
                const std::string* value = tree_.value_of(item.key, &item.rev, &ephemeral, &released);
                // Removed since (its delete is pending), or written already
                if (!value || ephemeral || released) continue;
                const uint64_t fp = value_fingerprint(*value);
                if (log_->has_value(item.key, fp)) {
                    unchanged_skipped_++;
                    if (item.flash_only) on_flash.push_back(i);
                    continue;
                }
                ok = log_->append_put(item.key, *value, item.rev, fp);
            } else {
                ok = log_->append_delete(item.key, item.rev);
            }
            if (!ok) {
                write_errors_++;
                result = refusal(item.key);
                // Out of memory may pass soon; the other causes do not
                const uint32_t retry_ms = result.code == StatusCode::OutOfMemory ? 1000 : 5000;
                std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
                schedule_flush_locked(item.key, retry_ms);
            } else {
                wrote = true;
                if (item.present && item.flash_only) on_flash.push_back(i);
            }
        }
        if (wrote && !log_->sync()) {
            write_errors_++;
            result = Status::InternalError("flash journal sync failed");
        } else if (!on_flash.empty()) {
            // Durable: the RAM copy goes, unless the key changed meanwhile
            std::lock_guard<std::recursive_mutex> lock(rw_mutex_);
            for (size_t i : on_flash) tree_.release_value(items[i].key, items[i].rev);
        }
        if (result.ok() && covered) {
            {
                std::lock_guard<std::mutex> lock(commit_mutex_);
                if (covered > committed_version_) committed_version_ = covered;
            }
            commit_cv_.notify_all();
        }
        return result;
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
