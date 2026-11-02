#include "app/improv_serial.hpp"

#include <cstring>

namespace kube32::app::improv {

static uint8_t checksum(const uint8_t* p, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum += p[i];
    return static_cast<uint8_t>(sum & 0xFF);
}

size_t remaining_after_prefix(const uint8_t* prefix) {
    return static_cast<size_t>(prefix[kPrefixSize - 1]) + 1;
}

Error parse_rpc(const uint8_t* packet, size_t size, Rpc& out) {
    out = Rpc{};
    if (size < kPrefixSize + 1 || std::memcmp(packet, kHeader, kHeaderSize) != 0) return kInvalidRpc;
    const uint8_t version = packet[kHeaderSize];
    const uint8_t type = packet[kHeaderSize + 1];
    const size_t len = packet[kHeaderSize + 2];
    if (version != kVersion || type != kRpc || size != kPrefixSize + len + 1) return kInvalidRpc;
    if (checksum(packet, size - 1) != packet[size - 1]) return kInvalidRpc;
    // <command> <length> <arguments>
    const uint8_t* data = packet + kPrefixSize;
    if (len < 2 || static_cast<size_t>(data[1]) + 2 != len) return kInvalidRpc;
    out.command = data[0];
    if (out.command != kWifiSettings) return kNoError;
    // <ssid length> <ssid> <password length> <password>
    const uint8_t* args = data + 2;
    const size_t args_len = data[1];
    if (args_len < 1) return kInvalidRpc;
    const size_t ssid_len = args[0];
    if (ssid_len < 1 || ssid_len > 32 || 1 + ssid_len + 1 > args_len) return kInvalidRpc;
    const size_t pass_len = args[1 + ssid_len];
    if (pass_len > 64 || 1 + ssid_len + 1 + pass_len != args_len) return kInvalidRpc;
    out.ssid.assign(reinterpret_cast<const char*>(args + 1), ssid_len);
    out.password.assign(reinterpret_cast<const char*>(args + 2 + ssid_len), pass_len);
    return kNoError;
}

std::string packet(PacketType type, const std::string& data) {
    const size_t len = data.size() < 255 ? data.size() : 255;
    std::string p(kHeader, kHeaderSize);
    p.push_back(static_cast<char>(kVersion));
    p.push_back(static_cast<char>(type));
    p.push_back(static_cast<char>(len));
    p.append(data, 0, len);
    p.push_back(static_cast<char>(checksum(reinterpret_cast<const uint8_t*>(p.data()), p.size())));
    p.push_back('\n');
    return p;
}

std::string state_packet(State state) {
    return packet(kCurrentState, std::string(1, static_cast<char>(state)));
}

std::string error_packet(Error error) {
    return packet(kErrorState, std::string(1, static_cast<char>(error)));
}

std::string rpc_result(Command command, const std::vector<std::string>& strings) {
    // <command> <length of what follows> then <length> <bytes> per string
    std::string body;
    for (const std::string& s : strings) {
        const size_t room = 255 - 2 - body.size();
        if (room < 1) break;
        const size_t n = s.size() < room - 1 ? s.size() : room - 1;
        body.push_back(static_cast<char>(n));
        body.append(s, 0, n);
    }
    std::string data;
    data.push_back(static_cast<char>(command));
    data.push_back(static_cast<char>(body.size()));
    data += body;
    return packet(kRpcResult, data);
}

} // namespace kube32::app::improv
