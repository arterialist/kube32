#include "kubelet/node_status_reporter.hpp"
#include "wasm3.h"
#include "runtime/pod_memory_pool.h"
#include "osal/system.hpp"
#include "hal/capability_firewall.hpp"
#include "hal/sd_card.hpp"
#include "api/version_info.hpp"

#include <charconv>

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
    t.free_psram_bytes = osal::System::get_free_psram_bytes();
    t.total_psram_bytes = osal::System::get_total_psram_bytes();
    // Memory parked in the pod pool at boot is free for scheduling purposes
    int pool_external = 0;
    size_t pool_free = kube32_pool_available(&pool_external);
#if !defined(ESP_PLATFORM)
    // The simulator's PSRAM is the external pool block it reserved
    // (kube32-sim --psram); the host itself has none.
    if (pool_external) t.total_psram_bytes = kube32_pool_block_size();
    // A modelled board heap (kube32-sim --psram 0): what pods leave of it
    if (const size_t budget = kube32_pool_heap_budget()) {
        const size_t used = kube32_pool_general_bytes();
        t.total_heap_bytes = budget;
        t.free_heap_bytes = used < budget ? budget - used : 0;
    }
#endif
    if (pool_external) t.free_psram_bytes += pool_free; else t.free_heap_bytes += pool_free;
    t.free_heap_bytes += kube32_pool_stack_available();
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

    hal::SdInfo sd = hal::sd_info();
    t.sd_present = sd.mounted;
    t.sd_total_bytes = sd.mounted ? sd.total_bytes : 0;
    t.sd_free_bytes = sd.mounted ? sd.free_bytes : 0;

    return t;
}

// One resource list (capacity or allocatable). Resources the node does not
// have are left out. Written piece by piece: no temporary strings.
template <typename Out>
static void append_resources(Out& json, const NodeTelemetry& t, bool capacity) {
    bool first = true;
    auto key = [&](const char* name) {
        if (!first) json += ",";
        first = false;
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
    if (t.total_heap_bytes > 0) kib("memory", capacity ? t.total_heap_bytes : t.free_heap_bytes);
    if (t.cpu_cores > 0) count("cpu", t.cpu_cores);
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
            else kib(name, t.free_psram_bytes);
        }
    }
    // The card: capacity is its size, allocatable the free space, both measured
    if (t.sd_present) quantity("hardware.kube32.io/sd", capacity ? t.sd_total_bytes : t.sd_free_bytes);
}

template <typename Out>
static void append_ready_condition(Out& json, const NodeTelemetry& t) {
    json += "{\"type\":\"Ready\",\"status\":\"";
    json += t.ready ? "True" : "False";
    json += "\",\"reason\":\"";
    json += t.ready_reason;
    json += "\",\"message\":\"";
    json += t.ready_message;
    json += "\"}";
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
    json += "\"}";
}

template <typename Out>
void NodeStatusReporter::append_registration(Out& json, const NodeTelemetry& telemetry) const {
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
    if (!annotations_.empty()) {
        json += ",\"annotations\":{";
        bool first = true;
        for (const auto& kv : annotations_) {
            if (!first) json += ",";
            first = false;
            json += "\"";
            json += kv.first;
            json += "\":\"";
            json += kv.second;
            json += "\"";
        }
        json += "}";
    }
    json += "},";
    json += "\"spec\":{\"unschedulable\":false},";
    json += "\"status\":{";
    json += "\"conditions\":[";
    append_ready_condition(json, telemetry);
    append_firmware_condition(json, telemetry);
    json += "],";
    json += "\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"";
    json += node_ip_;
    json += "\"}],";
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
    json += "\"capacity\":{";
    append_resources(json, telemetry, true);
    json += "},\"allocatable\":{";
    append_resources(json, telemetry, false);
    json += "}}}";
}

std::string NodeStatusReporter::build_registration_json(const NodeTelemetry& telemetry) const {
    std::string json;
    json.reserve(1024);
    append_registration(json, telemetry);
    return json;
}

bool NodeStatusReporter::write_registration_json(mem::FixedText& out, const NodeTelemetry& telemetry) const {
    out.clear();
    append_registration(out, telemetry);
    return !out.overflowed();
}

template <typename Out>
static void append_heartbeat_patch(Out& json, const NodeTelemetry& telemetry, bool memory_pressure) {
    json += "{\"status\":{";
    json += "\"conditions\":[";
    append_ready_condition(json, telemetry);
    json += ",{\"type\":\"MemoryPressure\",\"status\":\"";
    json += memory_pressure ? "True" : "False";
    json += "\",\"reason\":\"";
    json += memory_pressure ? "KubeletHasMemoryPressure" : "KubeletHasSufficientMemory";
    json += "\"}";
    append_firmware_condition(json, telemetry);
    json += "],";
    json += "\"allocatable\":{";
    append_resources(json, telemetry, false);
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
