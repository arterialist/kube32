#include "app/app_wiring.hpp"
#include "ipc/channel.hpp"
#include "runtime/app_pod.hpp"
#include "osal/system.hpp"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <iostream>

namespace kube32::app {

void install_default_platform_hooks() {
    runtime::AppPlatformHooks hooks = runtime::app_platform_hooks();
    if (!hooks.event_emit) {
        hooks.event_emit = [](const std::string& pod_id, std::string_view topic, const uint8_t* data, size_t len) -> int32_t {
            int32_t rc = ipc::ChannelManager::instance().publish(pod_id, std::string(topic), data, len);
            static int traced = 0;
            if (traced < 8) {
                traced++;
                printf("[TRACE] emit pod=%s topic=%.*s len=%u rc=%d peers=%u\n", pod_id.c_str(), (int)topic.size(), topic.data(), (unsigned)len, (int)rc,
                       (unsigned)ipc::ChannelManager::instance().peers().size());
            }
            return rc;
        };
    }
    if (!hooks.random) {
        hooks.random = []() -> uint32_t {
            return (static_cast<uint32_t>(std::rand()) << 16) ^ static_cast<uint32_t>(std::rand());
        };
    }
    if (!hooks.display_text) {
        hooks.display_text = [](int32_t x, int32_t y, const char* text, size_t len, uint32_t, uint32_t) -> int32_t {
            std::cout << "[DISPLAY " << x << "," << y << "] " << std::string(text, len) << std::endl;
            return 0;
        };
    }
    if (!hooks.display_rect) hooks.display_rect = [](int32_t, int32_t, int32_t, int32_t, uint32_t) -> int32_t { return 0; };
    if (!hooks.display_bar) hooks.display_bar = [](int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t) -> int32_t { return 0; };
    if (!hooks.key_read) hooks.key_read = []() -> int32_t { return -11; };
    if (!hooks.tone) hooks.tone = [](uint32_t f, uint32_t ms, uint32_t) -> int32_t {
        std::cout << "[TONE] " << f << " Hz for " << ms << " ms" << std::endl;
        return 0;
    };
    runtime::set_app_platform_hooks(hooks);
}

AppWiring::AppWiring(AppWiringOptions opts) : opts_(std::move(opts)) {}

AppWiring::~AppWiring() {
    stop();
}

bool AppWiring::start() {
    install_default_platform_hooks();

    if (opts_.run_pod_server) {
        api::HttpServerOptions o;
        o.host = "0.0.0.0";
        o.port = opts_.pod_port;
        o.backlog = 4;
        o.rx_buffer_size = 4096;
        o.max_body_size = 16 * 1024;
        o.workers = opts_.pod_workers;
        o.worker_stack_bytes = opts_.pod_worker_stack;
        o.worker_core_affinity = opts_.core_affinity;
        o.worker_stack_in_psram = opts_.stacks_in_psram;
        o.keepalive_timeout_ms = 15000;
        o.name = "k32_pod";
        pod_server_ = std::make_unique<api::HttpServer>(o, nullptr, nullptr);
        pod_server_->set_handler(&pod_handler_);
        if (!pod_server_->start()) {
            std::cout << "[APP] pod server failed to bind port " << opts_.pod_port << std::endl;
            return false;
        }
        std::cout << "[APP] pod HTTP server on :" << opts_.pod_port << " (" << o.workers << " workers)" << std::endl;
    }

    if (opts_.run_ingress) {
        IngressConfig ic;
        ic.app_name = opts_.app_name;
        ic.core_pod = opts_.core_pod;
        ic.core_namespace = opts_.core_namespace;
        ic.local_node_name = opts_.local_node_name;
        ic.store = opts_.store;
        ic.upstream_override_ip = opts_.upstream_override_ip;
        ic.upstream_port = opts_.pod_port;
        ingress_ = std::make_unique<Ingress>(ic);
        if (!opts_.cluster_events_from_hub) ingress_->sse().set_cluster_provider(nullptr);

        api::HttpServerOptions o;
        o.host = "0.0.0.0";
        o.port = opts_.ingress_port;
        o.backlog = 8;
        o.rx_buffer_size = opts_.ingress_rx_buffer;
        o.max_body_size = 16 * 1024;
        o.workers = opts_.ingress_workers;
        o.worker_stack_bytes = opts_.ingress_worker_stack;
        o.worker_core_affinity = opts_.core_affinity;
        o.worker_stack_in_psram = opts_.stacks_in_psram;
        o.keepalive_timeout_ms = 8000;
        o.accept_queue_depth = 8;
        o.name = "k32_ing";
        ingress_server_ = std::make_unique<api::HttpServer>(o, static_cast<storage::Store*>(nullptr), nullptr);
        ingress_server_->set_handler(ingress_.get());
        ingress_->set_http_server(ingress_server_.get());
        if (!ingress_server_->start()) {
            std::cout << "[APP] ingress failed to bind port " << opts_.ingress_port << std::endl;
            return false;
        }
        ingress_->sse().start();
        std::cout << "[APP] ingress on :" << opts_.ingress_port << " serving app '" << opts_.app_name << "'" << std::endl;

        // Cluster events -> browsers
        std::string events_topic = opts_.app_name + ".events";
        Ingress* ing = ingress_.get();
        sub_events_ = ipc::ChannelManager::instance().subscribe(events_topic, [ing](std::string_view, uint32_t, const uint8_t* data, size_t len) {
            std::string_view payload(reinterpret_cast<const char*>(data), len);
            // Payload is JSON; the event name comes from its "t" field when present, else "issue"
            std::string_view ev = "issue";
            size_t tpos = payload.find("\"type\":\"");
            size_t klen = 8;
            if (tpos == std::string_view::npos) { tpos = payload.find("\"t\":\""); klen = 5; }
            if (tpos != std::string_view::npos) {
                size_t start = tpos + klen;
                size_t end = payload.find('"', start);
                if (end != std::string_view::npos) ev = payload.substr(start, end - start);
            }
            ing->sse().broadcast(ev, payload);
        });
    }

    // Cluster events -> local pods exporting k32_event (both topics)
    std::string events_topic = opts_.app_name + ".events";
    std::string input_topic = opts_.app_name + ".input";
    sub_input_ = ipc::ChannelManager::instance().subscribe(input_topic, [input_topic](std::string_view, uint32_t, const uint8_t* data, size_t len) {
        static int traced = 0;
        if (traced < 8) { traced++; printf("[TRACE] input event len=%u -> %u pod(s): %.*s\n", (unsigned)len, (unsigned)runtime::AppPodHost::instance().list_pods().size(), (int)(len > 80 ? 80 : len), (const char*)data); }
        runtime::AppPodHost::instance().broadcast_event(input_topic, data, len);
    });
    sub_stats_ = ipc::ChannelManager::instance().subscribe(events_topic, [events_topic](std::string_view, uint32_t, const uint8_t* data, size_t len) {
        runtime::AppPodHost::instance().broadcast_event(events_topic, data, len);
    });
    return true;
}

void AppWiring::stop() {
    if (sub_events_) { ipc::ChannelManager::instance().unsubscribe(sub_events_); sub_events_ = 0; }
    if (sub_input_) { ipc::ChannelManager::instance().unsubscribe(sub_input_); sub_input_ = 0; }
    if (sub_stats_) { ipc::ChannelManager::instance().unsubscribe(sub_stats_); sub_stats_ = 0; }
    if (ingress_) ingress_->sse().stop();
    if (ingress_server_) ingress_server_->stop();
    if (pod_server_) pod_server_->stop();
}

} // namespace kube32::app
