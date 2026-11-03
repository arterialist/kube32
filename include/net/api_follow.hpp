#pragma once

#include <cstdint>
#include <string_view>

namespace kube32::net {

// A worker that has not heard its API server for this long and learns another
// address from the control plane's HELLO frames dials that address instead.
constexpr uint64_t kApiSilenceBeforeFollowMs = 30000;

// Whether a worker switches to the API address `announced`. `heard_ms` is the
// uptime of the last answer from the API server (or of the start, before the
// first one) and `current_host` the host the worker dials now. A switch needs
// more than kApiSilenceBeforeFollowMs of silence and an announced address
// that is not empty and not the one already in use.
inline bool should_follow_announced_api(uint64_t now_ms, uint64_t heard_ms, std::string_view announced, std::string_view current_host) {
    if (announced.empty() || announced == current_host) return false;
    return now_ms > heard_ms && now_ms - heard_ms > kApiSilenceBeforeFollowMs;
}

// A host that names the machine it is used on: localhost or an address of
// 127.0.0.0/8. A worker that dials one shares its host with the control plane.
inline bool is_loopback_host(std::string_view host) {
    if (host == "localhost") return true;
    return host.size() > 4 && host.substr(0, 4) == "127." && host.find_first_not_of("0123456789.", 4) == std::string_view::npos;
}

} // namespace kube32::net
