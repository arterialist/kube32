#pragma once

// Simulated I2C parts for the stub bus (hal::MockHal): a BME688 or BME680
// and an M5Stack Unit Mini Scales. Used by the tests of apps/drivers.

#include "hal/mock_hal.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>

namespace k32test {

// BME68x calibration (bme68x_calib_data)
struct Calib {
    int t1 = 26176, t2 = 26383, t3 = 3;
    int p1 = 36500, p2 = -10379, p3 = 88, p4 = 6713, p5 = -144, p6 = 30, p7 = 34, p8 = -2652, p9 = -2834, p10 = 30;
    int h1 = 778, h2 = 1012, h3 = 0, h4 = 45, h5 = 20, h6 = 120, h7 = -100;
    int gh1 = -30, gh2 = -5969, gh3 = 18, res_heat_range = 1, res_heat_val = 46, range_sw_err = -1;
};

// A BME688 (variant 1) or BME680 (variant 0) on the stub I2C bus: chip and
// variant id, soft reset, the calibration registers, writes of register and
// value pairs, and forced mode filling the field registers with the ADC
// values a test sets (new data after `busy_reads` reads of the status)
class Bme68x : public kube32::hal::MockI2cDevice {
public:
    std::mutex m;
    uint8_t regs[256] = {};
    uint32_t adc_t = 490000, adc_p = 352000, adc_h = 21000;
    uint16_t adc_gas = 600;
    uint8_t gas_range = 10;
    bool heat_stab = true, absent = false;
    int forced = 0, busy_reads = 2;

    explicit Bme68x(uint8_t variant, const Calib& c = Calib()) : variant_(variant) {
        regs[0xD0] = 0x61;
        regs[0xF0] = variant;
        auto le = [&](int reg, int v) { regs[reg] = static_cast<uint8_t>(v); regs[reg + 1] = static_cast<uint8_t>(v >> 8); };
        le(0x8A, c.t2); regs[0x8C] = static_cast<uint8_t>(c.t3);
        le(0x8E, c.p1); le(0x90, c.p2); regs[0x92] = static_cast<uint8_t>(c.p3); le(0x94, c.p4); le(0x96, c.p5);
        regs[0x98] = static_cast<uint8_t>(c.p7); regs[0x99] = static_cast<uint8_t>(c.p6); le(0x9C, c.p8); le(0x9E, c.p9);
        regs[0xA0] = static_cast<uint8_t>(c.p10);
        regs[0xE1] = static_cast<uint8_t>(c.h2 >> 4); regs[0xE2] = static_cast<uint8_t>((c.h2 & 15) << 4 | (c.h1 & 15));
        regs[0xE3] = static_cast<uint8_t>(c.h1 >> 4);
        regs[0xE4] = static_cast<uint8_t>(c.h3); regs[0xE5] = static_cast<uint8_t>(c.h4); regs[0xE6] = static_cast<uint8_t>(c.h5);
        regs[0xE7] = static_cast<uint8_t>(c.h6); regs[0xE8] = static_cast<uint8_t>(c.h7);
        le(0xE9, c.t1); le(0xEB, c.gh2); regs[0xED] = static_cast<uint8_t>(c.gh1); regs[0xEE] = static_cast<uint8_t>(c.gh3);
        regs[0x00] = static_cast<uint8_t>(c.res_heat_val);
        regs[0x02] = static_cast<uint8_t>(c.res_heat_range << 4 | 0x0A);   // other bits of the register set
        regs[0x04] = static_cast<uint8_t>(c.range_sw_err << 4 | 0x03);
    }

    int32_t write(const uint8_t* data, size_t len) override {
        std::lock_guard<std::mutex> lock(m);
        if (absent) return kube32::hal::HAL_ERR_NODEV;
        if (len == 0) return 0;
        reg_ = data[0];
        if (len >= 2) store(data[0], data[1]);
        for (size_t i = 2; i + 1 < len; i += 2) store(data[i], data[i + 1]);
        return static_cast<int32_t>(len);
    }

    int32_t read(uint8_t* buf, size_t len) override {
        std::lock_guard<std::mutex> lock(m);
        if (absent) return kube32::hal::HAL_ERR_NODEV;
        if (reg_ == 0x1D && countdown_ > 0 && --countdown_ == 0) regs[0x1D] = 0x80;   // new_data, gas index 0
        for (size_t i = 0; i < len; ++i) buf[i] = regs[(reg_ + i) & 0xFF];
        return static_cast<int32_t>(len);
    }

private:
    uint8_t variant_;
    uint8_t reg_ = 0;
    int countdown_ = 0;

    void store(uint8_t reg, uint8_t v) {
        if (reg == 0xE0) {
            if (v == 0xB6) for (int r = 0x1D; r <= 0x75; ++r) regs[r] = 0;
            return;
        }
        regs[reg] = v;
        if (reg != 0x74 || (v & 3) != 1) return;
        forced++;                                          // forced mode: one measurement, then sleep
        regs[0x74] = static_cast<uint8_t>(v & ~3);
        regs[0x1F] = static_cast<uint8_t>(adc_p >> 12); regs[0x20] = static_cast<uint8_t>(adc_p >> 4); regs[0x21] = static_cast<uint8_t>((adc_p & 15) << 4);
        regs[0x22] = static_cast<uint8_t>(adc_t >> 12); regs[0x23] = static_cast<uint8_t>(adc_t >> 4); regs[0x24] = static_cast<uint8_t>((adc_t & 15) << 4);
        regs[0x25] = static_cast<uint8_t>(adc_h >> 8); regs[0x26] = static_cast<uint8_t>(adc_h);
        const bool run_gas = (regs[0x71] & 0x30) != 0 && !(regs[0x70] & 0x08);
        const int g = variant_ == 1 ? 0x2C : 0x2A;
        regs[g] = static_cast<uint8_t>(adc_gas >> 2);
        regs[g + 1] = static_cast<uint8_t>((adc_gas & 3) << 6 | (run_gas ? 0x20 : 0) | (run_gas && heat_stab ? 0x10 : 0) | gas_range);
        regs[0x1D] = 0x20;                                 // measuring
        countdown_ = busy_reads + 1;
        if (busy_reads == 0) { regs[0x1D] = 0x80; countdown_ = 0; }
    }
};

// The M5Stack Unit Mini Scales (STM32 firmware, protocol v3): raw ADC,
// weight x 100 from the raw value, the offset and the gap (counts per
// gram), tare through the offset register, the LED, version and address
class MiniScale : public kube32::hal::MockI2cDevice {
public:
    std::mutex m;
    int32_t raw = 812345, offset = 800000, gap = 50;
    uint8_t led[3] = {0, 0, 0};
    bool absent = false;
    int tares = 0;

    int32_t write(const uint8_t* data, size_t len) override {
        std::lock_guard<std::mutex> lock(m);
        if (absent) return kube32::hal::HAL_ERR_NODEV;
        if (len == 0) return 0;
        reg_ = data[0];
        if (reg_ == 0x50 && len >= 2 && data[1] == 1) { offset = raw; tares++; }
        if (reg_ == 0x30) for (size_t i = 1; i < len && i <= 3; ++i) led[i - 1] = data[i];
        return static_cast<int32_t>(len);
    }

    int32_t read(uint8_t* buf, size_t len) override {
        std::lock_guard<std::mutex> lock(m);
        if (absent) return kube32::hal::HAL_ERR_NODEV;
        uint8_t r[16] = {};
        auto le32 = [&](int32_t v) { for (int i = 0; i < 4; ++i) r[i] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)); };
        switch (reg_) {
            case 0x00: le32(raw); break;
            case 0x60: le32(static_cast<int32_t>((static_cast<int64_t>(raw) - offset) * 100 / gap)); break;
            case 0x30: std::memcpy(r, led, 3); break;
            case 0xFE: r[0] = 2; break;
            case 0xFF: r[0] = 0x26; break;
            default: break;
        }
        std::memcpy(buf, r, len < sizeof r ? len : sizeof r);
        return static_cast<int32_t>(len);
    }

private:
    uint8_t reg_ = 0;
};

} // namespace k32test
