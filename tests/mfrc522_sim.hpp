#pragma once

// A simulated MFRC522 for the stub I2C bus (hal::MockHal): its registers,
// FIFO and Transceive command, with one ISO 14443-A tag that can be put into
// and taken out of the field (4- or 7-byte UID: anticollision, select, halt).
// Used by the tests of apps/rfid and apps/drivers/rfid.

#include "hal/mock_hal.hpp"

#include <cstdint>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <vector>

namespace k32test {

inline uint16_t crc_a(const uint8_t* p, size_t n) {
    uint16_t crc = 0x6363;
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = static_cast<uint8_t>(p[i] ^ static_cast<uint8_t>(crc));
        ch = static_cast<uint8_t>(ch ^ static_cast<uint8_t>(ch << 4));
        crc = static_cast<uint16_t>((crc >> 8) ^ (static_cast<uint16_t>(ch) << 8) ^ (static_cast<uint16_t>(ch) << 3) ^ (ch >> 4));
    }
    return crc;
}

// Registers, FIFO and the Transceive command of an MFRC522, with one tag
// that can be put into and taken out of the field.
class Mfrc522 : public kube32::hal::MockI2cDevice {
public:
    std::mutex m;
    std::vector<uint8_t> uid;   // empty: no tag in the field
    bool halted{false};
    bool absent{false};         // unplugged: nothing acknowledges
    int selects{0};

    int32_t write(const uint8_t* data, size_t len) override {
        std::lock_guard<std::mutex> lock(m);
        if (absent) return kube32::hal::HAL_ERR_NODEV;
        if (!data || len == 0) return 0;
        reg_ = data[0];
        for (size_t i = 1; i < len; ++i) store(reg_, data[i]);
        return static_cast<int32_t>(len);
    }
    int32_t read(uint8_t* buf, size_t len) override {
        std::lock_guard<std::mutex> lock(m);
        if (absent) return kube32::hal::HAL_ERR_NODEV;
        for (size_t i = 0; i < len; ++i) {
            if (reg_ == 0x09) {
                buf[i] = fifo_.empty() ? 0 : fifo_.front();
                if (!fifo_.empty()) fifo_.pop_front();
            } else if (reg_ == 0x0A) {
                buf[i] = static_cast<uint8_t>(fifo_.size());
            } else {
                buf[i] = regs_[reg_];
            }
        }
        return static_cast<int32_t>(len);
    }

private:
    uint8_t reg_{0};
    uint8_t regs_[64] = {};
    std::deque<uint8_t> fifo_;

    void store(uint8_t reg, uint8_t v) {
        switch (reg) {
            case 0x01:   // Command
                if (v == 0x0F) { for (auto& r : regs_) r = 0; fifo_.clear(); regs_[0x37] = 0x15; }
                regs_[0x01] = v;
                break;
            case 0x04: regs_[0x04] &= static_cast<uint8_t>(~v); break;   // ComIrq: writing 1s clears
            case 0x09: fifo_.push_back(v); break;
            case 0x0A: if (v & 0x80) fifo_.clear(); break;
            case 0x0D:
                regs_[0x0D] = v;
                if ((v & 0x80) && regs_[0x01] == 0x0C) transceive(v & 0x07);
                break;
            default: regs_[reg] = v; break;
        }
    }

    void answer(std::initializer_list<uint8_t> bytes) {
        fifo_.assign(bytes.begin(), bytes.end());
        regs_[0x04] |= 0x30;
    }
    void answer_crc(std::vector<uint8_t> bytes) {
        const uint16_t c = crc_a(bytes.data(), bytes.size());
        bytes.push_back(static_cast<uint8_t>(c));
        bytes.push_back(static_cast<uint8_t>(c >> 8));
        fifo_.assign(bytes.begin(), bytes.end());
        regs_[0x04] |= 0x30;
    }
    void silence() { fifo_.clear(); regs_[0x04] |= 0x01; }   // the timer runs out

    // UID bytes of a cascade level as sent in the anticollision answer
    std::vector<uint8_t> level_bytes(int level) const {
        std::vector<uint8_t> b;
        if (uid.size() == 4) b.assign(uid.begin(), uid.end());
        else if (level == 0) b = {0x88, uid[0], uid[1], uid[2]};
        else b.assign(uid.begin() + 3, uid.begin() + 7);
        b.push_back(static_cast<uint8_t>(b[0] ^ b[1] ^ b[2] ^ b[3]));
        return b;
    }

    void transceive(uint8_t last_bits) {
        std::vector<uint8_t> f(fifo_.begin(), fifo_.end());
        fifo_.clear();
        regs_[0x06] = 0;
        if (uid.empty() || f.empty()) return silence();
        if (last_bits == 7 && f.size() == 1 && (f[0] == 0x52 || (f[0] == 0x26 && !halted))) {
            halted = false;
            return answer({static_cast<uint8_t>(uid.size() == 4 ? 0x04 : 0x44), 0x00});
        }
        if (f.size() == 2 && (f[0] == 0x93 || f[0] == 0x95) && f[1] == 0x20) {
            auto b = level_bytes(f[0] == 0x93 ? 0 : 1);
            fifo_.assign(b.begin(), b.end());
            regs_[0x04] |= 0x30;
            return;
        }
        if (f.size() == 9 && (f[0] == 0x93 || f[0] == 0x95) && f[1] == 0x70) {
            if (crc_a(f.data(), 7) != static_cast<uint16_t>(f[7] | (f[8] << 8))) return silence();
            const int level = f[0] == 0x93 ? 0 : 1;
            if (std::vector<uint8_t>(f.begin() + 2, f.begin() + 7) != level_bytes(level)) return silence();
            selects++;
            const bool more = uid.size() == 7 && level == 0;
            return answer_crc({static_cast<uint8_t>(more ? 0x04 : (uid.size() == 4 ? 0x08 : 0x00))});
        }
        if (f.size() == 4 && f[0] == 0x50 && f[1] == 0x00) { halted = true; return silence(); }
        silence();
    }
};

} // namespace k32test
