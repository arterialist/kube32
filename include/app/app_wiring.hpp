#pragma once

// Glue shared by the firmware and the host simulator: starts the ingress and
// pod HTTP servers, connects the app ABI event hook to the channel layer, and
// routes cluster events to SSE viewers and to event-receiving pods.

#include "app/ingress.hpp"
#include "api/http_server.hpp"

#include <memory>
#include <string>

namespace kube32::app {

struct AppWiringOptions {
    std::string app_name{"tessera"};
    std::string core_pod{"tessera-core"};
    std::string core_namespace{"default"};
    std::string local_node_name;
    storage::Store* store{nullptr};
    std::string upstream_override_ip;   // host sim only

    bool run_ingress{false};
    uint16_t ingress_port{80};
    uint32_t ingress_workers{3};
    size_t ingress_worker_stack{8192};
    size_t ingress_rx_buffer{4096};

    bool run_pod_server{false};
    uint16_t pod_port{7000};
    uint32_t pod_workers{2};
    size_t pod_worker_stack{16384};

    int core_affinity{-1};
    bool stacks_in_psram{false};
    // When false the owner broadcasts the "cluster" SSE event itself from a
    // task with enough stack (the hub thread stays at 4 KB on the firmware).
    bool cluster_events_from_hub{true};
};

class AppWiring {
public:
    explicit AppWiring(AppWiringOptions opts);
    ~AppWiring();

    bool start();
    void stop();

    Ingress* ingress() { return ingress_.get(); }
    api::HttpServer* ingress_server() { return ingress_server_.get(); }
    api::HttpServer* pod_server() { return pod_server_.get(); }

private:
    AppWiringOptions opts_;
    std::unique_ptr<Ingress> ingress_;
    std::unique_ptr<api::HttpServer> ingress_server_;
    std::unique_ptr<api::HttpServer> pod_server_;
    PodServerHandler pod_handler_;
    uint32_t sub_events_{0};
    uint32_t sub_input_{0};
    uint32_t sub_stats_{0};
};

// Installs the default platform hooks: event_emit through the channel layer,
// random from std::rand (firmware overrides random and the hardware hooks).
void install_default_platform_hooks();

} // namespace kube32::app
