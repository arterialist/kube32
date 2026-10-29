#pragma once

// Server-side TLS (mbedTLS on the board; the host build has none).
//
// The server holds one ECDSA P-256 key and a self-signed certificate for it.
// Both are created on first use and kept by the caller (the firmware stores
// them in NVS), so the certificate a client trusts stays the same across
// reboots. The certificate names "kube32-apiserver", "localhost" and any
// further DNS names and IP addresses given at creation; a kubeconfig can pin
// it with certificate-authority-data and tls-server-name: kube32-apiserver,
// whatever address the board has.
//
// A connection becomes TLS with Socket::tls_accept(); from then on the
// socket's send/recv/close go through the session.

#include <sys/types.h>

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

// True when this build can serve TLS.
bool tls_available();

namespace detail {
// One TLS session on a socket descriptor (used by osal::Socket)
struct TlsSession;
TlsSession* tls_handshake(TlsServer& server, int fd);   // null on failure
ssize_t tls_send(TlsSession* session, const void* buf, size_t len);
ssize_t tls_recv(TlsSession* session, void* buf, size_t len);   // 0 at the end, -1 with errno EAGAIN on timeout
void tls_free(TlsSession* session);                     // close_notify and free; the caller closes the descriptor
bool tls_pending(TlsSession* session);
} // namespace detail

} // namespace kube32::osal
