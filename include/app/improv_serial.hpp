#pragma once

// Improv Wi-Fi over the serial console (https://www.improv-wifi.com/serial/):
// packet framing and RPC parsing. A browser installer such as ESP Web Tools
// sends the node's Wi-Fi credentials in these packets right after it writes
// the firmware.
//
//   "IMPROV" <version 1> <type> <length> <data: length bytes> <checksum> "\n"
//
// The checksum is the low byte of the sum of every byte before it. A client
// sends RPC packets (type 3) whose data is <command> <length> <arguments>;
// the node answers with current state (1), error state (2) and RPC result (4)
// packets. An RPC result's data is <command> <length> followed by strings,
// each a length byte and its UTF-8 bytes.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kube32::app::improv {

constexpr char kHeader[] = "IMPROV";
constexpr size_t kHeaderSize = 6;
constexpr uint8_t kVersion = 1;
// Version, type and length follow the header
constexpr size_t kPrefixSize = kHeaderSize + 3;

enum PacketType : uint8_t {
    kCurrentState = 0x01,
    kErrorState = 0x02,
    kRpc = 0x03,
    kRpcResult = 0x04,
};

enum State : uint8_t {
    kReady = 0x02,         // authorized: a serial client always is
    kProvisioning = 0x03,
    kProvisioned = 0x04,
};

enum Error : uint8_t {
    kNoError = 0x00,
    kInvalidRpc = 0x01,
    kUnknownRpc = 0x02,
    kUnableToConnect = 0x03,
    kUnknownError = 0xFF,
};

enum Command : uint8_t {
    kWifiSettings = 0x01,
    kCurrentStateRequest = 0x02,
    kDeviceInfo = 0x03,
    kScanNetworks = 0x04,
};

struct Rpc {
    uint8_t command{0};
    std::string ssid;       // kWifiSettings
    std::string password;   // kWifiSettings
};

// Bytes a packet holds after its first kPrefixSize bytes: data and checksum.
// `prefix` is the header, version, type and length as received.
size_t remaining_after_prefix(const uint8_t* prefix);

// Checks one whole packet (header to checksum, no trailing newline) and reads
// the RPC it carries. kInvalidRpc for a damaged or malformed packet, or Wi-Fi
// settings whose SSID is empty or longer than 32 bytes or whose password is
// longer than 64; a command this parser does not know is returned as is, with
// no arguments.
Error parse_rpc(const uint8_t* packet, size_t size, Rpc& out);

// A packet of `type` carrying `data` (at most 255 bytes), checksum and
// newline included
std::string packet(PacketType type, const std::string& data);
std::string state_packet(State state);
std::string error_packet(Error error);
// The answer to `command`: each string is cut to fit the 255-byte packet
std::string rpc_result(Command command, const std::vector<std::string>& strings);

} // namespace kube32::app::improv
