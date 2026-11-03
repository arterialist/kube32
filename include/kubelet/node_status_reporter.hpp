#pragma once

#include "models/kube_models.hpp"
#include "mem/fixed_text.hpp"
#include <atomic>
#include <map>
#include <string>
#include <cstdint>

namespace kube32::kubelet {

// Node annotations of a node with a mounted microSD card: its size, and its
// free space in whole MiB rounded down
constexpr const char* kAnnotationSdCapacity = "kube32.io/sd-capacity";
constexpr const char* kAnnotationSdFree = "kube32.io/sd-free";

// What the node reports in status.capacity / status.allocatable and its
// conditions. Every figure is measured on the node or read from the board
// profile; 0 means the node does not have (or cannot measure) the resource,
// and the resource is then left out of the Node object.
struct NodeTelemetry {
    size_t total_heap_bytes{0};      // internal heap
    size_t free_heap_bytes{0};       // free internal heap: the MemoryPressure condition, annotation kube32.io/heap-free
    size_t min_free_heap_bytes{0};
    size_t memory_capacity_bytes{0};     // capacity.memory: the board's RAM, PSRAM included
    size_t memory_allocatable_bytes{0};  // allocatable.memory: what pods can be given with no pod running (kube32_pool_node_room)
    size_t memory_free_bytes{0};         // what pods can be given now (kube32_pool_pod_room): annotation kube32.io/memory-free
    size_t system_reserved_bytes{0};     // kept back for the system, never offered to pods (kube32_pool_system_reserved): annotation kube32.io/system-reserved
    size_t psram_allocatable_bytes{0};  // allocatable esp32.io/psram (kube32_pool_node_room)
    size_t free_psram_bytes{0};      // free PSRAM: annotation kube32.io/psram-free
    size_t total_psram_bytes{0};     // capacity esp32.io/psram
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
    uint32_t total_rmt_tx_count{0};  // RMT (IR) TX channels with a pin to run on
    uint32_t allocatable_rmt_tx_count{0};
    uint32_t total_rmt_rx_count{0};  // RMT (IR) RX channels with a pin to run on
    uint32_t allocatable_rmt_rx_count{0};
    // pods: the most pods the kubelet admits at once (KubeletConfig::max_pods)
    uint32_t pods_capacity{0};
    // Device grants (hardware.kube32.io/*) the node can give: a mounted
    // microSD card, the board's on-board devices, the chip's temperature
    // sensor; channel IPC on every node. Each pod holds a grant at most once
    // and any number of pods share a device, so each grant's count is
    // pods_capacity.
    bool sd_present{false};
    bool has_display{false};
    bool has_keyboard{false};
    bool has_speaker{false};
    bool has_microphone{false};
    bool has_chip_temp{false};
    // Ready condition, from the kubelet's own health (Kubelet::fill_ready_condition).
    // Fixed storage: telemetry is collected every heartbeat.
    bool ready{false};
    const char* ready_reason{"KubeletNotReady"};
    char ready_message[72]{"kubelet has not completed a pod sync yet"};
    // FirmwareUpdate condition (kubelet/firmware_agent.hpp), on nodes that
    // update their firmware through the cluster
    bool firmware_reported{false};
    bool firmware_updating{false};
    char firmware_reason[16]{""};
    char firmware_message[128]{""};
    // Condition times in Unix milliseconds, filled by the kubelet
    // (Kubelet::time_conditions); 0 while the wall clock is unset, and the
    // field is then left out. lastTransitionTime is when this kubelet saw
    // the status change; the API server keeps the stored time while the
    // status on the Node stays the same (models::settle_node_condition_times).
    uint64_t heartbeat_unix_ms{0};
    uint64_t ready_transition_unix_ms{0};
    uint64_t memory_pressure_transition_unix_ms{0};
    uint64_t firmware_transition_unix_ms{0};
};

class NodeStatusReporter {
public:
    explicit NodeStatusReporter(const std::string& node_name, const std::string& node_ip, bool control_plane = false);
    ~NodeStatusReporter() = default;

    // Collect telemetry from OSAL & HAL
    NodeTelemetry collect_telemetry() const;

    // JSON builders. The registration is the Node object to create, or with
    // `as_patch` the patch over a Node that outlived this kubelet: each
    // resource the node no longer has is set to null in capacity and
    // allocatable, so it leaves the Node. A heartbeat carries allocatable,
    // and capacity too when the resources differ from node_resources().
    std::string build_registration_json(const NodeTelemetry& telemetry, bool as_patch = false) const;
    std::string build_heartbeat_patch_json(const NodeTelemetry& telemetry, bool memory_pressure) const;
    // The heartbeat patch or the Node object written into `out` (cleared
    // first), storage the caller owns: nothing is taken from the heap.
    // False when it did not fit.
    bool write_heartbeat_patch_json(mem::FixedText& out, const NodeTelemetry& telemetry, bool memory_pressure) const;
    bool write_registration_json(mem::FixedText& out, const NodeTelemetry& telemetry, bool as_patch = false) const;

    // Resources that come and go (device grants, peripheral counts), one bit
    // each: the ones `telemetry` has
    static uint32_t resource_bits(const NodeTelemetry& telemetry);
    // The ones the Node object has, as this kubelet last wrote it
    void set_node_resources(uint32_t bits) { node_resources_ = bits; }
    uint32_t node_resources() const { return node_resources_; }
    // capacity and allocatable pods
    void set_max_pods(uint32_t pods) { max_pods_ = pods; }

    // Extra labels for the Node object (board profile and the like).
    void set_labels(const std::map<std::string, std::string>& labels) { labels_ = labels; }
    // Port of the node's kubelet endpoint (status.daemonEndpoints), 0 for none
    void set_kubelet_port(uint16_t port) { kubelet_port_ = port; }
    // The firmware version in nodeInfo.osImage (this build's by default)
    void set_firmware_version(const std::string& version) { firmware_version_ = version; }
    // Annotations of the Node object (the running firmware image)
    void set_annotations(const std::map<std::string, std::string>& annotations) { annotations_ = annotations; }

    const std::string& node_name() const { return node_name_; }
    const std::string& node_ip() const { return node_ip_; }
    void set_node_ip(const std::string& ip) { node_ip_ = ip; }

private:
    template <typename Out>
    void append_registration(Out& json, const NodeTelemetry& telemetry, bool as_patch) const;
    template <typename Out>
    void append_heartbeat_patch(Out& json, const NodeTelemetry& telemetry, bool memory_pressure) const;

    std::string node_name_;
    std::string node_ip_;
    bool control_plane_{false};
    std::map<std::string, std::string> labels_;
    std::map<std::string, std::string> annotations_;
    std::string firmware_version_;
    uint16_t kubelet_port_{0};
    uint32_t max_pods_{0};
    std::atomic<uint32_t> node_resources_{0};
};

} // namespace kube32::kubelet
