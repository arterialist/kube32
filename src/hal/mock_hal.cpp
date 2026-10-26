#include "hal/mock_hal.hpp"
#include <cstring>
#include <algorithm>

namespace kube32::hal {

// =============================================================================
// Mock BMP280 Sensor Emulation
// =============================================================================

MockBmp280Device::MockBmp280Device() {
    registers_.fill(0);
    // BMP280 chip ID register 0xD0 is always 0x58
    registers_[0xD0] = 0x58;
    // Status register 0xF3: bit 3 (measuring) = 0, bit 0 (im_update) = 0
    registers_[0xF3] = 0x00;
    // Control measurement 0xF4
    registers_[0xF4] = 0x57; // Normal mode, osrs_t x2, osrs_p x16
    // Default calibration data in 0x88..0xA1
    registers_[0x88] = 0x70; // dig_T1 LSB
    registers_[0x89] = 0x6B; // dig_T1 MSB (27504)
    registers_[0x8A] = 0x60; // dig_T2 LSB
    registers_[0x8B] = 0x67; // dig_T2 MSB (26464)
    registers_[0x8C] = 0x18; // dig_T3 LSB
    registers_[0x8D] = 0xFC; // dig_T3 MSB (-1000)

    set_temperature_celsius(25.0f);
}

int32_t MockBmp280Device::write(const uint8_t* data, size_t len) {
    if (!data || len == 0) return HAL_ERR_INVAL;

    reg_ptr_ = data[0];
    for (size_t i = 1; i < len; ++i) {
        size_t target_reg = (reg_ptr_ + (i - 1)) & 0xFF;
        // Chip ID is read-only
        if (target_reg != 0xD0) {
            registers_[target_reg] = data[i];
        }
    }
    return static_cast<int32_t>(len);
}

int32_t MockBmp280Device::read(uint8_t* buf, size_t len) {
    if (!buf || len == 0) return HAL_ERR_INVAL;

    for (size_t i = 0; i < len; ++i) {
        size_t target_reg = (reg_ptr_ + i) & 0xFF;
        buf[i] = registers_[target_reg];
    }
    reg_ptr_ = (reg_ptr_ + len) & 0xFF;
    return static_cast<int32_t>(len);
}

void MockBmp280Device::set_temperature_celsius(float temp) {
    // 20-bit raw temperature ADC representation in registers 0xFA (msb), 0xFB (lsb), 0xFC (xlsb)
    uint32_t raw_adc = static_cast<uint32_t>((temp + 20.0f) * 10000.0f);
    registers_[0xFA] = static_cast<uint8_t>((raw_adc >> 12) & 0xFF);
    registers_[0xFB] = static_cast<uint8_t>((raw_adc >> 4) & 0xFF);
    registers_[0xFC] = static_cast<uint8_t>((raw_adc & 0x0F) << 4);
}

// =============================================================================
// Mock HAL Implementation
// =============================================================================

MockHal& MockHal::instance() {
    static MockHal s_instance;
    return s_instance;
}

MockHal::MockHal() {
    reset();
}

void MockHal::reset() {
    for (size_t i = 0; i < 64; ++i) {
        pin_levels_[i].store(0);
        pin_modes_[i].store(static_cast<uint8_t>(PinMode::Input));
        pin_write_counts_[i].store(0);
    }
    for (size_t i = 0; i < 16; ++i) {
        adc1_values_[i].store(1850); // ~1.5V baseline ADC
    }

    {
        std::lock_guard<std::mutex> lock(i2c_mutex_);
        i2c_devices_.clear();
        // Register default BMP280 on bus 0, addr 0x76
        i2c_devices_[{0, 0x76}] = std::make_shared<MockBmp280Device>();
    }

    {
        std::lock_guard<std::mutex> lock(uart_mutex_);
        for (auto& rx : uart_rx_buffers_) rx.clear();
        for (auto& tx : uart_tx_buffers_) tx.clear();
    }
}

int32_t MockHal::gpio_set_mode(uint32_t pin, PinMode mode) {
    if (pin >= 64) return HAL_ERR_INVAL;
    pin_modes_[pin].store(static_cast<uint8_t>(mode));
    return HAL_OK;
}

int32_t MockHal::gpio_write(uint32_t pin, uint32_t value) {
    if (pin >= 64) return HAL_ERR_INVAL;
    pin_levels_[pin].store(value ? 1 : 0);
    pin_write_counts_[pin].fetch_add(1);
    return HAL_OK;
}

int32_t MockHal::gpio_read(uint32_t pin) {
    if (pin >= 64) return HAL_ERR_INVAL;
    return pin_levels_[pin].load();
}

int32_t MockHal::adc1_read(uint32_t channel) {
    if (channel >= 16) return HAL_ERR_INVAL;
    return static_cast<int32_t>(adc1_values_[channel].load());
}

void MockHal::set_mock_adc_value(uint32_t channel, uint32_t raw_value) {
    if (channel < 16) {
        adc1_values_[channel].store(raw_value);
    }
}

int32_t MockHal::i2c_write(uint32_t bus, uint8_t addr, const uint8_t* data, size_t len) {
    if (bus >= 2) return HAL_ERR_INVAL;
    std::shared_ptr<MockI2cDevice> dev;
    {
        std::lock_guard<std::mutex> lock(i2c_mutex_);
        auto it = i2c_devices_.find({bus, addr});
        if (it != i2c_devices_.end()) {
            dev = it->second;
        }
    }
    if (!dev) return HAL_ERR_NODEV;
    return dev->write(data, len);
}

int32_t MockHal::i2c_read(uint32_t bus, uint8_t addr, uint8_t* buf, size_t len) {
    if (bus >= 2) return HAL_ERR_INVAL;
    std::shared_ptr<MockI2cDevice> dev;
    {
        std::lock_guard<std::mutex> lock(i2c_mutex_);
        auto it = i2c_devices_.find({bus, addr});
        if (it != i2c_devices_.end()) {
            dev = it->second;
        }
    }
    if (!dev) return HAL_ERR_NODEV;
    return dev->read(buf, len);
}

void MockHal::register_mock_i2c_device(uint32_t bus, uint8_t addr, std::shared_ptr<MockI2cDevice> dev) {
    std::lock_guard<std::mutex> lock(i2c_mutex_);
    i2c_devices_[{bus, addr}] = dev;
}

int32_t MockHal::uart_write(uint32_t port, const uint8_t* data, size_t len) {
    if (port >= 4 || !data) return HAL_ERR_INVAL;
    std::lock_guard<std::mutex> lock(uart_mutex_);
    uart_tx_buffers_[port].insert(uart_tx_buffers_[port].end(), data, data + len);
    return static_cast<int32_t>(len);
}

int32_t MockHal::uart_read(uint32_t port, uint8_t* buf, size_t max_len) {
    if (port >= 4 || !buf) return HAL_ERR_INVAL;
    std::lock_guard<std::mutex> lock(uart_mutex_);
    auto& rx = uart_rx_buffers_[port];
    size_t count = std::min(max_len, rx.size());
    if (count > 0) {
        std::memcpy(buf, rx.data(), count);
        rx.erase(rx.begin(), rx.begin() + count);
    }
    return static_cast<int32_t>(count);
}

void MockHal::inject_uart_rx_bytes(uint32_t port, const uint8_t* data, size_t len) {
    if (port < 4 && data && len > 0) {
        std::lock_guard<std::mutex> lock(uart_mutex_);
        uart_rx_buffers_[port].insert(uart_rx_buffers_[port].end(), data, data + len);
    }
}

std::vector<uint8_t> MockHal::get_uart_tx_bytes(uint32_t port) {
    if (port >= 4) return {};
    std::lock_guard<std::mutex> lock(uart_mutex_);
    return uart_tx_buffers_[port];
}

uint32_t MockHal::get_gpio_write_count(uint32_t pin) const {
    if (pin >= 64) return 0;
    return pin_write_counts_[pin].load();
}

} // namespace kube32::hal
