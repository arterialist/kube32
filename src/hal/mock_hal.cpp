#include "hal/mock_hal.hpp"
#include "models/device.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

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
// PCA9548A multiplexer
// =============================================================================

int32_t MockPca9548::write(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(m_);
    if (len == 0) return 0;   // an address probe
    mask_ = data[len - 1];
    writes_++;
    return static_cast<int32_t>(len);
}

int32_t MockPca9548::read(uint8_t* buf, size_t len) {
    std::lock_guard<std::mutex> lock(m_);
    for (size_t i = 0; i < len; ++i) buf[i] = mask_;
    return static_cast<int32_t>(len);
}

void MockPca9548::reachable(uint8_t addr, std::vector<std::shared_ptr<MockI2cDevice>>& out) const {
    std::lock_guard<std::mutex> lock(m_);
    for (const auto& kv : children_) {
        if (kv.first.second == addr && (mask_ & (1u << kv.first.first))) out.push_back(kv.second);
    }
}

void MockPca9548::attach(uint8_t channel, uint8_t addr, std::shared_ptr<MockI2cDevice> dev) {
    std::lock_guard<std::mutex> lock(m_);
    children_[{channel, addr}] = std::move(dev);
}

void MockPca9548::detach(uint8_t channel, uint8_t addr) {
    std::lock_guard<std::mutex> lock(m_);
    children_.erase({channel, addr});
}

uint8_t MockPca9548::mask() const {
    std::lock_guard<std::mutex> lock(m_);
    return mask_;
}

uint32_t MockPca9548::mask_writes() const {
    std::lock_guard<std::mutex> lock(m_);
    return writes_;
}

// =============================================================================
// Register-pointer devices
// =============================================================================

int32_t MockRegisterDevice::write(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(m_);
    if (len == 0) return 0;
    ptr_ = data[0];
    if (len > 1) writes_++;
    for (size_t i = 1; i < len; ++i) {
        regs_[ptr_] = data[i];
        stored(ptr_, data[i]);
        ptr_ = static_cast<uint8_t>(ptr_ + 1);
    }
    return static_cast<int32_t>(len);
}

int32_t MockRegisterDevice::read(uint8_t* buf, size_t len) {
    std::lock_guard<std::mutex> lock(m_);
    before_read(ptr_);
    for (size_t i = 0; i < len; ++i) {
        buf[i] = regs_[ptr_];
        ptr_ = static_cast<uint8_t>(ptr_ + 1);
    }
    return static_cast<int32_t>(len);
}

uint8_t MockRegisterDevice::reg(uint8_t r) const {
    std::lock_guard<std::mutex> lock(m_);
    return regs_[r];
}

void MockRegisterDevice::set_reg(uint8_t r, uint8_t v) {
    std::lock_guard<std::mutex> lock(m_);
    regs_[r] = v;
}

uint32_t MockRegisterDevice::writes() const {
    std::lock_guard<std::mutex> lock(m_);
    return writes_;
}

// =============================================================================
// BME688
// =============================================================================

namespace {

// Calibration coefficients of the simulated part, in the ranges BME68x parts carry
struct Bme688Calibration {
    uint16_t t1 = 26203; int16_t t2 = 26449; int8_t t3 = 3;
    uint16_t p1 = 36477; int16_t p2 = -10560; int8_t p3 = 88; int16_t p4 = 7044; int16_t p5 = -61;
    int8_t p6 = 30; int8_t p7 = 47; int16_t p8 = -1394; int16_t p9 = -3110; uint8_t p10 = 30;
    uint16_t h1 = 787; uint16_t h2 = 1009; int8_t h3 = 0; int8_t h4 = 45; int8_t h5 = 20; uint8_t h6 = 120; int8_t h7 = -100;
    int8_t g1 = -73; int16_t g2 = -15212; int8_t g3 = 18;
    uint8_t res_heat_range = 1; int8_t res_heat_val = 50;
};
const Bme688Calibration kBme688Cal;

// The datasheet's floating-point compensation
double bme_temperature(uint32_t adc, double& t_fine) {
    const Bme688Calibration& c = kBme688Cal;
    const double var1 = ((adc / 16384.0) - (c.t1 / 1024.0)) * c.t2;
    double var2 = (adc / 131072.0) - (c.t1 / 8192.0);
    var2 = var2 * var2 * (c.t3 * 16.0);
    t_fine = var1 + var2;
    return t_fine / 5120.0;
}

double bme_pressure(uint32_t adc, double t_fine) {
    const Bme688Calibration& c = kBme688Cal;
    double var1 = (t_fine / 2.0) - 64000.0;
    double var2 = var1 * var1 * (c.p6 / 131072.0);
    var2 = var2 + (var1 * c.p5 * 2.0);
    var2 = (var2 / 4.0) + (c.p4 * 65536.0);
    var1 = (((c.p3 * var1 * var1) / 16384.0) + (c.p2 * var1)) / 524288.0;
    var1 = (1.0 + (var1 / 32768.0)) * c.p1;
    double p = 1048576.0 - adc;
    p = ((p - (var2 / 4096.0)) * 6250.0) / var1;
    var1 = (c.p9 * p * p) / 2147483648.0;
    var2 = p * (c.p8 / 32768.0);
    const double var3 = (p / 256.0) * (p / 256.0) * (p / 256.0) * (c.p10 / 131072.0);
    return p + (var1 + var2 + var3 + (c.p7 * 128.0)) / 16.0;
}

double bme_humidity(uint32_t adc, double t_fine) {
    const Bme688Calibration& c = kBme688Cal;
    const double t = t_fine / 5120.0;
    const double var1 = adc - ((c.h1 * 16.0) + ((c.h3 / 2.0) * t));
    const double var2 = var1 * ((c.h2 / 262144.0) * (1.0 + ((c.h4 / 16384.0) * t) + ((c.h5 / 1048576.0) * t * t)));
    const double var3 = c.h6 / 16384.0;
    const double var4 = c.h7 / 2097152.0;
    return var2 + ((var3 + (var4 * t)) * var2 * var2);
}

// The raw value in [0, top] whose reading is closest to `want`, for a
// reading that rises (or falls) with the raw value
template <typename F>
uint32_t invert(F reading, double want, uint32_t top, bool rising) {
    uint32_t lo = 0, hi = top;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const bool below = rising ? reading(mid) < want : reading(mid) > want;
        if (below) lo = mid + 1; else hi = mid;
    }
    return lo;
}

void put_le(std::array<uint8_t, 256>& regs, uint8_t at, int32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) regs[static_cast<uint8_t>(at + i)] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i));
}

} // namespace

MockBme688::MockBme688() { reset_registers(); }

void MockBme688::reset_registers() {
    const Bme688Calibration& c = kBme688Cal;
    regs_.fill(0);
    regs_[0xD0] = 0x61;   // chip id
    regs_[0xF0] = 0x01;   // variant: BME688
    put_le(regs_, 0x8A, c.t2, 2);
    regs_[0x8C] = static_cast<uint8_t>(c.t3);
    put_le(regs_, 0x8E, c.p1, 2);
    put_le(regs_, 0x90, c.p2, 2);
    regs_[0x92] = static_cast<uint8_t>(c.p3);
    put_le(regs_, 0x94, c.p4, 2);
    put_le(regs_, 0x96, c.p5, 2);
    regs_[0x98] = static_cast<uint8_t>(c.p7);
    regs_[0x99] = static_cast<uint8_t>(c.p6);
    put_le(regs_, 0x9C, c.p8, 2);
    put_le(regs_, 0x9E, c.p9, 2);
    regs_[0xA0] = c.p10;
    regs_[0xE1] = static_cast<uint8_t>(c.h2 >> 4);
    regs_[0xE2] = static_cast<uint8_t>(((c.h2 & 0x0F) << 4) | (c.h1 & 0x0F));
    regs_[0xE3] = static_cast<uint8_t>(c.h1 >> 4);
    regs_[0xE4] = static_cast<uint8_t>(c.h3);
    regs_[0xE5] = static_cast<uint8_t>(c.h4);
    regs_[0xE6] = static_cast<uint8_t>(c.h5);
    regs_[0xE7] = c.h6;
    regs_[0xE8] = static_cast<uint8_t>(c.h7);
    put_le(regs_, 0xE9, c.t1, 2);
    put_le(regs_, 0xEB, c.g2, 2);
    regs_[0xED] = static_cast<uint8_t>(c.g1);
    regs_[0xEE] = static_cast<uint8_t>(c.g3);
    regs_[0x00] = static_cast<uint8_t>(c.res_heat_val);
    regs_[0x02] = static_cast<uint8_t>(c.res_heat_range << 4);
}

int32_t MockBme688::write(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(m_);
    if (len == 0) return 0;
    // [register, value, register, value, ...]: the first byte also sets the read pointer
    ptr_ = data[0];
    if (len > 1) writes_++;
    for (size_t i = 1; i < len; i += 2) {
        const uint8_t r = ptr_;
        if (r == 0xE0 && data[i] == 0xB6) {
            reset_registers();
        } else if (r != 0xD0 && r != 0xF0) {
            regs_[r] = data[i];
            if (r == 0x74 && (data[i] & 0x03) == 0x01) measure();
        }
        if (i + 1 < len) ptr_ = data[i + 1];
    }
    return static_cast<int32_t>(len);
}

void MockBme688::measure() {
    double t_fine = 0;
    const uint32_t t_adc = invert([](uint32_t a) { double f; return bme_temperature(a, f); }, temperature_c_, (1u << 20) - 1, true);
    bme_temperature(t_adc, t_fine);
    const uint32_t p_adc = invert([&](uint32_t a) { return bme_pressure(a, t_fine); }, pressure_hpa_ * 100.0, (1u << 20) - 1, false);
    const uint32_t h_adc = invert([&](uint32_t a) { return bme_humidity(a, t_fine); }, humidity_pct_, 0xFFFF, true);
    // Gas, BME688 formula: 1e6 * (262144 >> range) / (4096 + 3 * (adc - 512))
    uint32_t g_adc = 1023, g_range = 15;
    for (uint32_t range = 0; range < 16; ++range) {
        const double var2 = 1e6 * static_cast<double>(262144u >> range) / gas_ohms_;
        const double adc = (var2 - 4096.0) / 3.0 + 512.0;
        if (adc >= 0 && adc <= 1023) { g_adc = static_cast<uint32_t>(adc + 0.5); g_range = range; break; }
    }
    regs_[0x1D] = 0x80;   // new data, field 0
    regs_[0x1F] = static_cast<uint8_t>(p_adc >> 12);
    regs_[0x20] = static_cast<uint8_t>(p_adc >> 4);
    regs_[0x21] = static_cast<uint8_t>((p_adc & 0x0F) << 4);
    regs_[0x22] = static_cast<uint8_t>(t_adc >> 12);
    regs_[0x23] = static_cast<uint8_t>(t_adc >> 4);
    regs_[0x24] = static_cast<uint8_t>((t_adc & 0x0F) << 4);
    regs_[0x25] = static_cast<uint8_t>(h_adc >> 8);
    regs_[0x26] = static_cast<uint8_t>(h_adc);
    // gas_r: 10-bit value, then gas_valid, heat_stab and the range
    regs_[0x2C] = static_cast<uint8_t>(g_adc >> 2);
    regs_[0x2D] = static_cast<uint8_t>(((g_adc & 0x03) << 6) | 0x20 | 0x10 | g_range);
    regs_[0x74] &= static_cast<uint8_t>(~0x03);   // back to sleep
    measurements_++;
}

void MockBme688::set_readings(double temperature_c, double pressure_hpa, double humidity_pct, double gas_ohms) {
    std::lock_guard<std::mutex> lock(m_);
    temperature_c_ = temperature_c;
    pressure_hpa_ = pressure_hpa;
    humidity_pct_ = humidity_pct;
    gas_ohms_ = gas_ohms > 0 ? gas_ohms : 1;
}

uint32_t MockBme688::measurements() const {
    std::lock_guard<std::mutex> lock(m_);
    return measurements_;
}

// =============================================================================
// Unit Mini Scales
// =============================================================================

MockMiniScale::MockMiniScale(uint8_t address) {
    const float gap = 100.0f;   // raw counts per gram
    uint32_t bits = 0;
    std::memcpy(&bits, &gap, 4);
    put_le(regs_, 0x40, static_cast<int32_t>(bits), 4);
    regs_[0xFE] = 0x02;   // firmware version
    regs_[0xFF] = address;
    offset_ = 8388;
    update();
}

void MockMiniScale::update() {
    float gap = 0;
    uint32_t bits = static_cast<uint32_t>(regs_[0x40]) | (static_cast<uint32_t>(regs_[0x41]) << 8) |
                    (static_cast<uint32_t>(regs_[0x42]) << 16) | (static_cast<uint32_t>(regs_[0x43]) << 24);
    std::memcpy(&gap, &bits, 4);
    if (!(gap > 0)) gap = 1;
    raw_ = offset_ + static_cast<int32_t>(load_g_ * gap);
    const float grams = static_cast<float>(raw_ - offset_) / gap;
    put_le(regs_, 0x00, raw_, 4);
    std::memcpy(&bits, &grams, 4);
    put_le(regs_, 0x10, static_cast<int32_t>(bits), 4);
    put_le(regs_, 0x60, static_cast<int32_t>(grams * 100.0f), 4);
    char text[16] = {};
    snprintf(text, sizeof(text), "%.2f", static_cast<double>(grams));
    for (int i = 0; i < 16; ++i) regs_[static_cast<uint8_t>(0x70 + i)] = static_cast<uint8_t>(text[i]);
}

void MockMiniScale::stored(uint8_t r, uint8_t v) {
    if (r == 0x50 && v == 1) {
        // Tare: the load on the scale now weighs 0
        offset_ = raw_;
        load_g_ = 0;
        tares_++;
    }
    update();
}

void MockMiniScale::before_read(uint8_t) { update(); }

void MockMiniScale::set_weight_g(double grams) {
    std::lock_guard<std::mutex> lock(m_);
    load_g_ = grams;
    update();
}

void MockMiniScale::set_button(bool pressed) {
    std::lock_guard<std::mutex> lock(m_);
    regs_[0x20] = pressed ? 0 : 1;   // the button pulls its line low
}

uint32_t MockMiniScale::tares() const {
    std::lock_guard<std::mutex> lock(m_);
    return tares_;
}

// =============================================================================
// MFRC522
// =============================================================================

MockMfrc522::MockMfrc522(uint8_t version) : version_(version) { regs_[0x37] = version_; }

void MockMfrc522::stored(uint8_t r, uint8_t v) {
    switch (r) {
        case 0x01:   // CommandReg: SoftReset
            if ((v & 0x0F) == 0x0F) { regs_.fill(0); regs_[0x37] = version_; }
            break;
        case 0x04:   // ComIrqReg: bit 7 sets the bits written, otherwise clears them
            regs_[0x04] = (v & 0x80) ? static_cast<uint8_t>(regs_[0x04] | (v & 0x7F)) : static_cast<uint8_t>(regs_[0x04] & ~v);
            break;
        case 0x0D:   // BitFramingReg: StartSend of a Transceive; no tag answers
            if ((v & 0x80) && (regs_[0x01] & 0x0F) == 0x0C) regs_[0x04] |= 0x01;
            break;
        case 0x37:
            regs_[0x37] = version_;
            break;
        default:
            break;
    }
}

// =============================================================================
// Simulated part lists
// =============================================================================

bool MockHal::attach_parts(const std::string& spec, std::string& why) {
    struct Part { std::string name; int addr{-1}; int channel{-1}; };
    std::vector<Part> parts;
    size_t start = 0;
    while (start <= spec.size()) {
        size_t end = spec.find(',', start);
        if (end == std::string::npos) end = spec.size();
        std::string item = spec.substr(start, end - start);
        start = end + 1;
        if (item.empty()) continue;
        Part p;
        const size_t at = item.find('@');
        if (at != std::string::npos) {
            const std::string ch = item.substr(at + 1);
            if (ch.size() != 1 || ch[0] < '0' || ch[0] > '7') { why = item + ": the channel after @ is 0 to 7"; return false; }
            p.channel = ch[0] - '0';
            item = item.substr(0, at);
        }
        const size_t colon = item.find(':');
        if (colon != std::string::npos) {
            uint8_t a = 0;
            if (!models::parse_hex_byte(item.substr(colon + 1), a) || a < 0x08 || a > 0x77) { why = item + ": the address after : is 0x08 to 0x77"; return false; }
            p.addr = a;
            item = item.substr(0, colon);
        }
        p.name = item;
        parts.push_back(p);
    }
    std::shared_ptr<MockPca9548> hub;
    for (const auto& p : parts) {
        if (p.name != "pahub") continue;
        hub = std::make_shared<MockPca9548>();
        register_mock_i2c_device(0, static_cast<uint8_t>(p.addr >= 0 ? p.addr : 0x70), hub);
    }
    for (const auto& p : parts) {
        if (p.name == "pahub") continue;
        std::shared_ptr<MockI2cDevice> dev;
        uint8_t addr = 0;
        if (p.name == "bme688") { dev = std::make_shared<MockBme688>(); addr = 0x77; }
        else if (p.name == "miniscale") { addr = static_cast<uint8_t>(p.addr >= 0 ? p.addr : 0x26); dev = std::make_shared<MockMiniScale>(addr); }
        else if (p.name == "rfid") { dev = std::make_shared<MockMfrc522>(); addr = 0x28; }
        else { why = p.name + ": not a simulated part (pahub, bme688, miniscale, rfid)"; return false; }
        if (p.addr >= 0) addr = static_cast<uint8_t>(p.addr);
        if (p.channel < 0) {
            register_mock_i2c_device(0, addr, dev);
        } else if (hub) {
            hub->attach(static_cast<uint8_t>(p.channel), addr, dev);
        } else {
            why = p.name + "@" + std::to_string(p.channel) + ": a channel needs a pahub in the list";
            return false;
        }
    }
    return true;
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
    chip_temp_milli_c_.store(kChipTempMilliC);
    chip_temp_status_.store(HAL_OK);

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

    {
        std::lock_guard<std::mutex> lock(rmt_mutex_);
        rmt_tx_frames_.clear();
        rmt_tx_carrier_.clear();
    }
    rmt_loopback_.store(false);
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

void MockHal::set_mock_touch_points(const std::vector<TouchPoint>& points) {
    std::lock_guard<std::mutex> lock(touch_mutex_);
    touch_points_ = points;
}

int32_t MockHal::touch_read(TouchPoint* out, uint32_t max) {
    std::lock_guard<std::mutex> lock(touch_mutex_);
    const uint32_t n = std::min<uint32_t>(max, static_cast<uint32_t>(touch_points_.size()));
    for (uint32_t i = 0; i < n; ++i) out[i] = touch_points_[i];
    return static_cast<int32_t>(n);
}

void MockHal::set_mock_adc_value(uint32_t channel, uint32_t raw_value) {
    if (channel < 16) {
        adc1_values_[channel].store(raw_value);
    }
}

std::shared_ptr<MockI2cDevice> MockHal::i2c_target(uint32_t bus, uint8_t addr, int32_t& err) {
    std::vector<std::shared_ptr<MockI2cDevice>> found;
    {
        std::lock_guard<std::mutex> lock(i2c_mutex_);
        for (const auto& kv : i2c_devices_) {
            if (kv.first.first != bus) continue;
            if (kv.first.second == addr) found.push_back(kv.second);
            kv.second->reachable(addr, found);
        }
    }
    err = found.empty() ? HAL_ERR_NODEV : (found.size() > 1 ? HAL_ERR_IO : HAL_OK);
    return found.size() == 1 ? found[0] : nullptr;
}

int32_t MockHal::i2c_write(uint32_t bus, uint8_t addr, const uint8_t* data, size_t len) {
    if (bus >= 2) return HAL_ERR_INVAL;
    int32_t err = HAL_OK;
    std::shared_ptr<MockI2cDevice> dev = i2c_target(bus, addr, err);
    if (!dev) return err;
    return dev->write(data, len);
}

int32_t MockHal::i2c_read(uint32_t bus, uint8_t addr, uint8_t* buf, size_t len) {
    if (bus >= 2) return HAL_ERR_INVAL;
    int32_t err = HAL_OK;
    std::shared_ptr<MockI2cDevice> dev = i2c_target(bus, addr, err);
    if (!dev) return err;
    return dev->read(buf, len);
}

void MockHal::register_mock_i2c_device(uint32_t bus, uint8_t addr, std::shared_ptr<MockI2cDevice> dev) {
    std::lock_guard<std::mutex> lock(i2c_mutex_);
    i2c_devices_[{bus, addr}] = dev;
}

void MockHal::remove_mock_i2c_device(uint32_t bus, uint8_t addr) {
    std::lock_guard<std::mutex> lock(i2c_mutex_);
    i2c_devices_.erase({bus, addr});
}

std::shared_ptr<MockI2cDevice> MockHal::mock_i2c_device(uint32_t bus, uint8_t addr) {
    std::lock_guard<std::mutex> lock(i2c_mutex_);
    auto it = i2c_devices_.find({bus, addr});
    return it == i2c_devices_.end() ? nullptr : it->second;
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

int32_t MockHal::rmt_transmit(uint32_t pin, const uint16_t* durations, size_t count, uint32_t carrier_hz) {
    if (pin >= 64 || !durations) return HAL_ERR_INVAL;
    std::lock_guard<std::mutex> lock(rmt_mutex_);
    auto& frames = rmt_tx_frames_[pin];
    if (frames.size() == kRmtTxFramesKept) frames.erase(frames.begin());
    frames.emplace_back(durations, durations + count);
    rmt_tx_carrier_[pin] = carrier_hz;
    return static_cast<int32_t>(count);
}

std::vector<std::vector<uint16_t>> MockHal::get_rmt_tx_frames(uint32_t pin) {
    std::lock_guard<std::mutex> lock(rmt_mutex_);
    auto it = rmt_tx_frames_.find(pin);
    return it == rmt_tx_frames_.end() ? std::vector<std::vector<uint16_t>>() : it->second;
}

uint32_t MockHal::get_rmt_tx_carrier(uint32_t pin) {
    std::lock_guard<std::mutex> lock(rmt_mutex_);
    auto it = rmt_tx_carrier_.find(pin);
    return it == rmt_tx_carrier_.end() ? 0 : it->second;
}

void MockHal::set_rmt_loopback(bool on) {
    rmt_loopback_.store(on);
}

bool MockHal::rmt_loopback() const {
    return rmt_loopback_.load();
}

uint32_t MockHal::get_gpio_write_count(uint32_t pin) const {
    if (pin >= 64) return 0;
    return pin_write_counts_[pin].load();
}

PinMode MockHal::get_gpio_mode(uint32_t pin) const {
    return pin < 64 ? static_cast<PinMode>(pin_modes_[pin].load()) : PinMode::Input;
}

int32_t MockHal::chip_temp_read(int32_t& out_milli_c) {
    const int32_t status = chip_temp_status_.load();
    if (status == HAL_OK) out_milli_c = chip_temp_milli_c_.load();
    return status;
}

void MockHal::set_chip_temp(int32_t milli_c, int32_t status) {
    chip_temp_milli_c_.store(milli_c);
    chip_temp_status_.store(status);
}

} // namespace kube32::hal
