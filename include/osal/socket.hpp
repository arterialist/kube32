#pragma once

#include "mem/function_ref.hpp"

#include <cstdint>
#include <cstddef>
#include <string>

#if defined(_WIN32)
#error "Windows is not supported as a native build target."
#else
#include <sys/types.h>
#endif

namespace kube32::osal {

class TlsServer;
class TlsClient;
enum class TlsStatus;
namespace detail { struct TlsSession; }

enum class SocketType {
    Tcp,
    Udp
};

enum class ShutdownMode {
    Read,
    Write,
    Both
};

class Socket {
public:
    Socket();
    explicit Socket(int fd);
    ~Socket();

    // Movable, non-copyable (RAII ownership)
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Socket lifecycle
    bool create(SocketType type);
    bool bind(const char* ip, uint16_t port);
    bool listen(int backlog = 10);
    Socket accept(std::string* client_ip = nullptr, uint16_t* client_port = nullptr);
    bool connect(const char* host_or_ip, uint16_t port, uint32_t timeout_ms = 5000);
    // Connects to an IPv4 address (host byte order) in at most timeout_ms;
    // `stop` is asked every 250 ms whether to give up. Returns 0 with the
    // socket blocking again, or the errno of the failure (ETIMEDOUT,
    // ECONNREFUSED, ECONNRESET, EHOSTUNREACH, ENETUNREACH, ECANCELED, ...).
    int connect_within(uint32_t ipv4, uint16_t port, uint32_t timeout_ms, mem::FunctionRef<bool()> stop);

    // Stream operations (TCP)
    ssize_t send(const void* buf, size_t len, int flags = 0);
    ssize_t recv(void* buf, size_t len, int flags = 0);

    // Exact length helper for streaming serializers
    bool send_all(const void* buf, size_t len);

    // Datagram operations (UDP)
    ssize_t sendto(const void* buf, size_t len, const char* ip, uint16_t port);
    ssize_t recvfrom(void* buf, size_t len, std::string* sender_ip, uint16_t* sender_port);

    // Socket options
    bool set_nonblocking(bool nonblocking);
    bool set_timeout(uint32_t recv_timeout_ms, uint32_t send_timeout_ms);
    bool set_reuse_addr(bool reuse);
    bool set_tcp_nodelay(bool nodelay);
    // The peer closed its side and nothing is left to read: a request that
    // was read in full but whose client has gone. Does not block. Always
    // false on TLS sessions (the descriptor carries ciphertext).
    bool peer_closed() const;
    // Port the socket is bound to (0 when unknown)
    uint16_t local_port() const;

    void shutdown(ShutdownMode mode);
    void close();

    // Server side of a TLS handshake on this connected socket (osal/tls.hpp):
    // afterwards send, recv and close go through the session. On failure
    // the socket is closed.
    bool tls_accept(TlsServer& server);
    // Client side of a TLS handshake on this connected socket for `host`
    // (the name the server's certificate must carry) in at most timeout_ms;
    // `stop` is asked every 250 ms whether to give up. On failure the socket
    // is closed and `why` (optional) says what failed.
    TlsStatus tls_connect(TlsClient& client, const std::string& host, uint32_t timeout_ms, mem::FunctionRef<bool()> stop,
                          std::string* why = nullptr);
    bool is_tls() const { return tls_ != nullptr; }
    // TLS data already read from the descriptor and not yet handed out: the
    // descriptor may not turn readable for it.
    bool tls_pending() const;

    bool is_valid() const { return fd_ >= 0; }
    int native_handle() const { return fd_; }
    // Gives up ownership of the descriptor without closing it (plain
    // sockets only: a TLS session cannot outlive its Socket).
    int release() { int fd = fd_; fd_ = -1; return fd; }

    // Resolves a host name to an IPv4 address (host byte order) in at most
    // timeout_ms: lwIP's resolver on the boards, the system's on the host.
    // `stop` is asked every few ms whether to give up. Returns 0, ENOENT (no
    // such name, or no answer from the name server), ETIMEDOUT or ECANCELED.
    static int resolve_ipv4(const char* host, uint32_t timeout_ms, mem::FunctionRef<bool()> stop, uint32_t& out);

    // Platform networking initialization
    static bool init_networking();
    static void cleanup_networking();

private:
    int fd_{-1};
    detail::TlsSession* tls_{nullptr};
};

} // namespace kube32::osal
