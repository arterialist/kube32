#include "kubelet/node_status_reporter.hpp"
#include "wasm3.h"
#include "runtime/pod_memory_pool.h"
#include "osal/system.hpp"
#include "hal/capability_firewall.hpp"
#include "hal/sd_card.hpp"
#include "api/version_info.hpp"

#include <charconv>
#include <cstring>

namespace kube32::kubelet {

// The JSON writers take std::string (registration, tests) or mem::FixedText
// (the heartbeat, on the kubelet's stack).
template <typename Out>
static void append_uint(Out& out, uint64_t v) {
    char buf[24];
    out.append(buf, static_cast<size_t>(std::to_chars(buf, buf + sizeof(buf), v).ptr - buf));
}

// Exact byte count as a Kubernetes quantity: "30Mi"/"512Ki" when it divides, else plain bytes.
template <typename Out>
static void append_bytes_quantity(Out& out, uint64_t bytes) {
    if (bytes > 0 && bytes % (1024ULL * 1024ULL) == 0) {
        append_uint(out, bytes / (1024ULL * 1024ULL));
        out += "Mi";
    } else if (bytes > 0 && bytes % 1024ULL == 0) {
        append_uint(out, bytes / 1024ULL);
        out += "Ki";
    } else {
        append_uint(out, bytes);
    }
}

static uint32_t count_bits(uint64_t v) {
    uint32_t n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

NodeStatusReporter::NodeStatusReporter(const std::string& node_name, const std::string& node_ip, bool control_plane)
    : node_name_(node_name), node_ip_(node_ip), control_plane_(control_plane), firmware_version_(api::firmware_version()) {}

NodeTelemetry NodeStatusReporter::collect_telemetry() const {
    NodeTelemetry t;
    t.total_heap_bytes = osal::System::get_total_heap_bytes();
    t.free_heap_bytes = osal::System::get_free_heap_bytes();
    t.min_free_heap_bytes = osal::System::get_min_free_heap_bytes();
    t.total_psram_bytes = osal::System::get_total_psram_bytes();
    t.free_psram_bytes = osal::System::get_free_psram_bytes();
    // What pods can be given, counted as the kubelet admits them: with no
    // pod running (allocatable), and now
    kube32_pool_node_room(&t.memory_allocatable_bytes, &t.psram_allocatable_bytes);
    t.memory_free_bytes = kube32_pool_pod_room(0, nullptr);
    t.memory_capacity_bytes = t.total_heap_bytes + t.total_psram_bytes;
#if !defined(ESP_PLATFORM)
    // The simulator's PSRAM is the external pool block it reserved
    // (kube32-sim --psram) and, with a heap budget, the PSRAM besides it;
    // the host itself has none.
    int pool_external = 0;
    const size_t pool_free = kube32_pool_available(&pool_external);
    const size_t budget = kube32_pool_heap_budget();
    const size_t pods_heap = kube32_pool_general_bytes();
    if (pool_external) {
        t.total_psram_bytes = kube32_pool_block_size() + budget;
        t.free_psram_bytes = pool_free + (pods_heap < budget ? budget - pods_heap : 0);
    }
    t.memory_capacity_bytes = t.total_heap_bytes;
    if (budget) {
        // A modelled board heap (kube32-sim --heap-budget): what pods leave of it
        if (!pool_external) {
            t.total_heap_bytes = budget;
            t.free_heap_bytes = pods_heap < budget ? budget - pods_heap : 0;
        }
        t.memory_capacity_bytes = budget + kube32_pool_block_size();
    }
#endif
    t.cpu_cores = osal::System::get_cpu_cores();
    t.cpu_freq_mhz = osal::System::get_cpu_freq_mhz();
    t.uptime_ms = osal::System::get_uptime_ms();

    // Peripheral capacity is what the board profile lets pods claim;
    // allocatable is that minus what running pods hold.
    const hal::CapabilityFirewall& fw = hal::CapabilityFirewall::instance();
    const uint64_t gpio = fw.claimable_gpio_mask();
    t.total_gpio_count = count_bits(gpio);
    t.allocatable_gpio_count = count_bits(gpio & ~fw.get_allocated_gpio_mask());
    const uint16_t adc = fw.claimable_adc1_mask();
    t.total_adc_count = count_bits(adc);
    t.allocatable_adc_count = count_bits(adc & static_cast<uint16_t>(~fw.get_allocated_adc1_mask()));
    const uint8_t i2c = fw.claimable_i2c_mask();
    t.total_i2c_count = count_bits(i2c);
    t.allocatable_i2c_count = count_bits(i2c & static_cast<uint8_t>(~fw.get_allocated_i2c_mask()));
    const uint8_t uart = fw.claimable_uart_mask();
    t.total_uart_count = count_bits(uart);
    t.allocatable_uart_count = count_bits(uart & static_cast<uint8_t>(~fw.get_allocated_uart_mask()));
    t.total_rmt_tx_count = fw.claimable_rmt_tx_count();
    const uint32_t rmt_tx_held = fw.get_allocated_rmt_tx_count();
    t.allocatable_rmt_tx_count = t.total_rmt_tx_count > rmt_tx_held ? t.total_rmt_tx_count - rmt_tx_held : 0;
    t.total_rmt_rx_count = fw.claimable_rmt_rx_count();
    const uint32_t rmt_rx_held = fw.get_allocated_rmt_rx_count();
    t.allocatable_rmt_rx_count = t.total_rmt_rx_count > rmt_rx_held ? t.total_rmt_rx_count - rmt_rx_held : 0;

    t.pods_capacity = max_pods_;
    t.sd_present = hal::sd_is_mounted();
    const uint32_t onboard = fw.onboard_devices();
    t.has_display = (onboard & hal::KUBE32_CAP_DISPLAY) != 0;
    t.has_keyboard = (onboard & hal::KUBE32_CAP_KEYBOARD) != 0;
    t.has_speaker = (onboard & hal::KUBE32_CAP_SPEAKER) != 0;
    // The ESP32-S3's own sensor (hal/chip_temp.hpp); the simulator's HAL
    // stands in for it
    t.has_chip_temp = true;

    return t;
}

// Resources a node may have in one boot and not in another, or lose while
// it runs (a card taken out): a bit each, in this order
static const char* const kOptionalResources[] = {
    "esp.io/gpio", "esp32.io/gpio", "esp.io/i2c", "esp32.io/i2c", "esp.io/uart", "esp32.io/uart",
    "esp.io/adc", "esp32.io/adc", "esp.io/rmt-tx", "esp32.io/rmt-tx", "esp.io/rmt-rx", "esp32.io/rmt-rx",
    "hardware.kube32.io/ipc", "hardware.kube32.io/tft-display", "hardware.kube32.io/keyboard",
    "hardware.kube32.io/speaker", "hardware.kube32.io/chip-temp", "hardware.kube32.io/sd",
};
static constexpr size_t kOptionalCount = sizeof(kOptionalResources) / sizeof(kOptionalResources[0]);
static_assert(kOptionalCount <= 32, "one bit per resource in a uint32_t");

static uint32_t optional_bit(const char* name) {
    for (size_t i = 0; i < kOptionalCount; ++i) {
        if (std::strcmp(kOptionalResources[i], name) == 0) return 1u << i;
    }
    return 0;
}

// Takes the JSON and keeps nothing: what a resource list would hold
struct DiscardJson {
    DiscardJson& operator+=(const char*) { return *this; }
    DiscardJson& operator+=(char) { return *this; }
    void append(const char*, size_t) {}
};

// One resource list (capacity or allocatable). Resources the node does not
// have are left out, and those of `clear_bits` among them set to null (a
// patch takes them off the Node). Returns the optional resources written.
// Written piece by piece: no temporary strings.
template <typename Out>
static uint32_t append_resources(Out& json, const NodeTelemetry& t, bool capacity, uint32_t clear_bits = 0) {
    bool first = true;
    uint32_t written = 0;
    auto key = [&](const char* name) {
        if (!first) json += ",";
        first = false;
        written |= optional_bit(name);
        json += "\"";
        json += name;
        json += "\":\"";
    };
    auto count = [&](const char* name, uint64_t v) {
        key(name);
        append_uint(json, v);
        json += "\"";
    };
    auto kib = [&](const char* name, uint64_t bytes) {
        key(name);
        append_uint(json, bytes / 1024);
        json += "Ki\"";
    };
    auto quantity = [&](const char* name, uint64_t bytes) {
        key(name);
        append_bytes_quantity(json, bytes);
        json += "\"";
    };
    // Memory: allocatable is what pods can be given with no pod running, a
    // fixed figure; the scheduler subtracts what bound pods request
    if (t.memory_capacity_bytes > 0) kib("memory", capacity ? t.memory_capacity_bytes : t.memory_allocatable_bytes);
    if (t.cpu_cores > 0) count("cpu", t.cpu_cores);
    // As Kubernetes counts pods: allocatable is capacity, the scheduler and
    // the kubelet's admission count the pods on the node against it
    if (t.pods_capacity > 0) count("pods", t.pods_capacity);
    if (t.total_gpio_count > 0) {
        const uint64_t v = capacity ? t.total_gpio_count : t.allocatable_gpio_count;
        count("esp.io/gpio", v);
        count("esp32.io/gpio", v);
    }
    if (t.total_i2c_count > 0) {
        const uint64_t v = capacity ? t.total_i2c_count : t.allocatable_i2c_count;
        count("esp.io/i2c", v);
        count("esp32.io/i2c", v);
    }
    if (t.total_uart_count > 0) {
        const uint64_t v = capacity ? t.total_uart_count : t.allocatable_uart_count;
        count("esp.io/uart", v);
        count("esp32.io/uart", v);
    }
    if (t.total_adc_count > 0) {
        const uint64_t v = capacity ? t.total_adc_count : t.allocatable_adc_count;
        count("esp.io/adc", v);
        count("esp32.io/adc", v);
    }
    if (t.total_rmt_tx_count > 0) {
        const uint64_t v = capacity ? t.total_rmt_tx_count : t.allocatable_rmt_tx_count;
        count("esp.io/rmt-tx", v);
        count("esp32.io/rmt-tx", v);
    }
    if (t.total_rmt_rx_count > 0) {
        const uint64_t v = capacity ? t.total_rmt_rx_count : t.allocatable_rmt_rx_count;
        count("esp.io/rmt-rx", v);
        count("esp32.io/rmt-rx", v);
    }
    if (t.total_psram_bytes > 0) {
        for (const char* name : {"esp.io/psram", "esp32.io/psram"}) {
            if (capacity) quantity(name, t.total_psram_bytes);
            else kib(name, t.psram_allocatable_bytes);
        }
    }
    // Device grants: any number of pods share a device and each holds a
    // grant at most once, so as many pods as the node runs can hold one
    if (t.pods_capacity > 0) {
        count("hardware.kube32.io/ipc", t.pods_capacity);
        if (t.has_display) count("hardware.kube32.io/tft-display", t.pods_capacity);
        if (t.has_keyboard) count("hardware.kube32.io/keyboard", t.pods_capacity);
        if (t.has_speaker) count("hardware.kube32.io/speaker", t.pods_capacity);
        if (t.has_chip_temp) count("hardware.kube32.io/chip-temp", t.pods_capacity);
        if (t.sd_present) count("hardware.kube32.io/sd", t.pods_capacity);
    }
    for (size_t i = 0; i < kOptionalCount; ++i) {
        if (!(clear_bits & ~written & (1u << i))) continue;
        if (!first) json += ",";
        first = false;
        json += "\"";
        json += kOptionalResources[i];
        json += "\":null";
    }
    return written;
}

uint32_t NodeStatusReporter::resource_bits(const NodeTelemetry& telemetry) {
    DiscardJson none;
    return append_resources(none, telemetry, true);
}

// ,"lastHeartbeatTime":"...","lastTransitionTime":"..." for the times known
template <typename Out>
static void append_condition_times(Out& json, const NodeTelemetry& t, uint64_t transition_unix_ms) {
    char ts[32];
    for (const auto& field : {std::make_pair(",\"lastHeartbeatTime\":\"", t.heartbeat_unix_ms),
                              std::make_pair(",\"lastTransitionTime\":\"", transition_unix_ms)}) {
        const size_t n = field.second ? models::format_rfc3339(field.second, ts, sizeof(ts)) : 0;
        if (!n) continue;
        json += field.first;
        json.append(ts, n);
        json += "\"";
    }
}

// What is free now, as annotations: kube32.io/memory-free (what pods can be
// given now, on the base of allocatable.memory), kube32.io/heap-free (free
// internal RAM) and kube32.io/psram-free (free PSRAM)
template <typename Out>
static void append_memory_annotations(Out& json, const NodeTelemetry& t) {
    json += "\"kube32.io/memory-free\":\"";
    append_bytes_quantity(json, t.memory_free_bytes);
    json += "\",\"kube32.io/heap-free\":\"";
    append_bytes_quantity(json, t.free_heap_bytes);
    json += "\"";
    if (t.total_psram_bytes > 0) {
        json += ",\"kube32.io/psram-free\":\"";
        append_bytes_quantity(json, t.free_psram_bytes);
        json += "\"";
    }
}

template <typename Out>
static void append_ready_condition(Out& json, const NodeTelemetry& t) {
    json += "{\"type\":\"Ready\",\"status\":\"";
    json += t.ready ? "True" : "False";
    json += "\",\"reason\":\"";
    json += t.ready_reason;
    json += "\",\"message\":\"";
    json += t.ready_message;
    json += "\"";
    append_condition_times(json, t, t.ready_transition_unix_ms);
    json += "}";
}

template <typename Out>
static void append_firmware_condition(Out& json, const NodeTelemetry& t) {
    if (!t.firmware_reported) return;
    json += ",{\"type\":\"FirmwareUpdate\",\"status\":\"";
    json += t.firmware_updating ? "True" : "False";
    json += "\",\"reason\":\"";
    json += t.firmware_reason;
    json += "\",\"message\":\"";
    for (const char* p = t.firmware_message; *p; ++p) {
        if (*p == '"' || *p == '\\') json += '\\';
        json += *p;
    }
    json += "\"";
    append_condition_times(json, t, t.firmware_transition_unix_ms);
    json += "}";
}

template <typename Out>
void NodeStatusReporter::append_registration(Out& json, const NodeTelemetry& telemetry, bool as_patch) const {
    json += "{\"kind\":\"Node\",\"apiVersion\":\"v1\",";
    json += "\"metadata\":{\"name\":\"";
    json += node_name_;
    json += "\",\"labels\":{\"kubernetes.io/hostname\":\"";
    json += node_name_;
    json += "\",\"kubernetes.io/arch\":\"";
    json += api::build_info().arch;
    json += "\",";
    for (const auto& kv : labels_) {
        json += "\"";
        json += kv.first;
        json += "\":\"";
        json += kv.second;
        json += "\",";
    }
    json += control_plane_ ? "\"node-role.kubernetes.io/control-plane\":\"\"}" : "\"node-role.kubernetes.io/worker\":\"\"}";
    json += ",\"annotations\":{";
    append_memory_annotations(json, telemetry);
    for (const auto& kv : annotations_) {
        json += ",\"";
        json += kv.first;
        json += "\":\"";
        json += kv.second;
        json += "\"";
    }
    json += "}";
    json += "},";
    // No spec: registering again over an existing Node (after a reboot or
    // rejected heartbeats) must not uncordon it
    json += "\"status\":{";
    json += "\"conditions\":[";
    append_ready_condition(json, telemetry);
    append_firmware_condition(json, telemetry);
    json += "],";
    // No address until the board has one: 0.0.0.0 is nobody's address
    if (!node_ip_.empty() && node_ip_ != "0.0.0.0") {
        json += "\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"";
        json += node_ip_;
        json += "\"}],";
    }
    if (kubelet_port_) {
        json += "\"daemonEndpoints\":{\"kubeletEndpoint\":{\"Port\":";
        append_uint(json, kubelet_port_);
        json += "}},";
    }
    json += "\"nodeInfo\":{\"kubeletVersion\":\"";
    json += api::KUBE32_GIT_VERSION;
    json += "\",\"osImage\":\"kube32 ";
    json += firmware_version_;
    json += " (";
    json += osal::System::get_os_image();
    json += ")";
    json += "\",\"kernelVersion\":\"";
    json += osal::System::get_kernel_version();
    json += "\",\"containerRuntimeVersion\":\"wasm3://";
    json += M3_VERSION;
    json += "\"},";
    // Over a Node from an earlier boot, every resource this boot does not have goes
    const uint32_t clear = as_patch ? ~0u : 0u;
    json += "\"capacity\":{";
    append_resources(json, telemetry, true, clear);
    json += "},\"allocatable\":{";
    append_resources(json, telemetry, false, clear);
    json += "}}}";
}

std::string NodeStatusReporter::build_registration_json(const NodeTelemetry& telemetry, bool as_patch) const {
    std::string json;
    json.reserve(1024);
    append_registration(json, telemetry, as_patch);
    return json;
}

bool NodeStatusReporter::write_registration_json(mem::FixedText& out, const NodeTelemetry& telemetry, bool as_patch) const {
    out.clear();
    append_registration(out, telemetry, as_patch);
    return !out.overflowed();
}

template <typename Out>
void NodeStatusReporter::append_heartbeat_patch(Out& json, const NodeTelemetry& telemetry, bool memory_pressure) const {
    json += "{\"metadata\":{\"annotations\":{";
    append_memory_annotations(json, telemetry);
    json += "}},\"status\":{";
    json += "\"conditions\":[";
    append_ready_condition(json, telemetry);
    json += ",{\"type\":\"MemoryPressure\",\"status\":\"";
    json += memory_pressure ? "True" : "False";
    json += "\",\"reason\":\"";
    json += memory_pressure ? "KubeletHasMemoryPressure" : "KubeletHasSufficientMemory";
    json += "\"";
    append_condition_times(json, telemetry, telemetry.memory_pressure_transition_unix_ms);
    json += "}";
    append_firmware_condition(json, telemetry);
    json += "],";
    // Capacity only when the node's resources changed (a card taken out or
    // put in): then, as allocatable, without the ones it no longer has
    const uint32_t on_node = node_resources_.load();
    json += "\"allocatable\":{";
    const uint32_t has = append_resources(json, telemetry, false, on_node);
    if (has != on_node) {
        json += "},\"capacity\":{";
        append_resources(json, telemetry, true, on_node);
    }
    json += "}}}";
}

std::string NodeStatusReporter::build_heartbeat_patch_json(const NodeTelemetry& telemetry, bool memory_pressure) const {
    std::string json;
    json.reserve(512);
    append_heartbeat_patch(json, telemetry, memory_pressure);
    return json;
}

bool NodeStatusReporter::write_heartbeat_patch_json(mem::FixedText& out, const NodeTelemetry& telemetry, bool memory_pressure) const {
    out.clear();
    append_heartbeat_patch(out, telemetry, memory_pressure);
    return !out.overflowed();
}

} // namespace kube32::kubelet
