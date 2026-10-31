#pragma once

// I2C and UART controllers handed to pods.
//
// The capability firewall admits a pod that claims a bus (esp32.io/i2c-bus-N,
// esp32.io/uart-N) together with the pins it runs on; this module then
// installs the ESP-IDF driver on those pins (i2c_master for I2C, the UART
// driver for UART1/UART2) and removes it when the pod's claims are released,
// leaving the pins reset. Transfers from the esp32_i2c_* / esp32_uart_* host
// functions go through here once the firewall has allowed them.
//
// An I2C controller is either claimed whole by one pod or shared by the device
// grants (kube32.io/i2c-device) of several pods, each limited to one address
// by the capability firewall: one driver install serves them all, and every
// transfer runs under the bus lock. A device behind a PCA9548A multiplexer is
// reached by switching the multiplexer to its channel first (see
// i2c_device_write).
//
// On the host simulator the controllers are a stub with the same lifecycle:
// installing, releasing and the pin/speed bookkeeping behave as on a board;
// transfers reach the devices registered with MockHal.

#include "models/device.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kube32::hal {

enum class BusKind : uint8_t { I2c, Uart };

struct BusAssignment {
    BusKind kind{BusKind::I2c};
    uint8_t index{0};      // I2C controller 0/1, UART port 1/2
    uint8_t pin_a{0};      // I2C SDA, UART TX
    uint8_t pin_b{0};      // I2C SCL, UART RX
    uint32_t speed{0};     // I2C SCL Hz, UART baud
};

constexpr uint32_t kDefaultI2cHz = 100000;
constexpr uint32_t kDefaultUartBaud = 115200;

// Installs every bus of `buses` for the pod. On failure nothing it installed
// stays installed and `out_error` says why.
bool bus_install(const std::string& pod_id, const std::vector<BusAssignment>& buses, std::string& out_error);
// Removes the drivers the pod holds (no-op when it holds none), and its
// share of a shared bus.
void bus_release(const std::string& pod_id);
// Adds `user` (a pod with a device grant) to the users of shared I2C bus
// `b.index`, installing the driver on `b`'s pins for the first one. Refused
// while a pod holds the bus whole. bus_release(user) ends the share; the
// last one removes the driver.
bool i2c_share(const std::string& user, const BusAssignment& b, std::string& out_error);
// The node's own use of a free I2C bus (device discovery): installed
// without a pod and removed again by i2c_scan_end.
bool i2c_scan_begin(const BusAssignment& b, std::string& out_error);
void i2c_scan_end(uint32_t bus);
// Removes every driver (tests).
void bus_release_all();

// Transfers on an installed bus; HAL_ERR_NODEV when it is not installed.
// A zero-length write is an address probe: 0 when a device acknowledges,
// HAL_ERR_NODEV when none does.
int32_t i2c_bus_write(uint32_t bus, uint8_t addr, const uint8_t* data, size_t len);
int32_t i2c_bus_read(uint32_t bus, uint8_t addr, uint8_t* buf, size_t len);
// Transfers to the device at `loc`. With a multiplexer in the path the bus
// first writes its control byte (1 << channel) when the multiplexer was left
// on another channel; a device on the bus itself first has the multiplexers
// this bus switched deselected. Selection and transfer happen under the bus
// lock, so users of a shared bus never see each other's channel. A failed
// transfer leaves the multiplexers' state unknown: the next transfer writes
// it again.
int32_t i2c_device_write(const models::I2cLocation& loc, const uint8_t* data, size_t len);
int32_t i2c_device_read(const models::I2cLocation& loc, uint8_t* buf, size_t len);
// Deselects every multiplexer the bus has switched
void i2c_deselect(uint32_t bus);
// True when the part at `addr` on the bus itself keeps a PCA9548A control
// byte written to it (0x01, 0x80, then 0x00, read back each time), tested
// under the bus lock so that no other user's transfer switches it in
// between. A multiplexer is left with every channel off.
bool i2c_mux_test(uint32_t bus, uint8_t addr);
int32_t uart_port_write(uint32_t port, const uint8_t* data, size_t len);
int32_t uart_port_read(uint32_t port, uint8_t* buf, size_t max_len, uint32_t timeout_ms);

struct BusState {
    bool installed{false};
    std::string owner;
    uint8_t pin_a{0};
    uint8_t pin_b{0};
    uint32_t speed{0};
    uint32_t installs{0};   // driver installs since boot
    uint32_t recoveries{0}; // bus clears after failed transfers since the install
    uint32_t sharers{0};    // pods sharing the bus through device grants (0: claimed whole or unused)
    uint32_t mux_writes{0}; // multiplexer control bytes written since the install
};
BusState i2c_bus_state(uint32_t bus);
BusState uart_port_state(uint32_t port);

} // namespace kube32::hal
