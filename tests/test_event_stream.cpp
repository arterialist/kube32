// Event streams of the Ingress (kube32.io/event-stream): channel messages
// from two publishing threads, frames in bursts of four and tokens between
// them, reach an SSE client whole and in order while the client stalls,
// with the hub's socket send buffer as small as a board's.

#include "app/ingress_controller.hpp"
#include "ipc/channel.hpp"
#include "osal/socket.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace kube32;

static constexpr int kFrames = 240;   // 60 bursts of 4
static constexpr int kTokens = 160;

struct Received {
    std::vector<int> frames, tokens;
    int bad{0};
};

// Reads the stream until it has every event or the deadline passes; stalls
// for `stall_ms` after the 20th event, as a client on a weak link does
static Received read_stream(int fd, uint32_t stall_ms) {
    Received r;
    std::string buf;
    char chunk[512];
    bool stalled = false;
    const uint64_t deadline = osal::System::get_uptime_ms() + 20000;
    while (static_cast<int>(r.frames.size() + r.tokens.size()) < kFrames + kTokens && osal::System::get_uptime_ms() < deadline) {
        if (!stalled && r.frames.size() + r.tokens.size() >= 20) {
            stalled = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
        }
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        buf.append(chunk, static_cast<size_t>(n));
        size_t end;
        while ((end = buf.find("\n\n")) != std::string::npos) {
            const std::string ev = buf.substr(0, end);
            buf.erase(0, end + 2);
            const size_t d = ev.find("data: ");
            if (d == std::string::npos) continue;   // a comment or the retry line
            const std::string data = ev.substr(d + 6);
            if (data[0] == 'F') r.frames.push_back(std::atoi(data.c_str() + 1));
            else if (ev.rfind("event: token\n", 0) == 0) r.tokens.push_back(std::atoi(data.c_str() + data.find("\"n\":") + 4));
            else r.bad++;
        }
    }
    return r;
}

static bool in_order(const std::vector<int>& v, int n) {
    if (static_cast<int>(v.size()) != n) return false;
    for (int i = 0; i < n; ++i) {
        if (v[i] != i) return false;
    }
    return true;
}

static void test_bursts(uint32_t stall_ms) {
    std::cout << "=== frames and tokens from two threads, client stalls " << stall_ms << " ms ===" << std::endl;
    auto hub = std::make_shared<app::SseHub>();
    assert(hub->start());
    int fds[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    int small = 4096;   // about what lwIP gives a TCP connection to send
    ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    ::setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    const uint32_t frame_hash = ipc::fnv1a_32("test.frame"), text_hash = ipc::fnv1a_32("test.text");
    app::SseHub::Subscription sub;
    sub.stream_id = 1;
    sub.topics = {frame_hash, text_hash};
    hub->add(osal::Socket(fds[0]), sub);

    // As the Ingress subscribes its event streams
    auto& ch = ipc::ChannelManager::instance();
    const uint32_t s1 = ch.subscribe("test.frame", [hub, frame_hash](std::string_view, uint32_t, const uint8_t* d, size_t n) {
        hub->broadcast(frame_hash, "", std::string_view(reinterpret_cast<const char*>(d), n));
    });
    const uint32_t s2 = ch.subscribe("test.text", [hub, text_hash](std::string_view, uint32_t, const uint8_t* d, size_t n) {
        hub->broadcast(text_hash, "token", std::string_view(reinterpret_cast<const char*>(d), n));
    });
    const uint64_t dropped_before = ch.stats().dispatch_dropped.load();

    std::thread frames([&] {
        for (int i = 0; i < kFrames; i += 4) {
            for (int k = i; k < i + 4; ++k) {
                std::string f = "F" + std::to_string(k) + " ";
                f.resize(200, 'a' + (k % 26));
                ch.publish("test/draw", "test.frame", reinterpret_cast<const uint8_t*>(f.data()), f.size());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    });
    std::thread tokens([&] {
        for (int i = 0; i < kTokens; ++i) {
            const std::string t = "{\"t\":\"token\",\"s\":7,\"n\":" + std::to_string(i) + ",\"x\":\" word\"}";
            ch.publish("test/teller", "test.text", reinterpret_cast<const uint8_t*>(t.data()), t.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(11));
        }
    });
    Received r = read_stream(fds[1], stall_ms);
    frames.join();
    tokens.join();
    std::cout << "frames " << r.frames.size() << "/" << kFrames << ", tokens " << r.tokens.size() << "/" << kTokens << ", unknown " << r.bad
              << "; hub: " << hub->events_received() << " received, " << hub->events_sent() << " sent, " << hub->events_dropped()
              << " dropped; channel dispatch dropped " << (ch.stats().dispatch_dropped.load() - dropped_before) << std::endl;
    assert(in_order(r.frames, kFrames));
    assert(in_order(r.tokens, kTokens));
    assert(r.bad == 0);
    assert(hub->events_dropped() == 0 && ch.stats().dispatch_dropped.load() == dropped_before);
    assert(hub->count() == 1);   // the stalled client stays connected
    ch.unsubscribe(s1);
    ch.unsubscribe(s2);
    hub->stop();
    ::close(fds[1]);
}

int main() {
    auto& ch = ipc::ChannelManager::instance();
    assert(ch.init(0, 7475));
    assert(ch.start_background(60000, 262144, false));
    test_bursts(0);
    test_bursts(800);
    ch.shutdown();
    std::cout << "All event stream tests passed" << std::endl;
    return 0;
}
