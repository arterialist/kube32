#pragma once

#include "mem/function_ref.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kube32::net {

// A worker that has not heard its API server for this long looks for it at
// another address: its home address, or one its control plane announces.
constexpr uint64_t kApiSilenceBeforeFollowMs = 30000;
// HELLO frames come every 3 s: an announcement older than this is from a
// peer that is gone or no longer announces.
constexpr uint64_t kApiAnnouncementFreshMs = 15000;
// While the API server is silent, the other addresses are tried at most this
// often, each with one GET /version of at most kApiProbeTimeoutMs.
constexpr uint64_t kApiProbeIntervalMs = 10000;
constexpr uint32_t kApiProbeTimeoutMs = 1500;
// Announced addresses tried in one check, newest announcement first
constexpr size_t kApiProbesPerPoll = 4;

// The latest HELLO of a peer that announces an API server address
struct ApiAnnouncement {
    std::string node_name;
    std::string address;
    uint64_t heard_ms{0};
};

// Which API server address a worker dials. `home` is the address it was
// given (K32_CMD:API, --join) and `current` the one it dials now. After
// kApiSilenceBeforeFollowMs without an answer the worker tries its home
// address, then the addresses in the fresh announcements of its control
// plane, newest first, and dials the first that answers as a kube32 API server. Its
// control plane is the peer that announces the address the worker dials
// while that address answers; until one has, any peer's announcement counts.
class ApiFollower {
public:
    ApiFollower(std::string home, std::string current, std::string control_plane = std::string())
        : home_(std::move(home)), current_(std::move(current)), control_plane_(std::move(control_plane)) {
        if (current_.empty()) current_ = home_;
    }

    const std::string& home() const { return home_; }
    const std::string& current() const { return current_; }
    // The node name of the worker's control plane, "" while none is known
    const std::string& control_plane() const { return control_plane_; }

    // One check, every few seconds. `answered_ms` is the uptime of the last
    // answer from current() (0: none), `quiet_since_ms` the uptime from
    // which the silence counts when that is later. `answers` probes an
    // address. Returns the address to dial from now on, or "" to stay.
    std::string poll(uint64_t now_ms, uint64_t answered_ms, uint64_t quiet_since_ms, const std::vector<ApiAnnouncement>& heard,
                     mem::FunctionRef<bool(const std::string&)> answers);

private:
    std::string home_;
    std::string current_;
    std::string control_plane_;
    std::vector<std::string> noted_;   // refusals logged since the last switch: each is logged once
    uint64_t next_probe_ms_{0};
};

// Whether an answer to GET /version comes from a kube32 API server
bool is_kube32_version(int status, std::string_view body);

// GET /version at host:port in at most timeout_ms: true when a kube32 API
// server answers
bool api_server_answers(const std::string& host, uint16_t port, uint32_t timeout_ms = kApiProbeTimeoutMs);

// A host that names the machine it is used on: localhost or an address of
// 127.0.0.0/8. A worker that dials one shares its host with the control plane.
inline bool is_loopback_host(std::string_view host) {
    if (host == "localhost") return true;
    return host.size() > 4 && host.substr(0, 4) == "127." && host.find_first_not_of("0123456789.", 4) == std::string_view::npos;
}

} // namespace kube32::net
