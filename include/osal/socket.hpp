#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#if defined(_WIN32)
#error "Windows is not supported as a native build target."
#else
#include <sys/types.h>
#endif

namespace kube32::osal {

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

    void shutdown(ShutdownMode mode);
    void close();

    bool is_valid() const { return fd_ >= 0; }
    int native_handle() const { return fd_; }

    // Platform networking initialization
    static bool init_networking();
    static void cleanup_networking();

private:
    int fd_{-1};
};

} // namespace kube32::osal
