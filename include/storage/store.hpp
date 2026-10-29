#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <memory>
#include <utility>

namespace kube32::storage {

enum class StatusCode {
    Ok = 0,
    NotFound = 1,
    AlreadyExists = 2,
    InvalidArgument = 3,
    StorageFull = 4,
    VersionConflict = 5,
    InternalError = 6
};

struct Status {
    StatusCode code;
    std::string message;

    bool ok() const { return code == StatusCode::Ok; }

    static Status Ok() { return {StatusCode::Ok, ""}; }
    static Status NotFound(std::string msg = "Key not found") { 
        return {StatusCode::NotFound, std::move(msg)}; 
    }
    static Status AlreadyExists(std::string msg = "Key already exists") { 
        return {StatusCode::AlreadyExists, std::move(msg)}; 
    }
    static Status InvalidArgument(std::string msg) { 
        return {StatusCode::InvalidArgument, std::move(msg)}; 
    }
    static Status StorageFull(std::string msg = "Storage full or out of space") { 
        return {StatusCode::StorageFull, std::move(msg)}; 
    }
    static Status VersionConflict(std::string msg = "ResourceVersion conflict") { 
        return {StatusCode::VersionConflict, std::move(msg)}; 
    }
    static Status InternalError(std::string msg) { 
        return {StatusCode::InternalError, std::move(msg)}; 
    }
};

enum class WatchEventType {
    Added = 0,
    Modified = 1,
    Deleted = 2
};

struct WatchEvent {
    WatchEventType type;
    std::string key;
    std::string json;
    uint64_t resource_version;
};

using WatchCallback = std::function<void(const WatchEvent& event)>;

class FlashRegion;

struct StoreOptions {
    std::string base_path{"/spiffs"};
    bool enable_flash_persistence{true};
    size_t max_value_size_bytes{64 * 1024}; // 64KB max per JSON manifest
    // Raw flash for the log-structured journal (storage/flash_log.hpp). When
    // set, it replaces the file-per-key journal under base_path: changes are
    // written by flush(), coalesced per key, and Leases, Events and Node
    // heartbeats are kept off flash (storage/persist_policy.hpp).
    FlashRegion* flash_region{nullptr};
    uint32_t flush_delay_ms{1000};            // longest a change to a durable key waits for flush()
    uint32_t deferred_flush_delay_ms{600000}; // the same for Node status heartbeats
};

struct PersistenceStats {
    bool flash_log{false};
    size_t region_bytes{0};
    size_t capacity_bytes{0};     // live bytes the journal accepts
    size_t live_bytes{0};
    size_t live_keys{0};
    size_t used_sectors{0};
    size_t sector_count{0};
    size_t pending_keys{0};       // changed in RAM, not yet on flash
    uint64_t records_written{0};
    uint64_t bytes_written{0};
    uint64_t records_moved{0};
    uint64_t sectors_erased{0};
    uint64_t unchanged_skipped{0};  // flushes of a value identical to the one on flash
    uint64_t write_errors{0};
};

class Store {
public:
    virtual ~Store() = default;

    // Base contract (matches PROJECT.md Section 4.1)
    virtual Status get(const std::string& key, std::string& out_json) = 0;
    virtual Status put(const std::string& key, const std::string& json, bool overwrite) = 0;
    virtual Status remove(const std::string& key) = 0;
    virtual Status list(const std::string& prefix, std::vector<std::string>& out_jsons) = 0;
    virtual Status persist() = 0; // Atomic LittleFS commit
    virtual Status recover() = 0; // Restore on boot

    // Extended operational methods for Kubernetes controllers
    virtual Status get_extended(const std::string& key, std::string& out_json, uint64_t* out_version) = 0;
    virtual Status put_extended(const std::string& key, const std::string& json, 
                                bool overwrite, bool persist_to_flash, 
                                uint64_t expected_version = 0, 
                                uint64_t* out_version = nullptr) = 0;
    virtual Status list_extended(const std::string& prefix, 
                                 std::vector<std::string>& out_jsons, 
                                 std::vector<std::string>* out_keys) = 0;
    virtual uint32_t watch(const std::string& prefix, WatchCallback cb) = 0;
    virtual void unwatch(uint32_t watch_id) = 0;
    virtual uint64_t current_version() const = 0;

    // Writes the changes that are due (all of them with `all`) to the flash
    // journal. Runs on the caller's thread; on the ESP32 that thread's stack
    // must be in internal RAM. A no-op for the file journal and RAM-only stores.
    virtual Status flush(bool all = false) { (void)all; return Status::Ok(); }
    // False when the store has no flash journal.
    virtual bool persistence_stats(PersistenceStats& out) const { (void)out; return false; }
};

// Factory to create standard store instance
std::unique_ptr<Store> create_radix_store(StoreOptions options = {});

// Returns `json` with metadata.resourceVersion set to `version` (inserted when
// absent). Exposed for tests; the radix store applies it on every put.
std::string inject_resource_version(const std::string& json, uint64_t version);

} // namespace kube32::storage
