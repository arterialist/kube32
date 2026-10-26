#include "osal/socket.hpp"
#include "osal/thread.hpp"
#include "storage/store.hpp"
#include "scheduler/scheduler.hpp"
#include "api/http_server.hpp"

#include <csignal>
#include <iostream>
#include <atomic>

static std::atomic<bool> g_sim_running{true};

static void signal_handler(int sig) {
    (void)sig;
    g_sim_running.store(false);
}

static void seed_default_cluster_nodes(kube32::storage::Store& store) {
    // Node 1: esp32-node-01
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

    // Node 2: esp32-node-02
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
    (void)argc;
    (void)argv;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "==================================================================" << std::endl;
    std::cout << "  Kube32 Embedded Cluster Host Simulation Daemon (kube32-sim)     " << std::endl;
    std::cout << "  Architecture: ESP32-S3 Dual-Core Simulation on POSIX Host       " << std::endl;
    std::cout << "==================================================================" << std::endl;

    if (!kube32::osal::Socket::init_networking()) {
        std::cerr << "[FATAL] Failed to initialize host networking." << std::endl;
        return 1;
    }

    kube32::storage::StoreOptions store_opts;
    store_opts.base_path = "/tmp/kube32_sim_data";
    store_opts.enable_flash_persistence = false; // In-memory for fast simulation
    auto store = kube32::storage::create_radix_store(store_opts);

    // Seed default cluster state
    seed_default_cluster_nodes(*store);
    std::cout << "[INIT] Seeded simulated cluster nodes (esp32-node-01, esp32-node-02)." << std::endl;

    // Start Autonomous Scheduler
    kube32::scheduler::SchedulerConfig sched_config;
    sched_config.strategy = kube32::scheduler::ScoringStrategy::LeastAllocated;
    sched_config.reconcile_interval_ms = 1000;
    auto scheduler = kube32::scheduler::create_scheduler(*store, sched_config);
    if (!scheduler->start()) {
        std::cerr << "[FATAL] Failed to start autonomous scheduler." << std::endl;
        return 1;
    }
    std::cout << "[INIT] Autonomous scheduler started with LeastAllocated anti-fragmentation scoring." << std::endl;

    // Start HTTP Server
    kube32::api::HttpServerOptions http_opts;
    http_opts.host = "0.0.0.0";
    http_opts.port = 6443;
    http_opts.backlog = 20;
    http_opts.rx_buffer_size = 16384;
    kube32::api::HttpServer server(http_opts, *store, scheduler.get());

    if (!server.start()) {
        std::cerr << "[FATAL] Failed to bind HTTP API server to port 6443." << std::endl;
        scheduler->stop();
        return 1;
    }

    std::cout << "[READY] Kube32 API Server listening on http://0.0.0.0:6443" << std::endl;
    std::cout << "[READY] Ready for native kubectl and E2E test suite interactions." << std::endl;

    while (g_sim_running.load()) {
        kube32::osal::Thread::sleep_ms(250);
    }

    std::cout << "\n[SHUTDOWN] Intercepted termination signal. Stopping services..." << std::endl;
    server.stop();
    scheduler->stop();
    kube32::osal::Socket::cleanup_networking();
    std::cout << "[SHUTDOWN] Kube32 Simulation Daemon cleanly terminated." << std::endl;

    return 0;
}
