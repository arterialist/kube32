#include "net/network_manager.hpp"
#include <iostream>
#include <atomic>

#if defined(KUBE32_TARGET_ESP32)
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static const char* TAG = "Kube32Net";
static constexpr uint16_t k_small_heap_mtu = 576;
// Below this signal the station looks for a stronger AP of the same network,
// and moves when one is at least k_roam_margin_db better; a scan that finds
// none waits k_roam_retry_us before the next one.
static constexpr int8_t k_roam_rssi = -70;
static constexpr int8_t k_roam_margin_db = 10;
static constexpr uint64_t k_roam_retry_us = 60ULL * 1000 * 1000;
// lwIP's NAPT table: IP_NAPT_MAX (512) entries of 32 bytes
static constexpr size_t k_napt_table_bytes = 512 * 32;
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
            // Frames wait in these buffers until the radio sends them or
            // lwIP is done with them. Without PSRAM the station runs a 576
            // byte MTU (see ip_event), so each holds at most ~650 bytes; the
            // driver must never find the heap empty, or it stops sending.
            // A socket with unread data holds up to 7 RX buffers (receive
            // mailbox 4, one refused segment, 2 out of order): 12 leave the
            // rest of the node able to receive. A TCP send buffer of 5760
            // bytes is 11 segments of 536: fewer TX buffers make a burst
            // overflow, and the connection waits 250 ms to send again.
            cfg.dynamic_tx_buf_num = 12;
            cfg.dynamic_rx_buf_num = 12;
        }
        if (esp_wifi_init(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed");
            return false;
        }

        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &EspNetworkManager::wifi_event, this);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &EspNetworkManager::ip_event, this);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &EspNetworkManager::ip_lost_event, this);
        return true;
    }

    // Stations keep trying to rejoin the cluster AP forever; the AP may boot
    // after us or reboot while we run.
    static void wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
        (void)base; (void)data;
        auto* self = static_cast<EspNetworkManager*>(arg);
        if (id == WIFI_EVENT_STA_DISCONNECTED && self->mode_ != NetworkMode::SoftAP) {
            self->sta_has_ip_ = false;
            self->uplink_down();
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_START && self->mode_ != NetworkMode::SoftAP) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            esp_wifi_set_rssi_threshold(k_roam_rssi);
        } else if (id == WIFI_EVENT_STA_BSS_RSSI_LOW) {
            self->start_roam_scan();
        } else if (id == WIFI_EVENT_SCAN_DONE && self->roam_scan_) {
            self->roam_scan_ = false;
            self->finish_roam_scan();
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
        if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0 && self->sta_netif_) {
            // Without PSRAM the heap soon holds no 1.6 KB block, and a full
            // size Wi-Fi frame can then be neither sent nor received: TCP
            // stalls with its queued segments still holding memory. A 576
            // byte MTU keeps every segment, ours and the MSS peers are told
            // to use, under 600 bytes.
            esp_netif_tcpip_exec([](void* ctx) -> esp_err_t {
                auto* nif = static_cast<struct netif*>(esp_netif_get_netif_impl(static_cast<esp_netif_t*>(ctx)));
                if (nif) nif->mtu = k_small_heap_mtu;
                return ESP_OK;
            }, self->sta_netif_);
        }
        // With both an AP and a station interface, lwIP must route everything
        // that is not on the AP subnet through the uplink, whatever joined last.
        if (self->sta_netif_) esp_netif_set_default_netif(self->sta_netif_);
        ESP_LOGI(TAG, "station got ip " IPSTR " (default route)", IP2STR(&ev->ip_info.ip));
        if (self->mode_ == NetworkMode::StationAndSoftAP) self->uplink_up();
    }
    static void ip_lost_event(void* arg, esp_event_base_t, int32_t, void*) {
        auto* self = static_cast<EspNetworkManager*>(arg);
        self->sta_has_ip_ = false;
        self->uplink_down();
    }

    // Stations of the cluster AP reach hosts outside the cluster through
    // this node: while the uplink holds an address the AP's traffic is
    // forwarded to it with NAT (lwIP NAPT, a 16 KiB table taken on first use,
    // from PSRAM where the board has it), and the AP's DHCP server offers the
    // uplink's name server.
    void uplink_up() {
        if (!ap_netif_ || !sta_netif_) return;
        esp_netif_dns_info_t dns = {};
        if (esp_netif_get_dns_info(sta_netif_, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.u_addr.ip4.addr != 0) {
            esp_netif_set_dns_info(ap_netif_, ESP_NETIF_DNS_MAIN, &dns);
        }
        if (napt_on_) return;
        // The table must fit: lwIP asserts when it cannot allocate it
        if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < k_napt_table_bytes + 4096) {
            ESP_LOGW(TAG, "uplink up, but no %u byte block for the NAT table: cluster AP stations stay without a route out",
                     (unsigned)k_napt_table_bytes);
            return;
        }
        const size_t internal0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t psram0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (esp_netif_napt_enable(ap_netif_) != ESP_OK) {
            ESP_LOGW(TAG, "NAT for the cluster AP could not be enabled");
            return;
        }
        napt_on_ = true;
        printf("[NET] uplink up: NAT from the cluster AP on, DNS " IPSTR " offered (internal RAM %d bytes, PSRAM %d bytes)\n",
               IP2STR(&dns.ip.u_addr.ip4), (int)((long)internal0 - (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
               (int)((long)psram0 - (long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }
    void uplink_down() {
        if (!napt_on_ || !ap_netif_) return;
        esp_netif_napt_disable(ap_netif_);
        napt_on_ = false;
        printf("[NET] uplink down: NAT from the cluster AP off\n");
    }

    // Before the AP starts: its DHCP server offers a name server (the uplink's
    // once there is one; none until then) and two-minute leases, so a station
    // that joined before the uplink came up learns the name server when it
    // renews, within a minute
    void prepare_ap_dhcp() {
        uint8_t offer_dns = 1;
        esp_netif_dhcps_option(ap_netif_, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
        uint32_t lease_minutes = 2;
        esp_netif_dhcps_option(ap_netif_, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME, &lease_minutes, sizeof(lease_minutes));
    }

    // Networks with several APs under one name: the station joins the
    // strongest, not the first one a scan finds
    void fill_station_config(wifi_config_t& cfg) const {
        std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), config_.ssid.c_str(), sizeof(cfg.sta.ssid));
        std::strncpy(reinterpret_cast<char*>(cfg.sta.password), config_.password.c_str(), sizeof(cfg.sta.password));
        cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        cfg.sta.failure_retry_cnt = 2;
    }

    void start_roam_scan() {
        wifi_scan_config_t sc = {};
        sc.ssid = reinterpret_cast<uint8_t*>(const_cast<char*>(config_.ssid.c_str()));
        roam_scan_ = true;
        if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
            roam_scan_ = false;
            rearm_roam_later();
        }
    }

    // Leaving the AP makes the station rejoin the strongest one
    void finish_roam_scan() {
        wifi_ap_record_t current = {};
        if (esp_wifi_sta_get_ap_info(&current) != ESP_OK) {
            esp_wifi_clear_ap_list();
            return;
        }
        wifi_ap_record_t rec = {};
        wifi_ap_record_t best = {};
        bool found = false;
        while (esp_wifi_scan_get_ap_record(&rec) == ESP_OK) {
            if (std::memcmp(rec.bssid, current.bssid, sizeof(rec.bssid)) == 0) continue;
            if (!found || rec.rssi > best.rssi) { best = rec; found = true; }
        }
        esp_wifi_clear_ap_list();
        if (found && best.rssi >= current.rssi + k_roam_margin_db) {
            ESP_LOGI(TAG, "roaming: AP on channel %u at %d dBm, this one on channel %u at %d dBm",
                     best.primary, best.rssi, current.primary, current.rssi);
            esp_wifi_disconnect();
            return;
        }
        rearm_roam_later();
    }

    void rearm_roam_later() {
        if (!roam_timer_) {
            esp_timer_create_args_t args = {};
            args.callback = [](void*) { esp_wifi_set_rssi_threshold(k_roam_rssi); };
            args.name = "k32_roam";
            if (esp_timer_create(&args, &roam_timer_) != ESP_OK) return;
        }
        esp_timer_stop(roam_timer_);
        esp_timer_start_once(roam_timer_, k_roam_retry_us);
    }

    bool start_station() override {
        if (!sta_netif_) return false;

        wifi_config_t wifi_cfg = {};
        fill_station_config(wifi_cfg);

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
        if (!ap_netif_) {
            ap_netif_ = esp_netif_create_default_wifi_ap();
            prepare_ap_dhcp();
        }
        wifi_config_t ap_cfg = {};
        std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.ssid), config_.ap_ssid.c_str(), sizeof(ap_cfg.ap.ssid));
        std::strncpy(reinterpret_cast<char*>(ap_cfg.ap.password), config_.ap_password.c_str(), sizeof(ap_cfg.ap.password));
        ap_cfg.ap.ssid_len = config_.ap_ssid.length();
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config_t sta_cfg = {};
        fill_station_config(sta_cfg);
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

    bool rejoin() override {
        if (mode_ != NetworkMode::Station && mode_ != NetworkMode::StationAndSoftAP) return false;
        return esp_wifi_disconnect() == ESP_OK;
    }

    bool is_connected() const override { return connected_; }
    bool station_has_ip() const override { return sta_has_ip_; }
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
    volatile bool roam_scan_{false};
    bool napt_on_{false};   // read and written on the event loop task only
    esp_timer_handle_t roam_timer_{nullptr};
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
