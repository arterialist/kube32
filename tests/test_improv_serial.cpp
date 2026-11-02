// Improv Wi-Fi serial packets (include/app/improv_serial.hpp): the packets a
// browser client sends, as improv-wifi-serial-sdk writes them, and the
// answers parsed as that client parses them
#include "app/improv_serial.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace improv = kube32::app::improv;

// A client packet: header, version 1, type, length, data, checksum (the low
// byte of the sum of everything before it). The client also sends a newline,
// which the console reads as an empty line, so it is left out here.
static std::vector<uint8_t> client_packet(uint8_t type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> p = {'I', 'M', 'P', 'R', 'O', 'V', 1, type, static_cast<uint8_t>(data.size())};
    p.insert(p.end(), data.begin(), data.end());
    unsigned sum = 0;
    for (uint8_t b : p) sum += b;
    p.push_back(static_cast<uint8_t>(sum & 0xFF));
    return p;
}

static std::vector<uint8_t> rpc(uint8_t command, const std::vector<uint8_t>& args) {
    std::vector<uint8_t> data = {command, static_cast<uint8_t>(args.size())};
    data.insert(data.end(), args.begin(), args.end());
    return client_packet(improv::kRpc, data);
}

static std::vector<uint8_t> wifi_args(const std::string& ssid, const std::string& pass) {
    std::vector<uint8_t> a = {static_cast<uint8_t>(ssid.size())};
    a.insert(a.end(), ssid.begin(), ssid.end());
    a.push_back(static_cast<uint8_t>(pass.size()));
    a.insert(a.end(), pass.begin(), pass.end());
    return a;
}

static improv::Error parse(const std::vector<uint8_t>& p, improv::Rpc& out) {
    return improv::parse_rpc(p.data(), p.size(), out);
}

// A device packet read as the client reads one: header, version 1, the
// checksum over everything before it, then a newline
struct Received {
    uint8_t type;
    std::vector<uint8_t> data;
};
static Received receive(const std::string& s) {
    assert(s.size() >= 11);
    assert(s.compare(0, 6, "IMPROV") == 0);
    const auto* b = reinterpret_cast<const uint8_t*>(s.data());
    assert(b[6] == 1);
    const size_t len = b[8];
    assert(s.size() == 9 + len + 2);
    unsigned sum = 0;
    for (size_t i = 0; i < 9 + len; ++i) sum += b[i];
    assert(b[9 + len] == (sum & 0xFF));
    assert(b[10 + len] == '\n');
    return {b[7], std::vector<uint8_t>(b + 9, b + 9 + len)};
}

// The strings of an RPC result, as the client splits them
static std::vector<std::string> result_strings(const Received& r, uint8_t command) {
    assert(r.type == improv::kRpcResult);
    assert(r.data.size() >= 2);
    assert(r.data[0] == command);
    assert(r.data[1] + 2u == r.data.size());
    std::vector<std::string> out;
    size_t i = 2;
    while (i < r.data.size()) {
        const size_t n = r.data[i];
        assert(i + 1 + n <= r.data.size());
        out.emplace_back(reinterpret_cast<const char*>(&r.data[i + 1]), n);
        i += 1 + n;
    }
    return out;
}

static void test_client_requests() {
    // Request current state, byte for byte as the SDK writes it
    const std::vector<uint8_t> state = {'I', 'M', 'P', 'R', 'O', 'V', 0x01, 0x03, 0x02, 0x02, 0x00, 0xE5};
    improv::Rpc r;
    assert(parse(state, r) == improv::kNoError);
    assert(r.command == improv::kCurrentStateRequest);
    assert(improv::remaining_after_prefix(state.data()) == 3);

    assert(parse(rpc(improv::kDeviceInfo, {}), r) == improv::kNoError && r.command == improv::kDeviceInfo);
    assert(parse(rpc(improv::kScanNetworks, {}), r) == improv::kNoError && r.command == improv::kScanNetworks);

    // Wi-Fi settings, with bytes the console's line reader would split on
    // (a 10-byte SSID makes the length byte a newline, and a carriage return
    // in the password)
    const std::string ssid = "home-net\x0d!", pass = "pass\nword 12:";
    const auto wifi = rpc(improv::kWifiSettings, wifi_args(ssid, pass));
    assert(parse(wifi, r) == improv::kNoError);
    assert(r.command == improv::kWifiSettings && r.ssid == ssid && r.password == pass);
    assert(improv::remaining_after_prefix(wifi.data()) == wifi.size() - improv::kPrefixSize);

    // An open network and the longest SSID and password
    assert(parse(rpc(improv::kWifiSettings, wifi_args("open", "")), r) == improv::kNoError && r.password.empty());
    assert(parse(rpc(improv::kWifiSettings, wifi_args(std::string(32, 's'), std::string(64, 'p'))), r) == improv::kNoError);
    assert(r.ssid.size() == 32 && r.password.size() == 64);

    // A command this side does not know comes back as is, for the console to refuse
    assert(parse(rpc(0x07, {}), r) == improv::kNoError && r.command == 0x07);
    std::cout << "[PASS] test_client_requests" << std::endl;
}

static void test_damaged_packets() {
    improv::Rpc r;
    auto good = rpc(improv::kWifiSettings, wifi_args("net", "secret12"));

    auto bad_sum = good;
    bad_sum.back() ^= 0x01;
    assert(parse(bad_sum, r) == improv::kInvalidRpc);

    auto bad_header = good;
    bad_header[0] = 'X';
    assert(parse(bad_header, r) == improv::kInvalidRpc);

    auto truncated = good;
    truncated.pop_back();
    assert(parse(truncated, r) == improv::kInvalidRpc);

    // Version 2, and a state packet sent the wrong way
    auto v2 = client_packet(improv::kRpc, {improv::kCurrentStateRequest, 0});
    v2[6] = 2;
    v2.back() = static_cast<uint8_t>(v2.back() + 1);
    assert(parse(v2, r) == improv::kInvalidRpc);
    assert(parse(client_packet(improv::kCurrentState, {improv::kReady}), r) == improv::kInvalidRpc);

    // The RPC's own length disagrees with the packet's
    assert(parse(client_packet(improv::kRpc, {improv::kDeviceInfo, 3}), r) == improv::kInvalidRpc);
    assert(parse(client_packet(improv::kRpc, {improv::kDeviceInfo}), r) == improv::kInvalidRpc);

    // Wi-Fi settings out of bounds or out of shape
    assert(parse(rpc(improv::kWifiSettings, wifi_args("", "secret12")), r) == improv::kInvalidRpc);
    assert(parse(rpc(improv::kWifiSettings, wifi_args(std::string(33, 's'), "x")), r) == improv::kInvalidRpc);
    assert(parse(rpc(improv::kWifiSettings, wifi_args("net", std::string(65, 'p'))), r) == improv::kInvalidRpc);
    assert(parse(rpc(improv::kWifiSettings, {3, 'n', 'e', 't'}), r) == improv::kInvalidRpc);       // no password length
    assert(parse(rpc(improv::kWifiSettings, {3, 'n', 'e', 't', 2, 'p'}), r) == improv::kInvalidRpc); // password cut short
    assert(parse(rpc(improv::kWifiSettings, {9, 'n', 'e', 't', 0}), r) == improv::kInvalidRpc);      // SSID past the end
    assert(parse(rpc(improv::kWifiSettings, {}), r) == improv::kInvalidRpc);
    std::cout << "[PASS] test_damaged_packets" << std::endl;
}

static void test_device_packets() {
    // Current state: the header's bytes sum to 477, version, type, length 1
    // and the state make 484 (0xE4)
    const std::string provisioned = improv::state_packet(improv::kProvisioned);
    assert(provisioned == std::string("IMPROV\x01\x01\x01\x04\xE4\n", 12));
    Received s = receive(improv::state_packet(improv::kProvisioning));
    assert(s.type == improv::kCurrentState && s.data == std::vector<uint8_t>{improv::kProvisioning});
    Received e = receive(improv::error_packet(improv::kUnableToConnect));
    assert(e.type == improv::kErrorState && e.data == std::vector<uint8_t>{improv::kUnableToConnect});

    // Device information and the address after provisioning
    const std::vector<std::string> info = {"kube32", "0.4.0", "ESP32-S3", "node-a"};
    assert(result_strings(receive(improv::rpc_result(improv::kDeviceInfo, info)), improv::kDeviceInfo) == info);
    const std::vector<std::string> url = {"http://10.0.0.7:6443"};
    assert(result_strings(receive(improv::rpc_result(improv::kWifiSettings, url)), improv::kWifiSettings) == url);

    // An empty result ends a network list
    Received end = receive(improv::rpc_result(improv::kScanNetworks, {}));
    assert(result_strings(end, improv::kScanNetworks).empty());

    // Strings past the packet's 255 bytes are cut, never the framing
    const std::vector<std::string> big = {std::string(200, 'a'), std::string(200, 'b'), "c"};
    Received cut = receive(improv::rpc_result(improv::kDeviceInfo, big));
    assert(cut.data.size() == 255);
    const auto strings = result_strings(cut, improv::kDeviceInfo);
    assert(strings.size() == 2 && strings[0] == big[0] && strings[1] == std::string(51, 'b'));
    std::cout << "[PASS] test_device_packets" << std::endl;
}

int main() {
    test_client_requests();
    test_damaged_packets();
    test_device_packets();
    std::cout << "All Improv serial tests passed" << std::endl;
    return 0;
}
