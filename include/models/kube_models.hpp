#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include <string_view>

namespace kube32::models {

// =============================================================================
// Lightweight RFC 8259 JSON DOM
// =============================================================================

enum class JsonType {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object
};

struct JsonValue {
    JsonType type{JsonType::Null};
    bool bool_val{false};
    double num_val{0.0};
    std::string str_val;
    std::vector<JsonValue> arr_val;
    std::vector<std::pair<std::string, JsonValue>> obj_val;

    bool is_null() const { return type == JsonType::Null; }
    bool is_bool() const { return type == JsonType::Boolean; }
    bool is_number() const { return type == JsonType::Number; }
    bool is_string() const { return type == JsonType::String; }
    bool is_array() const { return type == JsonType::Array; }
    bool is_object() const { return type == JsonType::Object; }

    const JsonValue* get(const std::string& key) const;
    JsonValue* get_mut(const std::string& key);
    void set(const std::string& key, JsonValue val);
    bool remove(const std::string& key);

    std::string get_string(const std::string& key, const std::string& def = "") const;
    int64_t get_int(const std::string& key, int64_t def = 0) const;
    bool get_bool(const std::string& key, bool def = false) const;

    std::string to_string(bool pretty = false, int indent_level = 0) const;
};

// JSON Parser & Serializer Functions
bool parse_json(std::string_view input, JsonValue& out_value, std::string& out_error);
void merge_json(JsonValue& target, const JsonValue& patch);

// =============================================================================
// Strongly-Typed Model Structs for Table Serialization and Scheduler
// =============================================================================

struct KubeNode {
    std::string name;
    bool is_ready{true};
    std::string roles{"worker"};
    uint64_t age_seconds{~0ULL}; // AGE_UNKNOWN until creationTimestamp is known
    std::string version{"v1.33.0-kube32"};
    std::string internal_ip{"127.0.0.1"};
    std::string external_ip{"<none>"};
    std::string os_image{"ESP-IDF v5.2"};
    std::string kernel_version{"FreeRTOS-v10.4"};
    std::string container_runtime{"kube32-runtime://0.1.0"};

    uint64_t memory_capacity{320 * 1024};
    uint64_t memory_allocatable{240 * 1024};
    uint32_t gpio_allocatable{14};
    uint32_t i2c_allocatable{2};
    uint32_t adc_allocatable{2};
    uint64_t psram_allocatable{3840 * 1024};
    std::map<std::string, std::string> labels;
};

struct KubePod {
    std::string name;
    std::string namespace_name{"default"};
    std::string uid;                       // metadata.uid as assigned by the API server
    std::string deletion_timestamp;        // set while the pod is Terminating
    uint32_t ready_containers{1};
    uint32_t total_containers{1};
    std::string status{"Pending"}; // Running, Pending, CrashLoopBackOff
    uint32_t restart_count{0};
    uint64_t age_seconds{~0ULL};
    std::string ip{"<none>"};
    std::string node_name{"<none>"};

    // Resource requests
    uint64_t memory_request_bytes{16 * 1024};
    uint64_t psram_request_bytes{0};
    uint32_t gpio_request{0};
    uint32_t i2c_request{0};
    uint32_t adc_request{0};
    std::map<std::string, std::string> node_selector;

    // Workload image and runtime configuration (first container)
    std::string container_name;              // first container's name (events, metrics)
    std::string image;                       // e.g. "wasm://embedded/tessera-core:v1"
    uint64_t memory_limit_bytes{0};          // resources.limits.memory, 0 when absent
    std::vector<std::pair<std::string, std::string>> env; // container env name/value pairs
    std::map<std::string, std::string> labels;
    std::map<std::string, std::string> annotations;
};

// Parses a Kubernetes resource quantity ("1536Ki", "2Mi", "64K", "4096") into bytes.
uint64_t parse_quantity(const std::string& value);

struct KubeDeployment {
    std::string name;
    std::string namespace_name{"default"};
    uint32_t ready_replicas{1};
    uint32_t replicas{1};
    uint32_t updated_replicas{1};
    uint32_t available_replicas{1};
    uint64_t age_seconds{~0ULL};
    std::string containers{"app"};
    std::string images{"wasm://app"};
    std::string selector{"app=test"};
};

// =============================================================================
// Time and identity helpers shared by the API server, controllers and kubelet
// =============================================================================

// RFC 3339 UTC timestamp ("2026-10-28T09:12:33Z") from Unix milliseconds.
std::string format_rfc3339(uint64_t unix_ms);
// Current wall-clock time as RFC 3339, or "" while the platform clock is unset.
std::string now_rfc3339();
// Parses "YYYY-MM-DDTHH:MM:SS[.fff]Z" (and +00:00). Returns false on malformed input.
bool parse_rfc3339(std::string_view text, uint64_t& out_unix_ms);
// Seconds between a creationTimestamp and now. Returns UINT64_MAX when either
// side is unknown (no wall clock, missing or malformed timestamp).
uint64_t age_seconds_since(std::string_view rfc3339_timestamp);
constexpr uint64_t AGE_UNKNOWN = ~0ULL;
// RFC 4122 v4 layout from platform entropy; identifiers for metadata.uid.
std::string generate_uid();

// Converters from JSON AST to typed structs
KubeNode parse_node_manifest(const std::string& json_str);
KubePod parse_pod_manifest(const std::string& json_str);
KubeDeployment parse_deployment_manifest(const std::string& json_str);

} // namespace kube32::models
