#pragma once

// Incremental SHA-256: mbedTLS (the hardware SHA unit) on the board, a
// portable implementation on the host. The context lives in the object; a
// hash takes nothing from the heap.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#if defined(ESP_PLATFORM)
#include "mbedtls/sha256.h"
#endif

namespace kube32::osal {

class Sha256 {
public:
    using Digest = std::array<uint8_t, 32>;

    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    // Starts over
    void reset();
    void update(const void* data, size_t len);
    Digest finish();

    static Digest of(const void* data, size_t len);
    // 64 lowercase hex digits
    static std::string hex(const Digest& digest);
    // A SHA-256 written as 64 hex digits (either case)
    static bool valid_hex(std::string_view text);

private:
#if defined(ESP_PLATFORM)
    mbedtls_sha256_context ctx_;
#else
    uint32_t state_[8];
    uint64_t bytes_{0};
    uint8_t block_[64];
    size_t used_{0};
    void compress(const uint8_t* block);
#endif
};

} // namespace kube32::osal
