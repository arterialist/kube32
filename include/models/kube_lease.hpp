#pragma once

#include <string>
#include <cstddef>
#include <cstdint>

namespace kube32::models {

struct KubeLease {
    std::string name;
    std::string namespace_name{"kube-node-lease"};
    std::string holder_identity;
    uint32_t lease_duration_seconds{40};
    std::string renew_time;
    uint64_t resource_version{0};

    std::string to_json() const;
    static KubeLease from_json(const std::string& json_str);
};

// The wall clock as a metav1.MicroTime ("2026-10-28T12:00:00.123000Z"), empty
// while the clock is not set.
std::string now_micro_time();
// The same into `out` (28 bytes with the terminator); returns the length, 0
// while the clock is not set.
size_t now_micro_time(char* out, size_t cap);

} // namespace kube32::models
