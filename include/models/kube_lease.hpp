#pragma once

#include <string>
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

} // namespace kube32::models
