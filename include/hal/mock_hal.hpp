#pragma once

#include "hal/hal_types.hpp"
#include <atomic>
#include <array>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace kube32::hal {

// Virtual I2C Device Simulation (e.g. BMP280 barometric pressure & temperature sensor)
class MockI2cDevice {
public:
    virtual ~MockI2cDevice() = default;
    virtual int32_t write(const uint8_t* data, size_t len) = 0;
    virtual int32_t read(uint8_t* buf, size_t len) = 0;
    // A multiplexer adds the devices at `addr` its selected channels connect
    // to the bus
    virtual void reachable(uint8_t addr, std::vector<std::shared_ptr<MockI2cDevice>>& out) const { (void)addr; (void)out; }
};

// PCA9548A I2C multiplexer (M5Stack PaHub): the byte written last selects
// channels (bit n: channel n), a read returns it; a device attached behind
// a channel answers on the bus while that channel is selected.
class MockPca9548 : public MockI2cDevice {
public:
    int32_t write(const uint8_t* data, size_t len) override;
    int32_t read(uint8_t* buf, size_t len) override;
    void reachable(uint8_t addr, std::vector<std::shared_ptr<MockI2cDevice>>& out) const override;
    void attach(uint8_t channel, uint8_t addr, std::shared_ptr<MockI2cDevice> dev);
    void detach(uint8_t channel, uint8_t addr);
    uint8_t mask() const;
    uint32_t mask_writes() const;   // control bytes written

private:
    mutable std::mutex m_;
    uint8_t mask_{0};
    uint32_t writes_{0};
    std::map<std::pair<uint8_t, uint8_t>, std::shared_ptr<MockI2cDevice>> children_;   // (channel, address)
};

// A register file read and written through a register pointer: a write
// sets the pointer with its first byte and stores the rest from there, a
// read returns registers from the pointer on (both advance it).
class MockRegisterDevice : public MockI2cDevice {
public:
    int32_t write(const uint8_t* data, size_t len) override;
    int32_t read(uint8_t* buf, size_t len) override;
    uint8_t reg(uint8_t r) const;
    void set_reg(uint8_t r, uint8_t v);
    uint32_t writes() const;   // transfers that wrote more than the pointer

protected:
    // Called with the lock held for each register a write stores
    virtual void stored(uint8_t r, uint8_t v) { (void)r; (void)v; }
    virtual void before_read(uint8_t r) { (void)r; }
    mutable std::mutex m_;
    uint8_t ptr_{0};
    uint32_t writes_{0};
    std::array<uint8_t, 256> regs_{};
};

// Bosch BME688 (chip id 0x61 at 0xD0, variant 0x01 at 0xF0) with a
// calibration block in the datasheet's register layout. A forced-mode
// measurement (ctrl_meas 0x74 mode 01) fills the field-0 result registers
// with the raw values that the datasheet's compensation turns into the
// configured readings.
class MockBme688 : public MockRegisterDevice {
public:
    MockBme688();
    int32_t write(const uint8_t* data, size_t len) override;   // address/value pairs after the first value
    void set_readings(double temperature_c, double pressure_hpa, double humidity_pct, double gas_ohms);
    uint32_t measurements() const;

private:
    void measure();
    void reset_registers();
    double temperature_c_{22.5};
    double pressure_hpa_{1008.0};
    double humidity_pct_{41.0};
    double gas_ohms_{52000.0};
    uint32_t measurements_{0};
};

// M5Stack Unit Mini Scales (STM32 with an HX711): raw ADC at 0x00, the
// weight as a float at 0x10 and as int32 hundredths of a gram at 0x60, the
// button at 0x20, the RGB LED at 0x30, the gap (raw counts per gram) at 0x40,
// writing 1 to 0x50 tares, firmware version at 0xFE, its I2C address at 0xFF.
class MockMiniScale : public MockRegisterDevice {
public:
    explicit MockMiniScale(uint8_t address = 0x26);
    void set_weight_g(double grams);
    void set_button(bool pressed);
    uint32_t tares() const;

protected:
    void stored(uint8_t r, uint8_t v) override;
    void before_read(uint8_t r) override;

private:
    void update();
    int32_t raw_{0};
    int32_t offset_{0};
    double load_g_{0};
    uint32_t tares_{0};
};

// MFRC522-compatible reader (VersionReg 0x37) with an empty field:
// Transceive commands end in a timer timeout
class MockMfrc522 : public MockRegisterDevice {
public:
    explicit MockMfrc522(uint8_t version = 0x15);

protected:
    void stored(uint8_t r, uint8_t v) override;

private:
    uint8_t version_;
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

    // Touch panel: the points k32_touch_read reports until they change
    void set_mock_touch_points(const std::vector<TouchPoint>& points);
    int32_t touch_read(TouchPoint* out, uint32_t max);

    // I2C Operations
    // A transfer reaches the device at `addr` on the bus itself or behind a
    // selected multiplexer channel; two devices answering the same address
    // garble it (HAL_ERR_IO)
    int32_t i2c_write(uint32_t bus, uint8_t addr, const uint8_t* data, size_t len);
    int32_t i2c_read(uint32_t bus, uint8_t addr, uint8_t* buf, size_t len);
    void register_mock_i2c_device(uint32_t bus, uint8_t addr, std::shared_ptr<MockI2cDevice> dev);
    void remove_mock_i2c_device(uint32_t bus, uint8_t addr);
    std::shared_ptr<MockI2cDevice> mock_i2c_device(uint32_t bus, uint8_t addr);
    // Simulated parts on bus 0, from a list such as
    // "pahub,bme688@2,miniscale@3,rfid@4": pahub (PCA9548A, 0x70), bme688
    // (0x77), miniscale (0x26) and rfid (MFRC522, 0x28), each at its
    // default address or <part>:0x<address>, behind the PaHub's channel n
    // with @n. False with `why` for a list it cannot read.
    bool attach_parts(const std::string& spec, std::string& why);

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

    // Chip temperature sensor: a fixed 40.0 C unless a test sets another
    // reading, or a status the read returns instead (HAL_ERR_IO, say)
    int32_t chip_temp_read(int32_t& out_milli_c);
    void set_chip_temp(int32_t milli_c, int32_t status = HAL_OK);
    static constexpr int32_t kChipTempMilliC = 40000;

    // Test Inspection Helpers
    uint32_t get_gpio_write_count(uint32_t pin) const;
    PinMode get_gpio_mode(uint32_t pin) const;
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
    std::atomic<int32_t> chip_temp_milli_c_{kChipTempMilliC};
    std::atomic<int32_t> chip_temp_status_{HAL_OK};

    std::mutex i2c_mutex_;
    std::map<std::pair<uint32_t, uint8_t>, std::shared_ptr<MockI2cDevice>> i2c_devices_;
    std::mutex touch_mutex_;
    std::vector<TouchPoint> touch_points_;
    // The device a transfer to `addr` on `bus` reaches: nullptr with HAL_ERR_NODEV, or HAL_ERR_IO for two
    std::shared_ptr<MockI2cDevice> i2c_target(uint32_t bus, uint8_t addr, int32_t& err);

    std::mutex uart_mutex_;
    std::array<std::vector<uint8_t>, 4> uart_rx_buffers_;
    std::array<std::vector<uint8_t>, 4> uart_tx_buffers_;

    std::mutex rmt_mutex_;
    std::map<uint32_t, std::vector<std::vector<uint16_t>>> rmt_tx_frames_;
    std::map<uint32_t, uint32_t> rmt_tx_carrier_;
    std::atomic<bool> rmt_loopback_{false};
};

} // namespace kube32::hal
