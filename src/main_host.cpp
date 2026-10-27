#include "osal/socket.hpp"
#include "osal/thread.hpp"
#include "storage/store.hpp"
#include "scheduler/scheduler.hpp"
#include "controller/node_lifecycle_controller.hpp"
#include "api/http_server.hpp"
#include "runtime/wasm_runtime.hpp"
#include "kubelet/kubelet.hpp"
#include "ipc/channel.hpp"
#include "net/mdns_discovery.hpp"

#include <csignal>
#include <iostream>
#include <atomic>
#include <string>

static std::atomic<bool> g_sim_running{true};

static void signal_handler(int sig) {
    (void)sig;
    g_sim_running.store(false);
}

struct SimOptions {
    std::string role{"standalone"}; // "control-plane", "worker", "standalone"
    std::string node_name{"esp32-master"};
    uint16_t node_id{0};
    uint16_t port{6443};
    std::string join_url{""};
    std::string data_dir{""};
    bool enable_flash{false};
    uint16_t ipc_port{0};
    uint32_t lease_interval_ms{5000};
    uint32_t not_ready_timeout_ms{15000};
    uint32_t eviction_timeout_ms{30000};
    uint32_t reconcile_interval_ms{1000};
};

static SimOptions parse_sim_args(int argc, char* argv[]) {
    SimOptions opts;
    bool has_role = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--role" && i + 1 < argc) { opts.role = argv[++i]; has_role = true; }
        else if (arg == "--node-name" && i + 1 < argc) opts.node_name = argv[++i];
        else if (arg == "--node-id" && i + 1 < argc) opts.node_id = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--port" && i + 1 < argc) opts.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--join" && i + 1 < argc) opts.join_url = argv[++i];
        else if (arg == "--data-dir" && i + 1 < argc) opts.data_dir = argv[++i];
        else if (arg == "--enable-flash") opts.enable_flash = true;
        else if (arg == "--ipc-port" && i + 1 < argc) opts.ipc_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--lease-interval" && i + 1 < argc) opts.lease_interval_ms = std::stoul(argv[++i]);
        else if (arg == "--not-ready-timeout" && i + 1 < argc) opts.not_ready_timeout_ms = std::stoul(argv[++i]);
        else if (arg == "--eviction-timeout" && i + 1 < argc) opts.eviction_timeout_ms = std::stoul(argv[++i]);
        else if (arg == "--reconcile-interval" && i + 1 < argc) opts.reconcile_interval_ms = std::stoul(argv[++i]);
    }

    if (!has_role) {
        opts.role = "standalone";
    }
    if (opts.data_dir.empty()) {
        opts.data_dir = "/tmp/kube32_" + opts.node_name + "_data";
    }
    if (opts.ipc_port == 0) {
        opts.ipc_port = 7400 + opts.node_id;
    }
    return opts;
}

static bool parse_url(const std::string& url, std::string& out_host, uint16_t& out_port) {
    if (url.empty()) return false;
    std::string s = url;
    if (s.rfind("http://", 0) == 0) s = s.substr(7);
    else if (s.rfind("https://", 0) == 0) s = s.substr(8);

    size_t colon = s.find(':');
    if (colon != std::string::npos) {
        out_host = s.substr(0, colon);
        size_t slash = s.find('/', colon);
        std::string port_str = (slash != std::string::npos) ? s.substr(colon + 1, slash - (colon + 1)) : s.substr(colon + 1);
        out_port = static_cast<uint16_t>(std::stoi(port_str));
    } else {
        size_t slash = s.find('/');
        out_host = (slash != std::string::npos) ? s.substr(0, slash) : s;
        out_port = 80;
    }
    return true;
}

static void seed_default_cluster_nodes(kube32::storage::Store& store) {
    std::string node1 = "{\"kind\":\"Node\",\"apiVersion\":\"v1\","
                        "\"metadata\":{\"name\":\"esp32-node-01\",\"labels\":{"
                        "\"kubernetes.io/hostname\":\"esp32-node-01\","
                        "\"kubernetes.io/arch\":\"xtensa\","
                        "\"node-role.kubernetes.io/worker\":\"\"}},"
                        "\"spec\":{\"unschedulable\":false},"
                        "\"status\":{"
                        "\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\",\"reason\":\"KubeletReady\"}],"
                        "\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"127.0.0.1\"}],"
                        "\"nodeInfo\":{\"kubeletVersion\":\"v1.33.0-kube32\",\"osImage\":\"ESP-IDF v5.2\","
                        "\"kernelVersion\":\"FreeRTOS-v10.4\",\"containerRuntimeVersion\":\"kube32-runtime://0.1.0\"},"
                        "\"capacity\":{\"memory\":\"320Ki\",\"cpu\":\"2\",\"esp.io/gpio\":\"16\",\"esp32.io/gpio\":\"16\","
                        "\"esp.io/i2c\":\"2\",\"esp32.io/i2c\":\"2\",\"esp.io/psram\":\"4Mi\",\"esp32.io/psram\":\"4Mi\"},"
                        "\"allocatable\":{\"memory\":\"240Ki\",\"cpu\":\"2\",\"esp.io/gpio\":\"14\",\"esp32.io/gpio\":\"14\","
                        "\"esp.io/i2c\":\"2\",\"esp32.io/i2c\":\"2\",\"esp.io/psram\":\"3840Ki\",\"esp32.io/psram\":\"3840Ki\"}"
                        "}}";

    std::string node2 = "{\"kind\":\"Node\",\"apiVersion\":\"v1\","
                        "\"metadata\":{\"name\":\"esp32-node-02\",\"labels\":{"
                        "\"kubernetes.io/hostname\":\"esp32-node-02\","
                        "\"kubernetes.io/arch\":\"xtensa\","
                        "\"node-role.kubernetes.io/worker\":\"\"}},"
                        "\"spec\":{\"unschedulable\":false},"
                        "\"status\":{"
                        "\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\",\"reason\":\"KubeletReady\"}],"
                        "\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"127.0.0.2\"}],"
                        "\"nodeInfo\":{\"kubeletVersion\":\"v1.33.0-kube32\",\"osImage\":\"ESP-IDF v5.2\","
                        "\"kernelVersion\":\"FreeRTOS-v10.4\",\"containerRuntimeVersion\":\"kube32-runtime://0.1.0\"},"
                        "\"capacity\":{\"memory\":\"320Ki\",\"cpu\":\"2\",\"esp.io/gpio\":\"16\",\"esp32.io/gpio\":\"16\","
                        "\"esp.io/i2c\":\"2\",\"esp32.io/i2c\":\"2\",\"esp.io/psram\":\"4Mi\",\"esp32.io/psram\":\"4Mi\"},"
                        "\"allocatable\":{\"memory\":\"240Ki\",\"cpu\":\"2\",\"esp.io/gpio\":\"14\",\"esp32.io/gpio\":\"14\","
                        "\"esp.io/i2c\":\"2\",\"esp32.io/i2c\":\"2\",\"esp.io/psram\":\"3840Ki\",\"esp32.io/psram\":\"3840Ki\"}"
                        "}}";

    store.put("/registry/nodes/esp32-node-01", node1, true);
    store.put("/registry/nodes/esp32-node-02", node2, true);
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#if defined(SIGPIPE)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    SimOptions opts = parse_sim_args(argc, argv);

    std::cout << "==================================================================" << std::endl;
    std::cout << "  Kube32 Embedded Cluster Host Simulation Daemon (kube32-sim)     " << std::endl;
    std::cout << "  Role: " << opts.role << " | Node: " << opts.node_name << " (ID: " << opts.node_id << ")" << std::endl;
    std::cout << "  Port: " << opts.port << " | IPC Port: " << opts.ipc_port << std::endl;
    std::cout << "==================================================================" << std::endl;

    if (!kube32::osal::Socket::init_networking()) {
        std::cerr << "[FATAL] Failed to initialize host networking." << std::endl;
        return 1;
    }

    // Initialize Channel IPC
    kube32::ipc::ChannelManager::instance().init(opts.node_id, opts.ipc_port - opts.node_id);

    if (opts.role == "worker") {
        // WORKER ROLE
        std::string master_host = "127.0.0.1";
        uint16_t master_port = 6443;

        if (!opts.join_url.empty() && opts.join_url != "auto") {
            parse_url(opts.join_url, master_host, master_port);
        } else {
            // Auto-discovery via mDNS
            std::cout << "[M4-NET] Discovering Kube32 Control Plane via ZeroConf mDNS..." << std::endl;
            auto mdns = kube32::net::create_mdns_discovery();
            kube32::net::ServiceEndpoint ep;
            if (mdns->discover_service("_kube32-api._tcp", 3000, ep)) {
                master_host = ep.host_ip;
                master_port = ep.port;
                std::cout << "[M4-NET] Discovered Control Plane at " << master_host << ":" << master_port << std::endl;
            } else {
                std::cout << "[M4-NET] mDNS discovery timed out, falling back to 127.0.0.1:6443" << std::endl;
            }
        }

        auto wasm_runtime = kube32::runtime::create_wasm_runtime();
        kube32::kubelet::KubeletConfig kubelet_cfg;
        kubelet_cfg.node_name = opts.node_name;
        kubelet_cfg.api_server_host = master_host;
        kubelet_cfg.api_server_port = master_port;
        kubelet_cfg.node_ip = "127.0.0.1";
        kubelet_cfg.heartbeat_interval_ms = opts.lease_interval_ms;

        auto kubelet = std::make_unique<kube32::kubelet::Kubelet>(kubelet_cfg, *wasm_runtime);
        if (!kubelet->start()) {
            std::cerr << "[FATAL] Failed to start Kubelet worker agent." << std::endl;
            return 1;
        }

        std::cout << "[READY] Kubelet worker agent running for " << opts.node_name
                  << " -> connected to http://" << master_host << ":" << master_port << std::endl;

        while (g_sim_running.load()) {
            kube32::osal::Thread::sleep_ms(250);
        }

        std::cout << "\n[SHUTDOWN] Stopping Kubelet worker agent..." << std::endl;
        kubelet->stop();
        kube32::ipc::ChannelManager::instance().shutdown();
        kube32::osal::Socket::cleanup_networking();
        return 0;
    }

    // CONTROL PLANE / STANDALONE ROLE
    kube32::storage::StoreOptions store_opts;
    store_opts.base_path = opts.data_dir;
    store_opts.enable_flash_persistence = opts.enable_flash;
    auto store = kube32::storage::create_radix_store(store_opts);

    if (opts.enable_flash) {
        store->recover();
        std::cout << "[INIT] LittleFS journal state recovered from " << opts.data_dir << std::endl;
    }

    if (opts.role == "standalone") {
        seed_default_cluster_nodes(*store);
        std::cout << "[INIT] Seeded simulated cluster nodes (esp32-node-01, esp32-node-02)." << std::endl;
    }

    // Start Node Lifecycle Controller
    kube32::controller::NodeLifecycleConfig nl_cfg;
    nl_cfg.check_interval_ms = 500;
    nl_cfg.not_ready_threshold_ms = opts.not_ready_timeout_ms;
    nl_cfg.eviction_threshold_ms = opts.eviction_timeout_ms;
    auto node_lifecycle = std::make_unique<kube32::controller::NodeLifecycleController>(*store, nl_cfg);
    if (!node_lifecycle->start()) {
        std::cerr << "[FATAL] Failed to start Node Lifecycle Controller." << std::endl;
        return 1;
    }
    std::cout << "[INIT] Node Lifecycle Controller active (NotReady: " << opts.not_ready_timeout_ms
              << "ms, Evict: " << opts.eviction_timeout_ms << "ms)." << std::endl;

    // Start Autonomous Scheduler
    kube32::scheduler::SchedulerConfig sched_config;
    sched_config.strategy = kube32::scheduler::ScoringStrategy::LeastAllocated;
    sched_config.reconcile_interval_ms = opts.reconcile_interval_ms;
    sched_config.node_heartbeat_timeout_ms = opts.eviction_timeout_ms;
    auto scheduler = kube32::scheduler::create_scheduler(*store, sched_config);
    if (!scheduler->start()) {
        std::cerr << "[FATAL] Failed to start autonomous scheduler." << std::endl;
        node_lifecycle->stop();
        return 1;
    }
    std::cout << "[INIT] Autonomous scheduler started with LeastAllocated scoring." << std::endl;

    // Start HTTP Server
    kube32::api::HttpServerOptions http_opts;
    http_opts.host = "0.0.0.0";
    http_opts.port = opts.port;
    http_opts.backlog = 20;
    http_opts.rx_buffer_size = 16384;
    auto server = std::make_unique<kube32::api::HttpServer>(http_opts, *store, scheduler.get());

    // Connect Lease updates to Node Lifecycle Controller
    server->route_handler().set_lease_callback([nl = node_lifecycle.get()](const std::string& node_name) {
        if (nl) {
            nl->record_node_lease(node_name);
        }
    });

    if (!server->start()) {
        std::cerr << "[FATAL] Failed to bind HTTP API server to port " << opts.port << std::endl;
        scheduler->stop();
        node_lifecycle->stop();
        return 1;
    }

    std::cout << "[READY] Kube32 API Server listening on http://0.0.0.0:" << opts.port << std::endl;

    // Start ZeroConf mDNS announcement
    auto mdns = kube32::net::create_mdns_discovery();
    std::map<std::string, std::string> txt_records = {
        {"version", "v1.33.0-kube32"},
        {"cluster_id", "kube32-cluster"},
        {"node_name", opts.node_name}
    };
    mdns->start_service_announcement("_kube32-api._tcp", opts.port, txt_records);
    std::cout << "[M4-NET] ZeroConf mDNS announcing service _kube32-api._tcp on port " << opts.port << std::endl;

    std::unique_ptr<kube32::kubelet::Kubelet> local_kubelet;
    std::unique_ptr<kube32::kubelet::Kubelet> local_kubelet2;
    std::unique_ptr<kube32::runtime::Runtime> wasm_runtime;
    if (opts.role == "standalone") {
        wasm_runtime = kube32::runtime::create_wasm_runtime();
        kube32::kubelet::KubeletConfig kubelet_cfg;
        kubelet_cfg.node_name = "esp32-node-01";
        kubelet_cfg.api_server_host = "127.0.0.1";
        kubelet_cfg.api_server_port = opts.port;
        kubelet_cfg.node_ip = "127.0.0.1";
        kubelet_cfg.heartbeat_interval_ms = opts.lease_interval_ms;
        local_kubelet = std::make_unique<kube32::kubelet::Kubelet>(kubelet_cfg, *wasm_runtime);
        local_kubelet->start();
        std::cout << "[INIT] ESP32 Kubelet node agent running for esp32-node-01." << std::endl;

        kube32::kubelet::KubeletConfig kubelet_cfg2 = kubelet_cfg;
        kubelet_cfg2.node_name = "esp32-node-02";
        local_kubelet2 = std::make_unique<kube32::kubelet::Kubelet>(kubelet_cfg2, *wasm_runtime);
        local_kubelet2->start();
        std::cout << "[INIT] ESP32 Kubelet node agent running for esp32-node-02." << std::endl;
    }

    std::cout << "[READY] Ready for native kubectl and E2E test suite interactions." << std::endl;

    while (g_sim_running.load()) {
        kube32::osal::Thread::sleep_ms(250);
    }

    std::cout << "\n[SHUTDOWN] Intercepted termination signal. Stopping services..." << std::endl;
    if (local_kubelet) local_kubelet->stop();
    if (local_kubelet2) local_kubelet2->stop();
    mdns->stop_service_announcement();
    server->stop();
    scheduler->stop();
    node_lifecycle->stop();
    kube32::ipc::ChannelManager::instance().shutdown();
    kube32::osal::Socket::cleanup_networking();
    std::cout << "[SHUTDOWN] Kube32 Simulation Daemon cleanly terminated." << std::endl;

    return 0;
}
