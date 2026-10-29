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
// On the host simulator the controllers are a stub with the same lifecycle:
// installing, releasing and the pin/speed bookkeeping behave as on a board;
// transfers reach the devices registered with MockHal.

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
// Removes the drivers the pod holds (no-op when it holds none).
void bus_release(const std::string& pod_id);
// Removes every driver (tests).
void bus_release_all();

// Transfers on an installed bus; HAL_ERR_NODEV when it is not installed.
// A zero-length write is an address probe: 0 when a device acknowledges,
// HAL_ERR_NODEV when none does.
int32_t i2c_bus_write(uint32_t bus, uint8_t addr, const uint8_t* data, size_t len);
int32_t i2c_bus_read(uint32_t bus, uint8_t addr, uint8_t* buf, size_t len);
int32_t uart_port_write(uint32_t port, const uint8_t* data, size_t len);
int32_t uart_port_read(uint32_t port, uint8_t* buf, size_t max_len, uint32_t timeout_ms);

struct BusState {
    bool installed{false};
    std::string owner;
    uint8_t pin_a{0};
    uint8_t pin_b{0};
    uint32_t speed{0};
    uint32_t installs{0};   // driver installs since boot
};
BusState i2c_bus_state(uint32_t bus);
BusState uart_port_state(uint32_t port);

} // namespace kube32::hal
