#pragma once

// Application ingress (node 1, port 80) and pod HTTP server (any node, port 7000).
//
// Ingress responsibilities:
//   * serve the embedded, pre-gzipped web assets for one app from flash
//   * proxy /api/* to the request-handling pod, locally (direct Wasm call)
//     or over the cluster network to the node that runs it
//   * keep Server-Sent Events connections and fan out cluster events
//   * answer /api/cluster with live node, ingress and link telemetry
//
// PodServerHandler is the other side of the proxy: it turns HTTP requests
// into k32_handle calls on the local request-handling pod.

#include "api/http_server.hpp"
#include "runtime/app_pod.hpp"
#include "storage/store.hpp"
#include "osal/mutex.hpp"
#include "osal/thread.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace kube32::app {

// Adapts the Wasm response ABI to an HttpConnection (chunked transfer).
class HttpAppSink : public runtime::AppResponseSink {
public:
    explicit HttpAppSink(api::HttpConnection& conn) : conn_(conn) {}
    void begin(int status, std::string_view content_type) override;
    void header(std::string_view name, std::string_view value) override;
    bool write(const uint8_t* data, size_t len) override;
    void end() override;
    int status() const { return status_; }
    size_t bytes() const { return bytes_; }
private:
    bool flush_head();
    bool flush_buffer();
    api::HttpConnection& conn_;
    int status_{0};
    std::string content_type_;
    std::string extra_headers_;
    bool head_sent_{false};
    bool ended_{false};
    size_t bytes_{0};
    // Four full segments per write: a sub-MSS write per chunk left lwIP's
    // Nagle logic waiting one delayed-ACK round trip per 1400 bytes.
    static constexpr size_t kChunk = 5760;
    size_t buf_len_{0};
    uint8_t buf_[kChunk];
};

// Server-Sent Events fan-out. Owns the sockets handed over by the ingress.
class SseHub {
public:
    SseHub();
    ~SseHub();

    bool start();
    void stop();

    // Adds a connection; the HTTP head must already be sent.
    void add(osal::Socket sock);
    // Queues an event for every connection (broadcast happens on the hub thread).
    void broadcast(std::string_view event, std::string_view data);
    size_t count() const;
    uint64_t events_sent() const { return events_sent_.load(); }

    // Called every 2 s on the hub thread; the result is sent as event "cluster".
    void set_cluster_provider(std::function<std::string()> provider) { cluster_provider_ = std::move(provider); }
    void set_max_clients(size_t n) { max_clients_ = n; }

private:
    void run();
    bool send_to_all(const char* data, size_t len);

    mutable osal::Mutex mutex_;
    std::vector<osal::Socket> clients_;
    std::vector<std::string> pending_;
    std::unique_ptr<osal::Thread> thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> events_sent_{0};
    std::function<std::string()> cluster_provider_;
    size_t max_clients_{6};
};

struct IngressConfig {
    std::string app_name{"tessera"};
    std::string core_pod{"tessera-core"};
    std::string core_namespace{"default"};
    uint16_t upstream_port{7000};
    std::string local_node_name;
    storage::Store* store{nullptr};              // pod -> node -> address discovery
    std::string upstream_override_ip;            // host simulation: always this address
    uint32_t upstream_timeout_ms{4000};
    size_t upstream_pool_size{2};
};

class Ingress : public api::HttpHandler {
public:
    explicit Ingress(IngressConfig cfg);
    ~Ingress() override;

    bool handle(api::HttpConnection& conn) override;

    SseHub& sse() { return sse_; }
    void set_http_server(api::HttpServer* server) { server_ = server; }
    // Telemetry sources for /api/cluster
    void set_node_telemetry_provider(std::function<std::string(const std::string& node_name)> p) { node_telemetry_ = std::move(p); }

    std::string build_cluster_json();

    struct Stats {
        std::atomic<uint64_t> proxied{0};
        std::atomic<uint64_t> proxy_errors{0};
        std::atomic<uint64_t> local_calls{0};
        std::atomic<uint64_t> static_hits{0};
        std::atomic<uint64_t> upstream_reconnects{0};
    };
    const Stats& stats() const { return stats_; }

private:
    bool serve_static(api::HttpConnection& conn);
    bool serve_events(api::HttpConnection& conn);
    bool serve_api(api::HttpConnection& conn);
    bool proxy_upstream(api::HttpConnection& conn, const std::string& ip, uint16_t port);
    bool resolve_upstream(std::string& out_ip, uint16_t& out_port, bool& out_local);

    struct Upstream {
        osal::Socket sock;
        bool in_use{false};
        uint64_t last_used_ms{0};
    };
    Upstream* acquire_upstream(const std::string& ip, uint16_t port);
    void release_upstream(Upstream* u, bool reusable);

    IngressConfig cfg_;
    SseHub sse_;
    api::HttpServer* server_{nullptr};
    Stats stats_;
    std::function<std::string(const std::string&)> node_telemetry_;

    osal::Mutex pool_mutex_;
    std::vector<std::unique_ptr<Upstream>> pool_;
    std::string pool_ip_;
    uint16_t pool_port_{0};

    // Resolution cache
    osal::Mutex resolve_mutex_;
    std::string cached_ip_;
    uint16_t cached_port_{0};
    bool cached_local_{false};
    uint64_t cached_at_ms_{0};
};

// HTTP front for request-handling pods on the node that runs them.
class PodServerHandler : public api::HttpHandler {
public:
    bool handle(api::HttpConnection& conn) override;
};

} // namespace kube32::app
