#pragma once

// Ingress controller: serves port 80 on the control plane from the
// networking.k8s.io/v1 Ingress objects in the store (see ingress_routes.hpp
// for the rules, backends and annotations it understands).
//
//   * service backends: a pod on this node is called directly (k32_handle,
//     no network hop); a pod on another node is reached through that node's
//     pod HTTP server at the pod's IP and target port, over a small pool of
//     keep-alive connections.
//   * StaticBundle backends: files compiled into the firmware, gzip-encoded
//     ones sent with Content-Encoding: gzip; a path without a file extension
//     that names no file gets the bundle's index.html (single-page apps).
//   * event streams: Server-Sent Events fed by channel topics.
//   * cluster telemetry: JSON summary of nodes, pods, this server and the
//     channel layer.
//
// The routing table is rebuilt when an Ingress, Service or Pod changes in the
// store (at most every 250 ms on the request path, and from tick()).

#include "app/ingress_routes.hpp"
#include "api/http_server.hpp"
#include "storage/store.hpp"
#include "osal/mutex.hpp"
#include "osal/thread.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace kube32::app {

// Server-Sent Events fan-out. Owns the sockets handed over by the controller;
// frames are written on the hub thread.
class SseHub {
public:
    struct Subscription {
        uint32_t stream_id{0};
        std::vector<uint32_t> topics;   // fnv1a_32 of each topic name
        bool presence{false};           // receives "presence" when the stream's viewer count changes
    };

    SseHub();
    ~SseHub();

    bool start();
    void stop();

    // Adds a connection; the HTTP head must already be sent.
    void add(osal::Socket sock, Subscription sub);
    // Queues an event for every connection subscribed to `topic_hash`. An
    // empty `event` sends a nameless event (EventSource "message").
    void broadcast(uint32_t topic_hash, std::string_view event, std::string_view data);
    // Closes connections whose stream is not in `stream_ids`.
    void retain_streams(const std::vector<uint32_t>& stream_ids);

    size_t count() const;
    size_t count_streams(const std::vector<uint32_t>& stream_ids) const;
    bool has_listeners(uint32_t topic_hash) const;
    uint64_t events_sent() const { return events_sent_.load(); }
    void set_max_clients(size_t n) { max_clients_ = n; }

private:
    struct Client {
        osal::Socket sock;
        Subscription sub;
    };
    struct Frame {
        uint32_t topic_hash;
        std::string text;
    };
    void run();
    void send_to(uint32_t topic_hash, const std::string& frame);
    void send_presence();

    mutable osal::Mutex mutex_;
    std::vector<Client> clients_;
    std::vector<Frame> pending_;
    std::map<uint32_t, size_t> presence_sent_;   // stream id -> viewer count last announced (hub thread only)
    std::unique_ptr<osal::Thread> thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> events_sent_{0};
    size_t max_clients_{6};
};

struct IngressControllerOptions {
    storage::Store* store{nullptr};
    std::string local_node_name;
    // Address written to each served Ingress's status.loadBalancer (kubectl's
    // ADDRESS column); empty skips the status update.
    std::function<std::string()> address;

    uint16_t port{80};
    uint32_t workers{3};
    size_t worker_stack{8192};
    size_t rx_buffer{4096};
    int core_affinity{-1};
    bool stacks_in_psram{false};

    uint32_t upstream_timeout_ms{4000};
    size_t upstream_pool_size{4};   // keep-alive connections to pod servers, all targets together
    size_t max_stream_clients{6};
};

class IngressController : public api::HttpHandler {
public:
    explicit IngressController(IngressControllerOptions opts);
    ~IngressController() override;

    // Binds the port and starts the event-stream hub.
    bool start();
    void stop();

    bool handle(api::HttpConnection& conn) override;

    // Once a second from a thread with room for JSON parsing: rebuilds the
    // routing table when the store changed, records the Ingress address,
    // publishes kube32.io/stats-topic lines and feeds kube32.cluster streams.
    void tick();

    // Rebuilds the routing table from the store.
    void refresh_routes();
    std::shared_ptr<const RouteTable> routes();

    api::HttpServer* server() { return server_.get(); }
    SseHub& streams() { return *hub_; }
    std::string build_cluster_json();

    struct Stats {
        std::atomic<uint64_t> proxied{0};
        std::atomic<uint64_t> proxy_errors{0};
        std::atomic<uint64_t> local_calls{0};
        std::atomic<uint64_t> static_hits{0};
        std::atomic<uint64_t> not_found{0};
        std::atomic<uint64_t> upstream_reconnects{0};
    };
    const Stats& stats() const { return stats_; }

private:
    std::shared_ptr<const RouteTable> current_routes();
    void rebuild_locked();
    void sync_subscriptions_locked(const RouteTable& table);
    void sync_status();

    bool serve_service(api::HttpConnection& conn, const IngressRoute& route);
    bool serve_bundle(api::HttpConnection& conn, const IngressRoute& route, std::string_view path);
    bool serve_stream(api::HttpConnection& conn, const IngressRoute& route);
    bool proxy_upstream(api::HttpConnection& conn, const IngressEndpoint& ep);

    struct Upstream {
        osal::Socket sock;
        std::string ip;
        uint16_t port{0};
        bool in_use{false};
    };
    Upstream* acquire_upstream(const std::string& ip, uint16_t port);
    void release_upstream(Upstream* u, bool reusable);

    IngressControllerOptions opts_;
    std::shared_ptr<SseHub> hub_;   // shared with channel callbacks that may outlive a stop()
    std::unique_ptr<api::HttpServer> server_;
    Stats stats_;

    // Routing table
    osal::Mutex table_mutex_;
    std::shared_ptr<const RouteTable> table_;
    osal::Mutex build_mutex_;
    std::shared_ptr<std::atomic<bool>> dirty_;
    std::atomic<uint64_t> built_at_ms_{0};
    std::vector<uint32_t> store_watches_;
    std::map<std::string, uint32_t> topic_subs_;   // channel topic -> subscription id (build_mutex_)

    // Periodic work (tick)
    uint64_t last_telemetry_ms_{0};

    osal::Mutex pool_mutex_;
    std::vector<std::unique_ptr<Upstream>> pool_;
    std::atomic<uint32_t> rr_{0};
};

} // namespace kube32::app
