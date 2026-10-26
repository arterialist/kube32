#pragma once

#include <string>
#include <cstdint>
#include <memory>

namespace kube32::net {

enum class NetworkMode {
    Disconnected,
    Station,
    SoftAP,
    StationAndSoftAP
};

struct WifiConfig {
    std::string ssid;
    std::string password;
    std::string ap_ssid{"Kube32-Setup-AP"};
    std::string ap_password{"kube32mesh"};
    uint32_t connect_timeout_ms{10000};
    uint8_t max_retries{3};
};

class NetworkManager {
public:
    virtual ~NetworkManager() = default;

    virtual bool init(const WifiConfig& config) = 0;
    virtual bool start_station() = 0;
    virtual bool start_softap() = 0;
    virtual bool disconnect() = 0;

    virtual bool is_connected() const = 0;
    virtual NetworkMode current_mode() const = 0;
    virtual std::string get_ip_address() const = 0;
    virtual std::string get_mac_address() const = 0;
};

std::unique_ptr<NetworkManager> create_network_manager();

} // namespace kube32::net
