#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace kube32::hal {

// Standard POSIX Negative Error Codes
constexpr int32_t HAL_OK           = 0;
constexpr int32_t HAL_ERR_PERM     = -1;    // -EPERM  (Operation not permitted / Firewall denied)
constexpr int32_t HAL_ERR_NOENT    = -2;    // -ENOENT (No such peripheral)
constexpr int32_t HAL_ERR_IO       = -5;    // -EIO    (Physical I/O transmission error)
constexpr int32_t HAL_ERR_NOMEM    = -12;   // -ENOMEM (Out of memory)
constexpr int32_t HAL_ERR_FAULT    = -14;   // -EFAULT (Bad memory address / Wasm linear bounds violation)
constexpr int32_t HAL_ERR_BUSY     = -16;   // -EBUSY  (Resource collision / Device already claimed)
constexpr int32_t HAL_ERR_NODEV    = -19;   // -ENODEV (No such device / adapter)
constexpr int32_t HAL_ERR_INVAL    = -22;   // -EINVAL (Invalid argument)
constexpr int32_t HAL_ERR_TIMEDOUT = -110;  // -ETIMEDOUT (I/O transfer timeout)

enum class PeripheralType : uint8_t {
    GPIO = 0,
    ADC1 = 1,
    I2C  = 2,
    UART = 3
};

enum class AccessMode : uint8_t {
    Read      = 0x01,
    Write     = 0x02,
    ReadWrite = 0x03,
    Config    = 0x04
};

enum class PinMode : uint8_t {
    Input         = 0,
    Output        = 1,
    InputPullup   = 2,
    InputPulldown = 3
};

enum class SecurityAction : uint8_t {
    LogAndDeny       = 0, // Return -EPERM, log security violation
    TrapAndTerminate = 1  // Trigger Wasm trap and terminate container
};

// 20-Byte Ultra-Compact Device Grant Representation
#pragma pack(push, 1)
struct DeviceGrant {
    uint64_t gpio_read_mask{0};    // Bit i: Pod can read GPIO pin i
    uint64_t gpio_write_mask{0};   // Bit i: Pod can write/configure GPIO pin i
    uint16_t adc1_channel_mask{0}; // Bit c: Pod can read ADC1 channel c (0..9)
    uint8_t  i2c_bus_mask{0};      // Bit b: Pod can access I2C bus b (0 or 1)
    uint8_t  uart_port_mask{0};    // Bit p: Pod can access UART port p (1 or 2)

    inline bool is_gpio_permitted(uint32_t pin, AccessMode mode) const {
        if (pin >= 64) return false;
        uint64_t bit = (1ULL << pin);
        if (mode == AccessMode::Read) return (gpio_read_mask & bit) != 0;
        if (mode == AccessMode::Write || mode == AccessMode::Config) return (gpio_write_mask & bit) != 0;
        if (mode == AccessMode::ReadWrite) return ((gpio_read_mask & bit) != 0) && ((gpio_write_mask & bit) != 0);
        return false;
    }

    inline bool is_adc1_permitted(uint32_t channel) const {
        if (channel >= 16) return false;
        return (adc1_channel_mask & (1U << channel)) != 0;
    }

    inline bool is_i2c_permitted(uint32_t bus) const {
        if (bus >= 8) return false;
        return (i2c_bus_mask & (1U << bus)) != 0;
    }

    inline bool is_uart_permitted(uint32_t port) const {
        if (port >= 8) return false;
        return (uart_port_mask & (1U << port)) != 0;
    }
};
#pragma pack(pop)

static_assert(sizeof(DeviceGrant) == 20, "DeviceGrant must be exactly 20 bytes");

} // namespace kube32::hal
