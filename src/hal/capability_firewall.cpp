#include "hal/capability_firewall.hpp"
#include "models/kube_models.hpp"
#include "osal/thread.hpp"
#include <cctype>
#include <iostream>

namespace kube32::hal {

CapabilityFirewall& CapabilityFirewall::instance() {
    static CapabilityFirewall s_instance;
    return s_instance;
}

CapabilityFirewall::CapabilityFirewall() {
    reset();
}

void CapabilityFirewall::reset() {
    bus_release_all();
    rmt_release_all();
    osal::LockGuard guard(mutex_);
    profile_ = BoardProfile{};
    pod_records_.clear();
    for (auto& o : gpio_owners_) o.clear();
    for (auto& o : adc1_owners_) o.clear();
    for (auto& o : i2c_owners_) o.clear();
    for (auto& o : uart_owners_) o.clear();
    allocated_gpio_mask_ = 0;
    allocated_adc1_mask_ = 0;
    allocated_i2c_mask_ = 0;
    allocated_uart_mask_ = 0;
    lent_i2c_mask_ = 0;
    admissions_ = 0;
}

void CapabilityFirewall::set_board_profile(const BoardProfile& profile) {
    osal::LockGuard guard(mutex_);
    profile_ = profile;
    profile_.i2c_bus_mask &= KUBE32_CHIP_I2C_MASK;
    profile_.uart_port_mask &= KUBE32_CHIP_UART_MASK;
    profile_.rmt_tx_pin_mask &= KUBE32_CHIP_GPIO_MASK;
}

BoardProfile CapabilityFirewall::board_profile() const {
    osal::LockGuard guard(mutex_);
    return profile_;
}

uint64_t CapabilityFirewall::claimable_gpio_mask_locked() const {
    return KUBE32_SAFE_GPIO_MASK & ~profile_.reserved_gpio_mask;
}

uint16_t CapabilityFirewall::claimable_adc1_mask_locked() const {
    uint64_t pins = claimable_gpio_mask_locked();
    uint16_t mask = 0;
    for (uint32_t ch = 0; ch < KUBE32_ADC1_CHANNELS; ++ch) {
        if (pins & (1ULL << (ch + 1))) mask |= static_cast<uint16_t>(1U << ch);
    }
    return mask;
}

uint64_t CapabilityFirewall::claimable_gpio_mask() const {
    osal::LockGuard guard(mutex_);
    return claimable_gpio_mask_locked();
}

uint16_t CapabilityFirewall::claimable_adc1_mask() const {
    osal::LockGuard guard(mutex_);
    return claimable_adc1_mask_locked();
}

uint8_t CapabilityFirewall::claimable_i2c_mask() const {
    osal::LockGuard guard(mutex_);
    return profile_.i2c_bus_mask;
}

uint32_t CapabilityFirewall::claimable_rmt_count_locked(RmtDir dir) const {
    uint64_t pins = claimable_gpio_mask_locked();
    if (dir == RmtDir::Tx) pins |= profile_.rmt_tx_pin_mask;
    uint32_t n = 0;
    for (; pins; pins &= pins - 1) n++;
    return n < kRmtChannels ? n : kRmtChannels;
}

uint32_t CapabilityFirewall::allocated_rmt_count_locked(RmtDir dir) const {
    const uint32_t cap = dir == RmtDir::Tx ? KUBE32_CAP_RMT_TX : KUBE32_CAP_RMT_RX;
    uint32_t n = 0;
    for (const auto& kv : pod_records_) {
        if (kv.second.grant.has_capability(cap)) n++;
    }
    return n;
}

uint32_t CapabilityFirewall::claimable_rmt_tx_count() const {
    osal::LockGuard guard(mutex_);
    return claimable_rmt_count_locked(RmtDir::Tx);
}

uint32_t CapabilityFirewall::claimable_rmt_rx_count() const {
    osal::LockGuard guard(mutex_);
    return claimable_rmt_count_locked(RmtDir::Rx);
}

uint32_t CapabilityFirewall::get_allocated_rmt_tx_count() const {
    osal::LockGuard guard(mutex_);
    return allocated_rmt_count_locked(RmtDir::Tx);
}

uint32_t CapabilityFirewall::get_allocated_rmt_rx_count() const {
    osal::LockGuard guard(mutex_);
    return allocated_rmt_count_locked(RmtDir::Rx);
}

bool CapabilityFirewall::allocate_generic_gpios(uint32_t count, DeviceGrant& grant, const std::string& pod_id, std::vector<uint32_t>& out_claimed, std::string& out_error) {
    uint32_t allocated = 0;
    std::vector<uint32_t> claimed_pins;

    const uint64_t claimable = claimable_gpio_mask_locked();
    for (uint32_t pin = 1; pin < 64 && allocated < count; ++pin) {
        if ((claimable & (1ULL << pin)) == 0) continue;
        if (!gpio_owners_[pin].empty()) continue;

        claimed_pins.push_back(pin);
        allocated++;
    }

    if (allocated < count) {
        out_error = "Insufficient allocatable GPIO pins: requested " + std::to_string(count) + ", available " + std::to_string(allocated);
        return false;
    }

    for (uint32_t pin : claimed_pins) {
        grant.gpio_read_mask |= (1ULL << pin);
        grant.gpio_write_mask |= (1ULL << pin);
        gpio_owners_[pin] = pod_id;
        allocated_gpio_mask_ |= (1ULL << pin);
        out_claimed.push_back(pin);
    }
    return true;
}

bool CapabilityFirewall::allocate_generic_i2c(uint32_t count, DeviceGrant& grant, const std::string& pod_id, std::vector<uint32_t>& out_claimed, std::string& out_error) {
    uint32_t allocated = 0;
    std::vector<uint32_t> claimed_buses;

    for (uint32_t bus = 0; bus < 2 && allocated < count; ++bus) {
        if ((profile_.i2c_bus_mask & (1U << bus)) == 0) continue;
        if (!i2c_owners_[bus].empty()) continue;
        claimed_buses.push_back(bus);
        allocated++;
    }

    if (allocated < count) {
        out_error = "Insufficient allocatable I2C buses: requested " + std::to_string(count) + ", available " + std::to_string(allocated);
        return false;
    }

    for (uint32_t bus : claimed_buses) {
        grant.i2c_bus_mask |= (1U << bus);
        i2c_owners_[bus] = pod_id;
        allocated_i2c_mask_ |= (1U << bus);
        out_claimed.push_back(bus);
    }
    return true;
}

bool CapabilityFirewall::allocate_generic_adc(uint32_t count, DeviceGrant& grant, const std::string& pod_id, std::vector<uint32_t>& out_claimed, std::string& out_error) {
    uint32_t allocated = 0;
    std::vector<uint32_t> claimed_channels;

    const uint16_t claimable = claimable_adc1_mask_locked();
    for (uint32_t ch = 0; ch < KUBE32_ADC1_CHANNELS && allocated < count; ++ch) {
        if ((claimable & (1U << ch)) == 0) continue;
        if (!adc1_owners_[ch].empty()) continue;
        claimed_channels.push_back(ch);
        allocated++;
    }

    if (allocated < count) {
        out_error = "Insufficient allocatable ADC1 channels: requested " + std::to_string(count) + ", available " + std::to_string(allocated);
        return false;
    }

    for (uint32_t ch : claimed_channels) {
        grant.adc1_channel_mask |= (1U << ch);
        adc1_owners_[ch] = pod_id;
        allocated_adc1_mask_ |= (1U << ch);
        out_claimed.push_back(ch);
    }
    return true;
}

bool CapabilityFirewall::register_pod(const std::string& pod_id, const std::string& pod_manifest_json, std::string& out_error) {
    std::vector<BusAssignment> buses;
    std::vector<RmtAssignment> rmt;
    begin_admission();
    bool ok;
    {
        osal::LockGuard guard(mutex_);
        ok = register_pod_locked(pod_id, pod_manifest_json, buses, rmt, out_error);
    }
    end_admission();
    return ok && install_drivers(pod_id, buses, rmt, out_error);
}

void CapabilityFirewall::begin_admission() {
    {
        osal::LockGuard guard(mutex_);
        ++admissions_;
    }
    // A lent bus comes back within milliseconds (one scan of the bus);
    // no new lend starts while admissions_ is non-zero
    for (int i = 0; i < 200; ++i) {
        {
            osal::LockGuard guard(mutex_);
            if (lent_i2c_mask_ == 0) return;
        }
        osal::Thread::sleep_ms(5);
    }
}

void CapabilityFirewall::end_admission() {
    osal::LockGuard guard(mutex_);
    if (admissions_ > 0) --admissions_;
}

bool CapabilityFirewall::lend_i2c_bus(uint8_t bus, BusPins& out_pins) {
    osal::LockGuard guard(mutex_);
    if (bus >= 2 || admissions_ > 0 || (lent_i2c_mask_ & (1U << bus)) != 0) return false;
    if ((profile_.i2c_bus_mask & (1U << bus)) == 0 || !i2c_owners_[bus].empty()) return false;
    const BusPins pins = profile_.i2c_pins[bus];
    if (pins.a < 0 || pins.b < 0) return false;
    const uint64_t claimable = claimable_gpio_mask_locked();
    for (int8_t pin : {pins.a, pins.b}) {
        if ((claimable & (1ULL << pin)) == 0 || !gpio_owners_[pin].empty()) return false;
    }
    lent_i2c_mask_ |= static_cast<uint8_t>(1U << bus);
    out_pins = pins;
    return true;
}

void CapabilityFirewall::return_i2c_bus(uint8_t bus) {
    osal::LockGuard guard(mutex_);
    if (bus < 2) lent_i2c_mask_ &= static_cast<uint8_t>(~(1U << bus));
}

// Driver installs take milliseconds and touch no firewall state: they run
// without the lock, so host calls of other pods are not held up.
bool CapabilityFirewall::install_drivers(const std::string& pod_id, const std::vector<BusAssignment>& buses, const std::vector<RmtAssignment>& rmt,
                                         std::string& out_error) {
    if ((buses.empty() || bus_install(pod_id, buses, out_error)) && (rmt.empty() || rmt_install(pod_id, rmt, out_error))) return true;
    unregister_pod(pod_id);
    return false;
}

bool CapabilityFirewall::register_pod_locked(const std::string& pod_id, const std::string& pod_manifest_json, std::vector<BusAssignment>& out_buses,
                                             std::vector<RmtAssignment>& out_rmt, std::string& out_error) {
    if (pod_records_.find(pod_id) != pod_records_.end()) {
        out_error = "Pod already registered in capability firewall: " + pod_id;
        return false;
    }

    models::JsonValue doc;
    if (!models::parse_json(pod_manifest_json, doc, out_error) || !doc.is_object()) {
        out_error = "Failed to parse pod manifest: " + out_error;
        return false;
    }

    DeviceGrant grant;
    std::string sd_claim;
    bool sd_requested = false;
    bool card_volume = false;
    uint32_t generic_gpio_count = 0;
    uint32_t generic_i2c_count = 0;
    uint32_t generic_uart_count = 0;
    uint32_t generic_adc_count = 0;
    std::map<std::string, std::string> bus_annotations;   // kube32.io/i2c-bus-N, kube32.io/uart-N, kube32.io/rmt-tx, kube32.io/rmt-rx

    std::vector<uint32_t> claimed_gpios;
    std::vector<uint32_t> claimed_adcs;
    std::vector<uint32_t> claimed_i2cs;
    std::vector<uint32_t> claimed_uarts;

    auto cleanup_claimed = [&]() {
        for (uint32_t pin : claimed_gpios) {
            gpio_owners_[pin].clear();
            allocated_gpio_mask_ &= ~(1ULL << pin);
        }
        for (uint32_t ch : claimed_adcs) {
            adc1_owners_[ch].clear();
            allocated_adc1_mask_ &= ~(1U << ch);
        }
        for (uint32_t bus : claimed_i2cs) {
            i2c_owners_[bus].clear();
            allocated_i2c_mask_ &= ~(1U << bus);
        }
        for (uint32_t port : claimed_uarts) {
            uart_owners_[port].clear();
            allocated_uart_mask_ &= ~(1U << port);
        }
    };

    const models::JsonValue* meta = doc.get("metadata");
    if (meta && meta->is_object()) {
        const models::JsonValue* annotations = meta->get("annotations");
        if (annotations && annotations->is_object()) {
            std::string ipc_ann = annotations->get_string("kube32.io/ipc", "");
            if (ipc_ann == "true" || ipc_ann == "1") {
                grant.capabilities_mask |= KUBE32_CAP_IPC;
            }
            for (const auto& kv : annotations->obj_val) {
                if ((kv.first.rfind("kube32.io/i2c-bus-", 0) == 0 || kv.first.rfind("kube32.io/uart-", 0) == 0 || kv.first == "kube32.io/rmt-tx" ||
                     kv.first == "kube32.io/rmt-rx") &&
                    kv.second.is_string()) {
                    bus_annotations[kv.first] = kv.second.str_val;
                }
            }
        }
    }

    const models::JsonValue* spec = doc.get("spec");
    if (spec && spec->is_object()) {
        const models::JsonValue* containers = spec->get("containers");
        if (containers && containers->is_array()) {
            for (const auto& c : containers->arr_val) {
                if (!c.is_object()) continue;
                const models::JsonValue* res = c.get("resources");
                if (!res || !res->is_object()) continue;
                const models::JsonValue* req = res->get("requests");
                if (!req || !req->is_object()) req = res->get("limits");
                if (!req || !req->is_object()) continue;

                for (const auto& kv : req->obj_val) {
                    const std::string& key = kv.first;
                    std::string val_str = kv.second.is_string() ? kv.second.str_val : std::to_string(static_cast<long long>(kv.second.num_val));

                    // Specific GPIO Request: "esp32.io/gpio-2": "1" or "esp.io/gpio-2": "1"
                    if (key.rfind("esp32.io/gpio-", 0) == 0 || key.rfind("esp.io/gpio-", 0) == 0) {
                        size_t prefix_len = (key.rfind("esp32.io/gpio-", 0) == 0) ? 14 : 12;
                        uint32_t pin = 0;
                        try {
                            pin = std::stoul(key.substr(prefix_len));
                        } catch (...) {
                            cleanup_claimed();
                            out_error = "Invalid GPIO pin key format: " + key;
                            return false;
                        }
                        if (pin >= 64 || (KUBE32_SAFE_GPIO_MASK & (1ULL << pin)) == 0) {
                            cleanup_claimed();
                            out_error = "Security Violation: Pin GPIO " + std::to_string(pin) + " is reserved or unsafe.";
                            return false;
                        }
                        if ((profile_.reserved_gpio_mask & (1ULL << pin)) != 0) {
                            cleanup_claimed();
                            out_error = "Security Violation: Pin GPIO " + std::to_string(pin) + " is used by the " + profile_.name + " board.";
                            return false;
                        }
                        if (!gpio_owners_[pin].empty() && gpio_owners_[pin] != pod_id) {
                            cleanup_claimed();
                            out_error = "Hardware Collision: GPIO " + std::to_string(pin) + " already claimed by " + gpio_owners_[pin];
                            return false;
                        }
                        grant.gpio_read_mask |= (1ULL << pin);
                        grant.gpio_write_mask |= (1ULL << pin);
                        gpio_owners_[pin] = pod_id;
                        allocated_gpio_mask_ |= (1ULL << pin);
                        claimed_gpios.push_back(pin);
                    }
                    // Generic GPIO Request: "esp.io/gpio": "2" or "esp32.io/gpio": "2"
                    else if (key == "esp.io/gpio" || key == "esp32.io/gpio") {
                        try {
                            generic_gpio_count += std::stoul(val_str);
                        } catch (...) {}
                    }
                    // Specific ADC1 Channel: "esp32.io/adc1-channel-0": "1" or "esp.io/adc1-channel-0": "1"
                    else if (key.rfind("esp32.io/adc1-channel-", 0) == 0 || key.rfind("esp.io/adc1-channel-", 0) == 0) {
                        size_t prefix_len = (key.rfind("esp32.io/adc1-channel-", 0) == 0) ? 22 : 20;
                        uint32_t ch = 0;
                        try {
                            ch = std::stoul(key.substr(prefix_len));
                        } catch (...) {
                            cleanup_claimed();
                            out_error = "Invalid ADC1 channel key format: " + key;
                            return false;
                        }
                        if (ch >= KUBE32_ADC1_CHANNELS) {
                            cleanup_claimed();
                            out_error = "Invalid ADC1 channel: " + std::to_string(ch) + " (Max 9)";
                            return false;
                        }
                        if ((claimable_adc1_mask_locked() & (1U << ch)) == 0) {
                            cleanup_claimed();
                            out_error = "Security Violation: ADC1 channel " + std::to_string(ch) + " (GPIO " + std::to_string(ch + 1) +
                                        ") is used by the " + profile_.name + " board.";
                            return false;
                        }
                        if (!adc1_owners_[ch].empty() && adc1_owners_[ch] != pod_id) {
                            cleanup_claimed();
                            out_error = "Hardware Collision: ADC1 channel " + std::to_string(ch) + " claimed by " + adc1_owners_[ch];
                            return false;
                        }
                        grant.adc1_channel_mask |= (1U << ch);
                        adc1_owners_[ch] = pod_id;
                        allocated_adc1_mask_ |= (1U << ch);
                        claimed_adcs.push_back(ch);
                    }
                    // Generic ADC: "esp.io/adc": "1" or "esp32.io/adc": "1"
                    else if (key == "esp.io/adc" || key == "esp32.io/adc") {
                        try {
                            generic_adc_count += std::stoul(val_str);
                        } catch (...) {}
                    }
                    // Specific I2C Bus: "esp32.io/i2c-bus-0": "1" or "esp.io/i2c-bus-0": "1"
                    else if (key.rfind("esp32.io/i2c-bus-", 0) == 0 || key.rfind("esp.io/i2c-bus-", 0) == 0) {
                        size_t prefix_len = (key.rfind("esp32.io/i2c-bus-", 0) == 0) ? 17 : 15;
                        uint32_t bus = 0;
                        try {
                            bus = std::stoul(key.substr(prefix_len));
                        } catch (...) {
                            cleanup_claimed();
                            out_error = "Invalid I2C bus key format: " + key;
                            return false;
                        }
                        if (bus >= 2) {
                            cleanup_claimed();
                            out_error = "Invalid I2C bus: " + std::to_string(bus) + " (ESP32-S3 has buses 0 and 1)";
                            return false;
                        }
                        if ((profile_.i2c_bus_mask & (1U << bus)) == 0) {
                            cleanup_claimed();
                            out_error = "I2C bus " + std::to_string(bus) + " is not offered by the " + profile_.name + " board";
                            return false;
                        }
                        if (!i2c_owners_[bus].empty() && i2c_owners_[bus] != pod_id) {
                            cleanup_claimed();
                            out_error = "Hardware Collision: I2C bus " + std::to_string(bus) + " claimed by " + i2c_owners_[bus];
                            return false;
                        }
                        grant.i2c_bus_mask |= (1U << bus);
                        i2c_owners_[bus] = pod_id;
                        allocated_i2c_mask_ |= (1U << bus);
                        claimed_i2cs.push_back(bus);
                    }
                    // Generic I2C: "esp.io/i2c": "1" or "esp32.io/i2c": "1"
                    else if (key == "esp.io/i2c" || key == "esp32.io/i2c") {
                        try {
                            generic_i2c_count += std::stoul(val_str);
                        } catch (...) {}
                    }
                    // Generic UART: "esp.io/uart": "1" or "esp32.io/uart": "1"
                    else if (key == "esp.io/uart" || key == "esp32.io/uart") {
                        try {
                            generic_uart_count += std::stoul(val_str);
                        } catch (...) {}
                    }
                    // Specific UART Port: "esp32.io/uart-1": "1" or "esp.io/uart-1": "1"
                    else if (key.rfind("esp32.io/uart-", 0) == 0 || key.rfind("esp.io/uart-", 0) == 0) {
                        size_t prefix_len = (key.rfind("esp32.io/uart-", 0) == 0) ? 14 : 12;
                        uint32_t port = 0;
                        try {
                            port = std::stoul(key.substr(prefix_len));
                        } catch (...) {
                            cleanup_claimed();
                            out_error = "Invalid UART port key format: " + key;
                            return false;
                        }
                        if (port == 0 || port > 2) {
                            cleanup_claimed();
                            out_error = "Invalid UART port: " + std::to_string(port) + " (UART0 is console; allowed 1, 2)";
                            return false;
                        }
                        if ((profile_.uart_port_mask & (1U << port)) == 0) {
                            cleanup_claimed();
                            out_error = "UART" + std::to_string(port) + " is not offered by the " + profile_.name + " board";
                            return false;
                        }
                        if (!uart_owners_[port].empty() && uart_owners_[port] != pod_id) {
                            cleanup_claimed();
                            out_error = "Hardware Collision: UART " + std::to_string(port) + " claimed by " + uart_owners_[port];
                            return false;
                        }
                        grant.uart_port_mask |= (1U << port);
                        uart_owners_[port] = pod_id;
                        allocated_uart_mask_ |= (1U << port);
                        claimed_uarts.push_back(port);
                    }
                    // RMT (IR) channel: "esp32.io/rmt-tx": "1" or "esp32.io/rmt-rx": "1", one of each per pod
                    else if (key == "esp32.io/rmt-tx" || key == "esp.io/rmt-tx" || key == "esp32.io/rmt-rx" || key == "esp.io/rmt-rx") {
                        if (val_str == "0") continue;
                        if (val_str != "1") {
                            cleanup_claimed();
                            out_error = "Invalid " + key + ": \"" + val_str + "\" (a pod claims one channel: \"1\")";
                            return false;
                        }
                        grant.capabilities_mask |= (key.find("rmt-tx") != std::string::npos) ? KUBE32_CAP_RMT_TX : KUBE32_CAP_RMT_RX;
                    }
                    // Virtual Channel IPC: "kube32.io/ipc" / "hardware.kube32.io/ipc": "1" or "true"
                    else if (key == "kube32.io/ipc" || key == "hardware.kube32.io/ipc") {
                        if (val_str == "1" || val_str == "true") {
                            grant.capabilities_mask |= KUBE32_CAP_IPC;
                        }
                    }
                    // On-board peripherals exposed as extended resources
                    else if (key == "hardware.kube32.io/tft-display") {
                        if (val_str != "0") grant.capabilities_mask |= KUBE32_CAP_DISPLAY;
                    }
                    else if (key == "hardware.kube32.io/keyboard") {
                        if (val_str != "0") grant.capabilities_mask |= KUBE32_CAP_KEYBOARD;
                    }
                    else if (key == "hardware.kube32.io/speaker") {
                        if (val_str != "0") grant.capabilities_mask |= KUBE32_CAP_SPEAKER;
                    }
                    // microSD volume: "hardware.kube32.io/sd": "1" plus a spec.volumes claim
                    else if (key == "hardware.kube32.io/sd") {
                        if (val_str != "0") sd_requested = true;
                    }
                }
            }

            // securityContext.capabilities.add: [CHANNEL_IPC, DISPLAY, KEYBOARD, SPEAKER, SD, SERVICE_CALL]
            for (const auto& c : containers->arr_val) {
                if (!c.is_object()) continue;
                const models::JsonValue* sc = c.get("securityContext");
                if (!sc || !sc->is_object()) continue;
                const models::JsonValue* caps = sc->get("capabilities");
                if (!caps || !caps->is_object()) continue;
                const models::JsonValue* add = caps->get("add");
                if (!add || !add->is_array()) continue;
                for (const auto& cap : add->arr_val) {
                    if (!cap.is_string()) continue;
                    const std::string& name = cap.str_val;
                    if (name == "CHANNEL_IPC" || name == "IPC") grant.capabilities_mask |= KUBE32_CAP_IPC;
                    else if (name == "DISPLAY" || name == "TFT_DISPLAY") grant.capabilities_mask |= KUBE32_CAP_DISPLAY;
                    else if (name == "KEYBOARD") grant.capabilities_mask |= KUBE32_CAP_KEYBOARD;
                    else if (name == "SPEAKER" || name == "AUDIO") grant.capabilities_mask |= KUBE32_CAP_SPEAKER;
                    else if (name == "SD" || name == "SD_CARD") sd_requested = true;
                    else if (name == "SERVICE_CALL") grant.capabilities_mask |= KUBE32_CAP_SERVICE_CALL;
                }
            }
        }

        // volumes: [{name: data, "kube32.io/sd": {claim: notes}}], or the
        // standard shape kubectl validates: [{name: data, persistentVolumeClaim: {claimName: notes}}]
        const models::JsonValue* volumes = spec->get("volumes");
        if (volumes && volumes->is_array()) {
            for (const auto& v : volumes->arr_val) {
                if (!v.is_object()) continue;
                const models::JsonValue* sd = v.get("kube32.io/sd");
                const models::JsonValue* pvc = v.get("persistentVolumeClaim");
                std::string claim;
                if (sd && sd->is_object()) { claim = sd->get_string("claim", ""); card_volume = true; }
                else if (pvc && pvc->is_object()) claim = pvc->get_string("claimName", "");
                else continue;
                if (claim.empty()) claim = v.get_string("name", "");
                if (!valid_sd_claim(claim)) {
                    cleanup_claimed();
                    out_error = "Invalid kube32.io/sd volume claim '" + claim + "' (1..32 chars of a-z, 0-9, '-')";
                    return false;
                }
                if (!sd_claim.empty() && sd_claim != claim) {
                    cleanup_claimed();
                    out_error = "A pod may mount one kube32.io/sd volume (claims '" + sd_claim + "' and '" + claim + "')";
                    return false;
                }
                sd_claim = claim;
            }
        }
    }

    if (sd_requested && sd_claim.empty()) {
        cleanup_claimed();
        out_error = "hardware.kube32.io/sd requested but spec.volumes names no kube32.io/sd claim";
        return false;
    }
    // A volume lives on the node's storage root; kube32.io/sd volumes and
    // hardware.kube32.io/sd requests insist on the card
    if (!sd_claim.empty()) grant.capabilities_mask |= KUBE32_CAP_SD;

    // Buses first, then their pins, then pins handed out by count: a bus's
    // default pins must not go to the same pod as plain GPIOs.
    if (generic_i2c_count > 0 && !allocate_generic_i2c(generic_i2c_count, grant, pod_id, claimed_i2cs, out_error)) {
        cleanup_claimed();
        return false;
    }
    if (generic_uart_count > 0 && !allocate_generic_uart(generic_uart_count, grant, pod_id, claimed_uarts, out_error)) {
        cleanup_claimed();
        return false;
    }
    std::vector<BusAssignment> buses;
    if (!claim_bus_pins(pod_id, grant, bus_annotations, buses, claimed_gpios, out_error)) {
        cleanup_claimed();
        return false;
    }
    std::vector<RmtAssignment> rmt;
    if (!claim_rmt_pins(pod_id, grant, bus_annotations, rmt, claimed_gpios, out_error)) {
        cleanup_claimed();
        return false;
    }
    if (generic_gpio_count > 0 && !allocate_generic_gpios(generic_gpio_count, grant, pod_id, claimed_gpios, out_error)) {
        cleanup_claimed();
        return false;
    }
    if (generic_adc_count > 0 && !allocate_generic_adc(generic_adc_count, grant, pod_id, claimed_adcs, out_error)) {
        cleanup_claimed();
        return false;
    }

    PodSecurityRecord record;
    record.pod_id = pod_id;
    record.grant = grant;
    record.action = SecurityAction::LogAndDeny;
    record.violation_count = 0;
    record.sd_claim = sd_claim;
    record.card_required = !sd_claim.empty() && (sd_requested || card_volume);
    record.buses = buses;
    record.rmt = rmt;
    pod_records_[pod_id] = record;
    out_buses = std::move(buses);
    out_rmt = std::move(rmt);

    return true;
}

bool CapabilityFirewall::valid_sd_claim(const std::string& claim) {
    if (claim.empty() || claim.size() > 32 || claim[0] == '-') return false;
    for (char c : claim) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

bool CapabilityFirewall::volume_needs_card(const std::string& pod_id) const {
    osal::LockGuard guard(mutex_);
    auto it = pod_records_.find(pod_id);
    return it != pod_records_.end() && it->second.card_required;
}

std::string CapabilityFirewall::sd_claim(const std::string& pod_id) const {
    osal::LockGuard guard(mutex_);
    auto it = pod_records_.find(pod_id);
    if (it == pod_records_.end() || !it->second.grant.has_capability(KUBE32_CAP_SD)) return std::string();
    return it->second.sd_claim;
}

bool CapabilityFirewall::grant_capabilities(const std::string& pod_id, const DeviceGrant& grant, std::string& out_error) {
    std::vector<BusAssignment> buses;
    std::vector<RmtAssignment> rmt;
    begin_admission();
    bool ok;
    {
        osal::LockGuard guard(mutex_);
        ok = grant_capabilities_locked(pod_id, grant, buses, rmt, out_error);
    }
    end_admission();
    return ok && install_drivers(pod_id, buses, rmt, out_error);
}

bool CapabilityFirewall::grant_capabilities_locked(const std::string& pod_id, const DeviceGrant& grant, std::vector<BusAssignment>& out_buses,
                                                   std::vector<RmtAssignment>& out_rmt, std::string& out_error) {
    if (pod_records_.find(pod_id) != pod_records_.end()) {
        out_error = "Pod already registered in capability firewall: " + pod_id;
        return false;
    }

    // Validate GPIO mask against safety mask and collisions
    uint64_t combined_gpio = grant.gpio_read_mask | grant.gpio_write_mask;
    if ((combined_gpio & ~claimable_gpio_mask_locked()) != 0) {
        out_error = "Security Violation: Requested GPIOs contain reserved pins";
        return false;
    }

    std::vector<uint32_t> claimed_gpios;
    for (uint32_t pin = 0; pin < 64; ++pin) {
        if ((combined_gpio & (1ULL << pin)) != 0) {
            if (!gpio_owners_[pin].empty() && gpio_owners_[pin] != pod_id) {
                for (uint32_t p : claimed_gpios) {
                    gpio_owners_[p].clear();
                    allocated_gpio_mask_ &= ~(1ULL << p);
                }
                out_error = "Hardware Collision: GPIO " + std::to_string(pin) + " already claimed by " + gpio_owners_[pin];
                return false;
            }
            gpio_owners_[pin] = pod_id;
            allocated_gpio_mask_ |= (1ULL << pin);
            claimed_gpios.push_back(pin);
        }
    }

    std::vector<uint32_t> claimed_adcs;
    for (uint32_t ch = 0; ch < 16; ++ch) {
        if ((grant.adc1_channel_mask & (1U << ch)) != 0) {
            if (ch >= KUBE32_ADC1_CHANNELS || (claimable_adc1_mask_locked() & (1U << ch)) == 0 ||
                (!adc1_owners_[ch].empty() && adc1_owners_[ch] != pod_id)) {
                for (uint32_t p : claimed_gpios) {
                    gpio_owners_[p].clear();
                    allocated_gpio_mask_ &= ~(1ULL << p);
                }
                for (uint32_t c : claimed_adcs) {
                    adc1_owners_[c].clear();
                    allocated_adc1_mask_ &= ~(1U << c);
                }
                out_error = "Hardware Collision: ADC1 channel " + std::to_string(ch);
                return false;
            }
            adc1_owners_[ch] = pod_id;
            allocated_adc1_mask_ |= (1U << ch);
            claimed_adcs.push_back(ch);
        }
    }

    std::vector<uint32_t> claimed_i2cs;
    for (uint32_t b = 0; b < 8; ++b) {
        if ((grant.i2c_bus_mask & (1U << b)) != 0) {
            if (b >= 2 || (profile_.i2c_bus_mask & (1U << b)) == 0 || (!i2c_owners_[b].empty() && i2c_owners_[b] != pod_id)) {
                for (uint32_t p : claimed_gpios) {
                    gpio_owners_[p].clear();
                    allocated_gpio_mask_ &= ~(1ULL << p);
                }
                for (uint32_t c : claimed_adcs) {
                    adc1_owners_[c].clear();
                    allocated_adc1_mask_ &= ~(1U << c);
                }
                for (uint32_t bus : claimed_i2cs) {
                    i2c_owners_[bus].clear();
                    allocated_i2c_mask_ &= ~(1U << bus);
                }
                out_error = "Hardware Collision: I2C bus " + std::to_string(b);
                return false;
            }
            i2c_owners_[b] = pod_id;
            allocated_i2c_mask_ |= (1U << b);
            claimed_i2cs.push_back(b);
        }
    }

    std::vector<uint32_t> claimed_uarts;
    for (uint32_t u = 0; u < 8; ++u) {
        if ((grant.uart_port_mask & (1U << u)) != 0) {
            if (u == 0 || u > 2 || (profile_.uart_port_mask & (1U << u)) == 0 || (!uart_owners_[u].empty() && uart_owners_[u] != pod_id)) {
                for (uint32_t p : claimed_gpios) {
                    gpio_owners_[p].clear();
                    allocated_gpio_mask_ &= ~(1ULL << p);
                }
                for (uint32_t c : claimed_adcs) {
                    adc1_owners_[c].clear();
                    allocated_adc1_mask_ &= ~(1U << c);
                }
                for (uint32_t bus : claimed_i2cs) {
                    i2c_owners_[bus].clear();
                    allocated_i2c_mask_ &= ~(1U << bus);
                }
                for (uint32_t port : claimed_uarts) {
                    uart_owners_[port].clear();
                    allocated_uart_mask_ &= ~(1U << port);
                }
                out_error = "Hardware Collision: UART port " + std::to_string(u);
                return false;
            }
            uart_owners_[u] = pod_id;
            allocated_uart_mask_ |= (1U << u);
            claimed_uarts.push_back(u);
        }
    }

    // Granted buses and RMT channels run on the board's default pins
    std::vector<BusAssignment> buses;
    std::vector<RmtAssignment> rmt;
    std::vector<uint32_t> bus_pins;
    if (!claim_bus_pins(pod_id, grant, {}, buses, bus_pins, out_error) || !claim_rmt_pins(pod_id, grant, {}, rmt, bus_pins, out_error)) {
        for (uint32_t p : claimed_gpios) { gpio_owners_[p].clear(); allocated_gpio_mask_ &= ~(1ULL << p); }
        for (uint32_t p : bus_pins) { gpio_owners_[p].clear(); allocated_gpio_mask_ &= ~(1ULL << p); }
        for (uint32_t c : claimed_adcs) { adc1_owners_[c].clear(); allocated_adc1_mask_ &= ~(1U << c); }
        for (uint32_t b : claimed_i2cs) { i2c_owners_[b].clear(); allocated_i2c_mask_ &= ~(1U << b); }
        for (uint32_t u : claimed_uarts) { uart_owners_[u].clear(); allocated_uart_mask_ &= ~(1U << u); }
        return false;
    }

    PodSecurityRecord record;
    record.pod_id = pod_id;
    record.grant = grant;
    record.action = SecurityAction::LogAndDeny;
    record.violation_count = 0;
    record.buses = buses;
    record.rmt = rmt;
    pod_records_[pod_id] = record;
    out_buses = std::move(buses);
    out_rmt = std::move(rmt);
    return true;
}

// "sda=8,scl=9" or "pin=1,active=low" -> {name: value}, names in lower case;
// false when an item is not <name>=<value>.
static bool parse_settings(const std::string& spec, std::map<std::string, std::string>& out) {
    auto trim = [](std::string v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        return v;
    };
    size_t start = 0;
    while (start <= spec.size()) {
        size_t end = spec.find(',', start);
        if (end == std::string::npos) end = spec.size();
        std::string item = trim(spec.substr(start, end - start));
        start = end + 1;
        if (item.empty()) continue;
        size_t eq = item.find('=');
        if (eq == std::string::npos) return false;
        std::string key = trim(item.substr(0, eq));
        std::string val = trim(item.substr(eq + 1));
        if (key.empty() || val.empty()) return false;
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out[key] = val;
    }
    return true;
}

// A decimal number of at most 9 digits
static bool parse_setting_uint(const std::string& val, uint32_t& out) {
    if (val.empty() || val.size() > 9) return false;
    for (char c : val) if (c < '0' || c > '9') return false;
    out = static_cast<uint32_t>(std::stoul(val));
    return true;
}

// "sda=8,scl=9,hz=400000" -> {sda: 8, scl: 9, hz: 400000}; false when an item
// is not <name>=<decimal number>.
static bool parse_bus_spec(const std::string& spec, std::map<std::string, uint32_t>& out) {
    std::map<std::string, std::string> items;
    if (!parse_settings(spec, items)) return false;
    for (const auto& kv : items) {
        if (!parse_setting_uint(kv.second, out[kv.first])) return false;
    }
    return true;
}

bool CapabilityFirewall::claim_pin(const std::string& pod_id, uint32_t pin, const std::string& what, uint64_t also_allowed,
                                   std::vector<uint32_t>& out_claimed_pins, std::string& out_error) {
    const bool offered = pin < 64 && (also_allowed & (1ULL << pin)) != 0;
    if (!offered && (pin >= 64 || (KUBE32_SAFE_GPIO_MASK & (1ULL << pin)) == 0)) {
        out_error = "Security Violation: Pin GPIO " + std::to_string(pin) + " (" + what + ") is reserved or unsafe.";
        return false;
    }
    if (!offered && (profile_.reserved_gpio_mask & (1ULL << pin)) != 0) {
        out_error = "Security Violation: Pin GPIO " + std::to_string(pin) + " (" + what + ") is used by the " + profile_.name + " board.";
        return false;
    }
    if (!gpio_owners_[pin].empty()) {
        out_error = gpio_owners_[pin] == pod_id ? "GPIO " + std::to_string(pin) + " (" + what + ") is claimed twice by the pod"
                                                : "Hardware Collision: GPIO " + std::to_string(pin) + " (" + what + ") already claimed by " + gpio_owners_[pin];
        return false;
    }
    gpio_owners_[pin] = pod_id;
    allocated_gpio_mask_ |= (1ULL << pin);
    out_claimed_pins.push_back(pin);
    return true;
}

bool CapabilityFirewall::claim_bus_pins(const std::string& pod_id, const DeviceGrant& grant, const std::map<std::string, std::string>& annotations,
                                        std::vector<BusAssignment>& out_buses, std::vector<uint32_t>& out_claimed_pins, std::string& out_error) {
    struct Kind {
        BusKind kind;
        uint8_t mask;
        uint32_t first, last;
        const char* name;          // "I2C", "UART"
        const char* annotation;    // "kube32.io/i2c-bus-", "kube32.io/uart-"
        const char* pin_a;         // "sda", "tx"
        const char* pin_b;         // "scl", "rx"
        const char* speed;         // "hz", "baud"
        uint32_t speed_default, speed_min, speed_max;
        const BusPins* defaults;
    };
    const Kind kinds[] = {
        {BusKind::I2c, grant.i2c_bus_mask, 0, 1, "I2C", "kube32.io/i2c-bus-", "sda", "scl", "hz", kDefaultI2cHz, 1000, 1000000, profile_.i2c_pins},
        {BusKind::Uart, grant.uart_port_mask, 1, 2, "UART", "kube32.io/uart-", "tx", "rx", "baud", kDefaultUartBaud, 300, 5000000, profile_.uart_pins},
    };
    for (const Kind& k : kinds) {
        for (uint32_t n = k.first; n <= k.last; ++n) {
            if ((k.mask & (1U << n)) == 0) continue;
            const std::string label = std::string(k.name) + std::to_string(n);
            const std::string key = k.annotation + std::to_string(n);
            BusAssignment b;
            b.kind = k.kind;
            b.index = static_cast<uint8_t>(n);
            b.speed = k.speed_default;
            int32_t pa = k.defaults[n].a, pb = k.defaults[n].b;
            auto it = annotations.find(key);
            if (it != annotations.end()) {
                std::map<std::string, uint32_t> v;
                if (!parse_bus_spec(it->second, v) || !v.count(k.pin_a) || !v.count(k.pin_b)) {
                    out_error = "Invalid " + key + " \"" + it->second + "\" (want \"" + k.pin_a + "=<pin>," + k.pin_b + "=<pin>[," + k.speed + "=<n>]\")";
                    return false;
                }
                for (const auto& kv : v) {
                    if (kv.first != k.pin_a && kv.first != k.pin_b && kv.first != k.speed) {
                        out_error = "Invalid " + key + ": unknown setting \"" + kv.first + "\"";
                        return false;
                    }
                }
                pa = static_cast<int32_t>(v[k.pin_a]);
                pb = static_cast<int32_t>(v[k.pin_b]);
                if (v.count(k.speed)) b.speed = v[k.speed];
                if (b.speed < k.speed_min || b.speed > k.speed_max) {
                    out_error = "Invalid " + key + ": " + k.speed + " must be " + std::to_string(k.speed_min) + ".." + std::to_string(k.speed_max);
                    return false;
                }
            } else if (pa < 0 || pb < 0) {
                out_error = label + " has no default pins on the " + profile_.name + " board: set annotation " + key + ": \"" + k.pin_a + "=<pin>," +
                            k.pin_b + "=<pin>\"";
                return false;
            }
            if (pa == pb) {
                out_error = "Invalid " + key + ": " + k.pin_a + " and " + k.pin_b + " are the same pin";
                return false;
            }
            if (!claim_pin(pod_id, static_cast<uint32_t>(pa), label + " " + k.pin_a, 0, out_claimed_pins, out_error) ||
                !claim_pin(pod_id, static_cast<uint32_t>(pb), label + " " + k.pin_b, 0, out_claimed_pins, out_error)) {
                return false;
            }
            b.pin_a = static_cast<uint8_t>(pa);
            b.pin_b = static_cast<uint8_t>(pb);
            out_buses.push_back(b);
        }
    }
    return true;
}

bool CapabilityFirewall::claim_rmt_pins(const std::string& pod_id, const DeviceGrant& grant, const std::map<std::string, std::string>& annotations,
                                        std::vector<RmtAssignment>& out_rmt, std::vector<uint32_t>& out_claimed_pins, std::string& out_error) {
    for (RmtDir dir : {RmtDir::Tx, RmtDir::Rx}) {
        const bool tx = dir == RmtDir::Tx;
        if (!grant.has_capability(tx ? KUBE32_CAP_RMT_TX : KUBE32_CAP_RMT_RX)) continue;
        const std::string label = tx ? "RMT TX" : "RMT RX";
        const std::string key = tx ? "kube32.io/rmt-tx" : "kube32.io/rmt-rx";
        const char* want = tx ? "pin=<n>[,carrier=<Hz>][,duty=<percent>]" : "pin=<n>[,active=low|high][,idle_us=<n>]";
        if (allocated_rmt_count_locked(dir) >= claimable_rmt_count_locked(dir)) {
            out_error = "Insufficient allocatable " + label + " channels: requested 1, available 0";
            return false;
        }
        RmtAssignment a;
        a.dir = dir;
        a.carrier_hz = kRmtDefaultCarrierHz;
        a.duty_pct = kRmtDefaultDutyPct;
        a.active_low = true;
        a.idle_us = kRmtDefaultIdleUs;
        int32_t pin = tx ? profile_.rmt_tx_default_pin : profile_.rmt_rx_default_pin;
        auto it = annotations.find(key);
        if (it != annotations.end()) {
            std::map<std::string, std::string> v;
            uint32_t n = 0;
            if (!parse_settings(it->second, v) || !v.count("pin") || !parse_setting_uint(v["pin"], n)) {
                out_error = "Invalid " + key + " \"" + it->second + "\" (want \"" + want + "\")";
                return false;
            }
            pin = static_cast<int32_t>(n < 64 ? n : 64);
            for (const auto& kv : v) {
                const std::string& name = kv.first;
                if (name == "pin") continue;
                if (tx && name == "carrier") {
                    if (!parse_setting_uint(kv.second, n) || (n != 0 && (n < kRmtMinCarrierHz || n > kRmtMaxCarrierHz))) {
                        out_error = "Invalid " + key + ": carrier must be 0 (off) or " + std::to_string(kRmtMinCarrierHz) + ".." +
                                    std::to_string(kRmtMaxCarrierHz) + " Hz";
                        return false;
                    }
                    a.carrier_hz = n;
                } else if (tx && name == "duty") {
                    if (!parse_setting_uint(kv.second, n) || n < 1 || n > 99) {
                        out_error = "Invalid " + key + ": duty must be 1..99 percent";
                        return false;
                    }
                    a.duty_pct = static_cast<uint8_t>(n);
                } else if (!tx && name == "active") {
                    if (kv.second != "low" && kv.second != "high") {
                        out_error = "Invalid " + key + ": active must be low or high";
                        return false;
                    }
                    a.active_low = kv.second == "low";
                } else if (!tx && name == "idle_us") {
                    if (!parse_setting_uint(kv.second, n) || n < kRmtMinIdleUs || n > kRmtMaxIdleUs) {
                        out_error = "Invalid " + key + ": idle_us must be " + std::to_string(kRmtMinIdleUs) + ".." + std::to_string(kRmtMaxIdleUs);
                        return false;
                    }
                    a.idle_us = n;
                } else {
                    out_error = "Invalid " + key + ": unknown setting \"" + name + "\"";
                    return false;
                }
            }
        } else if (pin < 0) {
            out_error = label + " has no default pin on the " + profile_.name + " board: set annotation " + key + ": \"pin=<n>\"";
            return false;
        }
        if (!claim_pin(pod_id, static_cast<uint32_t>(pin), label, tx ? profile_.rmt_tx_pin_mask : 0, out_claimed_pins, out_error)) return false;
        a.pin = static_cast<uint8_t>(pin);
        out_rmt.push_back(a);
    }
    return true;
}

bool CapabilityFirewall::allocate_generic_uart(uint32_t count, DeviceGrant& grant, const std::string& pod_id, std::vector<uint32_t>& out_claimed, std::string& out_error) {
    std::vector<uint32_t> ports;
    for (uint32_t port = 1; port <= 2 && ports.size() < count; ++port) {
        if ((profile_.uart_port_mask & (1U << port)) == 0 || !uart_owners_[port].empty()) continue;
        ports.push_back(port);
    }
    if (ports.size() < count) {
        out_error = "Insufficient allocatable UART ports: requested " + std::to_string(count) + ", available " + std::to_string(ports.size());
        return false;
    }
    for (uint32_t port : ports) {
        grant.uart_port_mask |= (1U << port);
        uart_owners_[port] = pod_id;
        allocated_uart_mask_ |= (1U << port);
        out_claimed.push_back(port);
    }
    return true;
}

uint8_t CapabilityFirewall::claimable_uart_mask() const {
    osal::LockGuard guard(mutex_);
    return profile_.uart_port_mask;
}

std::vector<BusAssignment> CapabilityFirewall::pod_buses(const std::string& pod_id) const {
    osal::LockGuard guard(mutex_);
    auto it = pod_records_.find(pod_id);
    return it == pod_records_.end() ? std::vector<BusAssignment>() : it->second.buses;
}

std::vector<RmtAssignment> CapabilityFirewall::pod_rmt(const std::string& pod_id) const {
    osal::LockGuard guard(mutex_);
    auto it = pod_records_.find(pod_id);
    return it == pod_records_.end() ? std::vector<RmtAssignment>() : it->second.rmt;
}

void CapabilityFirewall::unregister_pod(const std::string& pod_id) {
    // The drivers go before the pins are free for anyone else
    bus_release(pod_id);
    rmt_release(pod_id);
    osal::LockGuard guard(mutex_);
    pod_records_.erase(pod_id);
    for (size_t i = 0; i < gpio_owners_.size(); ++i) {
        if (gpio_owners_[i] == pod_id) {
            gpio_owners_[i].clear();
            allocated_gpio_mask_ &= ~(1ULL << i);
        }
    }
    for (size_t i = 0; i < adc1_owners_.size(); ++i) {
        if (adc1_owners_[i] == pod_id) {
            adc1_owners_[i].clear();
            allocated_adc1_mask_ &= ~(1U << i);
        }
    }
    for (size_t i = 0; i < i2c_owners_.size(); ++i) {
        if (i2c_owners_[i] == pod_id) {
            i2c_owners_[i].clear();
            allocated_i2c_mask_ &= ~(1U << i);
        }
    }
    for (size_t i = 0; i < uart_owners_.size(); ++i) {
        if (uart_owners_[i] == pod_id) {
            uart_owners_[i].clear();
            allocated_uart_mask_ &= ~(1U << i);
        }
    }
}

bool CapabilityFirewall::check_permission(const std::string& pod_id, PeripheralType type, uint32_t pin_or_port, AccessMode mode) {
    osal::LockGuard guard(mutex_);
    auto it = pod_records_.find(pod_id);
    if (it == pod_records_.end()) {
        log_violation(pod_id, type, pin_or_port, mode);
        return false;
    }

    const DeviceGrant& grant = it->second.grant;
    bool permitted = false;

    switch (type) {
        case PeripheralType::GPIO:
            permitted = grant.is_gpio_permitted(pin_or_port, mode);
            break;
        case PeripheralType::ADC1:
            permitted = grant.is_adc1_permitted(pin_or_port);
            break;
        case PeripheralType::I2C:
            permitted = grant.is_i2c_permitted(pin_or_port);
            break;
        case PeripheralType::UART:
            permitted = grant.is_uart_permitted(pin_or_port);
            break;
        case PeripheralType::IPC:
            permitted = grant.is_ipc_permitted();
            break;
        case PeripheralType::Display:
            permitted = grant.has_capability(KUBE32_CAP_DISPLAY);
            break;
        case PeripheralType::Keyboard:
            permitted = grant.has_capability(KUBE32_CAP_KEYBOARD);
            break;
        case PeripheralType::Speaker:
            permitted = grant.has_capability(KUBE32_CAP_SPEAKER);
            break;
        case PeripheralType::SdCard:
            permitted = grant.has_capability(KUBE32_CAP_SD);
            break;
        case PeripheralType::ServiceCall:
            permitted = grant.has_capability(KUBE32_CAP_SERVICE_CALL);
            break;
        case PeripheralType::RmtTx:
            permitted = grant.has_capability(KUBE32_CAP_RMT_TX);
            break;
        case PeripheralType::RmtRx:
            permitted = grant.has_capability(KUBE32_CAP_RMT_RX);
            break;
    }

    if (!permitted) {
        it->second.violation_count++;
        log_violation(pod_id, type, pin_or_port, mode);
    }
    return permitted;
}

const PodSecurityRecord* CapabilityFirewall::get_pod_security_record(const std::string& pod_id) const {
    osal::LockGuard guard(mutex_);
    auto it = pod_records_.find(pod_id);
    if (it != pod_records_.end()) {
        return &it->second;
    }
    return nullptr;
}

uint64_t CapabilityFirewall::get_allocated_gpio_mask() const {
    osal::LockGuard guard(mutex_);
    return allocated_gpio_mask_;
}

uint16_t CapabilityFirewall::get_allocated_adc1_mask() const {
    osal::LockGuard guard(mutex_);
    return allocated_adc1_mask_;
}

uint8_t CapabilityFirewall::get_allocated_i2c_mask() const {
    osal::LockGuard guard(mutex_);
    return allocated_i2c_mask_;
}

uint8_t CapabilityFirewall::get_allocated_uart_mask() const {
    osal::LockGuard guard(mutex_);
    return allocated_uart_mask_;
}

void CapabilityFirewall::log_violation(const std::string& pod_id, PeripheralType type, uint32_t target, AccessMode mode) {
    // A denied hardware access is a real event worth seeing on the console;
    // bounded so a misbehaving pod cannot flood it.
    static int logged = 0;
    if (logged >= 16) return;
    logged++;
    static const char* const names[] = {"gpio", "adc", "i2c", "uart", "ipc", "display", "keyboard", "speaker", "sd", "service-call", "rmt-tx", "rmt-rx"};   // PeripheralType order
    unsigned ti = static_cast<unsigned>(type);
    std::cout << "[FIREWALL] denied " << pod_id << ": " << (ti < sizeof(names) / sizeof(names[0]) ? names[ti] : "peripheral")
              << " " << target << " " << (mode == AccessMode::Write ? "write" : "read") << std::endl;
}

} // namespace kube32::hal
