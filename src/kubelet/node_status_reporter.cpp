#include "kubelet/node_status_reporter.hpp"
#include "wasm3.h"
#include "runtime/pod_memory_pool.h"
#include "osal/system.hpp"
#include "hal/capability_firewall.hpp"
#include "hal/sd_card.hpp"
#include "api/version_info.hpp"

namespace kube32::kubelet {

// Exact byte count as a Kubernetes quantity: "30Mi"/"512Ki" when it divides, else plain bytes.
static std::string bytes_quantity(uint64_t bytes) {
    if (bytes > 0 && bytes % (1024ULL * 1024ULL) == 0) return std::to_string(bytes / (1024ULL * 1024ULL)) + "Mi";
    if (bytes > 0 && bytes % 1024ULL == 0) return std::to_string(bytes / 1024ULL) + "Ki";
    return std::to_string(bytes);
}

static uint32_t count_bits(uint64_t v) {
    uint32_t n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

NodeStatusReporter::NodeStatusReporter(const std::string& node_name, const std::string& node_ip, bool control_plane)
    : node_name_(node_name), node_ip_(node_ip), control_plane_(control_plane) {}

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

    hal::SdInfo sd = hal::sd_info();
    t.sd_present = sd.mounted;
    t.sd_total_bytes = sd.mounted ? sd.total_bytes : 0;
    t.sd_free_bytes = sd.mounted ? sd.free_bytes : 0;

    return t;
}

// One resource list (capacity or allocatable). Resources the node does not
// have are left out.
static void append_resources(std::string& json, const NodeTelemetry& t, bool capacity) {
    bool first = true;
    auto add = [&](const char* key, const std::string& value) {
        if (!first) json += ",";
        first = false;
        json += "\"";
        json += key;
        json += "\":\"" + value + "\"";
    };
    if (t.total_heap_bytes > 0) {
        add("memory", std::to_string((capacity ? t.total_heap_bytes : t.free_heap_bytes) / 1024) + "Ki");
    }
    if (t.cpu_cores > 0) add("cpu", std::to_string(t.cpu_cores));
    if (t.total_gpio_count > 0) {
        std::string v = std::to_string(capacity ? t.total_gpio_count : t.allocatable_gpio_count);
        add("esp.io/gpio", v);
        add("esp32.io/gpio", v);
    }
    if (t.total_i2c_count > 0) {
        std::string v = std::to_string(capacity ? t.total_i2c_count : t.allocatable_i2c_count);
        add("esp.io/i2c", v);
        add("esp32.io/i2c", v);
    }
    if (t.total_uart_count > 0) {
        std::string v = std::to_string(capacity ? t.total_uart_count : t.allocatable_uart_count);
        add("esp.io/uart", v);
        add("esp32.io/uart", v);
    }
    if (t.total_adc_count > 0) {
        std::string v = std::to_string(capacity ? t.total_adc_count : t.allocatable_adc_count);
        add("esp.io/adc", v);
        add("esp32.io/adc", v);
    }
    if (t.total_psram_bytes > 0) {
        std::string v = capacity ? bytes_quantity(t.total_psram_bytes) : std::to_string(t.free_psram_bytes / 1024) + "Ki";
        add("esp.io/psram", v);
        add("esp32.io/psram", v);
    }
    // The card: capacity is its size, allocatable the free space, both measured
    if (t.sd_present) add("hardware.kube32.io/sd", bytes_quantity(capacity ? t.sd_total_bytes : t.sd_free_bytes));
}

static std::string ready_condition_json(const NodeTelemetry& t) {
    return "{\"type\":\"Ready\",\"status\":\"" + std::string(t.ready ? "True" : "False") + "\",\"reason\":\"" + t.ready_reason +
           "\",\"message\":\"" + t.ready_message + "\"}";
}

std::string NodeStatusReporter::build_registration_json(const NodeTelemetry& telemetry) const {
    std::string json;
    json.reserve(1024);

    json += "{\"kind\":\"Node\",\"apiVersion\":\"v1\",";
    json += "\"metadata\":{\"name\":\"" + node_name_ + "\",\"labels\":{";
    json += "\"kubernetes.io/hostname\":\"" + node_name_ + "\",";
    json += "\"kubernetes.io/arch\":\"" + std::string(api::build_info().arch) + "\",";
    for (const auto& kv : labels_) json += "\"" + kv.first + "\":\"" + kv.second + "\",";
    json += control_plane_ ? "\"node-role.kubernetes.io/control-plane\":\"\"}}," : "\"node-role.kubernetes.io/worker\":\"\"}},";
    json += "\"spec\":{\"unschedulable\":false},";
    json += "\"status\":{";
    json += "\"conditions\":[" + ready_condition_json(telemetry) + "],";
    json += "\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"" + node_ip_ + "\"}],";
    if (kubelet_port_) json += "\"daemonEndpoints\":{\"kubeletEndpoint\":{\"Port\":" + std::to_string(kubelet_port_) + "}},";
    json += "\"nodeInfo\":{\"kubeletVersion\":\"" + std::string(api::KUBE32_GIT_VERSION) + "\",\"osImage\":\"" + osal::System::get_os_image() + "\",";
    json += "\"kernelVersion\":\"" + osal::System::get_kernel_version() + "\",\"containerRuntimeVersion\":\"wasm3://" + std::string(M3_VERSION) + "\"},";
    json += "\"capacity\":{";
    append_resources(json, telemetry, true);
    json += "},\"allocatable\":{";
    append_resources(json, telemetry, false);
    json += "}}}";
    return json;
}

std::string NodeStatusReporter::build_heartbeat_patch_json(const NodeTelemetry& telemetry, bool memory_pressure) const {
    std::string json;
    json.reserve(512);

    std::string mem_status = memory_pressure ? "True" : "False";
    std::string mem_reason = memory_pressure ? "KubeletHasMemoryPressure" : "KubeletHasSufficientMemory";

    json += "{\"status\":{";
    json += "\"conditions\":[" + ready_condition_json(telemetry) + ",";
    json += "{\"type\":\"MemoryPressure\",\"status\":\"" + mem_status + "\",\"reason\":\"" + mem_reason + "\"}";
    json += "],";
    json += "\"allocatable\":{";
    append_resources(json, telemetry, false);
    json += "}}}";
    return json;
}

} // namespace kube32::kubelet
