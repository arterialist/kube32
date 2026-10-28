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
    GPIO     = 0,
    ADC1     = 1,
    I2C      = 2,
    UART     = 3,
    IPC      = 4,
    Display  = 5,   // On-board TFT panel (hardware.kube32.io/tft-display)
    Keyboard = 6,   // On-board matrix keyboard (hardware.kube32.io/keyboard)
    Speaker  = 7    // On-board I2S speaker (hardware.kube32.io/speaker)
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

// Capabilities bitmask definitions
constexpr uint32_t KUBE32_CAP_IPC      = (1U << 0);
constexpr uint32_t KUBE32_CAP_DISPLAY  = (1U << 1);
constexpr uint32_t KUBE32_CAP_KEYBOARD = (1U << 2);
constexpr uint32_t KUBE32_CAP_SPEAKER  = (1U << 3);

// 24-Byte Ultra-Compact Device Grant Representation
#pragma pack(push, 1)
struct DeviceGrant {
    uint64_t gpio_read_mask{0};    // Bit i: Pod can read GPIO pin i
    uint64_t gpio_write_mask{0};   // Bit i: Pod can write/configure GPIO pin i
    uint16_t adc1_channel_mask{0}; // Bit c: Pod can read ADC1 channel c (0..9)
    uint8_t  i2c_bus_mask{0};      // Bit b: Pod can access I2C bus b (0 or 1)
    uint8_t  uart_port_mask{0};    // Bit p: Pod can access UART port p (1 or 2)
    uint32_t capabilities_mask{0}; // Bitmask of higher-level capabilities (e.g. KUBE32_CAP_IPC)

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

    inline bool is_ipc_permitted() const {
        return (capabilities_mask & KUBE32_CAP_IPC) != 0;
    }

    inline bool has_capability(uint32_t cap_bit) const {
        return (capabilities_mask & cap_bit) != 0;
    }
};
#pragma pack(pop)

static_assert(sizeof(DeviceGrant) == 24, "DeviceGrant must be exactly 24 bytes");

} // namespace kube32::hal
