#include "models/device.hpp"

#include <cstdio>

namespace kube32::models {

namespace {

// Linear memory of a driver pod: the drivers are small C modules whose
// stack and data fit in 3 KiB. Raise an entry when a driver needs more (its
// pod then fails to load with a memory-limit message).
constexpr uint32_t kDriverMemory = 3 * 1024;

const DeviceDriver kDrivers[] = {
    // name, bus, address, alt, chip, memory, interval, pins, input pins, resource, checks
    {"bme688", DeviceBus::I2c, 0x77, 0x76, "BME688", kDriverMemory, 5000, "", false, nullptr, 2,
     {{0xD0, 1, {0x61}, {}}, {0xF0, 1, {0x01}, {}}}},
    // M5Stack Unit Mini Scales: register 0xFF holds the unit's I2C address,
    // 0x20 its button (0 or 1)
    {"miniscale", DeviceBus::I2c, 0x26, 0, "MiniScale", kDriverMemory, 1000, "", false, nullptr, 2,
     {{0xFF, 1, {0x26}, {}}, {0x20, 2, {0x00, 0x01}, {}}}},
    // MFRC522-compatible readers: VersionReg (0x37)
    {"rfid", DeviceBus::I2c, 0x28, 0, "MFRC522", kDriverMemory, 200, "", false, nullptr, 1,
     {{0x37, 5, {0x15, 0x88, 0x91, 0x92, 0xB2}, {"WS1850S", "FM17522", "MFRC522", "MFRC522", "FM17522"}}}},
    {"chip-temp", DeviceBus::Internal, 0, 0, "", kDriverMemory, 10000, "", false, "hardware.kube32.io/chip-temp", 0, {}},
    {"button", DeviceBus::Gpio, 0, 0, "", kDriverMemory, 50, "pin", true, nullptr, 0, {}},
    {"switch", DeviceBus::Gpio, 0, 0, "", kDriverMemory, 30000, "pin", false, nullptr, 0, {}},
};

// ESP32-S3 pads: GPIO 0-21 and 26-48
bool chip_gpio(int64_t pin) { return (pin >= 0 && pin <= 21) || (pin >= 26 && pin <= 48); }

bool config_key(const std::string& k) {
    if (k.empty() || k.size() > 63) return false;
    for (char c : k) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

// An address from JSON: "0x77" or the number 119 (YAML reads an unquoted 0x77 as one)
bool json_byte(const JsonValue& v, uint8_t& out) {
    if (v.is_string()) return parse_hex_byte(v.str_val, out);
    if (v.is_number() && v.num_val >= 0 && v.num_val <= 255 && v.num_val == static_cast<double>(static_cast<int>(v.num_val))) {
        out = static_cast<uint8_t>(v.num_val);
        return true;
    }
    return false;
}

bool json_int(const JsonValue& v, int64_t& out) {
    if (!v.is_number() || v.num_val != static_cast<double>(static_cast<int64_t>(v.num_val))) return false;
    out = static_cast<int64_t>(v.num_val);
    return true;
}

std::string json_text(const JsonValue& v) {
    if (v.is_string()) return v.str_val;
    if (v.is_bool()) return v.bool_val ? "true" : "false";
    if (v.is_number()) {
        int64_t i = 0;
        if (json_int(v, i)) return std::to_string(i);
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", v.num_val);
        return buf;
    }
    return std::string();
}

JsonValue jstr(const std::string& v) { return JsonValue{JsonType::String, false, 0, v, {}, {}}; }
JsonValue jnum(double v) { return JsonValue{JsonType::Number, false, v, "", {}, {}}; }

std::string hex2(uint8_t v) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%02x", v);
    return buf;
}

} // namespace

const DeviceDriver* device_drivers(size_t& count) {
    count = sizeof(kDrivers) / sizeof(kDrivers[0]);
    return kDrivers;
}

const DeviceDriver* find_device_driver(std::string_view name) {
    for (const auto& d : kDrivers) {
        if (name == d.name) return &d;
    }
    return nullptr;
}

std::string device_driver_names() {
    std::string out;
    for (const auto& d : kDrivers) out += (out.empty() ? "\"" : ", \"") + std::string(d.name) + "\"";
    return out;
}

std::string hex_byte(uint8_t v) { return "0x" + hex2(v); }

bool parse_hex_byte(std::string_view s, uint8_t& out) {
    if (s.size() < 3 || s.size() > 4 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
    unsigned v = 0;
    for (char c : s.substr(2)) {
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v * 16 + static_cast<unsigned>(d);
    }
    out = static_cast<uint8_t>(v);
    return true;
}

bool parse_device_spec(const JsonValue* spec, DeviceSpec& out, std::string& why) {
    out = DeviceSpec();
    if (!spec || !spec->is_object()) { why = "spec: Required value"; return false; }
    const std::string driver = spec->get_string("driver", "");
    if (driver.empty()) { why = "spec.driver: Required value: one of " + device_driver_names(); return false; }
    out.driver = find_device_driver(driver);
    if (!out.driver) { why = "spec.driver: Unsupported value: \"" + driver + "\": supported values: " + device_driver_names(); return false; }
    const DeviceDriver& d = *out.driver;

    out.node_name = spec->get_string("nodeName", "");
    if (const JsonValue* sel = spec->get("nodeSelector"); sel && !sel->is_null()) {
        if (!sel->is_object()) { why = "spec.nodeSelector: Invalid value: must be a map of node labels"; return false; }
        for (const auto& kv : sel->obj_val) {
            if (!kv.second.is_string()) { why = "spec.nodeSelector." + kv.first + ": Invalid value: must be a string"; return false; }
            out.node_selector.emplace_back(kv.first, kv.second.str_val);
        }
    }
    if (out.node_name.empty() && out.node_selector.empty()) {
        why = "spec.nodeName: Required value: the node the part is wired to (spec.nodeName or spec.nodeSelector)";
        return false;
    }

    const JsonValue* i2c = spec->get("i2c");
    if (i2c && i2c->is_null()) i2c = nullptr;
    if (d.bus != DeviceBus::I2c && i2c) { why = "spec.i2c: Forbidden: driver " + driver + " does not use I2C"; return false; }
    if (d.bus == DeviceBus::I2c) {
        out.has_i2c = true;
        out.i2c.address = d.address;
        if (i2c && !i2c->is_object()) { why = "spec.i2c: Invalid value: must be an object"; return false; }
        if (i2c) {
            if (const JsonValue* b = i2c->get("bus")) {
                int64_t bus = -1;
                if (!json_int(*b, bus) || bus < 0 || bus > 1) { why = "spec.i2c.bus: Invalid value: " + json_text(*b) + ": the ESP32-S3 has I2C buses 0 and 1"; return false; }
                out.i2c.bus = static_cast<uint8_t>(bus);
            }
            if (const JsonValue* a = i2c->get("address")) {
                if (!json_byte(*a, out.i2c.address) || out.i2c.address < 0x08 || out.i2c.address > 0x77) {
                    why = "spec.i2c.address: Invalid value: \"" + json_text(*a) + "\": a 7-bit address from \"0x08\" to \"0x77\"";
                    return false;
                }
            }
            if (const JsonValue* m = i2c->get("mux"); m && !m->is_null()) {
                if (!m->is_object()) { why = "spec.i2c.mux: Invalid value: must be an object with address and channel"; return false; }
                out.i2c.mux = kMuxFirstAddress;
                if (const JsonValue* ma = m->get("address")) {
                    if (!json_byte(*ma, out.i2c.mux) || out.i2c.mux < kMuxFirstAddress || out.i2c.mux > kMuxLastAddress) {
                        why = "spec.i2c.mux.address: Invalid value: \"" + json_text(*ma) + "\": a PCA9548A answers at \"0x70\" to \"0x77\"";
                        return false;
                    }
                }
                const JsonValue* ch = m->get("channel");
                int64_t channel = -1;
                if (!ch) { why = "spec.i2c.mux.channel: Required value"; return false; }
                if (!json_int(*ch, channel) || channel < 0 || channel >= kMuxChannels) {
                    why = "spec.i2c.mux.channel: Invalid value: " + json_text(*ch) + ": a channel from 0 to 7";
                    return false;
                }
                out.i2c.channel = static_cast<uint8_t>(channel);
                if (out.i2c.mux == out.i2c.address) {
                    why = "spec.i2c.address: Invalid value: \"" + hex_byte(out.i2c.address) + "\": the multiplexer's own address";
                    return false;
                }
            }
        }
    }

    if (const JsonValue* pins = spec->get("pins"); pins && !pins->is_null()) {
        if (!pins->is_object()) { why = "spec.pins: Invalid value: must be a map of pin names to GPIO numbers"; return false; }
        const std::string wanted = std::string(",") + d.pins + ",";
        for (const auto& kv : pins->obj_val) {
            if (wanted.find("," + kv.first + ",") == std::string::npos || kv.first.empty()) {
                why = "spec.pins." + kv.first + ": Unsupported value: driver " + driver +
                      (*d.pins ? std::string(" takes pins: ") + d.pins : std::string(" takes no pins"));
                return false;
            }
            int64_t gpio = -1;
            if (!json_int(kv.second, gpio) || !chip_gpio(gpio)) {
                why = "spec.pins." + kv.first + ": Invalid value: " + json_text(kv.second) + ": an ESP32-S3 GPIO (0-21, 26-48)";
                return false;
            }
            // GPIO 0 straps the boot mode: read, never driven
            if (gpio == 0 && !d.input_pins) {
                why = "spec.pins." + kv.first + ": Invalid value: 0: GPIO 0 (the boot button) is only for drivers that read their pins";
                return false;
            }
            out.pins.emplace_back(kv.first, static_cast<uint32_t>(gpio));
        }
    }
    // Every pin the driver takes is given
    const std::string all = d.pins;
    for (size_t start = 0; start < all.size();) {
        size_t end = all.find(',', start);
        if (end == std::string::npos) end = all.size();
        const std::string name = all.substr(start, end - start);
        start = end + 1;
        bool given = false;
        for (const auto& p : out.pins) given |= p.first == name;
        if (!given) { why = "spec.pins." + name + ": Required value: driver " + driver + " takes pins: " + all; return false; }
    }

    out.interval_ms = d.interval_ms;
    if (const JsonValue* iv = spec->get("intervalMs"); iv && !iv->is_null()) {
        int64_t ms = 0;
        if (!json_int(*iv, ms) || ms < 50 || ms > 3600000) {
            why = "spec.intervalMs: Invalid value: " + json_text(*iv) + ": from 50 (ms) to 3600000 (1 h)";
            return false;
        }
        out.interval_ms = static_cast<uint32_t>(ms);
    }

    if (const JsonValue* cfg = spec->get("config"); cfg && !cfg->is_null()) {
        if (!cfg->is_object()) { why = "spec.config: Invalid value: must be a map of settings"; return false; }
        for (const auto& kv : cfg->obj_val) {
            if (!config_key(kv.first)) { why = "spec.config." + kv.first + ": Invalid value: keys are letters, digits, '-', '_' and '.'"; return false; }
            if (!kv.second.is_string() && !kv.second.is_number() && !kv.second.is_bool()) {
                why = "spec.config." + kv.first + ": Invalid value: must be a string, number or boolean";
                return false;
            }
            out.config.emplace_back(kv.first, json_text(kv.second));
        }
    }

    out.image = spec->get_string("image", "");
    if (out.image.empty()) {
        out.image = std::string("wasm://embedded/driver-") + d.name;
    } else if (out.image.rfind("wasm://", 0) != 0) {
        why = "spec.image: Invalid value: \"" + out.image + "\": a wasm:// image";
        return false;
    }

    if (const JsonValue* a = spec->get("activation"); a && !a->is_null()) {
        if (a->is_string() && a->str_val == "Always") out.activation = DeviceActivation::Always;
        else if (!a->is_string() || a->str_val != "OnDemand") {
            why = "spec.activation: Unsupported value: \"" + json_text(*a) + "\": supported values: \"OnDemand\", \"Always\"";
            return false;
        }
    }
    if (const JsonValue* idle = spec->get("idleSeconds"); idle && !idle->is_null()) {
        int64_t s = 0;
        if (!json_int(*idle, s) || s < kMinIdleSeconds || s > kMaxIdleSeconds) {
            why = "spec.idleSeconds: Invalid value: " + json_text(*idle) + ": from " + std::to_string(kMinIdleSeconds) + " to " +
                  std::to_string(kMaxIdleSeconds) + " (s)";
            return false;
        }
        out.idle_seconds = static_cast<uint32_t>(s);
    }
    return true;
}

void default_device(JsonValue& device) {
    JsonValue* spec = device.get_mut("spec");
    DeviceSpec ds;
    std::string why;
    if (!spec || !parse_device_spec(spec, ds, why)) return;
    const bool on_demand = ds.activation == DeviceActivation::OnDemand;
    spec->set("activation", jstr(on_demand ? "OnDemand" : "Always"));
    if (on_demand) spec->set("idleSeconds", jnum(ds.idle_seconds));
    if (!ds.has_i2c) return;
    JsonValue* i2c = spec->get_mut("i2c");
    if (!i2c || !i2c->is_object()) {
        JsonValue o;
        o.type = JsonType::Object;
        spec->set("i2c", std::move(o));
        i2c = spec->get_mut("i2c");
    }
    i2c->set("bus", jnum(ds.i2c.bus));
    i2c->set("address", jstr(hex_byte(ds.i2c.address)));
    if (ds.i2c.mux) {
        if (JsonValue* m = i2c->get_mut("mux")) m->set("address", jstr(hex_byte(ds.i2c.mux)));
    }
}

std::string i2c_device_annotation(const I2cLocation& loc) {
    std::string out = "bus=" + std::to_string(loc.bus) + ",addr=" + hex_byte(loc.address);
    if (loc.mux) out += ",mux=" + hex_byte(loc.mux) + ":" + std::to_string(loc.channel);
    return out;
}

bool parse_i2c_device_annotation(std::string_view value, I2cLocation& out, std::string& why) {
    out = I2cLocation();
    bool have_bus = false, have_addr = false;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(',', start);
        if (end == std::string_view::npos) end = value.size();
        std::string_view item = value.substr(start, end - start);
        start = end + 1;
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        if (eq == std::string_view::npos) { why = "\"" + std::string(item) + "\" is not <name>=<value>"; return false; }
        const std::string_view key = item.substr(0, eq), val = item.substr(eq + 1);
        if (key == "bus") {
            if (val != "0" && val != "1") { why = "bus must be 0 or 1"; return false; }
            out.bus = static_cast<uint8_t>(val[0] - '0');
            have_bus = true;
        } else if (key == "addr") {
            if (!parse_hex_byte(val, out.address) || out.address < 0x08 || out.address > 0x77) { why = "addr must be 0x08 to 0x77"; return false; }
            have_addr = true;
        } else if (key == "mux") {
            const size_t colon = val.find(':');
            if (colon == std::string_view::npos || colon + 2 != val.size() || !parse_hex_byte(val.substr(0, colon), out.mux) ||
                out.mux < kMuxFirstAddress || out.mux > kMuxLastAddress || val[colon + 1] < '0' || val[colon + 1] > '7') {
                why = "mux must be <0x70 to 0x77>:<channel 0 to 7>";
                return false;
            }
            out.channel = static_cast<uint8_t>(val[colon + 1] - '0');
        } else {
            why = "unknown setting \"" + std::string(key) + "\"";
            return false;
        }
        if (start > value.size()) break;
    }
    if (!have_bus || !have_addr) { why = "bus and addr are required"; return false; }
    if (out.mux && out.mux == out.address) { why = "addr is the multiplexer's own address"; return false; }
    return true;
}

std::string i2c_location_text(const I2cLocation& loc) {
    std::string out = "i2c" + std::to_string(loc.bus) + "/";
    if (loc.mux) out += hex_byte(loc.mux) + ":" + std::to_string(loc.channel) + "/";
    return out + hex_byte(loc.address);
}

std::string discovered_device_name(std::string_view driver, std::string_view node, const I2cLocation& loc) {
    std::string path = "b" + std::to_string(loc.bus);
    if (loc.mux) path += "-m" + hex2(loc.mux) + "c" + std::to_string(loc.channel);
    path += "-" + hex2(loc.address);
    std::string name = std::string(driver) + "-" + std::string(node) + "-" + path;
    if (name.size() <= 63) return name;
    uint32_t h = 2166136261u;
    for (unsigned char c : name) { h ^= c; h *= 16777619u; }
    char tag[12];
    snprintf(tag, sizeof(tag), "%06x", static_cast<unsigned>(h & 0xffffff));
    const size_t fixed = driver.size() + 1 + 1 + 6 + 1 + path.size();
    std::string cut = fixed < 63 ? std::string(node.substr(0, 63 - fixed)) : std::string();
    while (!cut.empty() && cut.back() == '-') cut.pop_back();
    return std::string(driver) + "-" + cut + (cut.empty() ? "" : "-") + tag + "-" + path;
}

std::string discovered_parts_json(const std::vector<DiscoveredPart>& parts) {
    std::string out = "[";
    for (const auto& p : parts) {
        if (out.size() > 1) out += ",";
        out += "{\"bus\":" + std::to_string(p.where.bus);
        if (p.where.mux) out += ",\"mux\":\"" + hex_byte(p.where.mux) + "\",\"channel\":" + std::to_string(p.where.channel);
        out += ",\"address\":\"" + hex_byte(p.where.address) + "\"";
        if (!p.chip.empty()) out += ",\"chip\":\"" + p.chip + "\"";
        if (!p.driver.empty()) out += ",\"driver\":\"" + p.driver + "\"";
        out += "}";
    }
    return out + "]";
}

bool parse_discovered_parts(std::string_view json, std::vector<DiscoveredPart>& out) {
    out.clear();
    JsonValue doc;
    std::string err;
    if (!parse_json(json, doc, err) || !doc.is_array()) return false;
    for (const auto& e : doc.arr_val) {
        if (!e.is_object()) continue;
        DiscoveredPart p;
        int64_t bus = e.get_int("bus", -1);
        const JsonValue* addr = e.get("address");
        if (bus < 0 || bus > 1 || !addr || !json_byte(*addr, p.where.address)) continue;
        p.where.bus = static_cast<uint8_t>(bus);
        if (const JsonValue* mux = e.get("mux"); mux && json_byte(*mux, p.where.mux)) {
            const int64_t ch = e.get_int("channel", -1);
            if (ch < 0 || ch >= kMuxChannels) continue;
            p.where.channel = static_cast<uint8_t>(ch);
        }
        p.chip = e.get_string("chip", "");
        p.driver = e.get_string("driver", "");
        out.push_back(std::move(p));
    }
    return true;
}

bool OnDemandDriver::selected_by(const std::vector<std::pair<std::string, std::string>>& selector) const {
    if (selector.empty()) return false;
    for (const auto& want : selector) {
        bool found = false;
        for (const auto& have : labels) found |= have == want;
        if (!found) return false;
    }
    return true;
}

std::vector<OnDemandDriver> on_demand_drivers(const std::vector<std::string>& device_jsons) {
    std::vector<OnDemandDriver> out;
    for (const auto& json : device_jsons) {
        JsonValue doc;
        std::string err, why;
        DeviceSpec spec;
        if (!parse_json(json, doc, err) || !parse_device_spec(doc.get("spec"), spec, why)) continue;
        if (spec.activation != DeviceActivation::OnDemand) continue;
        const JsonValue* meta = doc.get("metadata");
        if (!meta) continue;
        OnDemandDriver d;
        d.ns = meta->get_string("namespace", "default");
        d.device.name = meta->get_string("name", "");
        const JsonValue* st = doc.get("status");
        d.device.node = st ? st->get_string("nodeName", "") : "";
        if (d.device.node.empty()) d.device.node = spec.node_name;
        d.device.idle_seconds = spec.idle_seconds;
        if (const JsonValue* labels = meta->get("labels"); labels && labels->is_object()) {
            for (const auto& kv : labels->obj_val) {
                if (kv.second.is_string() && kv.first != kLabelDevice && kv.first != kLabelDriver) d.labels.emplace_back(kv.first, kv.second.str_val);
            }
        }
        d.labels.emplace_back(kLabelDevice, d.device.name);
        d.labels.emplace_back(kLabelDriver, spec.driver->name);
        out.push_back(std::move(d));
    }
    return out;
}

void select_on_demand(const std::vector<OnDemandDriver>& drivers, const std::string& ns,
                      const std::vector<std::pair<std::string, std::string>>& selector, std::vector<OnDemandDevice>& out) {
    for (const auto& d : drivers) {
        if (d.ns == ns && d.selected_by(selector)) out.push_back(d.device);
    }
}

std::string on_demand_annotation(const std::vector<OnDemandDevice>& devices) {
    std::string out;
    for (const auto& d : devices) {
        if (!out.empty()) out += ",";
        out += d.name + ":" + d.node + ":" + std::to_string(d.idle_seconds);
    }
    return out;
}

void parse_on_demand_annotation(std::string_view value, std::vector<OnDemandDevice>& out) {
    while (!value.empty()) {
        const size_t comma = value.find(',');
        std::string_view item = value.substr(0, comma);
        value = comma == std::string_view::npos ? std::string_view() : value.substr(comma + 1);
        const size_t a = item.find(':');
        const size_t b = a == std::string_view::npos ? a : item.find(':', a + 1);
        if (a == 0 || b == std::string_view::npos) continue;
        OnDemandDevice d;
        d.name = std::string(item.substr(0, a));
        d.node = std::string(item.substr(a + 1, b - a - 1));
        uint32_t s = 0;
        for (char c : item.substr(b + 1)) s = (c >= '0' && c <= '9' && s < kMaxIdleSeconds) ? s * 10 + static_cast<uint32_t>(c - '0') : s;
        if (s >= kMinIdleSeconds && s <= kMaxIdleSeconds) d.idle_seconds = s;
        out.push_back(std::move(d));
    }
}

} // namespace kube32::models
