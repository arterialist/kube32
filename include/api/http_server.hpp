#pragma once

#include "api/route_handler.hpp"
#include "api/watch_hub.hpp"
#include "storage/store.hpp"
#include "osal/socket.hpp"
#include "osal/thread.hpp"
#include "osal/mutex.hpp"
#include "osal/condition_variable.hpp"
#include "scheduler/scheduler.hpp"

#include <string>
#include <string_view>
#include <atomic>
#include <memory>
#include <vector>
#include <deque>

namespace kube32::api {

constexpr size_t MAX_REQUEST_BODY_SIZE = 2 * 1024 * 1024; // 2MB

struct HttpServerOptions {
    std::string host{"0.0.0.0"};
    uint16_t port{6443};
    int backlog{10};
    size_t rx_buffer_size{8192};
    size_t max_body_size{MAX_REQUEST_BODY_SIZE};
    // Worker pool. Each worker owns one rx buffer and serves one connection at
    // a time; the acceptor hands sockets over through a bounded queue.
    uint32_t workers{2};
    size_t worker_stack_bytes{12288};
    int worker_core_affinity{-1};
    bool worker_stack_in_psram{false};
    uint32_t keepalive_timeout_ms{5000};   // idle time allowed between requests on one connection
    uint32_t request_timeout_ms{5000};     // time allowed to finish reading headers+body
    uint32_t max_requests_per_connection{200};
    uint32_t accept_queue_depth{8};
    const char* name{"k32_http"};
    bool access_log{false};                // one console line per request (method, path, status, ms, client)
    // Long-lived streams (watch, log follow) served by one hub thread owned by
    // the server. 0 disables streaming (endpoints answer 501).
    uint32_t watch_streams{8};
    size_t watch_stack_bytes{16384};
    bool watch_stack_in_psram{false};
};

class HttpServer;

// One connection as seen by a custom handler. Helpers write complete HTTP/1.1
// responses with correct framing so handlers never touch the wire format.
class HttpConnection {
public:
    HttpConnection(HttpServer& server, osal::Socket& sock, HttpRequest& req, char* rx_buf, size_t rx_cap);

    const HttpRequest& request() const { return req_; }
    osal::Socket& socket() { return *sock_; }
    char* scratch() { return rx_buf_; }
    size_t scratch_capacity() const { return rx_cap_; }

    bool keep_alive() const { return keep_alive_; }
    void set_close() { keep_alive_ = false; }
    bool responded() const { return responded_; }
    bool detached() const { return detached_; }

    // Full response with Content-Length.
    bool send_simple(int status, std::string_view content_type, std::string_view body, std::string_view extra_headers = {});
    // Static blob, optionally pre-gzipped. Sends 304 when If-None-Match matches etag.
    bool send_blob(std::string_view content_type, const uint8_t* data, size_t len, bool gzip, std::string_view etag, std::string_view cache_control = "public, max-age=3600");
    // Chunked transfer: begin, write_chunk*, end_chunked.
    bool begin_chunked(int status, std::string_view content_type, std::string_view extra_headers = {});
    bool write_chunk(const void* data, size_t len);
    bool end_chunked();
    // Raw head + body passthrough used by the proxy. Marks the request answered.
    bool send_raw(const void* data, size_t len);
    void mark_responded(int status = 0) { responded_ = true; if (status) status_ = status; }
    // Handlers that keep the socket (SSE, upgrades) take ownership here.
    osal::Socket take_socket();

    static const char* status_text(int code);

private:
    HttpServer& server_;
    osal::Socket* sock_;
    osal::Socket owned_;
    HttpRequest& req_;
    char* rx_buf_;
    size_t rx_cap_;
    bool keep_alive_{true};
    bool responded_{false};
    bool detached_{false};
    bool chunked_open_{false};
    int status_{0};
public:
    int status() const { return status_; }   // last status written, 0 for raw passthrough
};

class HttpHandler {
public:
    virtual ~HttpHandler() = default;
    // Return true when the request was answered (or the socket was taken).
    virtual bool handle(HttpConnection& conn) = 0;
};

struct HttpServerMetrics {
    std::atomic<uint64_t> requests{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> connections{0};
    std::atomic<uint32_t> active{0};
    std::atomic<uint64_t> total_us{0};
    // Latency reservoir for percentiles (microseconds), ring of 128 samples
    static constexpr size_t RESERVOIR = 128;
    std::atomic<uint32_t> samples[RESERVOIR];
    std::atomic<uint32_t> sample_idx{0};

    void record(uint32_t us) {
        requests.fetch_add(1, std::memory_order_relaxed);
        total_us.fetch_add(us, std::memory_order_relaxed);
        uint32_t i = sample_idx.fetch_add(1, std::memory_order_relaxed) % RESERVOIR;
        samples[i].store(us, std::memory_order_relaxed);
    }
    // Fills p50/p99 from the reservoir
    void percentiles(uint32_t& p50, uint32_t& p99) const;
};

class HttpServer {
public:
    HttpServer(HttpServerOptions options, storage::Store& store, scheduler::Scheduler* scheduler = nullptr);
    HttpServer(HttpServerOptions options, storage::Store* store = nullptr, scheduler::Scheduler* scheduler = nullptr);
    ~HttpServer();

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    // Custom handler consulted before the Kubernetes router.
    void set_handler(HttpHandler* handler) { handler_ = handler; }
    void set_access_log(bool on) { options_.access_log = on; }

    RouteHandler& route_handler() { return *route_handler_; }
    bool has_route_handler() const { return static_cast<bool>(route_handler_); }
    WatchHub* watch_hub() { return watch_hub_.get(); }
    const HttpServerOptions& options() const { return options_; }
    HttpServerMetrics& metrics() { return metrics_; }
    size_t queued() const { osal::LockGuard guard(const_cast<osal::Mutex&>(queue_mutex_)); return queue_.size(); }

    // Parses a request head (status line + headers). Public for the serial console.
    static bool parse_request_headers(const char* buf, size_t len, HttpRequest& out_req, size_t& out_header_bytes);

private:
    struct Worker;
    void run_acceptor();
    void run_worker(Worker& w);
    void serve_connection(Worker& w, osal::Socket sock, const std::string& client_ip);
    bool send_status_only(osal::Socket& sock, int status, const char* reason);

    HttpServerOptions options_;
    storage::Store* store_{nullptr};
    std::unique_ptr<RouteHandler> route_handler_;
    std::unique_ptr<WatchHub> watch_hub_;
    HttpHandler* handler_{nullptr};
    osal::Socket listen_sock_;
    std::unique_ptr<osal::Thread> acceptor_thread_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> running_{false};
    HttpServerMetrics metrics_;

    // Accept queue: (fd, client ip, resumed keep-alive connection)
    struct QueuedConn { int fd; std::string ip; bool resumed; };
    osal::Mutex queue_mutex_;
    osal::ConditionVariable queue_cv_;
    std::deque<QueuedConn> queue_;

    // Idle keep-alive connections parked with the acceptor (select() watches them)
    struct IdleConn { int fd; std::string ip; uint64_t deadline_ms; };
    osal::Mutex idle_mutex_;
    std::vector<IdleConn> idle_;
    void park_idle(int fd, const std::string& ip);
    size_t serve_one(Worker& w, osal::Socket& sock, const std::string& client_ip, bool& keep_alive, size_t have);
};

} // namespace kube32::api
