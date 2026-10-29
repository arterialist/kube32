#include "net/network_manager.hpp"
#include <iostream>
#include <atomic>

#if defined(KUBE32_TARGET_ESP32)
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
        if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
            // Frames wait in these buffers (~1.6 KB of internal RAM each)
            // until the radio sends them or lwIP is done with them; a pod
            // streaming a large response filled all eight
            cfg.dynamic_tx_buf_num = 4;
            cfg.dynamic_rx_buf_num = 6;
        }
        if (esp_wifi_init(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed");
            return false;
        }

        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &EspNetworkManager::wifi_event, this);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &EspNetworkManager::ip_event, this);
        return true;
    }

    // Stations keep trying to rejoin the cluster AP forever; the AP may boot
    // after us or reboot while we run.
    static void wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
        (void)base; (void)data;
        auto* self = static_cast<EspNetworkManager*>(arg);
        if (id == WIFI_EVENT_STA_DISCONNECTED && self->mode_ != NetworkMode::SoftAP) {
            self->sta_has_ip_ = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_START && self->mode_ != NetworkMode::SoftAP) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_AP_STACONNECTED || id == WIFI_EVENT_AP_STADISCONNECTED) {
            // A client joining the cluster AP must not steal the default route
            if (self->mode_ == NetworkMode::StationAndSoftAP && self->sta_netif_ && self->sta_has_ip_) {
                esp_netif_set_default_netif(self->sta_netif_);
            }
        }
    }
    static void ip_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
        (void)base; (void)id;
        auto* self = static_cast<EspNetworkManager*>(arg);
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        self->sta_has_ip_ = true;
        // With both an AP and a station interface, lwIP must route everything
        // that is not on the AP subnet through the uplink, whatever joined last.
        if (self->sta_netif_) esp_netif_set_default_netif(self->sta_netif_);
        ESP_LOGI(TAG, "station got ip " IPSTR " (default route)", IP2STR(&ev->ip_info.ip));
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
        // Modem sleep adds hundreds of ms of latency per packet; a cluster node
        // is mains powered and serves requests, so keep the radio awake.
        esp_wifi_set_ps(WIFI_PS_NONE);

        // WIFI_EVENT_STA_START already issued the join; a second connect while
        // the first is pending returns ESP_ERR_WIFI_CONN and is not a failure.
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            return start_softap();
        }

        mode_ = NetworkMode::Station;
        connected_ = true;
        // Wait for DHCP so callers get a usable address; keep retrying the join
        uint32_t waited = 0;
        while (waited < config_.connect_timeout_ms && !sta_has_ip_) {
            vTaskDelay(pdMS_TO_TICKS(250));
            waited += 250;
        }
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
        ap_cfg.ap.max_connection = 3;
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

        esp_wifi_set_mode(WIFI_MODE_AP);
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        esp_wifi_start();
        esp_wifi_set_ps(WIFI_PS_NONE);

        mode_ = NetworkMode::SoftAP;
        connected_ = true;
        return true;
    }

    bool start_station_and_softap() override {
        if (!ap_netif_) ap_netif_ = esp_netif_create_default_wifi_ap();
        wifi_config_t ap_cfg = {};
        std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.ssid), config_.ap_ssid.c_str(), sizeof(ap_cfg.ap.ssid));
        std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.password), config_.ap_password.c_str(), sizeof(ap_cfg.ap.password));
        ap_cfg.ap.ssid_len = config_.ap_ssid.length();
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config_t sta_cfg = {};
        std::strncpy(reinterpret_cast<char*>(sta_cfg.sta.ssid), config_.ssid.c_str(), sizeof(sta_cfg.sta.ssid));
        std::strncpy(reinterpret_cast<char*>(sta_cfg.sta.password), config_.password.c_str(), sizeof(sta_cfg.sta.password));
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        esp_wifi_start();
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_connect();
        mode_ = NetworkMode::StationAndSoftAP;
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
        esp_netif_t* netif = (mode_ == NetworkMode::SoftAP || mode_ == NetworkMode::StationAndSoftAP) ? ap_netif_ : sta_netif_;
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
    volatile bool sta_has_ip_{false};
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
