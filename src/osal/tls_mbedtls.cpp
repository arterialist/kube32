#include "osal/tls.hpp"

#include <cerrno>
#include <cstring>

#if defined(ESP_PLATFORM)

#include <array>
#include <cstdio>
#include <mutex>
#include <vector>
#include <lwip/sockets.h>
#include <lwip/inet.h>

#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "esp_heap_caps.h"

// mbedTLS allocations (CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC): PSRAM first on a board
// that has it, so TLS sessions do not take internal RAM; internal otherwise.
extern "C" void* esp_mbedtls_mem_calloc(size_t n, size_t size) {
    static const bool psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    if (psram) return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

extern "C" void esp_mbedtls_mem_free(void* ptr) {
    heap_caps_free(ptr);
}

namespace kube32::osal {

struct TlsServer::Impl {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    std::mutex rng_mutex;   // the DRBG is shared by every handshake
    mbedtls_pk_context key;
    mbedtls_x509_crt cert;
    mbedtls_ssl_config conf;
    std::string pem;
};

namespace {

int locked_random(void* p, unsigned char* out, size_t len) {
    auto* impl = static_cast<TlsServer::Impl*>(p);
    std::lock_guard<std::mutex> lock(impl->rng_mutex);
    return mbedtls_ctr_drbg_random(&impl->drbg, out, len);
}

std::string mbedtls_message(int rc) {
    char buf[96];
    mbedtls_strerror(rc, buf, sizeof(buf));
    return buf;
}

// Self-signed certificate for `key`, valid until 2050, usable for server
// authentication and as its own trust anchor.
bool make_certificate(TlsServer::Impl& impl, const std::vector<std::string>& dns, const std::vector<std::string>& ips,
                      std::string& out_der, std::string& error) {
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    unsigned char serial[16];
    locked_random(&impl, serial, sizeof(serial));
    serial[0] &= 0x7F;   // positive
    std::vector<mbedtls_x509_san_list> sans(dns.size() + ips.size());
    std::vector<std::array<unsigned char, 4>> ip_bytes(ips.size());
    size_t n = 0;
    for (const auto& d : dns) {
        sans[n].node.type = MBEDTLS_X509_SAN_DNS_NAME;
        sans[n].node.san.unstructured_name.p = reinterpret_cast<unsigned char*>(const_cast<char*>(d.data()));
        sans[n].node.san.unstructured_name.len = d.size();
        ++n;
    }
    size_t used_ips = 0;
    for (const auto& ip : ips) {
        struct in_addr a;
        if (inet_pton(AF_INET, ip.c_str(), &a) != 1) continue;
        std::memcpy(ip_bytes[used_ips].data(), &a, 4);
        sans[n].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
        sans[n].node.san.unstructured_name.p = ip_bytes[used_ips].data();
        sans[n].node.san.unstructured_name.len = 4;
        ++n;
        ++used_ips;
    }
    sans.resize(n);
    for (size_t i = 0; i + 1 < sans.size(); ++i) sans[i].next = &sans[i + 1];
    if (!sans.empty()) sans.back().next = nullptr;

    mbedtls_asn1_sequence eku;
    std::memset(&eku, 0, sizeof(eku));
    eku.buf.tag = MBEDTLS_ASN1_OID;
    eku.buf.p = reinterpret_cast<unsigned char*>(const_cast<char*>(MBEDTLS_OID_SERVER_AUTH));
    eku.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);

    int rc = 0;
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &impl.key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &impl.key);
    if ((rc = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial))) == 0 &&
        (rc = mbedtls_x509write_crt_set_subject_name(&crt, "CN=kube32-apiserver,O=kube32")) == 0 &&
        (rc = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=kube32-apiserver,O=kube32")) == 0 &&
        (rc = mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20501231235959")) == 0 &&
        (rc = mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0)) == 0 &&
        (rc = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_CERT_SIGN)) == 0 &&
        (rc = mbedtls_x509write_crt_set_ext_key_usage(&crt, &eku)) == 0 &&
        (rc = mbedtls_x509write_crt_set_subject_key_identifier(&crt)) == 0 &&
        (rc = mbedtls_x509write_crt_set_authority_key_identifier(&crt)) == 0 &&
        (sans.empty() || (rc = mbedtls_x509write_crt_set_subject_alternative_name(&crt, sans.data())) == 0)) {
        std::vector<unsigned char> buf(2048);
        rc = mbedtls_x509write_crt_der(&crt, buf.data(), buf.size(), locked_random, &impl);
        if (rc > 0) out_der.assign(reinterpret_cast<const char*>(buf.data() + buf.size() - rc), static_cast<size_t>(rc));
    }
    mbedtls_x509write_crt_free(&crt);
    if (rc <= 0) {
        error = "certificate: " + mbedtls_message(rc);
        return false;
    }
    return true;
}

std::string der_to_pem(const std::string& der) {
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, reinterpret_cast<const unsigned char*>(der.data()), der.size());
    std::string b64(olen, '\0');
    mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&b64[0]), b64.size(), &olen,
                          reinterpret_cast<const unsigned char*>(der.data()), der.size());
    b64.resize(olen);
    std::string pem = "-----BEGIN CERTIFICATE-----\n";
    for (size_t i = 0; i < b64.size(); i += 64) pem += b64.substr(i, 64) + "\n";
    pem += "-----END CERTIFICATE-----\n";
    return pem;
}

} // namespace

TlsServer::TlsServer() : impl_(std::make_unique<Impl>()) {}

TlsServer::~TlsServer() {
    mbedtls_ssl_config_free(&impl_->conf);
    mbedtls_x509_crt_free(&impl_->cert);
    mbedtls_pk_free(&impl_->key);
    mbedtls_ctr_drbg_free(&impl_->drbg);
    mbedtls_entropy_free(&impl_->entropy);
}

const std::string& TlsServer::certificate_pem() const { return impl_->pem; }

bool tls_available() { return true; }

std::unique_ptr<TlsServer> TlsServer::create(const std::vector<std::string>& dns_names, const std::vector<std::string>& ip_addresses,
                                             const Loader& load, const Saver& save, std::string& error) {
    std::unique_ptr<TlsServer> server(new TlsServer());
    Impl& impl = *server->impl_;
    mbedtls_entropy_init(&impl.entropy);
    mbedtls_ctr_drbg_init(&impl.drbg);
    mbedtls_pk_init(&impl.key);
    mbedtls_x509_crt_init(&impl.cert);
    mbedtls_ssl_config_init(&impl.conf);
    static const char kPers[] = "kube32-apiserver";
    int rc = mbedtls_ctr_drbg_seed(&impl.drbg, mbedtls_entropy_func, &impl.entropy,
                                   reinterpret_cast<const unsigned char*>(kPers), sizeof(kPers) - 1);
    if (rc != 0) { error = "random generator: " + mbedtls_message(rc); return nullptr; }

    std::string key_der, cert_der;
    bool loaded = load && load(key_der, cert_der) && !key_der.empty() && !cert_der.empty() &&
                  mbedtls_pk_parse_key(&impl.key, reinterpret_cast<const unsigned char*>(key_der.data()), key_der.size(),
                                       nullptr, 0, locked_random, &impl) == 0 &&
                  mbedtls_x509_crt_parse_der(&impl.cert, reinterpret_cast<const unsigned char*>(cert_der.data()), cert_der.size()) == 0;
    if (!loaded) {
        mbedtls_pk_free(&impl.key);
        mbedtls_pk_init(&impl.key);
        mbedtls_x509_crt_free(&impl.cert);
        mbedtls_x509_crt_init(&impl.cert);
        if ((rc = mbedtls_pk_setup(&impl.key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0 ||
            (rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(impl.key), locked_random, &impl)) != 0) {
            error = "key: " + mbedtls_message(rc);
            return nullptr;
        }
        std::vector<unsigned char> buf(512);
        rc = mbedtls_pk_write_key_der(&impl.key, buf.data(), buf.size());
        if (rc <= 0) { error = "key: " + mbedtls_message(rc); return nullptr; }
        key_der.assign(reinterpret_cast<const char*>(buf.data() + buf.size() - rc), static_cast<size_t>(rc));
        std::vector<std::string> dns = {"kube32-apiserver", "localhost"};
        for (const auto& d : dns_names) if (!d.empty()) dns.push_back(d);
        std::vector<std::string> ips = {"127.0.0.1"};
        for (const auto& ip : ip_addresses) if (!ip.empty() && ip != "0.0.0.0") ips.push_back(ip);
        if (!make_certificate(impl, dns, ips, cert_der, error)) return nullptr;
        if ((rc = mbedtls_x509_crt_parse_der(&impl.cert, reinterpret_cast<const unsigned char*>(cert_der.data()), cert_der.size())) != 0) {
            error = "certificate: " + mbedtls_message(rc);
            return nullptr;
        }
        if (save) save(key_der, cert_der);
    }
    impl.pem = der_to_pem(cert_der);

    if ((rc = mbedtls_ssl_config_defaults(&impl.conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) != 0 ||
        (rc = mbedtls_ssl_conf_own_cert(&impl.conf, &impl.cert, &impl.key)) != 0) {
        error = "configuration: " + mbedtls_message(rc);
        return nullptr;
    }
    mbedtls_ssl_conf_rng(&impl.conf, locked_random, &impl);
    mbedtls_ssl_conf_authmode(&impl.conf, MBEDTLS_SSL_VERIFY_NONE);
    return server;
}

namespace detail {

struct TlsSession {
    mbedtls_ssl_context ssl;
    int fd;
};

// Blocking descriptors with SO_RCVTIMEO/SO_SNDTIMEO: a timeout surfaces as
// WANT_READ/WANT_WRITE, which every caller treats as the end of the wait
static int bio_send(void* ctx, const unsigned char* buf, size_t len) {
    int fd = *static_cast<int*>(ctx);
    int n = ::send(fd, buf, len, 0);
    if (n >= 0) return n;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void* ctx, unsigned char* buf, size_t len) {
    int fd = *static_cast<int*>(ctx);
    int n = ::recv(fd, buf, len, 0);
    if (n >= 0) return n;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

TlsSession* tls_handshake(TlsServer& server, int fd) {
    auto* s = new TlsSession();
    s->fd = fd;
    mbedtls_ssl_init(&s->ssl);
    if (mbedtls_ssl_setup(&s->ssl, &server.impl().conf) != 0) {
        mbedtls_ssl_free(&s->ssl);
        delete s;
        return nullptr;
    }
    mbedtls_ssl_set_bio(&s->ssl, &s->fd, bio_send, bio_recv, nullptr);
    int rc = mbedtls_ssl_handshake(&s->ssl);
    if (rc != 0) {
        static int logged = 0;
        if (logged < 8) { ++logged; printf("[TLS] handshake failed: %s\n", mbedtls_message(rc).c_str()); }
        mbedtls_ssl_free(&s->ssl);
        delete s;
        return nullptr;
    }
    return s;
}

ssize_t tls_send(TlsSession* s, const void* buf, size_t len) {
    int rc = mbedtls_ssl_write(&s->ssl, static_cast<const unsigned char*>(buf), len);
    if (rc >= 0) return rc;
    errno = (rc == MBEDTLS_ERR_SSL_WANT_WRITE) ? EAGAIN : ECONNRESET;
    return -1;
}

ssize_t tls_recv(TlsSession* s, void* buf, size_t len) {
    int rc = mbedtls_ssl_read(&s->ssl, static_cast<unsigned char*>(buf), len);
    if (rc > 0) return rc;
    if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
    errno = (rc == MBEDTLS_ERR_SSL_WANT_READ) ? EAGAIN : ECONNRESET;
    return -1;
}

bool tls_pending(TlsSession* s) {
    return mbedtls_ssl_get_bytes_avail(&s->ssl) > 0 || mbedtls_ssl_check_pending(&s->ssl) != 0;
}

void tls_free(TlsSession* s) {
    if (!s) return;
    mbedtls_ssl_close_notify(&s->ssl);
    mbedtls_ssl_free(&s->ssl);
    delete s;
}

} // namespace detail
} // namespace kube32::osal

#elif !defined(KUBE32_HAS_OPENSSL) // host build without OpenSSL: no TLS (tls_openssl.cpp has the OpenSSL one)

namespace kube32::osal {

struct TlsServer::Impl {
    std::string pem;
};

TlsServer::TlsServer() : impl_(std::make_unique<Impl>()) {}
TlsServer::~TlsServer() = default;
const std::string& TlsServer::certificate_pem() const { return impl_->pem; }
bool tls_available() { return false; }

std::unique_ptr<TlsServer> TlsServer::create(const std::vector<std::string>&, const std::vector<std::string>&,
                                             const Loader&, const Saver&, std::string& error) {
    error = "this build has no TLS";
    return nullptr;
}

namespace detail {
struct TlsSession {};
TlsSession* tls_handshake(TlsServer&, int) { return nullptr; }
ssize_t tls_send(TlsSession*, const void*, size_t) { errno = ENOTSUP; return -1; }
ssize_t tls_recv(TlsSession*, void*, size_t) { errno = ENOTSUP; return -1; }
void tls_free(TlsSession*) {}
bool tls_pending(TlsSession*) { return false; }
} // namespace detail
} // namespace kube32::osal

#endif
