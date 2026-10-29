#pragma once

#include "models/kube_models.hpp"
#include "mem/fixed_text.hpp"
#include <map>
#include <string>
#include <cstdint>

namespace kube32::kubelet {

// What the node reports in status.capacity / status.allocatable and its
// conditions. Every figure is measured on the node or read from the board
// profile; 0 means the node does not have (or cannot measure) the resource,
// and the resource is then left out of the Node object.
struct NodeTelemetry {
    size_t total_heap_bytes{0};      // capacity.memory
    size_t free_heap_bytes{0};       // allocatable.memory: free heap plus the pod pool
    size_t min_free_heap_bytes{0};
    size_t free_psram_bytes{0};
    size_t total_psram_bytes{0};
    uint32_t cpu_cores{0};
    uint32_t cpu_freq_mhz{0};
    uint64_t uptime_ms{0};
    uint32_t total_gpio_count{0};    // pins the board profile lets pods claim
    uint32_t allocatable_gpio_count{0};
    uint32_t total_i2c_count{0};     // I2C controllers the board profile offers
    uint32_t allocatable_i2c_count{0};
    uint32_t total_uart_count{0};    // UART ports the board profile offers (UART1, UART2)
    uint32_t allocatable_uart_count{0};
    uint32_t total_adc_count{0};     // ADC1 channels whose pin is claimable
    uint32_t allocatable_adc_count{0};
    // microSD card (hardware.kube32.io/sd): measured at collection, absent without a card
    bool sd_present{false};
    uint64_t sd_total_bytes{0};
    uint64_t sd_free_bytes{0};
    // Ready condition, from the kubelet's own health (Kubelet::fill_ready_condition).
    // Fixed storage: telemetry is collected every heartbeat.
    bool ready{false};
    const char* ready_reason{"KubeletNotReady"};
    char ready_message[72]{"kubelet has not completed a pod sync yet"};
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
    // The heartbeat patch or the Node object written into `out` (cleared
    // first), storage the caller owns: nothing is taken from the heap.
    // False when it did not fit.
    bool write_heartbeat_patch_json(mem::FixedText& out, const NodeTelemetry& telemetry, bool memory_pressure) const;
    bool write_registration_json(mem::FixedText& out, const NodeTelemetry& telemetry) const;

    // Extra labels for the Node object (board profile and the like).
    void set_labels(const std::map<std::string, std::string>& labels) { labels_ = labels; }
    // Port of the node's kubelet endpoint (status.daemonEndpoints), 0 for none
    void set_kubelet_port(uint16_t port) { kubelet_port_ = port; }

    const std::string& node_name() const { return node_name_; }
    const std::string& node_ip() const { return node_ip_; }
    void set_node_ip(const std::string& ip) { node_ip_ = ip; }

private:
    template <typename Out>
    void append_registration(Out& json, const NodeTelemetry& telemetry) const;

    std::string node_name_;
    std::string node_ip_;
    bool control_plane_{false};
    std::map<std::string, std::string> labels_;
    uint16_t kubelet_port_{0};
};

} // namespace kube32::kubelet
