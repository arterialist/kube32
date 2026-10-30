#pragma once

// I2C devices on the node's own buses, as node labels.
//
// A scan probes addresses 0x08-0x77 with zero-length writes on every I2C
// controller the board offers on default pins (BoardProfile::i2c_pins), and
// only while no pod holds the controller or its pins: the capability firewall
// lends the bus for the scan and pod admission waits for it. A bus a pod
// holds keeps the devices its last scan found. A bus whose lines read low
// with the internal pull-ups on is driven by something else and is not
// scanned.
//
// Each device becomes a label i2c.kube32.io/0x<address>: "bus-<n>" (the
// lowest bus when an address answers on both), so a pod can select a node
// by what is plugged into it:
//
//   spec:
//     nodeSelector: { i2c.kube32.io/0x28: bus-0 }

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kube32::hal {

class I2cDiscovery {
public:
    // Scans every free bus. Returns true when the devices found changed.
    bool scan();
    // i2c.kube32.io/0x<address> -> "bus-<n>" for every device found
    std::map<std::string, std::string> labels() const;
    // Devices found on `bus` by its last scan, in address order
    const std::vector<uint8_t>& devices(uint8_t bus) const;

private:
    std::vector<uint8_t> found_[2];
};

} // namespace kube32::hal
