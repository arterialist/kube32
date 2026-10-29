// The console relay: concurrent, streamed HTTP over the serial console
// (include/app/console_relay.hpp), against local TCP servers
#include "app/console_relay.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <csignal>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using kube32::app::ConsoleRelay;
using kube32::app::ConsoleRelayOptions;

namespace {

struct Frame {
    std::string kind;   // READY, DATA, END, ERR
    uint32_t id{0};
    std::string data;   // DATA payload
    int value{0};       // END status, READY slots
    std::string role;   // READY
};

// The console: collects what the relay writes and parses it into frames
class Console {
public:
    bool write(const char* d, size_t n) {
        std::lock_guard<std::mutex> lock(mu_);
        if (refuse_) return false;
        bytes_.append(d, n);
        cv_.notify_all();
        return true;
    }
    void refuse(bool r) {
        std::lock_guard<std::mutex> lock(mu_);
        refuse_ = r;
    }
    std::vector<Frame> frames() {
        std::lock_guard<std::mutex> lock(mu_);
        return parse(bytes_);
    }
    // Waits until `done` holds for the frames written so far
    bool wait(const std::function<bool(const std::vector<Frame>&)>& done, int timeout_ms = 3000) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return done(parse(bytes_)); });
    }

private:
    static std::vector<Frame> parse(const std::string& b) {
        std::vector<Frame> out;
        size_t p = 0;
        while (p < b.size()) {
            assert(b[p] == '\x1e');
            size_t nl = b.find('\n', p);
            assert(nl != std::string::npos);
            std::string head = b.substr(p + 1, nl - p - 1);
            p = nl + 1;
            Frame f;
            char kind[16] = {0};
            unsigned a = 0, c = 0, crc = 0;
            char word[32] = {0};
            sscanf(head.c_str(), "%15s", kind);
            f.kind = kind;
            if (f.kind == "K32_DATA") {
                int got = sscanf(head.c_str(), "K32_DATA %u %u %x", &a, &c, &crc);
                assert(got == 3);
                f.kind = "DATA";
                f.id = a;
                f.data = b.substr(p, c);
                assert(f.data.size() == c);
                assert(ConsoleRelay::crc32(f.data.data(), f.data.size()) == crc);
                p += c;
            } else if (f.kind == "K32_END") {
                int got = sscanf(head.c_str(), "K32_END %u %u", &a, &c);
                assert(got == 2);
                f.kind = "END";
                f.id = a;
                f.value = static_cast<int>(c);
            } else if (f.kind == "K32_READY") {
                int got = sscanf(head.c_str(), "K32_READY %u %31s", &a, word);
                assert(got == 2);
                f.kind = "READY";
                f.value = static_cast<int>(a);
                f.role = word;
            } else if (f.kind == "K32_ERR") {
                int got = sscanf(head.c_str(), "K32_ERR crc %u", &a);
                assert(got == 1);
                f.kind = "ERR";
                f.id = a;
            }
            out.push_back(f);
        }
        return out;
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::string bytes_;
    bool refuse_{false};
};

// The response body of stream `id` so far, and whether it ended (status)
std::string body_of(const std::vector<Frame>& fs, uint32_t id) {
    std::string s;
    for (const auto& f : fs) if (f.kind == "DATA" && f.id == id) s += f.data;
    return s;
}
int end_of(const std::vector<Frame>& fs, uint32_t id) {
    for (const auto& f : fs) if (f.kind == "END" && f.id == id) return f.value;
    return -1;
}

// One TCP listener on 127.0.0.1. Each connection reads a request head, then
// runs `serve` on its own thread.
class Server {
public:
    explicit Server(std::function<void(int)> serve) : serve_(std::move(serve)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int bound = ::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        assert(bound == 0);
        (void)bound;
        socklen_t len = sizeof(a);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        ::listen(fd_, 8);
        accept_ = std::thread([this] {
            while (true) {
                int c = ::accept(fd_, nullptr, nullptr);
                if (c < 0) return;
                if (stopping_) {
                    ::close(c);
                    return;
                }
                conns_.emplace_back([this, c] {
                    std::string head;
                    char ch;
                    while (head.find("\r\n\r\n") == std::string::npos && ::recv(c, &ch, 1, 0) == 1) head.push_back(ch);
                    serve_(c);
                    ::close(c);
                });
            }
        });
    }
    // A connection wakes accept(): closing the listener does not on every OS
    ~Server() {
        stopping_ = true;
        int w = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        ::connect(w, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        accept_.join();
        ::close(w);
        ::close(fd_);
        for (auto& t : conns_) t.join();
    }
    uint16_t port{0};

private:
    std::function<void(int)> serve_;
    std::atomic<bool> stopping_{false};
    int fd_{-1};
    std::thread accept_;
    std::vector<std::thread> conns_;
};

void send_str(int fd, const std::string& s) { ::send(fd, s.data(), s.size(), 0); }

std::string request(const char* path) {
    return std::string("GET ") + path + " HTTP/1.1\r\nHost: board\r\nConnection: close\r\n\r\n";
}

void open(ConsoleRelay& relay, uint32_t id, uint16_t port, const std::string& req) {
    relay.open(id, port, req, ConsoleRelay::crc32(req.data(), req.size()));
}

ConsoleRelayOptions options(size_t slots) {
    ConsoleRelayOptions o;
    o.slots = slots;
    o.chunk = 1024;
    o.role = "control-plane";
    return o;
}

// An event stream: a line every 20 ms until the peer goes away
struct EventServer {
    std::atomic<bool> peer_gone{false};
    Server server{[this](int c) {
        send_str(c, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n");
        for (int i = 0; i < 500; ++i) {
            std::string ev = "data: " + std::to_string(i) + "\n\n";
            if (::send(c, ev.data(), ev.size(), 0) <= 0) {
                peer_gone = true;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }};
};

}  // namespace

static void test_reset_reports_slots_and_role() {
    std::cout << "[RUN] test_reset_reports_slots_and_role" << std::endl;
    Console con;
    ConsoleRelay relay(options(3), [&](const char* d, size_t n) { return con.write(d, n); });
    relay.reset();
    assert(con.wait([](const std::vector<Frame>& fs) { return !fs.empty(); }));
    auto fs = con.frames();
    assert(fs.size() == 1 && fs[0].kind == "READY" && fs[0].value == 3 && fs[0].role == "control-plane");
}

// A response larger than a frame arrives whole, in order, then END 0
static void test_response_spans_frames() {
    std::cout << "[RUN] test_response_spans_frames" << std::endl;
    std::string body(5000, '\0');
    for (size_t i = 0; i < body.size(); ++i) body[i] = static_cast<char>('a' + i % 26);
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 5000\r\nConnection: close\r\n\r\n" + body;
    Server server([&](int c) { send_str(c, response); });
    Console con;
    ConsoleRelay relay(options(2), [&](const char* d, size_t n) { return con.write(d, n); });
    open(relay, 7, server.port, request("/big"));
    assert(con.wait([](const std::vector<Frame>& fs) { return end_of(fs, 7) >= 0; }));
    auto fs = con.frames();
    assert(end_of(fs, 7) == 0);
    assert(body_of(fs, 7) == response);
    size_t data_frames = 0;
    for (const auto& f : fs) if (f.kind == "DATA") { assert(f.data.size() <= 1024); ++data_frames; }
    assert(data_frames >= 5);
}

// An open event stream does not hold up a request beside it, and K32_CLOSE
// closes the stream's connection
static void test_stream_beside_request_and_close() {
    std::cout << "[RUN] test_stream_beside_request_and_close" << std::endl;
    EventServer events;
    Server plain([](int c) { send_str(c, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"); });
    Console con;
    ConsoleRelay relay(options(2), [&](const char* d, size_t n) { return con.write(d, n); });
    open(relay, 1, events.server.port, request("/events"));
    assert(con.wait([](const std::vector<Frame>& fs) { return body_of(fs, 1).find("data: 2\n\n") != std::string::npos; }));
    open(relay, 2, plain.port, request("/"));
    assert(con.wait([](const std::vector<Frame>& fs) { return end_of(fs, 2) == 0; }));
    assert(body_of(con.frames(), 2).find("\r\n\r\nok") != std::string::npos);
    assert(end_of(con.frames(), 1) == -1);   // still streaming
    relay.close(1);
    for (int i = 0; i < 100 && !events.peer_gone; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(events.peer_gone);
    assert(end_of(con.frames(), 1) == -1);   // the gateway closed it: no END
}

// Beyond the slots an open answers 503; a closed stream frees its slot
static void test_slots() {
    std::cout << "[RUN] test_slots" << std::endl;
    EventServer events;
    Console con;
    ConsoleRelay relay(options(1), [&](const char* d, size_t n) { return con.write(d, n); });
    open(relay, 1, events.server.port, request("/events"));
    assert(con.wait([](const std::vector<Frame>& fs) { return !body_of(fs, 1).empty(); }));
    open(relay, 2, events.server.port, request("/events"));
    assert(con.wait([](const std::vector<Frame>& fs) { return end_of(fs, 2) == 0; }));
    assert(body_of(con.frames(), 2).rfind("HTTP/1.1 503 ", 0) == 0);
    relay.close(1);
    open(relay, 3, events.server.port, request("/events"));
    assert(con.wait([](const std::vector<Frame>& fs) { return body_of(fs, 3).rfind("HTTP/1.1 200 ", 0) == 0; }));
    relay.reset();
}

// Nothing listening: 502 and END 0
static void test_refused_port() {
    std::cout << "[RUN] test_refused_port" << std::endl;
    uint16_t port;
    {
        Server s([](int) {});
        port = s.port;
    }
    Console con;
    ConsoleRelay relay(options(2), [&](const char* d, size_t n) { return con.write(d, n); });
    open(relay, 4, port, request("/"));
    assert(con.wait([](const std::vector<Frame>& fs) { return end_of(fs, 4) == 0; }));
    assert(body_of(con.frames(), 4).rfind("HTTP/1.1 502 ", 0) == 0);
}

// A request that does not match its CRC is not sent anywhere
static void test_request_crc_mismatch() {
    std::cout << "[RUN] test_request_crc_mismatch" << std::endl;
    std::atomic<int> served{0};
    Server server([&](int c) { ++served; send_str(c, "HTTP/1.1 204 No Content\r\n\r\n"); });
    Console con;
    ConsoleRelay relay(options(2), [&](const char* d, size_t n) { return con.write(d, n); });
    std::string req = request("/");
    relay.open(9, server.port, req, ConsoleRelay::crc32(req.data(), req.size()) ^ 1u);
    assert(con.wait([](const std::vector<Frame>& fs) { return !fs.empty(); }));
    auto fs = con.frames();
    assert(fs.size() == 1 && fs[0].kind == "ERR" && fs[0].id == 9);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(served == 0);
}

// A frame the console cannot take ends the stream: its connection closes
static void test_console_refuses_frame() {
    std::cout << "[RUN] test_console_refuses_frame" << std::endl;
    EventServer events;
    Console con;
    ConsoleRelay relay(options(2), [&](const char* d, size_t n) { return con.write(d, n); });
    open(relay, 5, events.server.port, request("/events"));
    assert(con.wait([](const std::vector<Frame>& fs) { return !body_of(fs, 5).empty(); }));
    con.refuse(true);
    for (int i = 0; i < 100 && !events.peer_gone; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(events.peer_gone);
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);   // the event servers write to connections the relay closes
    test_reset_reports_slots_and_role();
    test_response_spans_frames();
    test_stream_beside_request_and_close();
    test_slots();
    test_refused_port();
    test_request_crc_mismatch();
    test_console_refuses_frame();
    std::cout << "[PASS] console relay" << std::endl;
    return 0;
}
