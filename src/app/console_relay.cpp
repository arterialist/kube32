#include "app/console_relay.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>
#if defined(ESP_PLATFORM)
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <fcntl.h>
#include <unistd.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace kube32::app {

namespace {

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

bool send_all(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, data + off, len - off, kSendFlags);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

uint32_t ConsoleRelay::crc32(const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

ConsoleRelay::ConsoleRelay(ConsoleRelayOptions opts, FrameWriter write)
    : opts_(std::move(opts)), write_(std::move(write)) {
    if (opts_.slots == 0) opts_.slots = 1;
    if (opts_.chunk == 0) opts_.chunk = 512;
}

ConsoleRelay::~ConsoleRelay() {
    {
        osal::LockGuard<osal::Mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.is_joinable()) thread_.join();
}

void ConsoleRelay::reset() { push({Kind::Reset, 0, 0, {}}); }

void ConsoleRelay::open(uint32_t id, uint16_t port, std::string request, uint32_t crc) {
    if (crc32(request.data(), request.size()) != crc) {
        char head[40];
        int n = snprintf(head, sizeof(head), "\x1e" "K32_ERR crc %u\n", static_cast<unsigned>(id));
        write_(head, static_cast<size_t>(n));
        return;
    }
    push({Kind::Open, id, port, std::move(request)});
}

void ConsoleRelay::close(uint32_t id) { push({Kind::Close, id, 0, {}}); }

void ConsoleRelay::push(Command c) {
    {
        osal::LockGuard<osal::Mutex> lock(mu_);
        if (stop_) return;
        inbox_.push_back(std::move(c));
        if (!started_) {
            started_ = true;
            osal::Thread::Config cfg;
            cfg.name = "k32_relay";
            cfg.stack_size = opts_.stack_size;
            cfg.priority = 4;
            cfg.stack_in_psram = opts_.stack_in_psram;
            if (!thread_.start(&ConsoleRelay::entry, this, cfg)) {
                started_ = false;
                inbox_.clear();
                return;
            }
        }
    }
    cv_.notify_one();
}

void ConsoleRelay::entry(void* self) { static_cast<ConsoleRelay*>(self)->run(); }

void ConsoleRelay::run() {
    std::vector<char> buf(opts_.chunk);
    std::deque<Command> commands;
    while (true) {
        {
            osal::UniqueLock<osal::Mutex> lock(mu_);
            if (streams_.empty()) cv_.wait(lock, [this] { return stop_ || !inbox_.empty(); });
            if (stop_) break;
            commands.swap(inbox_);
        }
        for (auto& c : commands) handle(c);
        commands.clear();
        if (streams_.empty()) continue;

        // A short timeout: new commands wait at most this long
        fd_set rd;
        FD_ZERO(&rd);
        int maxfd = -1;
        for (const auto& s : streams_) {
            FD_SET(s.fd, &rd);
            if (s.fd > maxfd) maxfd = s.fd;
        }
        timeval tv{};
        tv.tv_usec = 50 * 1000;
        int ready = ::select(maxfd + 1, &rd, nullptr, nullptr, &tv);
        const uint64_t now = osal::Thread::get_time_ms();
        for (size_t i = 0; i < streams_.size();) {
            Stream& s = streams_[i];
            if (ready > 0 && FD_ISSET(s.fd, &rd)) {
                ssize_t n = ::recv(s.fd, buf.data(), buf.size(), 0);
                if (n > 0) {
                    s.last_ms = now;
                    if (!data_frame(s.id, buf.data(), static_cast<size_t>(n))) {
                        end_stream(i, true);
                        continue;
                    }
                } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    end_stream(i, false);
                    continue;
                }
            } else if (now - s.last_ms > opts_.idle_timeout_ms) {
                end_stream(i, true);
                continue;
            }
            ++i;
        }
    }
    for (const auto& s : streams_) ::close(s.fd);
    streams_.clear();
}

void ConsoleRelay::handle(Command& c) {
    switch (c.kind) {
    case Kind::Reset: {
        while (!streams_.empty()) drop_stream(streams_.size() - 1);
        char head[64];
        int n = snprintf(head, sizeof(head), "\x1e" "K32_READY %u %s\n", static_cast<unsigned>(opts_.slots), opts_.role.c_str());
        send_frame(head, static_cast<size_t>(n), nullptr, 0);
        break;
    }
    case Kind::Open:
        for (size_t i = 0; i < streams_.size(); ++i) {
            if (streams_[i].id == c.id) {
                drop_stream(i);
                break;
            }
        }
        if (streams_.size() >= opts_.slots) {
            respond(c.id, "503 Service Unavailable\r\nRetry-After: 1");
            break;
        }
        start_stream(c.id, c.port, c.request);
        break;
    case Kind::Close:
        for (size_t i = 0; i < streams_.size(); ++i) {
            if (streams_[i].id == c.id) {
                drop_stream(i);
                break;
            }
        }
        break;
    }
}

void ConsoleRelay::start_stream(uint32_t id, uint16_t port, const std::string& request) {
    int fd = connect_to(port);
    if (fd < 0) {
        respond(id, "502 Bad Gateway");
        return;
    }
    if (!send_all(fd, request.data(), request.size())) {
        ::close(fd);
        respond(id, "502 Bad Gateway");
        return;
    }
    streams_.push_back({id, fd, osal::Thread::get_time_ms()});
}

// The server's side is done: the response ended (cut: it was cut short)
void ConsoleRelay::end_stream(size_t index, bool cut) {
    const uint32_t id = streams_[index].id;
    drop_stream(index);
    end_frame(id, cut);
}

// The gateway's side is done: no END frame
void ConsoleRelay::drop_stream(size_t index) {
    ::close(streams_[index].fd);
    streams_.erase(streams_.begin() + static_cast<std::ptrdiff_t>(index));
}

int ConsoleRelay::connect_to(uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char service[8];
    snprintf(service, sizeof(service), "%u", static_cast<unsigned>(port));
    if (::getaddrinfo(opts_.host.c_str(), service, &hints, &res) != 0 || !res) return -1;
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        return -1;
    }
#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    ::freeaddrinfo(res);
    if (rc != 0 && errno == EINPROGRESS) {
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(fd, &wr);
        timeval tv{};
        tv.tv_sec = opts_.connect_timeout_ms / 1000;
        tv.tv_usec = (opts_.connect_timeout_ms % 1000) * 1000;
        int err = 0;
        socklen_t len = sizeof(err);
        if (::select(fd + 1, nullptr, &wr, nullptr, &tv) == 1 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) rc = 0;
    }
    if (rc != 0) {
        ::close(fd);
        return -1;
    }
    ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    timeval send_timeout{};
    send_timeout.tv_sec = 5;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    return fd;
}

bool ConsoleRelay::data_frame(uint32_t id, const char* data, size_t len) {
    char head[48];
    int n = snprintf(head, sizeof(head), "\x1e" "K32_DATA %u %u %08x\n", static_cast<unsigned>(id),
                     static_cast<unsigned>(len), static_cast<unsigned>(crc32(data, len)));
    return send_frame(head, static_cast<size_t>(n), data, len);
}

void ConsoleRelay::end_frame(uint32_t id, bool cut) {
    char head[32];
    int n = snprintf(head, sizeof(head), "\x1e" "K32_END %u %d\n", static_cast<unsigned>(id), cut ? 1 : 0);
    send_frame(head, static_cast<size_t>(n), nullptr, 0);
}

// A whole response from the relay itself
void ConsoleRelay::refuse(uint32_t id, const char* status) {
    respond(id, status);
}

void ConsoleRelay::respond(uint32_t id, const char* status) {
    std::string r = std::string("HTTP/1.1 ") + status + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    if (data_frame(id, r.data(), r.size())) end_frame(id, false);
}

bool ConsoleRelay::send_frame(const char* head, size_t head_len, const char* data, size_t len) {
    frame_.assign(head, head_len);
    if (len) frame_.append(data, len);
    return write_(frame_.data(), frame_.size());
}

}  // namespace kube32::app
