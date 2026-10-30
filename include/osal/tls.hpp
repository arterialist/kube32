#pragma once

// TLS: mbedTLS on the board, OpenSSL on the host when the build has it.
//
// Server side. The server holds one ECDSA P-256 key and a self-signed
// certificate for it. Both are created on first use and kept by the caller
// (the firmware stores them in NVS), so the certificate a client trusts stays
// the same across reboots. The certificate names "kube32-apiserver",
// "localhost" and any further DNS names and IP addresses given at creation; a
// kubeconfig can pin it with certificate-authority-data and tls-server-name:
// kube32-apiserver, whatever address the board has. A connection becomes TLS
// with Socket::tls_accept(); from then on the socket's send/recv/close go
// through the session.
//
// Client side. A server must present a certificate chain that leads to a
// trusted root and names the host the connection is for (a DNS name or an
// IPv4 address). The roots are the firmware's certificate bundle on the board
// (esp_crt_bundle, the common CA subset of Mozilla's list) and the system's
// store on the host, plus any given to TlsClient::create. The board checks
// validity dates once its wall clock is set. A connected socket becomes TLS
// with Socket::tls_connect().

#include "mem/function_ref.hpp"

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace kube32::osal {

class TlsServer {
public:
    // Reads a stored key and certificate (DER); false when there are none.
    using Loader = std::function<bool(std::string& key_der, std::string& cert_der)>;
    // Stores a newly created key and certificate.
    using Saver = std::function<void(const std::string& key_der, const std::string& cert_der)>;

    // Null (with `error` set) when the build has no TLS or the key or
    // certificate cannot be loaded or made.
    static std::unique_ptr<TlsServer> create(const std::vector<std::string>& dns_names,
                                             const std::vector<std::string>& ip_addresses,
                                             const Loader& load, const Saver& save, std::string& error);
    ~TlsServer();

    // The certificate as PEM, for kubeconfig certificate-authority-data.
    const std::string& certificate_pem() const;

    struct Impl;
    Impl& impl() { return *impl_; }

private:
    TlsServer();
    std::unique_ptr<Impl> impl_;
};

// Trust settings every client connection shares
class TlsClient {
public:
    // `extra_roots_pem`: further trusted roots (PEM, one or more
    // certificates), a private CA say. Null (with `error` set) when the build
    // has no TLS or they cannot be read.
    static std::unique_ptr<TlsClient> create(const std::string& extra_roots_pem, std::string& error);
    ~TlsClient();

    struct Impl;
    Impl& impl() { return *impl_; }

private:
    TlsClient();
    std::unique_ptr<Impl> impl_;
};

// Outcome of a client handshake
enum class TlsStatus {
    Ok,
    Certificate,   // the server's certificate was not accepted
    Handshake,     // the handshake failed otherwise (protocol, alert, connection closed)
    Timeout,
    Cancelled,     // `stop` asked to give up
};

// True when this build has TLS (both sides).
bool tls_available();

namespace detail {
// One TLS session on a socket descriptor (used by osal::Socket)
struct TlsSession;
TlsSession* tls_handshake(TlsServer& server, int fd);   // null on failure
// Client handshake for `host`, ending at `deadline_ms` (System uptime); the
// descriptor's receive timeout sets how often `stop` is asked. Null on
// failure, with `status` and `why` (optional) saying what failed.
TlsSession* tls_connect(TlsClient& client, int fd, const char* host, uint64_t deadline_ms, mem::FunctionRef<bool()> stop,
                        TlsStatus& status, std::string* why);
ssize_t tls_send(TlsSession* session, const void* buf, size_t len);
ssize_t tls_recv(TlsSession* session, void* buf, size_t len);   // 0 at the end, -1 with errno EAGAIN on timeout
void tls_free(TlsSession* session);                     // close_notify and free; the caller closes the descriptor
bool tls_pending(TlsSession* session);
} // namespace detail

} // namespace kube32::osal
