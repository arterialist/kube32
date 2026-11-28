#include "net/api_follow.hpp"

#include "net/network_policy.hpp"
#include "osal/osal.hpp"

#include <algorithm>
#include <cstdio>

namespace kube32::net {

// `then` is at most `window` ms before `now` (a clock that reads before it counts as within)
static bool within(uint64_t now_ms, uint64_t then_ms, uint64_t window_ms) {
    return now_ms <= then_ms || now_ms - then_ms <= window_ms;
}

std::string ApiFollower::poll(uint64_t now_ms, uint64_t answered_ms, uint64_t quiet_since_ms, const std::vector<ApiAnnouncement>& heard,
                              mem::FunctionRef<bool(const std::string&)> answers) {
    auto know = [this](const std::string& name, const std::string& at) {
        if (name == control_plane_) return;
        control_plane_ = name;
        std::printf("[NET] the control plane is %s, at %s\n", name.c_str(), at.c_str());
        std::fflush(stdout);
    };
    // While the API server answers, the peer that announces its address is the control plane
    if (answered_ms && within(now_ms, answered_ms, kApiSilenceBeforeFollowMs)) {
        const ApiAnnouncement* named = nullptr;
        for (const auto& a : heard) {
            if (a.address != current_ || !within(now_ms, a.heard_ms, kApiAnnouncementFreshMs)) continue;
            if (!named || a.heard_ms > named->heard_ms) named = &a;
        }
        if (named) know(named->node_name, current_);
        return {};
    }
    const uint64_t since = answered_ms > quiet_since_ms ? answered_ms : quiet_since_ms;
    if (within(now_ms, since, kApiSilenceBeforeFollowMs) || now_ms < next_probe_ms_) return {};
    next_probe_ms_ = now_ms + kApiProbeIntervalMs;

    auto note = [this](const std::string& key, const std::string& line) {
        for (const auto& k : noted_) {
            if (k == key) return;
        }
        if (noted_.size() >= kApiProbesPerPoll * 2) noted_.erase(noted_.begin());
        noted_.push_back(key);
        std::printf("[NET] %s\n", line.c_str());
        std::fflush(stdout);
    };
    // Each announced address once, from its latest announcement, newest first
    std::vector<const ApiAnnouncement*> candidates;
    for (const auto& a : heard) {
        if (a.address.empty() || a.address == current_ || a.address == home_ || !within(now_ms, a.heard_ms, kApiAnnouncementFreshMs)) continue;
        if (!control_plane_.empty() && a.node_name != control_plane_) {
            note(a.node_name + "=" + a.address, a.node_name + " announces " + a.address + "; the control plane is " + control_plane_ + ": not followed");
            continue;
        }
        bool seen = false;
        for (auto& c : candidates) {
            if (c->address != a.address) continue;
            seen = true;
            if (a.heard_ms > c->heard_ms) c = &a;
        }
        if (!seen) candidates.push_back(&a);
    }
    std::sort(candidates.begin(), candidates.end(), [](const ApiAnnouncement* x, const ApiAnnouncement* y) { return x->heard_ms > y->heard_ms; });
    auto switch_to = [this](const std::string& address) {
        current_ = address;
        noted_.clear();
        next_probe_ms_ = 0;
        return current_;
    };
    if (!home_.empty() && home_ != current_ && answers(home_)) return switch_to(home_);
    // A peer that announces an address where nothing answers does not hide
    // the others: up to kApiProbesPerPoll addresses are tried
    for (size_t i = 0; i < candidates.size() && i < kApiProbesPerPoll; ++i) {
        const ApiAnnouncement& a = *candidates[i];
        if (answers(a.address)) {
            if (control_plane_.empty()) know(a.node_name, a.address);
            return switch_to(a.address);
        }
        note(a.address, a.node_name + " announces " + a.address + ", where no API server answers: not followed");
    }
    return {};
}

bool is_kube32_version(int status, std::string_view body) {
    return status == 200 && body.find("\"kube32Version\"") != std::string_view::npos;
}

bool api_server_answers(const std::string& host, uint16_t port, uint32_t timeout_ms) {
    const uint64_t deadline = osal::System::get_uptime_ms() + timeout_ms;
    auto left = [deadline] {
        const uint64_t now = osal::System::get_uptime_ms();
        return now < deadline ? static_cast<uint32_t>(deadline - now) : 0u;
    };
    auto never = [] { return false; };
    uint32_t ip = 0;
    if (!parse_ipv4(host, ip) && osal::Socket::resolve_ipv4(host.c_str(), left(), never, ip) != 0) return false;
    osal::Socket sock;
    if (!sock.create(osal::SocketType::Tcp) || left() == 0 || sock.connect_within(ip, port, left(), never) != 0) return false;
    if (left() == 0) return false;
    sock.set_timeout(left(), left());
    const std::string req = "GET /version HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    if (!sock.send_all(req.data(), req.size())) return false;
    // The answer is a few hundred bytes; the status and the body are read until the server closes
    char buf[768];
    size_t len = 0;
    while (len < sizeof(buf) && left() > 0) {
        const ssize_t n = sock.recv(buf + len, sizeof(buf) - len);
        if (n <= 0) break;
        len += static_cast<size_t>(n);
    }
    const std::string_view answer(buf, len);
    int status = 0;
    if (answer.size() > 12 && answer.substr(0, 7) == "HTTP/1.") {
        for (size_t i = 9; i < 12 && answer[i] >= '0' && answer[i] <= '9'; ++i) status = status * 10 + (answer[i] - '0');
    }
    const size_t body = answer.find("\r\n\r\n");
    return body != std::string_view::npos && is_kube32_version(status, answer.substr(body + 4));
}

} // namespace kube32::net
