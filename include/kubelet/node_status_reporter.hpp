#pragma once

#include "models/kube_models.hpp"
#include <string>
#include <cstdint>

namespace kube32::kubelet {

struct NodeTelemetry {
    size_t total_heap_bytes{320 * 1024};
    size_t free_heap_bytes{0};
    size_t min_free_heap_bytes{0};
    size_t free_psram_bytes{0};
    size_t total_psram_bytes{0};
    uint32_t cpu_freq_mhz{240};
    uint64_t uptime_ms{0};
    uint32_t total_gpio_count{16};
    uint32_t allocatable_gpio_count{14};
    uint32_t total_i2c_count{2};
    uint32_t allocatable_i2c_count{2};
    uint32_t total_adc_count{2};
    uint32_t allocatable_adc_count{2};
    // microSD card (hardware.kube32.io/sd): measured at collection, absent without a card
    bool sd_present{false};
    uint64_t sd_total_bytes{0};
    uint64_t sd_free_bytes{0};
};

class NodeStatusReporter {
public:
    explicit NodeStatusReporter(const std::string& node_name, const std::string& node_ip, bool control_plane = false);
    ~NodeStatusReporter() = default;

    // Collect telemetry from OSAL & HAL
    NodeTelemetry collect_telemetry() const;

    // JSON builders
    std::string build_registration_json(const NodeTelemetry& telemetry) const;
    std::string build_heartbeat_patch_json(const NodeTelemetry& telemetry, bool memory_pressure) const;

    const std::string& node_name() const { return node_name_; }
    const std::string& node_ip() const { return node_ip_; }
    void set_node_ip(const std::string& ip) { node_ip_ = ip; }

private:
    std::string node_name_;
    std::string node_ip_;
    bool control_plane_{false};
};

} // namespace kube32::kubelet
