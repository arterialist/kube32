#pragma once

// I2C devices on the node's own buses, as node labels and as a report of
// identified parts for the Device controller.
//
// A scan probes addresses 0x08-0x77 with zero-length writes on every I2C
// controller the board offers on default pins (BoardProfile::i2c_pins). A
// free bus is lent by the capability firewall for the scan and pod admission
// waits for it; a bus shared by device grants (kube32.io/i2c-device) is
// scanned as it is installed, transfer by transfer under the same bus lock
// as the pods' transfers; a bus a pod claims whole is not scanned and keeps
// what its last scan found. A free bus whose lines read low with the
// internal pull-ups on is driven by something else and is not scanned.
//
// An address from 0x70 to 0x77 that keeps a control byte written to it
// (0x01, 0x80, then 0x00) is a PCA9548A multiplexer (M5Stack PaHub): each of
// its eight channels is selected alone and scanned for the addresses that
// answer there and not on the bus itself, and the multiplexer is deselected
// afterwards. Parts at an address of the driver catalog (models/device.hpp)
// are identified by the catalog's register reads; a scan writes nothing but
// multiplexer control bytes and register pointers. A part is identified when
// it first shows up, and not while a pod's device grant holds its address.
//
// Each device becomes a label i2c.kube32.io/0x<address>: "bus-<n>" (the
// lowest bus when an address answers on both), or "bus-<n>.mux-<hh>.<channel>"
// for a part behind a multiplexer when the address is not on a bus itself,
// so a pod can select a node by what is plugged into it:
//
//   spec:
//     nodeSelector: { i2c.kube32.io/0x28: bus-0 }
//
// annotation() is the node annotation kube32.io/i2c-devices: every part
// found, with its multiplexer channel and what it was identified as. A part
// leaves it (and a part behind a multiplexer its label) after two scans in
// a row that miss it; the labels of parts on a bus itself follow each scan.

#include "models/device.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kube32::hal {

class I2cDiscovery {
public:
    // Scans every bus that may be scanned now. Returns true when what was
    // found changed.
    bool scan();
    // i2c.kube32.io/0x<address> -> "bus-<n>" or "bus-<n>.mux-<hh>.<channel>"
    std::map<std::string, std::string> labels() const;
    // Everything the last scans found, buses in order
    std::vector<models::DiscoveredPart> parts() const;
    // parts() as the kube32.io/i2c-devices annotation value
    std::string annotation() const;
    // Devices found on `bus` itself by its last scan, in address order
    const std::vector<uint8_t>& devices(uint8_t bus) const;

private:
    std::vector<uint8_t> found_[2];
    std::vector<models::DiscoveredPart> parts_[2];
    std::vector<models::I2cLocation> missing_[2];   // parts kept although the last scan missed them
};

} // namespace kube32::hal
