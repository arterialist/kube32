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
    // Joins an upstream network as a station while still hosting the cluster AP.
    virtual bool start_station_and_softap() { return start_softap(); }
    virtual bool disconnect() = 0;
    // Station: leaves the access point and joins it again (the station
    // rejoins by itself after any disconnect). False without a station.
    virtual bool rejoin() { return false; }
    // Station: stops and starts the Wi-Fi driver, which drops every link
    // state it kept (802.11w keys included); the station joins again after
    // the start. False without a station.
    virtual bool restart_radio() { return false; }

    virtual bool is_connected() const = 0;
    // Station: associated and holding an address
    virtual bool station_has_ip() const { return false; }
    virtual NetworkMode current_mode() const = 0;
    virtual std::string get_ip_address() const = 0;
    virtual std::string get_mac_address() const = 0;
};

std::unique_ptr<NetworkManager> create_network_manager();

} // namespace kube32::net
