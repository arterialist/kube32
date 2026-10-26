#pragma once

#include "hal/hal_types.hpp"
#include "osal/mutex.hpp"

#include <string>
#include <map>
#include <array>
#include <vector>

namespace kube32::hal {

// ESP32-S3 Hardware Safety Masks
// Reserved: Pins 26-32 (Octal Flash/PSRAM), Pin 43-44 (UART0 console), Pin 19-20 (USB)
constexpr uint64_t KUBE32_RESERVED_GPIO_MASK = 
    (0x7FULL << 26) | (1ULL << 43) | (1ULL << 44) | (1ULL << 19) | (1ULL << 20);
constexpr uint64_t KUBE32_SAFE_GPIO_MASK = (~KUBE32_RESERVED_GPIO_MASK) & 0x0001FFFFFFFFFFFFULL;

struct PodSecurityRecord {
    std::string pod_id;
    DeviceGrant grant;
    SecurityAction action{SecurityAction::LogAndDeny};
    uint32_t violation_count{0};
};

class CapabilityFirewall {
public:
    static CapabilityFirewall& instance();

    // Pod Deployment Lifecycle
    // Compiles resources.requests from pod JSON manifest into a DeviceGrant.
    // Detects hardware collisions and updates the global hardware allocation registry.
    bool register_pod(const std::string& pod_id, const std::string& pod_manifest_json, std::string& out_error);

    // Direct registration with pre-compiled grant
    bool grant_capabilities(const std::string& pod_id, const DeviceGrant& grant, std::string& out_error);

    // Release all claimed peripherals upon pod termination
    void unregister_pod(const std::string& pod_id);

    // Gatekeeper Enforcement Check (O(1) fast path)
    bool check_permission(const std::string& pod_id, PeripheralType type, uint32_t pin_or_port, AccessMode mode);

    // Context Pointer Lookup for Wasm3 Userdata Binding
    const PodSecurityRecord* get_pod_security_record(const std::string& pod_id) const;

    // Global Telemetry & Status
    uint64_t get_allocated_gpio_mask() const;
    uint16_t get_allocated_adc1_mask() const;
    uint8_t  get_allocated_i2c_mask() const;
    uint8_t  get_allocated_uart_mask() const;

    // Reset all allocations (used in test teardown)
    void reset();

    // Security violation audit hook
    void log_violation(const std::string& pod_id, PeripheralType type, uint32_t target, AccessMode mode);

private:
    CapabilityFirewall();
    ~CapabilityFirewall() = default;

    CapabilityFirewall(const CapabilityFirewall&) = delete;
    CapabilityFirewall& operator=(const CapabilityFirewall&) = delete;

    bool allocate_generic_gpios(uint32_t count, DeviceGrant& grant, const std::string& pod_id, std::string& out_error);
    bool allocate_generic_i2c(uint32_t count, DeviceGrant& grant, const std::string& pod_id, std::string& out_error);
    bool allocate_generic_adc(uint32_t count, DeviceGrant& grant, const std::string& pod_id, std::string& out_error);

    mutable osal::Mutex mutex_;
    std::map<std::string, PodSecurityRecord> pod_records_;

    // Global Hardware Ownership Registry (for collision detection)
    std::array<std::string, 64> gpio_owners_;
    std::array<std::string, 16> adc1_owners_;
    std::array<std::string, 8>  i2c_owners_;
    std::array<std::string, 8>  uart_owners_;
};

} // namespace kube32::hal
