#include "kubelet/node_status_reporter.hpp"
#include "osal/system.hpp"
#include "hal/capability_firewall.hpp"

namespace kube32::kubelet {

NodeStatusReporter::NodeStatusReporter(const std::string& node_name, const std::string& node_ip)
    : node_name_(node_name), node_ip_(node_ip) {}

NodeTelemetry NodeStatusReporter::collect_telemetry() const {
    NodeTelemetry t;
    t.free_heap_bytes = osal::System::get_free_heap_bytes();
    t.min_free_heap_bytes = osal::System::get_min_free_heap_bytes();
    t.free_psram_bytes = osal::System::get_free_psram_bytes();
    t.total_psram_bytes = osal::System::get_total_psram_bytes();
    t.cpu_freq_mhz = osal::System::get_cpu_freq_mhz();
    t.uptime_ms = osal::System::get_uptime_ms();

    uint64_t allocated_gpio = hal::CapabilityFirewall::instance().get_allocated_gpio_mask();
    uint32_t free_gpio = 0;
    for (uint32_t pin = 1; pin < 64; ++pin) {
        if ((hal::KUBE32_SAFE_GPIO_MASK & (1ULL << pin)) != 0) {
            if ((allocated_gpio & (1ULL << pin)) == 0) {
                free_gpio++;
            }
        }
    }
    t.allocatable_gpio_count = free_gpio;
    t.allocatable_i2c_count = 2;
    t.allocatable_adc_count = 2;
    return t;
}

std::string NodeStatusReporter::build_registration_json(const NodeTelemetry& telemetry) const {
    (void)telemetry;
    std::string json;
    json.reserve(1024);

    json += "{\"kind\":\"Node\",\"apiVersion\":\"v1\",";
    json += "\"metadata\":{\"name\":\"" + node_name_ + "\",\"labels\":{";
    json += "\"kubernetes.io/hostname\":\"" + node_name_ + "\",";
    json += "\"kubernetes.io/arch\":\"xtensa\",";
    json += "\"node-role.kubernetes.io/worker\":\"\"}},";
    json += "\"spec\":{\"unschedulable\":false},";
    json += "\"status\":{";
    json += "\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\",\"reason\":\"KubeletReady\",\"message\":\"kubelet is operational and reporting healthy lease\"}],";
    json += "\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"" + node_ip_ + "\"}],";
    json += "\"nodeInfo\":{\"kubeletVersion\":\"v1.33.0-kube32\",\"osImage\":\"ESP-IDF v5.2\",";
    json += "\"kernelVersion\":\"FreeRTOS-v10.4\",\"containerRuntimeVersion\":\"kube32-runtime://0.1.0\"},";
    json += "\"capacity\":{\"memory\":\"320Ki\",\"cpu\":\"2\",\"esp.io/gpio\":\"16\",\"esp32.io/gpio\":\"16\",";
    json += "\"esp.io/i2c\":\"2\",\"esp32.io/i2c\":\"2\",\"esp.io/psram\":\"4Mi\",\"esp32.io/psram\":\"4Mi\"},";
    json += "\"allocatable\":{\"memory\":\"240Ki\",\"cpu\":\"2\",\"esp.io/gpio\":\"14\",\"esp32.io/gpio\":\"14\",";
    json += "\"esp.io/i2c\":\"2\",\"esp32.io/i2c\":\"2\",\"esp.io/psram\":\"3840Ki\",\"esp32.io/psram\":\"3840Ki\"}";
    json += "}}";

    return json;
}

std::string NodeStatusReporter::build_heartbeat_patch_json(const NodeTelemetry& telemetry, bool memory_pressure) const {
    (void)telemetry;
    std::string json;
    json.reserve(512);

    std::string mem_status = memory_pressure ? "True" : "False";
    std::string mem_reason = memory_pressure ? "KubeletHasMemoryPressure" : "KubeletHasSufficientMemory";

    json += "{\"status\":{";
    json += "\"conditions\":[";
    json += "{\"type\":\"Ready\",\"status\":\"True\",\"reason\":\"KubeletReady\",\"message\":\"kubelet is operational and reporting healthy lease\"},";
    json += "{\"type\":\"MemoryPressure\",\"status\":\"" + mem_status + "\",\"reason\":\"" + mem_reason + "\"}";
    json += "],";
    json += "\"allocatable\":{";
    json += "\"memory\":\"240Ki\",\"esp32.io/psram\":\"3840Ki\"";
    json += "}}}";

    return json;
}

} // namespace kube32::kubelet
