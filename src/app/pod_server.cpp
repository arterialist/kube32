#include "app/pod_server.hpp"
#include "app/node_storage_api.hpp"
#include "ipc/channel.hpp"
#include "osal/thread.hpp"
#include "osal/system.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace kube32::app {

static constexpr size_t kMinFreeHeapToServe = 10 * 1024;

// =============================================================================
// HttpAppSink
// =============================================================================
void HttpAppSink::begin(int status, std::string_view content_type) {
    status_ = status;
    content_type_.assign(content_type.data(), content_type.size());
}

void HttpAppSink::header(std::string_view name, std::string_view value) {
    if (head_sent_) return;
    extra_headers_.append(name.data(), name.size());
    extra_headers_.append(": ");
    extra_headers_.append(value.data(), value.size());
    extra_headers_.append("\r\n");
}

void HttpAppSink::body_length(size_t len) {
    if (head_sent_) return;
    fixed_ = true;
    fixed_len_ = len;
}

bool HttpAppSink::flush_head() {
    if (head_sent_) return true;
    head_sent_ = true;
    const int status = status_ > 0 ? status_ : 200;
    std::string_view ctype = content_type_.empty() ? std::string_view("application/json") : std::string_view(content_type_);
    if (fixed_) return conn_.begin_fixed(status, ctype, fixed_len_, extra_headers_);
    return conn_.begin_chunked(status, ctype, extra_headers_);
}

bool HttpAppSink::flush_buffer() {
    if (buf_len_ == 0) return true;
    uint64_t t0 = osal::Thread::get_time_us();
    bool ok = conn_.write_chunk(buf_, buf_len_);
    socket_us_ += static_cast<uint32_t>(osal::Thread::get_time_us() - t0);
    flushes_++;
    buf_len_ = 0;
    return ok;
}

bool HttpAppSink::write(const uint8_t* data, size_t len) {
    if (ended_) return false;
    if (!flush_head()) return false;
    bytes_ += len;
    if (conn_.request().method == api::HttpMethod::Head) return true;   // head only
    if (fixed_) {
        // Known length: straight to the socket, no chunk framing or copy
        uint64_t t0 = osal::Thread::get_time_us();
        bool ok = bytes_ <= fixed_len_ && conn_.send_raw(data, len);
        socket_us_ += static_cast<uint32_t>(osal::Thread::get_time_us() - t0);
        flushes_++;
        return ok;
    }
    while (len > 0) {
        size_t room = kChunk - buf_len_;
        size_t take = len < room ? len : room;
        std::memcpy(buf_ + buf_len_, data, take);
        buf_len_ += take;
        data += take;
        len -= take;
        if (buf_len_ == kChunk && !flush_buffer()) return false;
    }
    return true;
}

void HttpAppSink::end() {
    if (ended_) return;
    ended_ = true;
    if (!flush_head()) return;
    if (fixed_) {
        // A body shorter than announced leaves the client waiting for the rest
        if (bytes_ != fixed_len_) conn_.set_close();
        return;
    }
    if (conn_.request().method == api::HttpMethod::Head) return;
    flush_buffer();
    conn_.end_chunked();
}

runtime::AppRequest app_request_from(const api::HttpRequest& req) {
    runtime::AppRequest areq;
    areq.method = api::http_method_str(req.method);
    areq.path_and_query = req.raw_uri;
    areq.cookie = req.cookie;
    areq.content_type = req.content_type;
    areq.request_id = req.request_id;
    areq.forwarded_for = req.client_ip;
    areq.body = req.body;
    areq.accepts_gzip = req.accepts_gzip;
    areq.call_chain = req.call_chain;
    return areq;
}

// =============================================================================
// PodServerHandler
// =============================================================================
bool PodServerHandler::streams_body(const api::HttpRequest& req) {
    return req.target_pod.empty() && is_node_storage_path(req.path);
}

bool PodServerHandler::handle(api::HttpConnection& conn) {
    const api::HttpRequest& req = conn.request();
    if (req.path == "/healthz") return conn.send_simple(200, "text/plain", "ok\n");
    // The kubelet endpoint: node storage, reached through the API server's node proxy
    if (req.target_pod.empty() && is_node_storage_path(req.path)) return serve_node_storage(conn, req.path, token_, max_upload_);
    std::string pod = req.target_pod;
    if (pod.empty()) pod = runtime::AppPodHost::instance().default_request_pod();
    if (pod.empty() || !runtime::AppPodHost::instance().pod_handles_requests(pod)) {
        return conn.send_simple(503, "application/json", "{\"error\":\"unavailable\",\"message\":\"no request-handling pod on this node\"}", "Retry-After: 2\r\n");
    }
    // Without PSRAM a response in flight takes several KB of the internal
    // heap in TCP and Wi-Fi buffers, and the Wi-Fi driver stops sending for
    // good once an allocation of its own fails: start none on a low heap
    if (osal::System::get_total_psram_bytes() == 0 && osal::System::get_free_heap_bytes() < kMinFreeHeapToServe) {
        return conn.send_simple(503, "application/json", "{\"error\":\"unavailable\",\"message\":\"node memory is low\"}", "Retry-After: 1\r\n");
    }
    runtime::AppRequest areq = app_request_from(req);
    HttpAppSink sink(conn);
    const uint64_t t0 = osal::Thread::get_time_us();
    int32_t rc = runtime::AppPodHost::instance().handle(pod, areq, sink);
    const uint32_t total_ms = static_cast<uint32_t>((osal::Thread::get_time_us() - t0) / 1000);
    if (total_ms >= 2000) {
        // Slow calls, and how much of the time went to writing the response
        printf("[POD-HTTP] %s %s: %u ms, %u ms writing %u bytes in %u writes\n", pod.c_str(), req.raw_uri.c_str(), (unsigned)total_ms,
               (unsigned)(sink.socket_us() / 1000), (unsigned)sink.bytes(), (unsigned)sink.flushes());
    }
    if (rc < 0 && !conn.responded()) {
        return conn.send_simple(502, "application/json", "{\"error\":\"pod_failure\"}");
    }
    return true;
}

// =============================================================================
// PodServer
// =============================================================================
bool PodServer::start() {
    api::HttpServerOptions o;
    o.host = "0.0.0.0";
    o.port = opts_.port;
    o.backlog = opts_.backlog;
    o.accept_queue_depth = opts_.accept_queue_depth;
    o.max_idle_connections = opts_.max_idle_connections;
    o.rx_buffer_size = opts_.rx_buffer;
    o.max_body_size = 16 * 1024;
    o.workers = opts_.workers;
    o.worker_stack_bytes = opts_.worker_stack;
    o.worker_core_affinity = opts_.core_affinity;
    o.worker_stack_in_psram = opts_.stacks_in_psram;
    o.worker_stack_buffer = opts_.stack_buffer;
    o.keepalive_timeout_ms = 15000;
    o.name = "k32_pod";
    server_ = std::make_unique<api::HttpServer>(o, nullptr, nullptr);
    server_->set_handler(&handler_);
    if (!server_->start()) {
        std::cout << "[POD-HTTP] failed to bind port " << opts_.port << std::endl;
        server_.reset();
        return false;
    }
    std::cout << "[POD-HTTP] pod HTTP server on :" << opts_.port << " (" << o.workers << " workers)" << std::endl;
    return true;
}

void PodServer::stop() {
    if (server_) server_->stop();
}

// =============================================================================
// Platform hooks
// =============================================================================
void install_default_platform_hooks() {
    runtime::AppPlatformHooks hooks = runtime::app_platform_hooks();
    if (!hooks.event_emit) {
        hooks.event_emit = [](const std::string& pod_id, std::string_view topic, const uint8_t* data, size_t len) -> int32_t {
            return ipc::ChannelManager::instance().publish(pod_id, std::string(topic), data, len);
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

} // namespace kube32::app
