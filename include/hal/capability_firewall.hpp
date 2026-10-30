#pragma once

#include "hal/hal_types.hpp"
#include "hal/pod_buses.hpp"
#include "hal/pod_rmt.hpp"
#include "osal/mutex.hpp"

#include <string>
#include <map>
#include <array>
#include <vector>

namespace kube32::hal {

// ESP32-S3 pads: GPIO 0-21 and 26-48 (the chip has no GPIO 22-25).
constexpr uint64_t KUBE32_CHIP_GPIO_MASK = ((1ULL << 22) - 1) | (((1ULL << 49) - 1) & ~((1ULL << 26) - 1));
// Reserved on every board: pin 0 (boot strapping), 19-20 (native USB),
// 26-32 (SPI flash and quad PSRAM), 43-44 (UART0 console).
constexpr uint64_t KUBE32_RESERVED_GPIO_MASK =
    (1ULL << 0) | (0x7FULL << 26) | (1ULL << 43) | (1ULL << 44) | (1ULL << 19) | (1ULL << 20);
constexpr uint64_t KUBE32_SAFE_GPIO_MASK = KUBE32_CHIP_GPIO_MASK & ~KUBE32_RESERVED_GPIO_MASK;
// ADC1 channel N samples GPIO N+1 (channels 0-9).
constexpr uint32_t KUBE32_ADC1_CHANNELS = 10;
// The chip's two I2C controllers, and UART1/UART2 (UART0 is the console).
constexpr uint8_t KUBE32_CHIP_I2C_MASK = 0x3;
constexpr uint8_t KUBE32_CHIP_UART_MASK = 0x6;

// Pins a bus runs on: SDA/SCL for I2C, TX/RX for UART; -1 when unset.
struct BusPins {
    int8_t a{-1};
    int8_t b{-1};
};

// What the board around the chip takes for itself and which buses it offers.
// Set once at boot from the board profile (main_esp32.cpp); the defaults
// describe a bare devkit.
struct BoardProfile {
    std::string name{"generic"};
    uint64_t reserved_gpio_mask{0};                    // pins wired to on-board peripherals
    uint8_t i2c_bus_mask{KUBE32_CHIP_I2C_MASK};        // I2C controllers pods may claim
    uint8_t uart_port_mask{KUBE32_CHIP_UART_MASK};     // UART ports pods may claim
    // Pins a claimed bus uses when the pod names none: I2C0 on GPIO 8/9 and
    // UART1 on GPIO 17/18 (the ESP32-S3 defaults). A bus without defaults
    // needs pins in the pod's annotations.
    BusPins i2c_pins[2]{{8, 9}, {}};
    BusPins uart_pins[3]{{}, {17, 18}, {}};
    // RMT (IR) channels run on claimable pins and, for TX, also on the pins
    // of rmt_tx_pin_mask: an IR LED on a pad the chip mask reserves (a
    // console UART pin the board leaves unused). The default pins serve a
    // claim without a pin in its annotation; -1 means the claim must name one.
    uint64_t rmt_tx_pin_mask{0};
    int8_t rmt_tx_default_pin{-1};
    int8_t rmt_rx_default_pin{-1};
};

struct PodSecurityRecord {
    std::string pod_id;
    DeviceGrant grant;
    SecurityAction action{SecurityAction::LogAndDeny};
    uint32_t violation_count{0};
    // Volume claim: a directory under <node storage root>/kube32/volumes/
    // (hal/node_storage.hpp), from a persistentVolumeClaim or kube32.io/sd volume
    std::string sd_claim;
    bool card_required{false};   // kube32.io/sd volume or hardware.kube32.io/sd: only a microSD card will do
    std::vector<BusAssignment> buses;   // I2C/UART controllers with their pins (drivers installed)
    std::vector<RmtAssignment> rmt;     // RMT channels with their pins and settings (channels created)
};

class CapabilityFirewall {
public:
    static CapabilityFirewall& instance();

    // Pod Deployment Lifecycle
    // Compiles resources.requests from pod JSON manifest into a DeviceGrant.
    // Detects hardware collisions and updates the global hardware allocation registry.
    //
    // I2C and UART claims (esp32.io/i2c-bus-N, esp32.io/i2c, esp32.io/uart-N,
    // esp32.io/uart) take their pins from the pod's annotations
    //   kube32.io/i2c-bus-N: "sda=<pin>,scl=<pin>[,hz=<SCL Hz>]"
    //   kube32.io/uart-N:    "tx=<pin>,rx=<pin>[,baud=<rate>]"
    // or from the board profile's defaults. The pins are claimed like GPIO
    // claims (refused when the chip or the board reserves them or another pod
    // holds them) and the bus driver is installed on them; unregister_pod
    // removes it.
    //
    // RMT claims (esp32.io/rmt-tx, esp32.io/rmt-rx: "1", one channel of each
    // per pod) take their pin and settings from
    //   kube32.io/rmt-tx: "pin=<n>[,carrier=<Hz>][,duty=<percent>]"
    //   kube32.io/rmt-rx: "pin=<n>[,active=low|high][,idle_us=<n>]"
    // or the board profile's default pin, and are claimed and installed the
    // same way.
    bool register_pod(const std::string& pod_id, const std::string& pod_manifest_json, std::string& out_error);

    // Direct registration with pre-compiled grant
    bool grant_capabilities(const std::string& pod_id, const DeviceGrant& grant, std::string& out_error);

    // Release all claimed peripherals upon pod termination
    void unregister_pod(const std::string& pod_id);

    // Gatekeeper Enforcement Check (O(1) fast path)
    bool check_permission(const std::string& pod_id, PeripheralType type, uint32_t pin_or_port, AccessMode mode);

    // Context Pointer Lookup for Wasm3 Userdata Binding
    const PodSecurityRecord* get_pod_security_record(const std::string& pod_id) const;

    // Volume claim of a pod (persistentVolumeClaim, kube32.io/sd); empty otherwise.
    std::string sd_claim(const std::string& pod_id) const;
    // True when the pod's volume must be on a microSD card.
    bool volume_needs_card(const std::string& pod_id) const;
    // A claim is a directory name: 1..32 characters of [a-z0-9-], no leading '-'.
    static bool valid_sd_claim(const std::string& claim);

    // Global Telemetry & Status
    uint64_t get_allocated_gpio_mask() const;
    uint16_t get_allocated_adc1_mask() const;
    uint8_t  get_allocated_i2c_mask() const;
    uint8_t  get_allocated_uart_mask() const;

    // Board profile; pins and buses outside it cannot be claimed.
    void set_board_profile(const BoardProfile& profile);
    BoardProfile board_profile() const;
    // What pods can claim on this board, whether claimed or not: the node's
    // capacity for esp32.io/gpio, esp32.io/adc and esp32.io/i2c.
    uint64_t claimable_gpio_mask() const;
    uint16_t claimable_adc1_mask() const;
    uint8_t  claimable_i2c_mask() const;
    uint8_t  claimable_uart_mask() const;
    // RMT channels pods can claim on this board (esp32.io/rmt-tx and
    // esp32.io/rmt-rx capacity): one per pin a channel can use, at most the
    // chip's four of each direction
    uint32_t claimable_rmt_tx_count() const;
    uint32_t claimable_rmt_rx_count() const;
    uint32_t get_allocated_rmt_tx_count() const;
    uint32_t get_allocated_rmt_rx_count() const;
    // Buses (with pins) the pod was admitted with
    std::vector<BusAssignment> pod_buses(const std::string& pod_id) const;
    // RMT channels the pod was admitted with
    std::vector<RmtAssignment> pod_rmt(const std::string& pod_id) const;

    // Lends I2C controller `bus` on the board's default pins to the node
    // itself (device discovery, hal/i2c_discovery.hpp) when the board offers
    // the bus and no pod holds the controller or the pins. Pod admission
    // waits until the bus is returned; a lend is refused while an admission
    // is in progress.
    bool lend_i2c_bus(uint8_t bus, BusPins& out_pins);
    void return_i2c_bus(uint8_t bus);

    // Reset all allocations and the board profile (used in test teardown)
    void reset();

    // Security violation audit hook
    void log_violation(const std::string& pod_id, PeripheralType type, uint32_t target, AccessMode mode);

private:
    CapabilityFirewall();
    ~CapabilityFirewall() = default;

    CapabilityFirewall(const CapabilityFirewall&) = delete;
    CapabilityFirewall& operator=(const CapabilityFirewall&) = delete;

    bool allocate_generic_gpios(uint32_t count, DeviceGrant& grant, const std::string& pod_id, std::vector<uint32_t>& out_claimed, std::string& out_error);
    bool allocate_generic_i2c(uint32_t count, DeviceGrant& grant, const std::string& pod_id, std::vector<uint32_t>& out_claimed, std::string& out_error);
    bool allocate_generic_adc(uint32_t count, DeviceGrant& grant, const std::string& pod_id, std::vector<uint32_t>& out_claimed, std::string& out_error);
    bool allocate_generic_uart(uint32_t count, DeviceGrant& grant, const std::string& pod_id, std::vector<uint32_t>& out_claimed, std::string& out_error);
    // Claims `pin` for the pod as part of `what` ("I2C0 sda", "RMT TX"):
    // refused when the chip or the board reserves it (unless `also_allowed`
    // offers it) or when another pod, or this one already, holds it.
    bool claim_pin(const std::string& pod_id, uint32_t pin, const std::string& what, uint64_t also_allowed, std::vector<uint32_t>& out_claimed_pins,
                   std::string& out_error);
    // Pins of every bus in `grant`: from `annotations` (kube32.io/i2c-bus-N,
    // kube32.io/uart-N) or the board defaults, claimed for the pod.
    bool claim_bus_pins(const std::string& pod_id, const DeviceGrant& grant, const std::map<std::string, std::string>& annotations,
                        std::vector<BusAssignment>& out_buses, std::vector<uint32_t>& out_claimed_pins, std::string& out_error);
    // The RMT channels of `grant` with their pins and settings: from
    // `annotations` (kube32.io/rmt-tx, kube32.io/rmt-rx) or the board
    // defaults, pins claimed for the pod.
    bool claim_rmt_pins(const std::string& pod_id, const DeviceGrant& grant, const std::map<std::string, std::string>& annotations,
                        std::vector<RmtAssignment>& out_rmt, std::vector<uint32_t>& out_claimed_pins, std::string& out_error);
    bool register_pod_locked(const std::string& pod_id, const std::string& pod_manifest_json, std::vector<BusAssignment>& out_buses,
                             std::vector<RmtAssignment>& out_rmt, std::string& out_error);
    bool grant_capabilities_locked(const std::string& pod_id, const DeviceGrant& grant, std::vector<BusAssignment>& out_buses,
                                   std::vector<RmtAssignment>& out_rmt, std::string& out_error);
    // Installs the bus drivers and RMT channels of a registered pod;
    // unregisters it on failure.
    bool install_drivers(const std::string& pod_id, const std::vector<BusAssignment>& buses, const std::vector<RmtAssignment>& rmt,
                         std::string& out_error);
    uint64_t claimable_gpio_mask_locked() const;
    uint16_t claimable_adc1_mask_locked() const;
    // Marks a pod admission in progress and waits for buses lent to the node
    void begin_admission();
    void end_admission();
    uint32_t claimable_rmt_count_locked(RmtDir dir) const;
    uint32_t allocated_rmt_count_locked(RmtDir dir) const;

    mutable osal::Mutex mutex_;
    std::map<std::string, PodSecurityRecord> pod_records_;
    uint64_t allocated_gpio_mask_{0};
    uint16_t allocated_adc1_mask_{0};
    uint8_t  allocated_i2c_mask_{0};
    uint8_t  allocated_uart_mask_{0};
    uint8_t  lent_i2c_mask_{0};    // I2C controllers lent to the node
    uint32_t admissions_{0};       // pod admissions in progress
    BoardProfile profile_;

    // Global Hardware Ownership Registry (for collision detection)
    std::array<std::string, 64> gpio_owners_;
    std::array<std::string, 16> adc1_owners_;
    std::array<std::string, 8>  i2c_owners_;
    std::array<std::string, 8>  uart_owners_;
};

} // namespace kube32::hal
