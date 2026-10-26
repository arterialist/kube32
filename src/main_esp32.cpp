#include "storage/store.hpp"
#include "scheduler/scheduler.hpp"
#include "controller/node_lifecycle_controller.hpp"
#include "api/http_server.hpp"
#include "runtime/wasm_runtime.hpp"
#include "kubelet/kubelet.hpp"
#include "ipc/channel.hpp"
#include "net/network_manager.hpp"
#include "osal/thread.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <iostream>

#if defined(KUBE32_TARGET_ESP32)
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "Kube32Main";

// Known MAC Addresses for the Connected Physical Hardware
static const char* NODE2_MAC = "02:00:00:00:00:01"; // 2MB PSRAM (Worker Node 02)

extern "C" void app_main() {
    // 1. Initialize NVS Flash
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Read Factory MAC Address
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[24];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    printf("\n");
    printf("================================================================\n");
    printf("     _  ___   _ ___  ______ ___ ___ \n");
    printf("    | |/ / | | | _ )|__ /_  )  \\/  /\n");
    printf("    | ' <| |_| | _ \\ |_ \\/ /| |\\/| |\n");
    printf("    |_|\\_\\\\___/|___/|___/___|_|  |_|\n");
    printf("  Embedded Kubernetes Control Plane & WebAssembly Runtime\n");
    printf("================================================================\n");
    printf("[BOOT] Chip: ESP32-S3 (Xtensa LX7 @ 240MHz)\n");
    printf("[BOOT] Hardware MAC: %s\n", mac_str);
    printf("[BOOT] Free Internal SRAM: %u bytes\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("[BOOT] Free PSRAM:         %u bytes\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("[BOOT] Total Free Heap:    %u bytes\n", (unsigned)esp_get_free_heap_size());
    printf("----------------------------------------------------------------\n");

    // 3. Determine Node Role based on physical device MAC
    bool is_node2 = (std::strcmp(mac_str, NODE2_MAC) == 0);
    std::string node_name = is_node2 ? "esp32-node-02" : "esp32-node-01";
    uint16_t node_id = is_node2 ? 2 : 1;

    // 4. Initialize Network Layer
    kube32::net::WifiConfig wifi_cfg;
    wifi_cfg.ap_ssid = "Kube32-Cluster";
    wifi_cfg.ap_password = "kube32password";
    wifi_cfg.ssid = "Kube32-Cluster";
    wifi_cfg.password = "kube32password";

    auto net_mgr = kube32::net::create_network_manager();
    if (!net_mgr->init(wifi_cfg)) {
        ESP_LOGE(TAG, "Failed to initialize NetworkManager");
    }

    if (is_node2) {
        printf("[ROLE] Booting as Worker Node: %s (Node ID: %u)\n", node_name.c_str(), node_id);
        net_mgr->start_station();
    } else {
        printf("[ROLE] Booting as Master Control Plane: %s (Node ID: %u)\n", node_name.c_str(), node_id);
        net_mgr->start_softap();
    }

    std::string ip = net_mgr->get_ip_address();
    printf("[NET] Node IP Address: %s\n", ip.c_str());

    // 5. Initialize Virtual Channel IPC (ESP-NOW transport)
    printf("[IPC] Initializing ESP-NOW Virtual Channel Transport...\n");
    kube32::ipc::ChannelManager::instance().init(node_id, 7400);

    if (is_node2) {
        // -------------------------------------------------------------
        // WORKER NODE (esp32-node-02)
        // -------------------------------------------------------------
        std::string master_host = "192.168.4.1";
        uint16_t master_port = 6443;

        printf("[WORKER] Connecting Kubelet to Master at http://%s:%u...\n", master_host.c_str(), master_port);

        auto wasm_runtime = kube32::runtime::create_wasm_runtime();
        kube32::kubelet::KubeletConfig kubelet_cfg;
        kubelet_cfg.node_name = node_name;
        kubelet_cfg.api_server_host = master_host;
        kubelet_cfg.api_server_port = master_port;
        kubelet_cfg.node_ip = ip;
        kubelet_cfg.heartbeat_interval_ms = 5000;

        auto kubelet = std::make_unique<kube32::kubelet::Kubelet>(kubelet_cfg, *wasm_runtime);
        if (!kubelet->start()) {
            ESP_LOGE(TAG, "Failed to start Kubelet worker agent");
            return;
        }

        printf("[WORKER] Kubelet agent online. Listening for Pods and IPC messages...\n");

        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    } else {
        // -------------------------------------------------------------
        // CONTROL PLANE + LOCAL AGENT (esp32-node-01 / Master)
        // -------------------------------------------------------------
        printf("[CONTROL-PLANE] Initializing Radix State Store...\n");
        kube32::storage::StoreOptions store_opts;
        store_opts.base_path = "/littlefs";
        store_opts.enable_flash_persistence = false; // RAM-backed for fast boot
        auto store = kube32::storage::create_radix_store(store_opts);

        // Pre-register both physical hardware nodes into cluster registry
        std::string node1_manifest = 
            "{\"kind\":\"Node\",\"apiVersion\":\"v1\","
            "\"metadata\":{\"name\":\"esp32-node-01\",\"labels\":{"
            "\"kubernetes.io/hostname\":\"esp32-node-01\","
            "\"kubernetes.io/arch\":\"xtensa\","
            "\"node-role.kubernetes.io/control-plane\":\"\","
            "\"node-role.kubernetes.io/master\":\"\"}},"
            "\"spec\":{\"unschedulable\":false},"
            "\"status\":{"
            "\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\",\"reason\":\"KubeletReady\"}],"
            "\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"192.168.4.1\"}],"
            "\"allocatable\":{\"cpu\":\"2\",\"memory\":\"400Ki\",\"hardware.kube32.io/gpio\":\"12\"},"
            "\"capacity\":{\"cpu\":\"2\",\"memory\":\"512Ki\",\"hardware.kube32.io/gpio\":\"16\"}}}";

        std::string node2_manifest = 
            "{\"kind\":\"Node\",\"apiVersion\":\"v1\","
            "\"metadata\":{\"name\":\"esp32-node-02\",\"labels\":{"
            "\"kubernetes.io/hostname\":\"esp32-node-02\","
            "\"kubernetes.io/arch\":\"xtensa\","
            "\"node-role.kubernetes.io/worker\":\"\"}},"
            "\"spec\":{\"unschedulable\":false},"
            "\"status\":{"
            "\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\",\"reason\":\"KubeletReady\"}],"
            "\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"192.168.4.2\"}],"
            "\"allocatable\":{\"cpu\":\"2\",\"memory\":\"2048Ki\",\"hardware.kube32.io/gpio\":\"14\"},"
            "\"capacity\":{\"cpu\":\"2\",\"memory\":\"2048Ki\",\"hardware.kube32.io/gpio\":\"18\"}}}";

        store->put("/registry/nodes/esp32-node-01", node1_manifest, true);
        store->put("/registry/nodes/esp32-node-02", node2_manifest, true);
        printf("[CONTROL-PLANE] Initialized nodes in cluster registry: esp32-node-01, esp32-node-02\n");

        // Start Node Lifecycle Controller
        kube32::controller::NodeLifecycleConfig nl_cfg;
        nl_cfg.check_interval_ms = 1000;
        nl_cfg.not_ready_threshold_ms = 15000;
        nl_cfg.eviction_threshold_ms = 30000;
        auto node_lifecycle = std::make_unique<kube32::controller::NodeLifecycleController>(*store, nl_cfg);
        node_lifecycle->start();
        printf("[CONTROL-PLANE] Node Lifecycle Controller active (NotReady: 15s, Evict: 30s)\n");

        // Start Autonomous Scheduler
        kube32::scheduler::SchedulerConfig sched_cfg;
        sched_cfg.strategy = kube32::scheduler::ScoringStrategy::LeastAllocated;
        sched_cfg.reconcile_interval_ms = 1000;
        sched_cfg.node_heartbeat_timeout_ms = 30000;
        auto scheduler = kube32::scheduler::create_scheduler(*store, sched_cfg);
        scheduler->start();
        printf("[CONTROL-PLANE] Autonomous Scheduler active (LeastAllocated)\n");

        // Start HTTP API Server on port 6443
        kube32::api::HttpServerOptions http_opts;
        http_opts.host = "0.0.0.0";
        http_opts.port = 6443;
        http_opts.backlog = 8;
        http_opts.rx_buffer_size = 4096;
        auto server = std::make_unique<kube32::api::HttpServer>(http_opts, *store, scheduler.get());
        server->start();
        printf("[CONTROL-PLANE] Kubernetes API Server listening on http://0.0.0.0:6443\n");
        printf("[CONTROL-PLANE] Connect via: kubectl --server=http://%s:6443 get nodes -o wide\n", ip.c_str());

        // Start local Kubelet agent on Node 1
        auto wasm_runtime = kube32::runtime::create_wasm_runtime();
        kube32::kubelet::KubeletConfig kubelet_cfg;
        kubelet_cfg.node_name = node_name;
        kubelet_cfg.api_server_host = "127.0.0.1";
        kubelet_cfg.api_server_port = 6443;
        kubelet_cfg.node_ip = ip;
        kubelet_cfg.heartbeat_interval_ms = 5000;

        auto kubelet = std::make_unique<kube32::kubelet::Kubelet>(kubelet_cfg, *wasm_runtime);
        kubelet->start();
        printf("[CONTROL-PLANE] Local Kubelet agent online for %s\n", node_name.c_str());

        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
#endif
