#include "osal/sha256.hpp"

#include <cstring>

namespace kube32::osal {

#if defined(ESP_PLATFORM)

Sha256::Sha256() {
    mbedtls_sha256_init(&ctx_);
    reset();
}

Sha256::~Sha256() { mbedtls_sha256_free(&ctx_); }

void Sha256::reset() { mbedtls_sha256_starts(&ctx_, 0); }

void Sha256::update(const void* data, size_t len) {
    mbedtls_sha256_update(&ctx_, static_cast<const unsigned char*>(data), len);
}

Sha256::Digest Sha256::finish() {
    Digest d{};
    mbedtls_sha256_finish(&ctx_, d.data());
    reset();
    return d;
}

#else

namespace {

constexpr uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

} // namespace

Sha256::Sha256() { reset(); }

Sha256::~Sha256() = default;

void Sha256::reset() {
    static constexpr uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(state_, kInit, sizeof(state_));
    bytes_ = 0;
    used_ = 0;
}

void Sha256::compress(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    bytes_ += len;
    while (len > 0) {
        const size_t take = std::min(len, sizeof(block_) - used_);
        std::memcpy(block_ + used_, p, take);
        used_ += take;
        p += take;
        len -= take;
        if (used_ == sizeof(block_)) {
            compress(block_);
            used_ = 0;
        }
    }
}

Sha256::Digest Sha256::finish() {
    const uint64_t bits = bytes_ * 8;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero = 0;
    while (used_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(len, 8);
    Digest d{};
    for (int i = 0; i < 8; ++i) {
        d[4 * i] = static_cast<uint8_t>(state_[i] >> 24);
        d[4 * i + 1] = static_cast<uint8_t>(state_[i] >> 16);
        d[4 * i + 2] = static_cast<uint8_t>(state_[i] >> 8);
        d[4 * i + 3] = static_cast<uint8_t>(state_[i]);
    }
    reset();
    return d;
}

#endif

Sha256::Digest Sha256::of(const void* data, size_t len) {
    Sha256 h;
    h.update(data, len);
    return h.finish();
}

std::string Sha256::hex(const Digest& digest) {
    static const char kHex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (size_t i = 0; i < digest.size(); ++i) {
        out[2 * i] = kHex[digest[i] >> 4];
        out[2 * i + 1] = kHex[digest[i] & 0xf];
    }
    return out;
}

bool Sha256::valid_hex(std::string_view text) {
    if (text.size() != 64) return false;
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

} // namespace kube32::osal
