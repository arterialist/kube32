// Server-side TLS for host builds (OpenSSL), the counterpart of the mbedTLS
// implementation the firmware uses (tls_mbedtls.cpp): same certificate shape,
// same session interface for osal::Socket.

#if !defined(ESP_PLATFORM) && defined(KUBE32_HAS_OPENSSL)

#include "osal/tls.hpp"

#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace kube32::osal {

struct TlsServer::Impl {
    SSL_CTX* ctx{nullptr};
    EVP_PKEY* key{nullptr};
    X509* cert{nullptr};
    std::string pem;
};

namespace {

std::string openssl_error() {
    unsigned long e = ERR_get_error();
    if (!e) return "unknown error";
    char buf[160];
    ERR_error_string_n(e, buf, sizeof(buf));
    return buf;
}

bool add_extension(X509* cert, int nid, const std::string& value) {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
    if (!ext) return false;
    bool ok = X509_add_ext(cert, ext, -1) == 1;
    X509_EXTENSION_free(ext);
    return ok;
}

// Self-signed certificate valid until 2050 for server authentication, its own
// trust anchor (as the firmware makes it)
X509* make_certificate(EVP_PKEY* key, const std::vector<std::string>& dns, const std::vector<std::string>& ips) {
    X509* cert = X509_new();
    if (!cert) return nullptr;
    unsigned char serial[16];
    RAND_bytes(serial, sizeof(serial));
    serial[0] &= 0x7F;
    BIGNUM* bn = BN_bin2bn(serial, sizeof(serial), nullptr);
    BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert));
    BN_free(bn);
    std::string san;
    for (const auto& d : dns) san += (san.empty() ? "DNS:" : ",DNS:") + d;
    for (const auto& ip : ips) san += (san.empty() ? "IP:" : ",IP:") + ip;
    X509_NAME* name = X509_get_subject_name(cert);
    bool ok = X509_set_version(cert, 2) == 1 && X509_gmtime_adj(X509_getm_notBefore(cert), 0) &&
              ASN1_TIME_set_string(X509_getm_notAfter(cert), "20501231235959Z") == 1 && X509_set_pubkey(cert, key) == 1 &&
              X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("kube32"), -1, -1, 0) == 1 &&
              X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("kube32-apiserver"), -1, -1, 0) == 1 &&
              X509_set_issuer_name(cert, name) == 1 &&
              add_extension(cert, NID_basic_constraints, "critical,CA:TRUE,pathlen:0") &&
              add_extension(cert, NID_key_usage, "critical,digitalSignature,keyCertSign") &&
              add_extension(cert, NID_ext_key_usage, "serverAuth") &&
              add_extension(cert, NID_subject_key_identifier, "hash") &&
              (san.empty() || add_extension(cert, NID_subject_alt_name, san)) &&
              X509_sign(cert, key, EVP_sha256()) > 0;
    if (!ok) { X509_free(cert); return nullptr; }
    return cert;
}

} // namespace

TlsServer::TlsServer() : impl_(std::make_unique<Impl>()) {}

TlsServer::~TlsServer() {
    if (impl_->ctx) SSL_CTX_free(impl_->ctx);
    if (impl_->cert) X509_free(impl_->cert);
    if (impl_->key) EVP_PKEY_free(impl_->key);
}

const std::string& TlsServer::certificate_pem() const { return impl_->pem; }

bool tls_available() { return true; }

std::unique_ptr<TlsServer> TlsServer::create(const std::vector<std::string>& dns_names, const std::vector<std::string>& ip_addresses,
                                             const Loader& load, const Saver& save, std::string& error) {
    std::unique_ptr<TlsServer> server(new TlsServer());
    Impl& impl = *server->impl_;
    std::string key_der, cert_der;
    if (load && load(key_der, cert_der) && !key_der.empty() && !cert_der.empty()) {
        const unsigned char* kp = reinterpret_cast<const unsigned char*>(key_der.data());
        const unsigned char* cp = reinterpret_cast<const unsigned char*>(cert_der.data());
        impl.key = d2i_AutoPrivateKey(nullptr, &kp, static_cast<long>(key_der.size()));
        impl.cert = d2i_X509(nullptr, &cp, static_cast<long>(cert_der.size()));
    }
    if (!impl.key || !impl.cert) {
        if (impl.key) { EVP_PKEY_free(impl.key); impl.key = nullptr; }
        if (impl.cert) { X509_free(impl.cert); impl.cert = nullptr; }
        impl.key = EVP_EC_gen("P-256");
        if (!impl.key) { error = "key: " + openssl_error(); return nullptr; }
        std::vector<std::string> dns = {"kube32-apiserver", "localhost"};
        for (const auto& d : dns_names) if (!d.empty()) dns.push_back(d);
        std::vector<std::string> ips = {"127.0.0.1"};
        for (const auto& ip : ip_addresses) if (!ip.empty() && ip != "0.0.0.0" && ip != "127.0.0.1") ips.push_back(ip);
        impl.cert = make_certificate(impl.key, dns, ips);
        if (!impl.cert) { error = "certificate: " + openssl_error(); return nullptr; }
        unsigned char* buf = nullptr;
        int n = i2d_PrivateKey(impl.key, &buf);
        if (n > 0) { key_der.assign(reinterpret_cast<char*>(buf), static_cast<size_t>(n)); OPENSSL_free(buf); }
        buf = nullptr;
        n = i2d_X509(impl.cert, &buf);
        if (n > 0) { cert_der.assign(reinterpret_cast<char*>(buf), static_cast<size_t>(n)); OPENSSL_free(buf); }
        if (save && !key_der.empty() && !cert_der.empty()) save(key_der, cert_der);
    }
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio && PEM_write_bio_X509(bio, impl.cert) == 1) {
        char* data = nullptr;
        long len = BIO_get_mem_data(bio, &data);
        impl.pem.assign(data, static_cast<size_t>(len));
    }
    if (bio) BIO_free(bio);

    impl.ctx = SSL_CTX_new(TLS_server_method());
    if (!impl.ctx || SSL_CTX_set_min_proto_version(impl.ctx, TLS1_2_VERSION) != 1 ||
        SSL_CTX_use_certificate(impl.ctx, impl.cert) != 1 || SSL_CTX_use_PrivateKey(impl.ctx, impl.key) != 1) {
        error = "configuration: " + openssl_error();
        return nullptr;
    }
    return server;
}

namespace detail {

struct TlsSession {
    SSL* ssl;
};

TlsSession* tls_handshake(TlsServer& server, int fd) {
    SSL* ssl = SSL_new(server.impl().ctx);
    if (!ssl) return nullptr;
    SSL_set_fd(ssl, fd);
    if (SSL_accept(ssl) != 1) {
        static int logged = 0;
        if (logged < 8) { ++logged; std::printf("[TLS] handshake failed: %s\n", openssl_error().c_str()); }
        SSL_free(ssl);
        return nullptr;
    }
    return new TlsSession{ssl};
}

ssize_t tls_send(TlsSession* s, const void* buf, size_t len) {
    int n = SSL_write(s->ssl, buf, static_cast<int>(len));
    if (n > 0) return n;
    errno = (SSL_get_error(s->ssl, n) == SSL_ERROR_WANT_WRITE) ? EAGAIN : ECONNRESET;
    return -1;
}

ssize_t tls_recv(TlsSession* s, void* buf, size_t len) {
    int n = SSL_read(s->ssl, buf, static_cast<int>(len));
    if (n > 0) return n;
    const int err = SSL_get_error(s->ssl, n);
    if (err == SSL_ERROR_ZERO_RETURN || (err == SSL_ERROR_SYSCALL && errno == 0)) return 0;
    errno = (err == SSL_ERROR_WANT_READ) ? EAGAIN : ECONNRESET;
    ERR_clear_error();
    return -1;
}

bool tls_pending(TlsSession* s) {
    return SSL_pending(s->ssl) > 0 || SSL_has_pending(s->ssl) == 1;
}

void tls_free(TlsSession* s) {
    if (!s) return;
    SSL_shutdown(s->ssl);
    SSL_free(s->ssl);
    delete s;
}

} // namespace detail
} // namespace kube32::osal

#endif
