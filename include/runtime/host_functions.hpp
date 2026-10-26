#pragma once

#include "hal/hal_types.hpp"
#include <cstdint>
#include <cstddef>
#include <string>

// Forward declarations for Wasm3 internal types
typedef struct M3Runtime*  IM3Runtime;
typedef struct M3Module*   IM3Module;

namespace kube32::runtime {

// Registers all standard Kube32 ESP32 host function imports into a Wasm3 module
// Module import namespace: "env"
bool link_host_functions(IM3Module module, const std::string& pod_id);

// ABI-Stable C Host Function Declarations
extern "C" {
    int32_t esp32_gpio_set_mode(uint32_t pin, uint32_t mode);
    int32_t esp32_gpio_write(uint32_t pin, uint32_t value);
    int32_t esp32_gpio_read(uint32_t pin);
    int32_t esp32_adc_read(uint32_t channel);
    int32_t esp32_i2c_write(uint32_t bus, uint8_t addr, const uint8_t* data, uint32_t len);
    int32_t esp32_i2c_read(uint32_t bus, uint8_t addr, uint8_t* buf, uint32_t len);
    int32_t esp32_uart_write(uint32_t port, const uint8_t* data, uint32_t len);
    int32_t esp32_uart_read(uint32_t port, uint8_t* buf, uint32_t max_len);
    int64_t esp32_get_time_us();
    void    esp32_delay_ms(uint32_t ms);
}

} // namespace kube32::runtime
