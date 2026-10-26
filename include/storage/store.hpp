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

struct StoreOptions {
    std::string base_path{"/spiffs"};
    bool enable_flash_persistence{true};
    size_t max_value_size_bytes{64 * 1024}; // 64KB max per JSON manifest
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
};

// Factory to create standard store instance
std::unique_ptr<Store> create_radix_store(StoreOptions options = {});

} // namespace kube32::storage
