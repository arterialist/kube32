#include "net/network_manager.hpp"
#include <iostream>
#include <atomic>

#if defined(KUBE32_TARGET_ESP32)
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "Kube32Net";
#endif

namespace kube32::net {

#if defined(KUBE32_TARGET_ESP32)

class EspNetworkManager : public NetworkManager {
public:
    EspNetworkManager() = default;
    ~EspNetworkManager() override { disconnect(); }

    bool init(const WifiConfig& config) override {
        config_ = config;
        esp_netif_init();
        esp_event_loop_create_default();
        sta_netif_ = esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed");
            return false;
        }

        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        return true;
    }

    bool start_station() override {
        if (!sta_netif_) return false;

        wifi_config_t wifi_cfg = {};
        std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.ssid), config_.ssid.c_str(), sizeof(wifi_cfg.sta.ssid));
        std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.password), config_.password.c_str(), sizeof(wifi_cfg.sta.password));

        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        if (esp_wifi_start() != ESP_OK) {
            return start_softap(); // Fallback
        }

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            return start_softap();
        }

        mode_ = NetworkMode::Station;
        connected_ = true;
        return true;
    }

    bool start_softap() override {
        if (!ap_netif_) {
            ap_netif_ = esp_netif_create_default_wifi_ap();
        }

        wifi_config_t ap_cfg = {};
        std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.ssid), config_.ap_ssid.c_str(), sizeof(ap_cfg.ap.ssid));
        std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.password), config_.ap_password.c_str(), sizeof(ap_cfg.ap.password));
        ap_cfg.ap.ssid_len = config_.ap_ssid.length();
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

        esp_wifi_set_mode(WIFI_MODE_AP);
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        esp_wifi_start();

        mode_ = NetworkMode::SoftAP;
        connected_ = true;
        return true;
    }

    bool disconnect() override {
        esp_wifi_disconnect();
        esp_wifi_stop();
        connected_ = false;
        mode_ = NetworkMode::Disconnected;
        return true;
    }

    bool is_connected() const override { return connected_; }
    NetworkMode current_mode() const override { return mode_; }

    std::string get_ip_address() const override {
        if (!connected_) return "0.0.0.0";
        esp_netif_ip_info_t ip_info;
        esp_netif_t* netif = (mode_ == NetworkMode::SoftAP) ? ap_netif_ : sta_netif_;
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char buf[32];
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
            return std::string(buf);
        }
        return "0.0.0.0";
    }

    std::string get_mac_address() const override {
        uint8_t mac[6] = {0};
        esp_wifi_get_mac((mode_ == NetworkMode::SoftAP) ? WIFI_IF_AP : WIFI_IF_STA, mac);
        char buf[20];
        snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return std::string(buf);
    }

private:
    WifiConfig config_;
    esp_netif_t* sta_netif_{nullptr};
    esp_netif_t* ap_netif_{nullptr};
    NetworkMode mode_{NetworkMode::Disconnected};
    bool connected_{false};
};

#else // Host Simulation (POSIX)

class PosixNetworkManager : public NetworkManager {
public:
    PosixNetworkManager() = default;
    ~PosixNetworkManager() override = default;

    bool init(const WifiConfig& config) override {
        config_ = config;
        return true;
    }

    bool start_station() override {
        mode_ = NetworkMode::Station;
        connected_ = true;
        return true;
    }

    bool start_softap() override {
        mode_ = NetworkMode::SoftAP;
        connected_ = true;
        return true;
    }

    bool disconnect() override {
        connected_ = false;
        mode_ = NetworkMode::Disconnected;
        return true;
    }

    bool is_connected() const override { return connected_; }
    NetworkMode current_mode() const override { return mode_; }

    std::string get_ip_address() const override {
        return connected_ ? "127.0.0.1" : "0.0.0.0";
    }

    std::string get_mac_address() const override {
        return "02:42:c0:a8:01:01";
    }

private:
    WifiConfig config_;
    NetworkMode mode_{NetworkMode::Station};
    bool connected_{true};
};

#endif

std::unique_ptr<NetworkManager> create_network_manager() {
#if defined(KUBE32_TARGET_ESP32)
    return std::make_unique<EspNetworkManager>();
#else
    return std::make_unique<PosixNetworkManager>();
#endif
}

} // namespace kube32::net
