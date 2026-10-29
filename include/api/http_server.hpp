#pragma once

#include "api/route_handler.hpp"
#include "api/watch_hub.hpp"
#include "storage/store.hpp"
#include "osal/socket.hpp"
#include "osal/tls.hpp"
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
    void* worker_stack_buffer{nullptr};    // the first worker runs on this caller-owned stack (worker_stack_bytes)
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
    // Connections that open with a TLS ClientHello are served over TLS with
    // this server's certificate; plain HTTP on the same port keeps working.
    osal::TlsServer* tls{nullptr};
    // Idle keep-alive connections the acceptor holds, and how many of them
    // may be TLS sessions (each keeps an SSL context and session keys; on the
    // board 3 to 6 KB, in PSRAM). The oldest is closed when a bound is hit.
    uint32_t max_idle_connections{16};
    uint32_t max_idle_tls_sessions{4};
};

class HttpServer;

// One connection as seen by a custom handler. Helpers write complete HTTP/1.1
// responses with correct framing so handlers never touch the wire format.
class HttpConnection {
public:
    HttpConnection(osal::Socket& sock, HttpRequest& req, char* rx_buf, size_t rx_cap);

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
    // Head with Content-Length; the body follows through send_raw (nothing
    // for a HEAD request).
    bool begin_fixed(int status, std::string_view content_type, size_t length, std::string_view extra_headers = {});
    // Chunked transfer: begin, write_chunk*, end_chunked.
    bool begin_chunked(int status, std::string_view content_type, std::string_view extra_headers = {});
    bool write_chunk(const void* data, size_t len);
    bool end_chunked();
    // Raw head + body passthrough used by the proxy. Marks the request answered.
    bool send_raw(const void* data, size_t len);
    void mark_responded(int status = 0) { responded_ = true; if (status) status_ = status; }
    // Handlers that keep the socket (SSE, upgrades) take ownership here.
    osal::Socket take_socket();

    // Body of a request the handler streams itself (HttpHandler::streams_body):
    // up to `cap` bytes per call, 0 once the whole body is read (Content-Length
    // bytes, or the last chunk of a chunked body), -1 on a socket error, a
    // timeout or broken chunk framing. `dst` may be scratch().
    long read_body(void* dst, size_t cap);
    // Bytes left (Content-Length), or 1 while a chunked body has not ended
    size_t body_remaining() const { return body_left_; }
    bool body_chunked() const { return chunked_body_; }
    void set_body_stream(size_t buffered, size_t total, bool chunked) {
        pre_len_ = buffered; pre_off_ = 0; body_left_ = chunked ? 1 : total; chunked_body_ = chunked;
    }

    static const char* status_text(int code);

private:
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
    size_t pre_len_{0};      // streamed body bytes already in rx_buf_ (from offset 0)
    size_t pre_off_{0};
    size_t body_left_{0};
    // Chunked request body: decoder state between read_body calls
    bool chunked_body_{false};
    uint8_t ck_state_{0};    // 0 size line, 1 extension, 2 data, 3 data CRLF, 4 trailer, 5 trailer line
    size_t ck_left_{0};
    bool ck_digits_{false};
    long raw_body(void* dst, size_t cap);
public:
    int status() const { return status_; }   // last status written, 0 for raw passthrough
};

class HttpHandler {
public:
    virtual ~HttpHandler() = default;
    // Return true when the request was answered (or the socket was taken).
    virtual bool handle(HttpConnection& conn) = 0;
    // True when handle() reads this request's body itself (read_body), for
    // uploads larger than the server buffers: the server does not read it and
    // max_body_size does not apply. handle() must then answer the request.
    virtual bool streams_body(const HttpRequest& req) { (void)req; return false; }
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
    // Loopback datagram that wakes the acceptor when a connection is parked,
    // so it watches the connection at once instead of after its select timeout
    osal::Socket wake_sock_;
    uint16_t wake_port_{0};
    std::unique_ptr<osal::Thread> acceptor_thread_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> running_{false};
    HttpServerMetrics metrics_;

    // Accept queue: (connection, client ip, resumed keep-alive connection)
    struct QueuedConn { osal::Socket sock; std::string ip; bool resumed; };
    osal::Mutex queue_mutex_;
    osal::ConditionVariable queue_cv_;
    std::deque<QueuedConn> queue_;

    // Idle keep-alive connections parked with the acceptor (select() watches them)
    struct IdleConn { osal::Socket sock; std::string ip; uint64_t deadline_ms; };
    osal::Mutex idle_mutex_;
    std::vector<IdleConn> idle_;
    void park_idle(osal::Socket sock, const std::string& ip);
    size_t serve_one(Worker& w, osal::Socket& sock, const std::string& client_ip, bool& keep_alive, size_t have);
};

} // namespace kube32::api
