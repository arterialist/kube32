#pragma once

// Pod HTTP server (port 7000 on every node) and the pieces it shares with
// the ingress controller.
//
// The server turns each HTTP request into one k32_handle call on a local
// request-handling pod: the pod named by the X-Kube32-Pod header (set by the
// ingress controller and by service calls from other nodes), else the first
// registered one. HttpAppSink streams what the guest writes back as a chunked
// response, or with Content-Length when the length is known up front.
//
// It is also the node's kubelet endpoint (Node status.daemonEndpoints): a
// request without X-Kube32-Pod for /storage/... reaches the node's storage
// (app/node_storage_api.hpp), which the API server's node proxy forwards to.

#include "api/http_server.hpp"
#include "runtime/app_pod.hpp"

#include <cstdint>
#include <memory>

namespace kube32::app {

// Adapts the Wasm response ABI to an HttpConnection (chunked transfer).
class HttpAppSink : public runtime::AppResponseSink {
public:
    explicit HttpAppSink(api::HttpConnection& conn) : conn_(conn) {}
    void begin(int status, std::string_view content_type) override;
    void header(std::string_view name, std::string_view value) override;
    bool write(const uint8_t* data, size_t len) override;
    void end() override;
    void body_length(size_t len) override;
    int status() const { return status_; }
    size_t bytes() const { return bytes_; }
    uint32_t socket_us() const { return socket_us_; }   // time spent inside socket writes
    uint32_t flushes() const { return flushes_; }
private:
    bool flush_head();
    bool flush_buffer();
    api::HttpConnection& conn_;
    int status_{0};
    std::string content_type_;
    std::string extra_headers_;
    bool head_sent_{false};
    bool ended_{false};
    bool fixed_{false};          // Content-Length framing (body_length called before the head went out)
    size_t fixed_len_{0};
    size_t bytes_{0};
    uint32_t socket_us_{0};
    uint32_t flushes_{0};
    // Four full segments per write: a sub-MSS write per chunk left lwIP's
    // Nagle logic waiting one delayed-ACK round trip per 1400 bytes.
    static constexpr size_t kChunk = 5760;
    size_t buf_len_{0};
    uint8_t buf_[kChunk];
};

// Builds the AppRequest for a k32_handle call from an HTTP request.
runtime::AppRequest app_request_from(const api::HttpRequest& req);

class PodServerHandler : public api::HttpHandler {
public:
    bool handle(api::HttpConnection& conn) override;
    bool streams_body(const api::HttpRequest& req) override;
    // Node storage over HTTP: the bearer token callers must present (empty:
    // none) and the largest upload accepted
    void set_storage(std::string token, size_t max_upload_bytes) { token_ = std::move(token); max_upload_ = max_upload_bytes; }
private:
    std::string token_;
    size_t max_upload_{0};
};

struct PodServerOptions {
    uint16_t port{7000};
    uint32_t workers{2};
    size_t worker_stack{16384};
    size_t rx_buffer{4096};
    // Connections accepted and not yet served, and idle keep-alive ones.
    // Each socket holding unread bytes pins Wi-Fi RX buffers; a board
    // without PSRAM has few.
    uint32_t accept_queue_depth{8};
    uint32_t max_idle_connections{16};
    int backlog{4};
    int core_affinity{-1};
    bool stacks_in_psram{false};
    void* stack_buffer{nullptr};             // the (first) worker's stack, owned by the caller (worker_stack bytes)
    std::string token;                       // node storage: bearer token required, empty for none
    size_t max_upload_bytes{8u * 1024 * 1024};
};

class PodServer {
public:
    explicit PodServer(PodServerOptions opts) : opts_(std::move(opts)) { handler_.set_storage(opts_.token, opts_.max_upload_bytes); }
    ~PodServer() { stop(); }
    bool start();
    void stop();
    api::HttpServer* server() { return server_.get(); }
private:
    PodServerOptions opts_;
    PodServerHandler handler_;
    std::unique_ptr<api::HttpServer> server_;
};

// Fills the platform hooks the board did not install: event_emit through the
// channel layer, random from std::rand, and printing stand-ins for the
// display, keypad and speaker.
void install_default_platform_hooks();

} // namespace kube32::app
