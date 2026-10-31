#include "hal/i2c_discovery.hpp"
#include "hal/capability_firewall.hpp"
#include "hal/hal_types.hpp"
#include "hal/pod_buses.hpp"

#include <algorithm>
#include <cstdio>
#include <tuple>

#if defined(ESP_PLATFORM)
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#endif

namespace kube32::hal {

namespace {

constexpr uint8_t kFirstAddress = 0x08;
constexpr uint8_t kLastAddress = 0x77;
constexpr const char* kScanUser = "node/i2c-discovery";

#if defined(ESP_PLATFORM)
// False when the lines of a free bus are held low, which no idle I2C bus does
bool lines_idle(const BusPins& pins) {
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << pins.a) | (1ULL << pins.b);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
    esp_rom_delay_us(50);
    const bool idle = gpio_get_level(static_cast<gpio_num_t>(pins.a)) == 1 && gpio_get_level(static_cast<gpio_num_t>(pins.b)) == 1;
    if (!idle) {
        gpio_reset_pin(static_cast<gpio_num_t>(pins.a));
        gpio_reset_pin(static_cast<gpio_num_t>(pins.b));
    }
    return idle;
}
#else
bool lines_idle(const BusPins&) { return true; }
#endif

// One register of the part at `loc`, through a register pointer write
bool read_register(const models::I2cLocation& loc, uint8_t reg, uint8_t& out) {
    return i2c_device_write(loc, &reg, 1) == 1 && i2c_device_read(loc, &out, 1) == 1;
}

// The catalog part at `loc`, by its driver's register reads: chip and driver
// set, or both left empty
void identify(const models::I2cLocation& loc, models::DiscoveredPart& part) {
    size_t n = 0;
    const models::DeviceDriver* drivers = models::device_drivers(n);
    for (size_t i = 0; i < n; ++i) {
        const models::DeviceDriver& d = drivers[i];
        if (d.bus != models::DeviceBus::I2c || d.checks == 0 || (loc.address != d.address && loc.address != d.alt_address)) continue;
        const char* chip = d.chip;
        bool match = true;
        for (uint8_t c = 0; c < d.checks && match; ++c) {
            const models::ChipCheck& check = d.check[c];
            uint8_t v = 0;
            if (!read_register(loc, check.reg, v)) return;   // stopped answering
            match = false;
            for (uint8_t k = 0; k < check.count; ++k) {
                if (v != check.values[k]) continue;
                match = true;
                if (check.chips[k]) chip = check.chips[k];
                break;
            }
        }
        if (!match) continue;
        part.chip = chip;
        part.driver = d.name;
        return;
    }
}

// Addresses that answer at `route` (its multiplexer channel selected), less
// `skip`; false when the bus stretched the clock past three probes
bool probe_all(const models::I2cLocation& route, const std::vector<uint8_t>& skip, std::vector<uint8_t>& out) {
    int timeouts = 0;
    models::I2cLocation loc = route;
    for (uint8_t addr = kFirstAddress; addr <= kLastAddress; ++addr) {
        if (std::find(skip.begin(), skip.end(), addr) != skip.end()) continue;
        loc.address = addr;
        const int32_t r = i2c_device_write(loc, nullptr, 0);
        if (r == HAL_OK) out.push_back(addr);
        // A device stretching the clock forever: stop rather than wait per address
        if (r == HAL_ERR_TIMEDOUT && ++timeouts >= 3) return false;
    }
    return true;
}

// Scans an installed bus. `known` is what the last scan found there: parts
// it identified are not read again.
bool scan_bus(uint8_t bus, const std::vector<models::DiscoveredPart>& known, std::vector<uint8_t>& direct,
              std::vector<models::DiscoveredPart>& out) {
    models::I2cLocation route{bus, 0, 0, 0};
    if (!probe_all(route, {}, direct)) return false;

    // The part at `where`: what the last scan identified there, else read now
    // (unless a pod's device grant holds the address)
    auto take = [&](const models::I2cLocation& where) {
        models::DiscoveredPart p{where, "", ""};
        for (const auto& k : known) {
            if (k.where == where && !k.chip.empty()) { p.chip = k.chip; p.driver = k.driver; return p; }
        }
        if (!CapabilityFirewall::instance().i2c_device_granted(where)) identify(where, p);
        return p;
    };
    // Multiplexers: the addresses of 0x70-0x77 that are no catalog part
    std::vector<uint8_t> muxes;
    std::vector<models::DiscoveredPart> parts;
    for (uint8_t addr : direct) {
        if (addr < models::kMuxFirstAddress || addr > models::kMuxLastAddress) continue;
        models::DiscoveredPart p = take(models::I2cLocation{bus, addr, 0, 0});
        if (p.driver.empty() && i2c_mux_test(bus, addr)) {
            muxes.push_back(addr);
            p.chip = "PCA9548A";
        }
        parts.push_back(std::move(p));
    }
    if (!muxes.empty()) {
        // A channel left open before showed its parts as on the bus: with
        // every multiplexer off, only the bus's own parts answer
        std::vector<uint8_t> again;
        if (!probe_all(route, muxes, again)) return false;
        again.insert(again.end(), muxes.begin(), muxes.end());
        std::sort(again.begin(), again.end());
        direct.swap(again);
    }
    for (uint8_t addr : direct) {
        auto it = std::find_if(parts.begin(), parts.end(), [&](const models::DiscoveredPart& p) { return p.where.address == addr; });
        out.push_back(it != parts.end() ? *it : take(models::I2cLocation{bus, addr, 0, 0}));
    }

    // Behind each multiplexer, one channel at a time; the bus's own parts
    // answer on every channel and are left out
    std::vector<uint8_t> skip = direct;
    for (uint8_t mux : muxes) {
        for (uint8_t ch = 0; ch < models::kMuxChannels; ++ch) {
            std::vector<uint8_t> behind;
            const bool ok = probe_all(models::I2cLocation{bus, 0, mux, ch}, skip, behind);
            for (uint8_t addr : behind) out.push_back(take(models::I2cLocation{bus, addr, mux, ch}));
            if (!ok) { i2c_deselect(bus); return false; }
        }
    }
    i2c_deselect(bus);
    return true;
}

} // namespace

bool I2cDiscovery::scan() {
    bool changed = false;
    for (uint8_t bus = 0; bus < 2; ++bus) {
        BusPins pins;
        bool shared = false;
        if (!CapabilityFirewall::instance().lend_i2c_bus(bus, pins, &shared)) continue;   // held by a pod, or not offered
        BusAssignment b;
        b.kind = BusKind::I2c;
        b.index = bus;
        b.pin_a = static_cast<uint8_t>(pins.a);
        b.pin_b = static_cast<uint8_t>(pins.b);
        b.speed = kDefaultI2cHz;
        std::vector<uint8_t> direct;
        std::vector<models::DiscoveredPart> found;
        std::string err;
        bool ok;
        if (shared) {
            // Installed for device grants: scanned beside them, kept installed meanwhile
            ok = i2c_share(kScanUser, b, err) && scan_bus(bus, parts_[bus], direct, found);
            bus_release(kScanUser);
        } else {
            ok = lines_idle(pins) && i2c_scan_begin(b, err) && scan_bus(bus, parts_[bus], direct, found);
            i2c_scan_end(bus);
        }
        CapabilityFirewall::instance().return_i2c_bus(bus);
        if (!ok) {
            direct.clear();
            found.clear();
        }
        // A part the last scan found and this one does not stays one more
        // scan: a transfer that fails once (a reader that misses its
        // address for a moment, a scan that gave up) does not take its
        // Device's driver away
        std::vector<models::I2cLocation> missing;
        for (const auto& p : parts_[bus]) {
            const bool seen = std::any_of(found.begin(), found.end(), [&](const models::DiscoveredPart& f) { return f.where == p.where; });
            const bool missed_before = std::find(missing_[bus].begin(), missing_[bus].end(), p.where) != missing_[bus].end();
            if (seen || missed_before) continue;
            found.push_back(p);
            missing.push_back(p.where);
        }
        missing_[bus] = std::move(missing);
        // The bus's own parts first, then by multiplexer, channel and address
        std::sort(found.begin(), found.end(), [](const models::DiscoveredPart& a, const models::DiscoveredPart& b) {
            return std::tie(a.where.mux, a.where.channel, a.where.address) < std::tie(b.where.mux, b.where.channel, b.where.address);
        });
        auto same = [](const std::vector<models::DiscoveredPart>& a, const std::vector<models::DiscoveredPart>& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (a[i].where != b[i].where || a[i].chip != b[i].chip) return false;
            }
            return true;
        };
        if (direct != found_[bus] || !same(found, parts_[bus])) changed = true;
        found_[bus] = std::move(direct);
        parts_[bus] = std::move(found);
    }
    return changed;
}

std::map<std::string, std::string> I2cDiscovery::labels() const {
    std::map<std::string, std::string> out;
    char key[24];
    for (uint8_t bus = 0; bus < 2; ++bus) {
        for (uint8_t addr : found_[bus]) {
            snprintf(key, sizeof(key), "i2c.kube32.io/0x%02x", addr);
            out.emplace(key, "bus-" + std::to_string(bus));   // the lowest bus wins
        }
    }
    // Behind a multiplexer: unless the address is on a bus itself
    for (uint8_t bus = 0; bus < 2; ++bus) {
        for (const auto& p : parts_[bus]) {
            if (!p.where.mux) continue;
            snprintf(key, sizeof(key), "i2c.kube32.io/0x%02x", p.where.address);
            char value[32];
            snprintf(value, sizeof(value), "bus-%u.mux-%02x.%u", bus, p.where.mux, p.where.channel);
            out.emplace(key, value);
        }
    }
    return out;
}

std::vector<models::DiscoveredPart> I2cDiscovery::parts() const {
    std::vector<models::DiscoveredPart> out = parts_[0];
    out.insert(out.end(), parts_[1].begin(), parts_[1].end());
    return out;
}

std::string I2cDiscovery::annotation() const {
    return models::discovered_parts_json(parts());
}

const std::vector<uint8_t>& I2cDiscovery::devices(uint8_t bus) const {
    static const std::vector<uint8_t> none;
    return bus < 2 ? found_[bus] : none;
}

} // namespace kube32::hal
