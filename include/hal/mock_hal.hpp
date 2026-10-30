#pragma once

#include "hal/hal_types.hpp"
#include <atomic>
#include <array>
#include <vector>
#include <map>
#include <memory>
#include <mutex>

namespace kube32::hal {

// Virtual I2C Device Simulation (e.g. BMP280 barometric pressure & temperature sensor)
class MockI2cDevice {
public:
    virtual ~MockI2cDevice() = default;
    virtual int32_t write(const uint8_t* data, size_t len) = 0;
    virtual int32_t read(uint8_t* buf, size_t len) = 0;
};

// Default BMP280 Sensor Emulation: Address 0x76 / 0x77, Chip ID 0x58 at reg 0xD0
class MockBmp280Device : public MockI2cDevice {
public:
    MockBmp280Device();
    ~MockBmp280Device() override = default;

    int32_t write(const uint8_t* data, size_t len) override;
    int32_t read(uint8_t* buf, size_t len) override;
    void set_temperature_celsius(float temp);

private:
    uint8_t reg_ptr_{0};
    std::array<uint8_t, 256> registers_;
};

class MockHal {
public:
    static MockHal& instance();

    // GPIO Operations
    int32_t gpio_set_mode(uint32_t pin, PinMode mode);
    int32_t gpio_write(uint32_t pin, uint32_t value);
    int32_t gpio_read(uint32_t pin);

    // ADC Operations
    int32_t adc1_read(uint32_t channel);
    void set_mock_adc_value(uint32_t channel, uint32_t raw_value);

    // I2C Operations
    int32_t i2c_write(uint32_t bus, uint8_t addr, const uint8_t* data, size_t len);
    int32_t i2c_read(uint32_t bus, uint8_t addr, uint8_t* buf, size_t len);
    void register_mock_i2c_device(uint32_t bus, uint8_t addr, std::shared_ptr<MockI2cDevice> dev);

    // UART Operations
    int32_t uart_write(uint32_t port, const uint8_t* data, size_t len);
    int32_t uart_read(uint32_t port, uint8_t* buf, size_t max_len);
    void inject_uart_rx_bytes(uint32_t port, const uint8_t* data, size_t len);
    std::vector<uint8_t> get_uart_tx_bytes(uint32_t port);

    // RMT (IR) Operations: the frames sent on each pin (the last
    // kRmtTxFramesKept), and an IR loopback in which every frame sent also
    // reaches the node's receivers, as an LED facing them would
    int32_t rmt_transmit(uint32_t pin, const uint16_t* durations, size_t count, uint32_t carrier_hz);
    std::vector<std::vector<uint16_t>> get_rmt_tx_frames(uint32_t pin);
    uint32_t get_rmt_tx_carrier(uint32_t pin);
    void set_rmt_loopback(bool on);
    bool rmt_loopback() const;
    static constexpr size_t kRmtTxFramesKept = 16;

    // Test Inspection Helpers
    uint32_t get_gpio_write_count(uint32_t pin) const;
    void reset();

private:
    MockHal();
    ~MockHal() = default;

    MockHal(const MockHal&) = delete;
    MockHal& operator=(const MockHal&) = delete;

    std::array<std::atomic<uint8_t>, 64> pin_levels_;
    std::array<std::atomic<uint8_t>, 64> pin_modes_;
    std::array<std::atomic<uint32_t>, 64> pin_write_counts_;
    std::array<std::atomic<uint32_t>, 16> adc1_values_;

    std::mutex i2c_mutex_;
    std::map<std::pair<uint32_t, uint8_t>, std::shared_ptr<MockI2cDevice>> i2c_devices_;

    std::mutex uart_mutex_;
    std::array<std::vector<uint8_t>, 4> uart_rx_buffers_;
    std::array<std::vector<uint8_t>, 4> uart_tx_buffers_;

    std::mutex rmt_mutex_;
    std::map<uint32_t, std::vector<std::vector<uint16_t>>> rmt_tx_frames_;
    std::map<uint32_t, uint32_t> rmt_tx_carrier_;
    std::atomic<bool> rmt_loopback_{false};
};

} // namespace kube32::hal
