#include "hal/i2c_discovery.hpp"
#include "hal/capability_firewall.hpp"
#include "hal/hal_types.hpp"
#include "hal/mock_hal.hpp"

#include <cstdio>

#if defined(ESP_PLATFORM)
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#endif

namespace kube32::hal {

namespace {

constexpr uint8_t kFirstAddress = 0x08;
constexpr uint8_t kLastAddress = 0x77;

#if defined(ESP_PLATFORM)
// The caller holds the bus (lent by the firewall). False when the lines are
// held low, which no idle I2C bus does.
bool probe_bus(uint8_t bus, const BusPins& pins, std::vector<uint8_t>& out) {
    const gpio_num_t sda = static_cast<gpio_num_t>(pins.a);
    const gpio_num_t scl = static_cast<gpio_num_t>(pins.b);
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << pins.a) | (1ULL << pins.b);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
    esp_rom_delay_us(50);
    const bool idle_high = gpio_get_level(sda) == 1 && gpio_get_level(scl) == 1;
    bool ok = false;
    if (idle_high) {
        i2c_master_bus_config_t cfg = {};
        cfg.i2c_port = static_cast<i2c_port_num_t>(bus);
        cfg.sda_io_num = sda;
        cfg.scl_io_num = scl;
        cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        cfg.glitch_ignore_cnt = 7;
        cfg.flags.enable_internal_pullup = true;
        i2c_master_bus_handle_t handle = nullptr;
        if (i2c_new_master_bus(&cfg, &handle) == ESP_OK) {
            ok = true;
            int timeouts = 0;
            for (uint8_t addr = kFirstAddress; addr <= kLastAddress; ++addr) {
                const esp_err_t e = i2c_master_probe(handle, addr, 20);
                if (e == ESP_OK) out.push_back(addr);
                // A device stretching the clock forever: stop rather than wait 20 ms per address
                if (e == ESP_ERR_TIMEOUT && ++timeouts >= 3) { ok = false; break; }
            }
            i2c_del_master_bus(handle);
        }
    }
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
    return ok;
}
#else
bool probe_bus(uint8_t bus, const BusPins&, std::vector<uint8_t>& out) {
    const uint8_t none = 0;
    for (uint8_t addr = kFirstAddress; addr <= kLastAddress; ++addr) {
        if (MockHal::instance().i2c_write(bus, addr, &none, 0) != HAL_ERR_NODEV) out.push_back(addr);   // any answer is an acknowledge
    }
    return true;
}
#endif

} // namespace

bool I2cDiscovery::scan() {
    bool changed = false;
    for (uint8_t bus = 0; bus < 2; ++bus) {
        BusPins pins;
        if (!CapabilityFirewall::instance().lend_i2c_bus(bus, pins)) continue;   // held by a pod, or not offered
        std::vector<uint8_t> found;
        const bool ok = probe_bus(bus, pins, found);
        CapabilityFirewall::instance().return_i2c_bus(bus);
        if (!ok) found.clear();
        if (found != found_[bus]) {
            found_[bus] = std::move(found);
            changed = true;
        }
    }
    return changed;
}

std::map<std::string, std::string> I2cDiscovery::labels() const {
    std::map<std::string, std::string> out;
    for (uint8_t bus = 0; bus < 2; ++bus) {
        for (uint8_t addr : found_[bus]) {
            char key[24];
            snprintf(key, sizeof(key), "i2c.kube32.io/0x%02x", addr);
            out.emplace(key, "bus-" + std::to_string(bus));   // the lowest bus wins
        }
    }
    return out;
}

const std::vector<uint8_t>& I2cDiscovery::devices(uint8_t bus) const {
    static const std::vector<uint8_t> none;
    return bus < 2 ? found_[bus] : none;
}

} // namespace kube32::hal
