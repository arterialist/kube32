#include "hal/capability_firewall.hpp"
#include "models/kube_models.hpp"
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
    osal::LockGuard guard(mutex_);
    pod_records_.clear();
    for (auto& o : gpio_owners_) o.clear();
    for (auto& o : adc1_owners_) o.clear();
    for (auto& o : i2c_owners_) o.clear();
    for (auto& o : uart_owners_) o.clear();
    allocated_gpio_mask_ = 0;
    allocated_adc1_mask_ = 0;
    allocated_i2c_mask_ = 0;
    allocated_uart_mask_ = 0;
}

bool CapabilityFirewall::allocate_generic_gpios(uint32_t count, DeviceGrant& grant, const std::string& pod_id, std::vector<uint32_t>& out_claimed, std::string& out_error) {
    uint32_t allocated = 0;
    std::vector<uint32_t> claimed_pins;

    for (uint32_t pin = 1; pin < 64 && allocated < count; ++pin) {
        if ((KUBE32_SAFE_GPIO_MASK & (1ULL << pin)) == 0) continue;
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
        if (!i2c_owners_[bus].empty()) continue;
        claimed_buses.push_back(bus);
        allocated++;
    }

    if (allocated < count) {
        out_error = "Insufficient allocatable I2C buses: requested " + std::to_string(count);
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

    for (uint32_t ch = 0; ch < 10 && allocated < count; ++ch) {
        if (!adc1_owners_[ch].empty()) continue;
        claimed_channels.push_back(ch);
        allocated++;
    }

    if (allocated < count) {
        out_error = "Insufficient allocatable ADC1 channels: requested " + std::to_string(count);
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
    osal::LockGuard guard(mutex_);

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
    uint32_t generic_gpio_count = 0;
    uint32_t generic_i2c_count = 0;
    uint32_t generic_adc_count = 0;

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
                        if (ch >= 10) {
                            cleanup_claimed();
                            out_error = "Invalid ADC1 channel: " + std::to_string(ch) + " (Max 9)";
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
                }
            }

            // securityContext.capabilities.add: [CHANNEL_IPC, DISPLAY, KEYBOARD, SPEAKER]
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
                }
            }
        }
    }

    if (generic_gpio_count > 0 && !allocate_generic_gpios(generic_gpio_count, grant, pod_id, claimed_gpios, out_error)) {
        cleanup_claimed();
        return false;
    }
    if (generic_i2c_count > 0 && !allocate_generic_i2c(generic_i2c_count, grant, pod_id, claimed_i2cs, out_error)) {
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
    pod_records_[pod_id] = record;

    return true;
}

bool CapabilityFirewall::grant_capabilities(const std::string& pod_id, const DeviceGrant& grant, std::string& out_error) {
    osal::LockGuard guard(mutex_);

    if (pod_records_.find(pod_id) != pod_records_.end()) {
        out_error = "Pod already registered in capability firewall: " + pod_id;
        return false;
    }

    // Validate GPIO mask against safety mask and collisions
    uint64_t combined_gpio = grant.gpio_read_mask | grant.gpio_write_mask;
    if ((combined_gpio & KUBE32_RESERVED_GPIO_MASK) != 0) {
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
            if (ch >= 10 || (!adc1_owners_[ch].empty() && adc1_owners_[ch] != pod_id)) {
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
            if (b >= 2 || (!i2c_owners_[b].empty() && i2c_owners_[b] != pod_id)) {
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
            if (u == 0 || u > 2 || (!uart_owners_[u].empty() && uart_owners_[u] != pod_id)) {
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

    PodSecurityRecord record;
    record.pod_id = pod_id;
    record.grant = grant;
    record.action = SecurityAction::LogAndDeny;
    record.violation_count = 0;
    pod_records_[pod_id] = record;
    return true;
}

void CapabilityFirewall::unregister_pod(const std::string& pod_id) {
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
    // In production or host sim, we can log to debug/console
    (void)pod_id;
    (void)type;
    (void)target;
    (void)mode;
}

} // namespace kube32::hal
