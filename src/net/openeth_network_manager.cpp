// Network through the OpenCores Ethernet MAC of an emulator (CONFIG_ETH_USE_OPENETH,
// for QEMU's open_eth model): no Wi-Fi, and an address from DHCP. The firmware's
// role logic is unchanged, the link only comes up differently.

#include "net/network_manager.hpp"

#include "sdkconfig.h"

#if CONFIG_ETH_USE_OPENETH

#include "esp_eth.h"
#include "esp_eth_mac_openeth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"

#include <cstdio>
#include <string>

namespace kube32::net {
namespace {

const char* const TAG = "Kube32Eth";
constexpr int kGotIp = BIT0;

class OpenEthNetworkManager final : public NetworkManager {
public:
    bool init(const WifiConfig&) override {
        esp_netif_init();
        esp_event_loop_create_default();
        events_ = xEventGroupCreate();
        esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
        netif_ = esp_netif_new(&cfg);
        eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
        eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
        phy_cfg.phy_addr = 1;
        phy_cfg.reset_gpio_num = -1;
        esp_eth_mac_t* mac = esp_eth_mac_new_openeth(&mac_cfg);
        esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phy_cfg);
        esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
        if (!mac || !phy || esp_eth_driver_install(&eth_cfg, &eth_) != ESP_OK) {
            ESP_LOGE(TAG, "Ethernet driver install failed");
            return false;
        }
        esp_netif_attach(netif_, esp_eth_new_netif_glue(eth_));
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip, this);
        return true;
    }

    bool start_station() override {
        if (esp_eth_start(eth_) != ESP_OK) return false;
        xEventGroupWaitBits(events_, kGotIp, pdFALSE, pdTRUE, pdMS_TO_TICKS(20000));
        return station_has_ip();
    }
    bool start_softap() override { return start_station(); }
    bool start_station_and_softap() override { return start_station(); }
    bool disconnect() override { return esp_eth_stop(eth_) == ESP_OK; }

    bool is_connected() const override { return station_has_ip(); }
    bool station_has_ip() const override { return (xEventGroupGetBits(events_) & kGotIp) != 0; }
    NetworkMode current_mode() const override { return NetworkMode::Station; }

    std::string get_ip_address() const override {
        esp_netif_ip_info_t info = {};
        if (esp_netif_get_ip_info(netif_, &info) != ESP_OK) return "0.0.0.0";
        char buf[16];
        esp_ip4addr_ntoa(&info.ip, buf, sizeof(buf));
        return buf;
    }

    std::string get_mac_address() const override {
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_ETH);
        char buf[18];
        std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return buf;
    }

private:
    static void on_got_ip(void* arg, esp_event_base_t, int32_t, void*) {
        xEventGroupSetBits(static_cast<OpenEthNetworkManager*>(arg)->events_, kGotIp);
    }

    esp_netif_t* netif_{nullptr};
    esp_eth_handle_t eth_{nullptr};
    EventGroupHandle_t events_{nullptr};
};

}  // namespace

std::unique_ptr<NetworkManager> create_openeth_network_manager() { return std::make_unique<OpenEthNetworkManager>(); }

}  // namespace kube32::net

#endif  // CONFIG_ETH_USE_OPENETH
