#pragma once

// Devices (kube32.io/v1 Device, docs/devices.md): parts wired to a node,
// each read or driven by a driver pod the Device controller runs on that
// node and reached through a Service of the Device's name.
//
// The driver catalog below is the one table the API server (validation),
// the Device controller (the driver pod it creates) and I2C discovery (the
// parts it identifies) share. Each driver runs from the image
// wasm://embedded/driver-<name>; memory_bytes is its pod's memory request
// and limit (its Wasm linear memory).

#include "models/kube_models.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kube32::models {

// Pod labels the controller sets on a driver pod, besides the Device's own
constexpr const char* kLabelDevice = "kube32.io/device";
constexpr const char* kLabelDriver = "kube32.io/driver";
// Label of the Devices the controller creates from what discovery reports
constexpr const char* kLabelDiscovered = "kube32.io/discovered";
// Node label of kube32-sim nodes: their peripherals are simulated
constexpr const char* kLabelSimulator = "kube32.io/simulator";
// Pod annotation of a device grant: one address on a shared I2C bus,
// "bus=<n>,addr=0x<hh>[,mux=0x<hh>:<channel>]"
constexpr const char* kAnnotationI2cDevice = "kube32.io/i2c-device";
// Node annotation with what I2C discovery found: a JSON list of
// {"bus":0,"mux":"0x70","channel":2,"address":"0x77","chip":"BME688","driver":"bme688"}
// (mux and channel only behind a multiplexer, chip and driver only when identified)
constexpr const char* kAnnotationI2cDevices = "kube32.io/i2c-devices";

// PCA9548A / TCA9548A I2C multiplexers (M5Stack PaHub): addresses 0x70-0x77,
// eight channels, one control byte with a bit per channel
constexpr uint8_t kMuxFirstAddress = 0x70;
constexpr uint8_t kMuxLastAddress = 0x77;
constexpr uint8_t kMuxChannels = 8;

enum class DeviceBus : uint8_t { I2c, Gpio, Internal };

// One register read that identifies a part: register `reg` reads one of
// values[0..count); chips[i], when set, names the part for values[i]
struct ChipCheck {
    uint8_t reg;
    uint8_t count;
    uint8_t values[5];
    const char* chips[5];
};

struct DeviceDriver {
    const char* name;          // spec.driver
    DeviceBus bus;
    uint8_t address;           // default I2C address (0: not on I2C)
    uint8_t alt_address;       // the other address the part can be strapped to (0: none)
    const char* chip;          // the part as discovery reports it
    uint32_t memory_bytes;     // the driver pod's memory request and limit
    uint32_t interval_ms;      // reading period when spec.intervalMs is not set
    const char* pins;          // GPIO drivers: the names spec.pins must give, comma-separated
    bool input_pins;           // the pins are only read (claimed as esp32.io/gpio-input-<n>; GPIO 0 allowed)
    const char* resource;      // a further resource the driver pod requests ("1"), or nullptr
    uint8_t checks;            // discovery identifies the part with these reads, in order
    ChipCheck check[2];
};

// The catalog, and the entry named `name` (nullptr when there is none)
const DeviceDriver* device_drivers(size_t& count);
const DeviceDriver* find_device_driver(std::string_view name);
// Comma-separated names of the catalog, for messages
std::string device_driver_names();

// Where an I2C part is: its address, and the multiplexer channel it sits
// behind (mux 0: on the bus itself)
struct I2cLocation {
    uint8_t bus{0};
    uint8_t address{0};
    uint8_t mux{0};
    uint8_t channel{0};
    bool operator==(const I2cLocation& o) const { return bus == o.bus && address == o.address && mux == o.mux && channel == o.channel; }
    bool operator!=(const I2cLocation& o) const { return !(*this == o); }
};

// A Device's spec, read and checked
struct DeviceSpec {
    const DeviceDriver* driver{nullptr};
    std::string node_name;
    std::vector<std::pair<std::string, std::string>> node_selector;
    bool has_i2c{false};
    I2cLocation i2c;
    std::vector<std::pair<std::string, uint32_t>> pins;      // name -> GPIO
    uint32_t interval_ms{0};                                 // spec.intervalMs or the driver's
    std::vector<std::pair<std::string, std::string>> config;
    std::string image;                                       // spec.image or wasm://embedded/driver-<driver>
};

// Reads `spec` (a Device's spec) with the driver's defaults filled in;
// false with the field and the problem in `why` ("spec.i2c.address: ...")
bool parse_device_spec(const JsonValue* spec, DeviceSpec& out, std::string& why);
// Fills the defaults kube-apiserver style into a Device about to be stored
// (spec.i2c.bus and spec.i2c.address of I2C drivers, addresses as "0x77")
void default_device(JsonValue& device);

// "0x77"
std::string hex_byte(uint8_t v);
// "0x77" (or "0X77"), one or two hex digits after the prefix
bool parse_hex_byte(std::string_view s, uint8_t& out);
// The value of kube32.io/i2c-device for `loc`, and back
std::string i2c_device_annotation(const I2cLocation& loc);
bool parse_i2c_device_annotation(std::string_view value, I2cLocation& out, std::string& why);
// Where `loc` is, as tables print it: "i2c0/0x77" or "i2c0/0x70:2/0x77"
std::string i2c_location_text(const I2cLocation& loc);

// Name of the Device the controller creates for a part discovery found:
// <driver>-<node>-b<bus>[-m<mux>c<channel>]-<address>, e.g.
// bme688-node-b0-m70c2-77; cut to a DNS label (63 characters) with a hash
// when longer, since the Device's Service carries the same name
std::string discovered_device_name(std::string_view driver, std::string_view node, const I2cLocation& loc);

// One entry of a node's kube32.io/i2c-devices annotation
struct DiscoveredPart {
    I2cLocation where;
    std::string chip;     // empty: not identified
    std::string driver;   // catalog driver for the part, empty for none
};
std::string discovered_parts_json(const std::vector<DiscoveredPart>& parts);
bool parse_discovered_parts(std::string_view json, std::vector<DiscoveredPart>& out);

} // namespace kube32::models
