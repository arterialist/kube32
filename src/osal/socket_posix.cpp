#include "osal/socket.hpp"
#include "osal/tls.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <memory>
#include <new>

#if defined(ESP_PLATFORM)
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/inet.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <fcntl.h>
#define KUBE32_CLOSE_SOCKET lwip_close
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <thread>
#define KUBE32_CLOSE_SOCKET ::close
#endif

namespace kube32::osal {

Socket::Socket() : fd_(-1) {}
Socket::Socket(int fd) : fd_(fd) {}

Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_), tls_(other.tls_) {
    other.fd_ = -1;
    other.tls_ = nullptr;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        tls_ = other.tls_;
        other.fd_ = -1;
        other.tls_ = nullptr;
    }
    return *this;
}

bool Socket::create(SocketType type) {
    close();
    int sock_type = (type == SocketType::Tcp) ? SOCK_STREAM : SOCK_DGRAM;
    fd_ = ::socket(AF_INET, sock_type, 0);
    if (fd_ < 0) return false;
#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
    int opt = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif
    return true;
}

bool Socket::bind(const char* ip, uint16_t port) {
    if (fd_ < 0) return false;
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!ip || std::strlen(ip) == 0 || std::strcmp(ip, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) return false;
    }
    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "[OSAL] bind to port %u failed: %s\n", static_cast<unsigned>(port), std::strerror(errno));
        return false;
    }
    return true;
}

bool Socket::listen(int backlog) {
    if (fd_ < 0) return false;
    return (::listen(fd_, backlog) == 0);
}

Socket Socket::accept(std::string* client_ip, uint16_t* client_port) {
    if (fd_ < 0) return Socket(-1);
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = ::accept(fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
    if (client_fd < 0) return Socket(-1);

#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
    int opt = 1;
    ::setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

    if (client_ip) {
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        *client_ip = ip_str;
    }
    if (client_port) {
        *client_port = ntohs(client_addr.sin_port);
    }
    return Socket(client_fd);
}

bool Socket::connect(const char* host_or_ip, uint16_t port, uint32_t timeout_ms) {
    if (fd_ < 0) return false;
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host_or_ip, &addr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(host_or_ip);
        if (!he || !he->h_addr_list[0]) return false;
        std::memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
    }

    if (timeout_ms > 0) {
        set_timeout(timeout_ms, timeout_ms);
    }
    return (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
}

int Socket::connect_within(uint32_t ipv4, uint16_t port, uint32_t timeout_ms, mem::FunctionRef<bool()> stop) {
    if (fd_ < 0) return EBADF;
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(ipv4);
    if (!set_nonblocking(true)) return EIO;
    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) return set_nonblocking(false) ? 0 : EIO;
    if (errno != EINPROGRESS) return errno;
    // Writable (or failed) once the handshake ends; waited for in 250 ms
    // slices so that a stopping caller is noticed
    const uint64_t deadline = System::get_uptime_ms() + timeout_ms;
    for (;;) {
        const uint64_t now = System::get_uptime_ms();
        if (now >= deadline) return ETIMEDOUT;
        if (stop()) return ECANCELED;
        const uint64_t slice = (deadline - now < 250) ? deadline - now : 250;
        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(fd_, &wfds);
        FD_SET(fd_, &efds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = static_cast<long>(slice * 1000);
        const int n = ::select(fd_ + 1, nullptr, &wfds, &efds, &tv);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return errno;
        if (n == 0) continue;
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0) return errno;
        if (so_error != 0) return so_error;
        return set_nonblocking(false) ? 0 : EIO;
    }
}

#if defined(ESP_PLATFORM)
namespace {
// One lookup in lwIP's resolver, which runs on the tcpip thread. The query
// is shared by the caller and the resolver's callback; whichever lets go
// last frees it, so a caller that gave up leaves nothing behind.
struct DnsQuery {
    std::atomic<int> refs{2};
    std::atomic<int> state{0};    // 0 pending, 1 found, 2 failed
    uint32_t addr{0};             // host byte order
    char name[DNS_MAX_NAME_LENGTH + 1];
};

void dns_release(DnsQuery* q) {
    if (q->refs.fetch_sub(1) == 1) delete q;
}

void dns_found(const char*, const ip_addr_t* ip, void* arg) {
    auto* q = static_cast<DnsQuery*>(arg);
    if (ip && IP_IS_V4(ip)) {
        q->addr = lwip_ntohl(ip_2_ip4(ip)->addr);
        q->state.store(1);
    } else {
        q->state.store(2);
    }
    dns_release(q);
}

void dns_start(void* arg) {
    auto* q = static_cast<DnsQuery*>(arg);
    ip_addr_t ip;
    const err_t e = dns_gethostbyname_addrtype(q->name, &ip, dns_found, q, LWIP_DNS_ADDRTYPE_IPV4);
    if (e == ERR_OK) {
        dns_found(q->name, &ip, q);   // cached: no callback follows
    } else if (e != ERR_INPROGRESS) {
        q->state.store(2);
        dns_release(q);
    }
}
} // namespace

int Socket::resolve_ipv4(const char* host, uint32_t timeout_ms, mem::FunctionRef<bool()> stop, uint32_t& out) {
    const size_t len = host ? std::strlen(host) : 0;
    if (len == 0 || len > DNS_MAX_NAME_LENGTH) return ENOENT;
    auto* q = new (std::nothrow) DnsQuery;
    if (!q) return ENOMEM;
    std::memcpy(q->name, host, len + 1);
    if (tcpip_callback(dns_start, q) != ERR_OK) {
        delete q;
        return ENOMEM;
    }
    const uint64_t deadline = System::get_uptime_ms() + timeout_ms;
    int rc = 0;
    for (;;) {
        const int st = q->state.load();
        if (st == 1) { out = q->addr; break; }
        if (st == 2) { rc = ENOENT; break; }
        if (System::get_uptime_ms() >= deadline) { rc = ETIMEDOUT; break; }
        if (stop()) { rc = ECANCELED; break; }
        Thread::sleep_ms(10);
    }
    dns_release(q);
    return rc;
}
#else
int Socket::resolve_ipv4(const char* host, uint32_t timeout_ms, mem::FunctionRef<bool()> stop, uint32_t& out) {
    if (!host || !*host) return ENOENT;
    // getaddrinfo cannot be interrupted: it runs on a thread of its own that
    // a caller who gave up leaves to finish
    struct Query {
        std::atomic<int> state{0};    // 0 pending, 1 found, 2 failed
        uint32_t addr{0};
        std::string name;
    };
    auto q = std::make_shared<Query>();
    q->name = host;
    std::thread([q]() {
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        if (::getaddrinfo(q->name.c_str(), nullptr, &hints, &res) == 0 && res) {
            q->addr = ntohl(reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr.s_addr);
            q->state.store(1);
        } else {
            q->state.store(2);
        }
        if (res) ::freeaddrinfo(res);
    }).detach();
    const uint64_t deadline = System::get_uptime_ms() + timeout_ms;
    for (;;) {
        const int st = q->state.load();
        if (st == 1) { out = q->addr; return 0; }
        if (st == 2) return ENOENT;
        if (System::get_uptime_ms() >= deadline) return ETIMEDOUT;
        if (stop()) return ECANCELED;
        Thread::sleep_ms(5);
    }
}
#endif

ssize_t Socket::send(const void* buf, size_t len, int flags) {
    if (fd_ < 0 || !buf) return -1;
    if (tls_) return detail::tls_send(tls_, buf, len);
#if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
#endif
    return ::send(fd_, static_cast<const char*>(buf), len, flags);
}

ssize_t Socket::recv(void* buf, size_t len, int flags) {
    if (fd_ < 0 || !buf) return -1;
    if (tls_) return detail::tls_recv(tls_, buf, len);
    return ::recv(fd_, static_cast<char*>(buf), len, flags);
}

bool Socket::send_all(const void* buf, size_t len) {
    if (fd_ < 0 || !buf) return false;
    const char* ptr = static_cast<const char*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
#if defined(MSG_NOSIGNAL)
        int send_flags = MSG_NOSIGNAL;
#else
        int send_flags = 0;
#endif
        ssize_t sent = tls_ ? detail::tls_send(tls_, ptr, remaining) : ::send(fd_, ptr, remaining, send_flags);
        if (sent <= 0) return false;
        ptr += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

ssize_t Socket::sendto(const void* buf, size_t len, const char* ip, uint16_t port) {
    if (fd_ < 0 || !buf || !ip) return -1;
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) return -1;
    return ::sendto(fd_, static_cast<const char*>(buf), len, 0,
                    reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
}

ssize_t Socket::recvfrom(void* buf, size_t len, std::string* sender_ip, uint16_t* sender_port) {
    if (fd_ < 0 || !buf) return -1;
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);
    ssize_t r = ::recvfrom(fd_, static_cast<char*>(buf), len, 0,
                           reinterpret_cast<struct sockaddr*>(&sender_addr), &addr_len);
    if (r >= 0) {
        if (sender_ip) {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sender_addr.sin_addr, ip_str, sizeof(ip_str));
            *sender_ip = ip_str;
        }
        if (sender_port) {
            *sender_port = ntohs(sender_addr.sin_port);
        }
    }
    return r;
}

bool Socket::set_nonblocking(bool nonblocking) {
    if (fd_ < 0) return false;
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0) return false;
    flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return (fcntl(fd_, F_SETFL, flags) == 0);
}

bool Socket::set_timeout(uint32_t recv_timeout_ms, uint32_t send_timeout_ms) {
    if (fd_ < 0) return false;
    struct timeval tv_recv;
    tv_recv.tv_sec = recv_timeout_ms / 1000;
    tv_recv.tv_usec = (recv_timeout_ms % 1000) * 1000;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv_recv, sizeof(tv_recv));

    struct timeval tv_send;
    tv_send.tv_sec = send_timeout_ms / 1000;
    tv_send.tv_usec = (send_timeout_ms % 1000) * 1000;
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv_send, sizeof(tv_send));
    return true;
}

bool Socket::set_reuse_addr(bool reuse) {
    if (fd_ < 0) return false;
    int opt = reuse ? 1 : 0;
    return (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0);
}

uint16_t Socket::local_port() const {
    if (fd_ < 0) return 0;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

bool Socket::set_tcp_nodelay(bool nodelay) {
    if (fd_ < 0) return false;
    int opt = nodelay ? 1 : 0;
    return (setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == 0);
}

bool Socket::peer_closed() const {
    if (fd_ < 0) return true;
    if (tls_) return false;
    char c;
    const ssize_t n = ::recv(fd_, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
}

void Socket::shutdown(ShutdownMode mode) {
    if (fd_ < 0) return;
    int how = SHUT_RDWR;
    if (mode == ShutdownMode::Read) how = SHUT_RD;
    else if (mode == ShutdownMode::Write) how = SHUT_WR;
    ::shutdown(fd_, how);
}

void Socket::close() {
    if (tls_) {
        detail::tls_free(tls_);
        tls_ = nullptr;
    }
    if (fd_ >= 0) {
        KUBE32_CLOSE_SOCKET(fd_);
        fd_ = -1;
    }
}

bool Socket::tls_pending() const {
    return tls_ && detail::tls_pending(tls_);
}

bool Socket::tls_accept(TlsServer& server) {
    if (fd_ < 0 || tls_) return false;
    tls_ = detail::tls_handshake(server, fd_);
    if (!tls_) close();
    return tls_ != nullptr;
}

bool Socket::init_networking() { return true; }
void Socket::cleanup_networking() {}

} // namespace kube32::osal
