#include "osal/socket.hpp"
#include "hal/sd_card.hpp"
#include "hal/node_storage.hpp"
#include "storage/flash_log.hpp"
#include "osal/thread.hpp"
#include "storage/store.hpp"
#include "scheduler/scheduler.hpp"
#include "controller/node_lifecycle_controller.hpp"
#include "controller/deployment_controller.hpp"
#include "controller/workload_controller.hpp"
#include "runtime/pod_memory_pool.h"
#include "api/http_server.hpp"
#include "runtime/wasm_runtime.hpp"
#include "kubelet/kubelet.hpp"
#include "ipc/channel.hpp"
#include "net/mdns_discovery.hpp"
#include "app/ingress_controller.hpp"
#include "app/pod_server.hpp"
#include "app/service_client.hpp"
#include "app/node_storage_api.hpp"
#include "api/node_proxy.hpp"
#include "kubelet/kube_api_client.hpp"
#include "osal/system.hpp"
#include "runtime/image_registry.hpp"
#include "embedded/embedded_blobs.hpp"
#include "runtime/pod_log.hpp"
#include "models/kube_models.hpp"

#include "osal/tls.hpp"

#include <csignal>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <iostream>
#include <atomic>
#include <string>
#include <vector>

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
    // Application traffic
    uint16_t ingress_port{0};      // control plane: Ingress controller on this port (0: off)
    uint16_t pod_port{7000};       // every node: pod HTTP server for request-handling pods
    std::vector<std::string> peers; // "name:id:ipc_port" static peers for UDP channels
    // Standalone: the control plane's own kubelet and the in-process workers
    std::string kubelet_name{"esp32-node-01"};
    std::vector<std::string> workers{"esp32-node-02"};
    std::string board{"generic"};          // kube32.io/board of this process's kubelet (worker role)
    std::string worker_board{"cardputer"}; // kube32.io/board of the in-process workers
    std::string token;                     // API bearer token (also KUBE32_API_TOKEN)
    bool tls{false};                       // HTTPS on the API port next to plain HTTP
    std::string node_ip{"127.0.0.1"};      // address the nodes report (pod IPs): a LAN address lets boards reach their pods
    // --psram 0 models a board without PSRAM: the internal pod pool, the
    // shared interpreter stack and this much heap left for pods and the
    // system (a Cardputer has about 51 KiB at idle)
    size_t heap_budget{51 * 1024};
    bool board_without_psram{false};       // --psram 0 given
    // Stand-in for PSRAM: a block of host memory reserved like the control
    // plane board's PSRAM pod pool. -1: the role's default (1284Ki, the
    // board's reservation, except for workers, which model the Cardputer).
    int64_t psram_bytes{-1};
};

static void print_sim_usage(std::ostream& out) {
    out << "Usage: kube32-sim [options]\n"
           "\n"
           "Runs kube32 nodes as host processes. With no options, one process runs the\n"
           "control plane, its kubelet and one in-process worker.\n"
           "\n"
           "Cluster:\n"
           "  --role <role>               standalone (default), control-plane or worker\n"
           "  --port <port>               API server port (default 6443)\n"
           "  --join <url>                worker: API server to register with\n"
           "  --token <token>             API bearer token (also KUBE32_API_TOKEN)\n"
           "  --tls                       serve HTTPS next to plain HTTP on the API port\n"
           "  --data-dir <dir>            store and node storage root (default /tmp/kube32_<node>_data)\n"
           "  --enable-flash              persist the object store in <data-dir>\n"
           "\n"
           "Nodes:\n"
           "  --node-name <name>          control-plane name (default esp32-master)\n"
           "  --kubelet-name <name>       name of this process's kubelet (default esp32-node-01)\n"
           "  --workers <a,b,...>         in-process worker nodes (default esp32-node-02, \"\" for none)\n"
           "  --board <board>             kube32.io/board label of this process's kubelet (default generic)\n"
           "  --worker-board <board>      kube32.io/board label of in-process workers (default cardputer)\n"
           "  --node-ip <ip>              address nodes report for their pods (default 127.0.0.1)\n"
           "  --psram <size>              PSRAM to model, e.g. 8Mi; 0 models a board without PSRAM\n"
           "  --heap-budget <size>        heap left for pods and system with --psram 0 (default 51Ki)\n"
           "\n"
           "Traffic:\n"
           "  --ingress-port <port>       Ingress controller port (default off)\n"
           "  --pod-port <port>           pod HTTP server port (default 7000)\n"
           "  --node-id <id>              node id for IPC channels (default 0)\n"
           "  --ipc-port <port>           UDP port for IPC channels (default 7400 + node id)\n"
           "  --peer <name:id:port>       static IPC peer, repeatable\n"
           "\n"
           "Timing (milliseconds):\n"
           "  --lease-interval <ms>       node heartbeat interval (default 5000)\n"
           "  --not-ready-timeout <ms>    NotReady after this without a heartbeat (default 15000)\n"
           "  --eviction-timeout <ms>     taint a node NoExecute after this long without a heartbeat (default 30000)\n"
           "  --reconcile-interval <ms>   controller loop interval (default 1000)\n"
           "\n"
           "  -h, --help                  show this help\n";
}

static SimOptions parse_sim_args(int argc, char* argv[]) {
    SimOptions opts;
    bool has_role = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { print_sim_usage(std::cout); std::exit(0); }
        else if (arg == "--role" && i + 1 < argc) { opts.role = argv[++i]; has_role = true; }
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
        else if (arg == "--ingress-port" && i + 1 < argc) opts.ingress_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--pod-port" && i + 1 < argc) opts.pod_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--peer" && i + 1 < argc) opts.peers.push_back(argv[++i]);
        else if (arg == "--kubelet-name" && i + 1 < argc) opts.kubelet_name = argv[++i];
        else if (arg == "--workers" && i + 1 < argc) {
            opts.workers.clear();
            std::string list = argv[++i];
            size_t start = 0;
            while (start <= list.size()) {
                size_t comma = list.find(',', start);
                if (comma == std::string::npos) comma = list.size();
                if (comma > start) opts.workers.push_back(list.substr(start, comma - start));
                start = comma + 1;
            }
        }
        else if (arg == "--board" && i + 1 < argc) opts.board = argv[++i];
        else if (arg == "--worker-board" && i + 1 < argc) opts.worker_board = argv[++i];
        else if (arg == "--token" && i + 1 < argc) opts.token = argv[++i];
        else if (arg == "--tls") opts.tls = true;
        else if (arg == "--node-ip" && i + 1 < argc) opts.node_ip = argv[++i];
        else if (arg == "--heap-budget" && i + 1 < argc) opts.heap_budget = static_cast<size_t>(kube32::models::parse_quantity(argv[++i]));
        else if (arg == "--psram" && i + 1 < argc) {
            opts.psram_bytes = static_cast<int64_t>(kube32::models::parse_quantity(argv[++i]));
            opts.board_without_psram = (opts.psram_bytes == 0);
        }
        else {
            std::cerr << "kube32-sim: unknown option or missing value: " << arg << "\n\n";
            print_sim_usage(std::cerr);
            std::exit(2);
        }
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
    if (opts.psram_bytes < 0) {
        opts.psram_bytes = (opts.role == "worker") ? 0 : 20 * 64 * 1024 + 4096;
    }
    if (opts.token.empty()) {
        const char* t = std::getenv("KUBE32_API_TOKEN");
        if (t) opts.token = t;
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

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#if defined(SIGPIPE)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    SimOptions opts = parse_sim_args(argc, argv);

    // Host has memory to spare: 16 KiB per pod log ring (the device keeps 2 KiB)
    kube32::runtime::PodLogStore::runtime().set_ring_capacity(16 * 1024);
    kube32::runtime::PodLogStore::apiserver().set_ring_capacity(16 * 1024);

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
    kube32::ipc::ChannelManager::instance().set_local_identity(opts.node_id, opts.node_name);
    for (const auto& p : opts.peers) {
        // name:id:port
        size_t c1 = p.find(':');
        size_t c2 = (c1 == std::string::npos) ? std::string::npos : p.find(':', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) continue;
        kube32::ipc::PeerInfo info;
        info.node_name = p.substr(0, c1);
        info.node_id = static_cast<uint32_t>(std::stoul(p.substr(c1 + 1, c2 - c1 - 1)));
        info.endpoint.is_espnow = false;
        info.endpoint.ip_address = "127.0.0.1";
        info.endpoint.udp_port = static_cast<uint16_t>(std::stoul(p.substr(c2 + 1)));
        info.endpoint.node_id = info.node_id;
        kube32::ipc::ChannelManager::instance().add_peer(info);
    }
    kube32::ipc::ChannelManager::instance().start_background(2000);
    // Pods may emit events and print to the stand-in display from their first call
    kube32::app::install_default_platform_hooks();

    // Local image registry from the embedded table
    {
        size_t n = 0;
        const auto* imgs = kube32::embedded::images(n);
        kube32::runtime::ImageRegistry::instance().add_all(imgs, n);
        std::cout << "[INIT] Image registry: " << n << " embedded image(s)" << std::endl;
    }

    // The host has no PSRAM. The simulator reserves a block of host memory as
    // its stand-in, the way the control plane board reserves its PSRAM pod
    // pool; nodes report it as esp32.io/psram.
    if (opts.psram_bytes > 0) {
        kube32_pool_reserve_external(static_cast<size_t>(opts.psram_bytes));
        std::cout << "[SIM] PSRAM stand-in: " << (kube32_pool_block_size() / 1024) << " KiB of host memory (--psram)" << std::endl;
    } else if (opts.board_without_psram) {
        // A board without PSRAM, as main_esp32.cpp sets it up: the 80 KiB pod
        // pool, the 28 KiB interpreter stack, and pods limited to the heap the
        // board has (--heap-budget) less the system's 24 KiB
        kube32_pool_reserve(80 * 1024);
        kube32_pool_reserve_stack(28672);
        const size_t interp = kube32_pool_stack_size();
        kube32_interp_stack_set(kube32_pool_stack_alloc(interp), interp);
        kube32_pool_set_heap_budget(opts.heap_budget);
        std::cout << "[SIM] board without PSRAM: " << (kube32_pool_block_size() / 1024) << " KiB pod pool, " << (interp / 1024)
                  << " KiB interpreter stack, " << (opts.heap_budget / 1024) << " KiB heap (--heap-budget)" << std::endl;
    }

    // Node storage: the simulated microSD card (the KUBE32_SD_ROOT directory)
    // when set, otherwise --data-dir standing in for internal flash
    if (kube32::hal::node_storage_mount(true, opts.data_dir)) {
        kube32::hal::NodeStorageInfo si = kube32::hal::node_storage_info();
        std::cout << "[STORAGE] " << kube32::hal::storage_medium_name(si.medium) << " at " << si.root << ": "
                  << (si.total_bytes / (1024 * 1024)) << " MB total, " << (si.free_bytes / (1024 * 1024)) << " MB free" << std::endl;
    } else {
        std::cout << "[STORAGE] none: " << kube32::hal::node_storage_info().error << std::endl;
    }

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
        kubelet_cfg.node_ip = opts.node_ip;
        kubelet_cfg.heartbeat_interval_ms = opts.lease_interval_ms;
        kubelet_cfg.api_token = opts.token;
        kubelet_cfg.node_labels["kube32.io/board"] = opts.board;
        kubelet_cfg.kubelet_port = opts.pod_port;

        // k32_http_call: Services resolved through the API server
        auto api_client = std::make_shared<kube32::kubelet::HttpKubeClient>(master_host, master_port);
        api_client->set_bearer_token(opts.token);
        kube32::app::ServiceClient service_client(kube32::app::make_api_service_resolver(
            [api_client](const std::string& path, std::string& body) { return api_client->get(path, body); }));
        kube32::app::install_service_client(&service_client);

        auto kubelet = std::make_unique<kube32::kubelet::Kubelet>(kubelet_cfg, *wasm_runtime);
        if (!kubelet->start()) {
            std::cerr << "[FATAL] Failed to start Kubelet worker agent." << std::endl;
            return 1;
        }

        std::cout << "[READY] Kubelet worker agent running for " << opts.node_name
                  << " -> connected to http://" << master_host << ":" << master_port << std::endl;

        kube32::app::PodServerOptions po;
        po.port = opts.pod_port;
        po.workers = 2;
        po.worker_stack = 262144;
        po.token = opts.token;
        kube32::app::PodServer pod_server(po);
        pod_server.start();

        // As on the boards: a pod whose k32_tick returns >0 has work left and
        // is ticked again after 10 ms; otherwise once a second.
        uint64_t last_tick = 0;
        bool busy = false;
        while (g_sim_running.load()) {
            kube32::osal::Thread::sleep_ms(busy ? 10 : 250);
            uint64_t now = kube32::osal::System::get_uptime_ms();
            if (busy || now - last_tick >= 1000) {
                if (now - last_tick >= 1000) last_tick = now;
                busy = false;
                for (const auto& id : kube32::runtime::AppPodHost::instance().list_pods()) {
                    if (kube32::runtime::AppPodHost::instance().tick(id, static_cast<uint32_t>(now)) > 0) busy = true;
                }
            }
        }

        std::cout << "\n[SHUTDOWN] Stopping Kubelet worker agent..." << std::endl;
        kube32::app::install_service_client(nullptr);
        pod_server.stop();
        kubelet->stop();
        kube32::ipc::ChannelManager::instance().shutdown();
        kube32::osal::Socket::cleanup_networking();
        return 0;
    }

    // CONTROL PLANE / STANDALONE ROLE
    // --enable-flash: the object store journal lives on the node storage
    // root, as on the board (<root>/kube32/store/journal.img)
    kube32::storage::StoreOptions store_opts;
    store_opts.base_path = opts.data_dir;
    store_opts.enable_flash_persistence = false;
    std::unique_ptr<kube32::storage::FlashRegion> journal;
    if (opts.enable_flash) {
        const std::string dir = kube32::hal::node_storage_path("store");
        if (!dir.empty() && kube32::hal::node_storage_make_dirs(dir)) {
            journal = kube32::storage::create_file_flash_region(dir + "/journal.img", 4 * 1024 * 1024);
        }
        if (journal) {
            store_opts.enable_flash_persistence = true;
            store_opts.flash_region = journal.get();
        } else {
            std::cerr << "[STORE] no journal: node storage is not mounted" << std::endl;
        }
    }
    auto store = kube32::storage::create_radix_store(store_opts);

    if (journal) {
        kube32::storage::Status rs = store->recover();
        std::vector<std::string> keys;
        store->list("/registry/", keys);
        std::cout << "[STORE] journal " << kube32::hal::node_storage_path("store/journal.img") << ": " << keys.size()
                  << " object(s) recovered" << (rs.ok() ? "" : " (" + rs.message + ")") << std::endl;
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
              << "ms, NoExecute: " << opts.eviction_timeout_ms << "ms)." << std::endl;

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

    // Deployments: replicas from the template, status from the pods that exist
    kube32::controller::DeploymentControllerConfig dc_cfg;
    dc_cfg.reconcile_interval_ms = opts.reconcile_interval_ms;
    auto deployments = std::make_unique<kube32::controller::DeploymentController>(*store, scheduler.get(), dc_cfg);
    if (!deployments->start()) {
        std::cerr << "[FATAL] Failed to start Deployment controller." << std::endl;
        scheduler->stop();
        node_lifecycle->stop();
        return 1;
    }
    std::cout << "[INIT] Deployment controller active." << std::endl;

    // DaemonSets and Jobs: their pods, from the nodes and pods that exist
    kube32::controller::WorkloadControllerConfig wc_cfg;
    wc_cfg.reconcile_interval_ms = opts.reconcile_interval_ms;
    auto workloads = std::make_unique<kube32::controller::WorkloadController>(*store, scheduler.get(), wc_cfg);
    if (!workloads->start()) {
        std::cerr << "[FATAL] Failed to start DaemonSet/Job controller." << std::endl;
        deployments->stop();
        scheduler->stop();
        node_lifecycle->stop();
        return 1;
    }
    std::cout << "[INIT] DaemonSet and Job controller active." << std::endl;

    // Start HTTP Server
    kube32::api::HttpServerOptions http_opts;
    http_opts.host = "0.0.0.0";
    http_opts.port = opts.port;
    http_opts.backlog = 20;
    http_opts.rx_buffer_size = 16384;
    http_opts.watch_streams = 16;        // k9s opens several informers at once
    http_opts.watch_stack_bytes = 65536;
    // --tls: the key and self-signed certificate live on the node storage
    // root (<root>/kube32/tls/), so the certificate survives restarts
    std::unique_ptr<kube32::osal::TlsServer> tls;
    if (opts.tls) {
        const std::string dir = kube32::hal::node_storage_path("tls");
        auto read_file = [](const std::string& path, std::string& out) {
            std::ifstream f(path, std::ios::binary);
            if (!f) return false;
            out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            return !out.empty();
        };
        std::string err;
        tls = kube32::osal::TlsServer::create({opts.node_name}, {},
            [&](std::string& key, std::string& cert) { return !dir.empty() && read_file(dir + "/key.der", key) && read_file(dir + "/cert.der", cert); },
            [&](const std::string& key, const std::string& cert) {
                if (dir.empty() || !kube32::hal::node_storage_make_dirs(dir)) return;
                std::ofstream(dir + "/key.der", std::ios::binary) << key;
                std::ofstream(dir + "/cert.der", std::ios::binary) << cert;
            }, err);
        if (tls) {
            http_opts.tls = tls.get();
            std::cout << "[TLS] https://127.0.0.1:" << opts.port << " (tls-server-name kube32-apiserver)" << std::endl;
        } else {
            std::cerr << "[TLS] off: " << err << std::endl;
        }
    }
    auto server = std::make_unique<kube32::api::HttpServer>(http_opts, *store, scheduler.get());

    // Bearer token for every API request except the health probes and /version
    server->route_handler().set_bearer_token(opts.token);

    // Node proxy (/api/v1/nodes/<node>/proxy/...): node storage of this
    // process's own node in-process, other nodes through their kubelet endpoint
    const std::string local_node = (opts.role == "standalone") ? opts.kubelet_name : opts.node_name;
    kube32::api::NodeProxyHandler node_proxy(*store, server->route_handler(), local_node,
        [](kube32::api::HttpConnection& conn, std::string_view path) {
            if (!kube32::app::is_node_storage_path(path)) return false;
            return kube32::app::serve_node_storage(conn, path, std::string(), 64u * 1024 * 1024);
        });
    node_proxy.set_node_token(opts.token);
    server->set_handler(&node_proxy);

    // k32_http_call: Services resolved from the store
    kube32::app::ServiceClient service_client(kube32::app::make_store_service_resolver(*store));
    kube32::app::install_service_client(&service_client);

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
    std::vector<std::unique_ptr<kube32::kubelet::Kubelet>> worker_kubelets;
    std::unique_ptr<kube32::runtime::Runtime> wasm_runtime;
    // The standalone simulator's nodes share one pod HTTP server, which is
    // also their kubelet endpoint (node storage through the node proxy)
    std::unique_ptr<kube32::app::PodServer> pod_server;
    if (opts.role == "standalone") {
        kube32::app::PodServerOptions po;
        po.port = opts.pod_port;
        po.worker_stack = 262144;
        po.token = opts.token;
        pod_server = std::make_unique<kube32::app::PodServer>(po);
        if (!pod_server->start()) pod_server.reset();
    }
    if (opts.role == "standalone") {
        wasm_runtime = kube32::runtime::create_wasm_runtime();
        kube32::kubelet::KubeletConfig kubelet_cfg;
        kubelet_cfg.node_name = opts.kubelet_name;
        kubelet_cfg.control_plane = true;
        kubelet_cfg.api_server_host = "127.0.0.1";
        kubelet_cfg.api_server_port = opts.port;
        kubelet_cfg.node_ip = opts.node_ip;
        kubelet_cfg.heartbeat_interval_ms = opts.lease_interval_ms;
        kubelet_cfg.api_token = opts.token;
        kubelet_cfg.node_labels["kube32.io/board"] = opts.board;
        kubelet_cfg.kubelet_port = pod_server ? opts.pod_port : 0;
        local_kubelet = std::make_unique<kube32::kubelet::Kubelet>(kubelet_cfg, *wasm_runtime);
        local_kubelet->start();
        std::cout << "[INIT] Kubelet running for simulated node " << opts.kubelet_name << " (control plane)." << std::endl;

        for (const auto& worker : opts.workers) {
            kube32::kubelet::KubeletConfig wcfg = kubelet_cfg;
            wcfg.node_name = worker;
            wcfg.control_plane = false;
            wcfg.node_labels["kube32.io/board"] = opts.worker_board;
            worker_kubelets.push_back(std::make_unique<kube32::kubelet::Kubelet>(wcfg, *wasm_runtime));
            worker_kubelets.back()->start();
            std::cout << "[INIT] Kubelet running for simulated node " << worker << " (worker)." << std::endl;
        }
    }

    std::cout << "[READY] Ready for native kubectl and E2E test suite interactions." << std::endl;

    // Ingress controller: port --ingress-port, routes from Ingress objects
    std::unique_ptr<kube32::app::IngressController> ingress;
    if (opts.ingress_port > 0) {
        kube32::app::IngressControllerOptions io;
        io.store = store.get();
        io.local_node_name = (opts.role == "standalone") ? opts.kubelet_name : opts.node_name;
        io.address = [ip = opts.node_ip]() { return ip; };
        io.port = opts.ingress_port;
        io.workers = 4;
        io.worker_stack = 262144;
        ingress = std::make_unique<kube32::app::IngressController>(io);
        ingress->start();
    }

    // Pod ticks as on the boards (10 ms while a pod reports work left, else
    // 1 Hz); ingress and store housekeeping once a second.
    uint64_t last_tick = 0;
    bool busy = false;
    while (g_sim_running.load()) {
        kube32::osal::Thread::sleep_ms(busy ? 10 : 250);
        uint64_t now = kube32::osal::System::get_uptime_ms();
        const bool second = now - last_tick >= 1000;
        if (busy || second) {
            busy = false;
            for (const auto& id : kube32::runtime::AppPodHost::instance().list_pods()) {
                if (kube32::runtime::AppPodHost::instance().tick(id, static_cast<uint32_t>(now)) > 0) busy = true;
            }
        }
        if (second) {
            last_tick = now;
            if (ingress) ingress->tick();
            store->flush(false);
        }
    }

    std::cout << "\n[SHUTDOWN] Intercepted termination signal. Stopping services..." << std::endl;
    kube32::app::install_service_client(nullptr);
    if (ingress) ingress->stop();
    if (pod_server) pod_server->stop();
    if (local_kubelet) local_kubelet->stop();
    for (auto& k : worker_kubelets) k->stop();
    mdns->stop_service_announcement();
    server->stop();
    workloads->stop();
    deployments->stop();
    scheduler->stop();
    node_lifecycle->stop();
    store->flush(true);
    kube32::ipc::ChannelManager::instance().shutdown();
    kube32::osal::Socket::cleanup_networking();
    std::cout << "[SHUTDOWN] Kube32 Simulation Daemon cleanly terminated." << std::endl;

    return 0;
}
