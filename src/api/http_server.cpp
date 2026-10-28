#include "api/http_server.hpp"
#include "mem/monotonic_arena.hpp"
#include "osal/system.hpp"

#include <cstring>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <cctype>
#include <vector>
#include <algorithm>
#include <iostream>
#include <sys/select.h>
#include <sys/time.h>
#include <exception>

namespace kube32::api {

// =============================================================================
// Metrics
// =============================================================================
void HttpServerMetrics::percentiles(uint32_t& p50, uint32_t& p99) const {
    uint32_t tmp[RESERVOIR];
    size_t n = 0;
    uint64_t total = requests.load(std::memory_order_relaxed);
    size_t count = total < RESERVOIR ? static_cast<size_t>(total) : RESERVOIR;
    for (size_t i = 0; i < count; ++i) tmp[n++] = samples[i].load(std::memory_order_relaxed);
    if (n == 0) { p50 = 0; p99 = 0; return; }
    std::sort(tmp, tmp + n);
    p50 = tmp[(n * 50) / 100];
    size_t i99 = (n * 99) / 100;
    if (i99 >= n) i99 = n - 1;
    p99 = tmp[i99];
}

// =============================================================================
// HttpConnection
// =============================================================================
HttpConnection::HttpConnection(HttpServer& server, osal::Socket& sock, HttpRequest& req, char* rx_buf, size_t rx_cap)
    : server_(server), sock_(&sock), req_(req), rx_buf_(rx_buf), rx_cap_(rx_cap) {
    keep_alive_ = req.keep_alive;
}

const char* HttpConnection::status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Status";
    }
}

static size_t write_head(char* buf, size_t cap, int status, std::string_view content_type, bool keep_alive,
                         const char* framing, std::string_view extra) {
    int n = snprintf(buf, cap,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %.*s\r\n"
                     "%s"
                     "Connection: %s\r\n"
                     "%.*s"
                     "\r\n",
                     status, HttpConnection::status_text(status),
                     static_cast<int>(content_type.size()), content_type.data(),
                     framing,
                     keep_alive ? "keep-alive" : "close",
                     static_cast<int>(extra.size()), extra.data());
    if (n < 0) return 0;
    return static_cast<size_t>(n) >= cap ? cap - 1 : static_cast<size_t>(n);
}

bool HttpConnection::send_simple(int status, std::string_view content_type, std::string_view body, std::string_view extra_headers) {
    status_ = status;
    if (responded_) return false;
    responded_ = true;
    char framing[64];
    size_t body_len = (req_.method == HttpMethod::Head) ? 0 : body.size();
    snprintf(framing, sizeof(framing), "Content-Length: %zu\r\n", body.size());
    char head[640];
    size_t hl = write_head(head, sizeof(head), status, content_type, keep_alive_, framing, extra_headers);
    if (hl == 0 || !sock_->send_all(head, hl)) return false;
    if (body_len > 0 && !sock_->send_all(body.data(), body_len)) return false;
    return true;
}

bool HttpConnection::send_blob(std::string_view content_type, const uint8_t* data, size_t len, bool gzip, std::string_view etag, std::string_view cache_control) {
    status_ = 200;   // or 304 below
    if (responded_) return false;
    responded_ = true;
    char extra[256];
    if (!etag.empty() && !req_.if_none_match.empty() && req_.if_none_match.find(etag) != std::string::npos) {
        int n = snprintf(extra, sizeof(extra), "ETag: \"%.*s\"\r\nCache-Control: %.*s\r\n",
                         static_cast<int>(etag.size()), etag.data(), static_cast<int>(cache_control.size()), cache_control.data());
        char head[512];
        size_t hl = write_head(head, sizeof(head), 304, content_type, keep_alive_, "Content-Length: 0\r\n", std::string_view(extra, n > 0 ? n : 0));
        return hl > 0 && sock_->send_all(head, hl);
    }
    int n = snprintf(extra, sizeof(extra), "%s%s%.*s%sCache-Control: %.*s\r\n",
                     gzip ? "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n" : "",
                     etag.empty() ? "" : "ETag: \"",
                     static_cast<int>(etag.size()), etag.data(),
                     etag.empty() ? "" : "\"\r\n",
                     static_cast<int>(cache_control.size()), cache_control.data());
    char framing[64];
    snprintf(framing, sizeof(framing), "Content-Length: %zu\r\n", len);
    char head[640];
    size_t hl = write_head(head, sizeof(head), 200, content_type, keep_alive_, framing, std::string_view(extra, n > 0 ? n : 0));
    if (hl == 0 || !sock_->send_all(head, hl)) return false;
    if (req_.method == HttpMethod::Head || len == 0) return true;
    // Stream in bounded slices so a slow client cannot pin a large flash blob in a single send
    const size_t slice = 2048;
    size_t off = 0;
    while (off < len) {
        size_t n2 = std::min(slice, len - off);
        if (!sock_->send_all(data + off, n2)) return false;
        off += n2;
    }
    return true;
}

bool HttpConnection::begin_chunked(int status, std::string_view content_type, std::string_view extra_headers) {
    status_ = status;
    if (responded_) return false;
    responded_ = true;
    chunked_open_ = true;
    char head[768];
    size_t hl = write_head(head, sizeof(head), status, content_type, keep_alive_, "Transfer-Encoding: chunked\r\n", extra_headers);
    return hl > 0 && sock_->send_all(head, hl);
}

bool HttpConnection::write_chunk(const void* data, size_t len) {
    if (!chunked_open_ || len == 0) return chunked_open_;
    char size_line[16];
    int n = snprintf(size_line, sizeof(size_line), "%zx\r\n", len);
    if (!sock_->send_all(size_line, static_cast<size_t>(n))) return false;
    if (!sock_->send_all(data, len)) return false;
    return sock_->send_all("\r\n", 2);
}

bool HttpConnection::end_chunked() {
    if (!chunked_open_) return false;
    chunked_open_ = false;
    return sock_->send_all("0\r\n\r\n", 5);
}

bool HttpConnection::send_raw(const void* data, size_t len) {
    responded_ = true;
    return sock_->send_all(data, len);
}

osal::Socket HttpConnection::take_socket() {
    detached_ = true;
    responded_ = true;
    keep_alive_ = false;
    return std::move(*sock_);
}

// =============================================================================
// HttpServer
// =============================================================================
struct HttpServer::Worker {
    HttpServer* server{nullptr};
    uint32_t index{0};
    std::unique_ptr<osal::Thread> thread;
    std::vector<char> rx_buf;
};

HttpServer::HttpServer(HttpServerOptions options, storage::Store& store, scheduler::Scheduler* scheduler)
    : options_(std::move(options)), store_(&store), route_handler_(std::make_unique<RouteHandler>(store, scheduler)) {}

HttpServer::HttpServer(HttpServerOptions options, storage::Store* store, scheduler::Scheduler* scheduler)
    : options_(std::move(options)), store_(store) {
    if (store) route_handler_ = std::make_unique<RouteHandler>(*store, scheduler);
}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
    if (running_.load()) return true;

    if (!osal::Socket::init_networking()) return false;
    if (!listen_sock_.create(osal::SocketType::Tcp)) return false;

    listen_sock_.set_reuse_addr(true);
    listen_sock_.set_tcp_nodelay(true);

    if (!listen_sock_.bind(options_.host.c_str(), options_.port)) {
        listen_sock_.close();
        return false;
    }
    if (!listen_sock_.listen(options_.backlog)) {
        listen_sock_.close();
        return false;
    }

    running_.store(true);

    // Streaming hub for the Kubernetes router (watch, log follow)
    if (route_handler_ && store_ && options_.watch_streams > 0) {
        WatchHubOptions wo;
        wo.max_streams = options_.watch_streams;
        wo.stack_bytes = options_.watch_stack_bytes;
        wo.stack_in_psram = options_.watch_stack_in_psram;
        wo.core_affinity = options_.worker_core_affinity;
        static char hub_name[16];
        snprintf(hub_name, sizeof(hub_name), "%.10s_wh", options_.name);
        wo.thread_name = hub_name;
        watch_hub_ = std::make_unique<WatchHub>(*store_, wo);
        if (watch_hub_->start()) {
            route_handler_->set_watch_hub(watch_hub_.get());
        } else {
            watch_hub_.reset();
        }
    }

    uint32_t nworkers = options_.workers == 0 ? 1 : options_.workers;
    for (uint32_t i = 0; i < nworkers; ++i) {
        auto w = std::make_unique<Worker>();
        w->server = this;
        w->index = i;
        w->rx_buf.resize(options_.rx_buffer_size);
        w->thread = std::make_unique<osal::Thread>();
        osal::Thread::Config cfg;
        static char names[8][16];
        snprintf(names[i % 8], sizeof(names[0]), "%.9s_w%u", options_.name, i);
        cfg.name = names[i % 8];
        cfg.stack_size = options_.worker_stack_bytes;
        cfg.priority = 5;
        cfg.core_affinity = options_.worker_core_affinity;
        cfg.stack_in_psram = options_.worker_stack_in_psram;
        Worker* raw = w.get();
        if (!w->thread->start([](void* p) {
                auto* self = static_cast<Worker*>(p);
                self->server->run_worker(*self);
            }, raw, cfg)) {
            running_.store(false);
            listen_sock_.close();
            return false;
        }
        workers_.push_back(std::move(w));
    }

    acceptor_thread_ = std::make_unique<osal::Thread>();
    osal::Thread::Config cfg;
    cfg.name = options_.name;
    cfg.stack_size = 3072;
    cfg.priority = 6;
    cfg.core_affinity = options_.worker_core_affinity;
    if (!acceptor_thread_->start([](void* p) {
            static_cast<HttpServer*>(p)->run_acceptor();
        }, this, cfg)) {
        running_.store(false);
        listen_sock_.close();
        return false;
    }
    return true;
}

void HttpServer::stop() {
    if (!running_.load()) return;
    running_.store(false);

    listen_sock_.shutdown(osal::ShutdownMode::Both);
    listen_sock_.close();

    {
        osal::LockGuard guard(queue_mutex_);
        queue_cv_.notify_all();
    }
    if (acceptor_thread_) {
        acceptor_thread_->join();
        acceptor_thread_.reset();
    }
    for (auto& w : workers_) {
        {
            osal::LockGuard guard(queue_mutex_);
            queue_cv_.notify_all();
        }
        if (w->thread) w->thread->join();
    }
    workers_.clear();
    if (watch_hub_) {
        if (route_handler_) route_handler_->set_watch_hub(nullptr);
        watch_hub_->stop();
        watch_hub_.reset();
    }
    // Drain any sockets never served
    for (auto& item : queue_) {
        osal::Socket s(item.fd);
        s.close();
    }
    queue_.clear();
    for (auto& ic : idle_) {
        osal::Socket s(ic.fd);
        s.close();
    }
    idle_.clear();
}

void HttpServer::park_idle(int fd, const std::string& ip) {
    osal::LockGuard guard(idle_mutex_);
    if (idle_.size() >= 16) {
        // Too many idle connections: drop the oldest
        osal::Socket victim(idle_.front().fd);
        victim.close();
        idle_.erase(idle_.begin());
    }
    idle_.push_back({fd, ip, osal::System::get_uptime_ms() + options_.keepalive_timeout_ms});
}

void HttpServer::run_acceptor() {
    int listen_fd = listen_sock_.native_handle();
    while (running_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;
        {
            osal::LockGuard guard(idle_mutex_);
            for (const auto& ic : idle_) {
                FD_SET(ic.fd, &rfds);
                if (ic.fd > maxfd) maxfd = ic.fd;
            }
        }
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int ready = ::select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (!running_.load()) break;
        if (ready < 0) {
            osal::Thread::sleep_ms(5);
            continue;
        }

        // Idle connections that became readable (next request) or expired
        uint64_t now = osal::System::get_uptime_ms();
        std::vector<QueuedConn> resumed;
        {
            osal::LockGuard guard(idle_mutex_);
            for (size_t i = 0; i < idle_.size();) {
                if (FD_ISSET(idle_[i].fd, &rfds)) {
                    resumed.push_back({idle_[i].fd, idle_[i].ip, true});
                    idle_.erase(idle_.begin() + static_cast<long>(i));
                } else if (now >= idle_[i].deadline_ms) {
                    osal::Socket s(idle_[i].fd);
                    s.close();
                    idle_.erase(idle_.begin() + static_cast<long>(i));
                } else {
                    ++i;
                }
            }
        }
        if (!resumed.empty()) {
            osal::LockGuard guard(queue_mutex_);
            for (auto& r : resumed) queue_.push_back(std::move(r));
            queue_cv_.notify_all();
        }

        if (FD_ISSET(listen_fd, &rfds)) {
            std::string client_ip;
            osal::Socket client = listen_sock_.accept(&client_ip);
            if (!client.is_valid()) {
                // Usually EMFILE/ENFILE: lwIP's socket table is full and the
                // peer's SYN is being dropped. Say so, a few times per boot.
                static int logged = 0;
                if (logged < 8) { logged++; printf("[HTTP] %s accept failed: errno %d (%s)\n", options_.name, errno, strerror(errno)); }
                continue;
            }
            metrics_.connections.fetch_add(1, std::memory_order_relaxed);
            client.set_tcp_nodelay(true);   // responses are written in large coalesced chunks
            int fd = client.native_handle();
            osal::LockGuard guard(queue_mutex_);
            if (queue_.size() >= options_.accept_queue_depth) {
                const char* busy = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nRetry-After: 1\r\nConnection: close\r\n\r\n";
                client.send_all(busy, std::strlen(busy));
                client.close();
                metrics_.errors.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            client.release();
            queue_.push_back({fd, client_ip, false});
            queue_cv_.notify_one();
        }
    }
}

void HttpServer::run_worker(Worker& w) {
    while (running_.load()) {
        QueuedConn qc{-1, "", false};
        {
            osal::UniqueLock<osal::Mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, 500, [this]() { return !queue_.empty() || !running_.load(); });
            if (!running_.load()) break;
            if (queue_.empty()) continue;
            qc = std::move(queue_.front());
            queue_.pop_front();
        }
        osal::Socket sock(qc.fd);
        metrics_.active.fetch_add(1, std::memory_order_relaxed);
        serve_connection(w, std::move(sock), qc.ip);
        metrics_.active.fetch_sub(1, std::memory_order_relaxed);
        osal::Thread::yield();
    }
}

bool HttpServer::send_status_only(osal::Socket& sock, int status, const char* reason) {
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status, reason);
    return n > 0 && sock.send_all(buf, static_cast<size_t>(n));
}

static bool header_is(std::string_view key, const char* name) {
    size_t n = std::strlen(name);
    return key.size() == n && strncasecmp(key.data(), name, n) == 0;
}

bool HttpServer::parse_request_headers(const char* buf, size_t len, HttpRequest& out_req, size_t& out_header_bytes) {
    std::string_view all(buf, len);
    size_t end = all.find("\r\n\r\n");
    size_t sep = 4;
    if (end == std::string_view::npos) {
        end = all.find("\n\n");
        sep = 2;
        if (end == std::string_view::npos) return false;
    }
    out_header_bytes = end + sep;
    std::string_view header_view = all.substr(0, out_header_bytes);

    size_t first_line_end = header_view.find('\n');
    if (first_line_end == std::string_view::npos) return false;
    std::string_view req_line = header_view.substr(0, first_line_end);
    if (!req_line.empty() && req_line.back() == '\r') req_line.remove_suffix(1);

    size_t space1 = req_line.find(' ');
    if (space1 == std::string_view::npos) return false;
    size_t space2 = req_line.find(' ', space1 + 1);
    std::string_view method_sv = req_line.substr(0, space1);
    std::string_view uri_sv = (space2 == std::string_view::npos) ? req_line.substr(space1 + 1) : req_line.substr(space1 + 1, space2 - space1 - 1);
    std::string_view version_sv = (space2 == std::string_view::npos) ? std::string_view("HTTP/1.0") : req_line.substr(space2 + 1);

    out_req.method = parse_http_method(method_sv);
    out_req.raw_uri = std::string(uri_sv);
    normalize_uri(out_req.raw_uri, out_req.path, out_req.query_string);
    out_req.http11 = (version_sv == "HTTP/1.1");
    out_req.keep_alive = out_req.http11;

    size_t cur = first_line_end + 1;
    while (cur < out_header_bytes) {
        size_t next_line = header_view.find('\n', cur);
        if (next_line == std::string_view::npos) break;
        std::string_view line = header_view.substr(cur, next_line - cur);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        cur = next_line + 1;
        if (line.empty()) break;

        size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        std::string_view key = line.substr(0, colon);
        std::string_view val = line.substr(colon + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.remove_prefix(1);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.remove_suffix(1);

        if (header_is(key, "Accept")) {
            out_req.accept = std::string(val);
            if (out_req.accept.find("as=Table") != std::string::npos) out_req.as_table = true;
        } else if (header_is(key, "Content-Type")) {
            out_req.content_type = std::string(val);
        } else if (header_is(key, "Content-Length")) {
            bool ok = !val.empty();
            for (char c : val) if (!std::isdigit(static_cast<unsigned char>(c))) { ok = false; break; }
            if (!ok) {
                out_req.invalid_content_length = true;
                out_req.content_length = 0;
            } else {
                try { out_req.content_length = std::stoul(std::string(val)); }
                catch (...) { out_req.invalid_content_length = true; out_req.content_length = 0; }
            }
        } else if (header_is(key, "Connection")) {
            if (val.size() >= 5 && strncasecmp(val.data(), "close", 5) == 0) out_req.keep_alive = false;
            else if (val.size() >= 10 && strncasecmp(val.data(), "keep-alive", 10) == 0) out_req.keep_alive = true;
        } else if (header_is(key, "Cookie")) {
            out_req.cookie = std::string(val);
        } else if (header_is(key, "Accept-Encoding")) {
            out_req.accepts_gzip = (val.find("gzip") != std::string_view::npos);
        } else if (header_is(key, "If-None-Match")) {
            out_req.if_none_match = std::string(val);
        } else if (header_is(key, "Host")) {
            out_req.host = std::string(val);
        } else if (header_is(key, "X-Request-Id")) {
            out_req.request_id = std::string(val);
        } else if (header_is(key, "X-Kube32-Pod")) {
            out_req.target_pod = std::string(val);
        } else if (header_is(key, "Transfer-Encoding")) {
            if (val.find("chunked") != std::string_view::npos) out_req.chunked_body = true;
        }
    }
    return true;
}

void HttpServer::serve_connection(Worker& w, osal::Socket sock, const std::string& client_ip) {
    size_t have = 0;
    for (uint32_t served = 0; served < options_.max_requests_per_connection && running_.load(); ++served) {
        bool keep_alive = false;
        size_t leftover = serve_one(w, sock, client_ip, keep_alive, have);
        if (leftover == static_cast<size_t>(-1)) break;      // connection finished or failed
        if (!keep_alive) break;
        if (leftover > 0) { have = leftover; continue; }     // pipelined request already buffered
        // Nothing pending: let the acceptor watch this idle connection so this
        // worker can serve someone else
        park_idle(sock.release(), client_ip);
        return;
    }
    sock.close();
}

// Serves one request from `sock`. Returns the number of pipelined bytes left
// in the worker buffer, or (size_t)-1 when the connection must close.
size_t HttpServer::serve_one(Worker& w, osal::Socket& sock, const std::string& client_ip, bool& keep_alive, size_t have) {
    char* buf = w.rx_buf.data();
    const size_t cap = w.rx_buf.size();
    keep_alive = false;

    sock.set_timeout(options_.request_timeout_ms, 5000);
    HttpRequest req;
    size_t header_bytes = 0;
    bool have_head = false;
    uint64_t t_start = 0;
    while (true) {
        if (have > 0) {
            buf[have] = '\0';
            if (parse_request_headers(buf, have, req, header_bytes)) { have_head = true; break; }
            if (have >= cap - 1) break;
        }
        ssize_t n = sock.recv(buf + have, cap - 1 - have);
        if (n <= 0) break;
        if (t_start == 0) t_start = osal::Thread::get_time_us();
        have += static_cast<size_t>(n);
    }
    if (!have_head) {
        if (have > 0) {
            send_status_only(sock, have >= cap - 1 ? 431 : 400, have >= cap - 1 ? "Request Header Fields Too Large" : "Bad Request");
            metrics_.errors.fetch_add(1);
            if (options_.access_log) printf("[HTTP] %s unparsable request head (%u bytes) -> %d\n", client_ip.c_str(), (unsigned)have, have >= cap - 1 ? 431 : 400);
        }
        return static_cast<size_t>(-1);
    }
    if (t_start == 0) t_start = osal::Thread::get_time_us();

    if (req.invalid_content_length) { send_status_only(sock, 400, "Bad Request"); metrics_.errors.fetch_add(1); return static_cast<size_t>(-1); }
    if (req.chunked_body) { send_status_only(sock, 411, "Length Required"); metrics_.errors.fetch_add(1); return static_cast<size_t>(-1); }
    if (req.content_length > options_.max_body_size) { send_status_only(sock, 413, "Payload Too Large"); metrics_.errors.fetch_add(1); return static_cast<size_t>(-1); }

    size_t body_in_buf = have - header_bytes;
    size_t take = std::min(body_in_buf, req.content_length);
    req.body.assign(buf + header_bytes, take);
    size_t leftover = body_in_buf - take;
    while (req.body.size() < req.content_length) {
        size_t needed = std::min(cap, req.content_length - req.body.size());
        ssize_t r = sock.recv(buf, needed);
        if (r <= 0) return static_cast<size_t>(-1);
        req.body.append(buf, static_cast<size_t>(r));
    }
    if (leftover > 0 && take == req.content_length) {
        std::memmove(buf, buf + header_bytes + take, leftover);
    } else {
        leftover = 0;
    }
    req.client_ip = client_ip;

    HttpConnection conn(*this, sock, req, buf, cap);
    bool handled = false;
    try {
        if (handler_) handled = handler_->handle(conn);
    } catch (const std::exception& e) {
        metrics_.errors.fetch_add(1, std::memory_order_relaxed);
        if (!conn.responded()) {
            conn.set_close();
            conn.send_simple(503, "application/json", "{\"error\":\"resource_exhausted\"}", "Retry-After: 1\r\n");
        }
        handled = true;
    }
    if (conn.detached()) {
        metrics_.record(static_cast<uint32_t>(osal::Thread::get_time_us() - t_start));
        return static_cast<size_t>(-1);
    }
    if (!handled) {
        if (route_handler_) {
            HttpResponse resp;
            try {
                mem::MonotonicArena arena;
                resp = route_handler_->handle_request(req, arena, &sock);
                arena.reset();
            } catch (const std::exception& e) {
                resp = RouteHandler::make_status(503, "ServiceUnavailable", "node is out of memory");
                metrics_.errors.fetch_add(1, std::memory_order_relaxed);
            }
            if (resp.socket_taken) {
                // Streaming handler owns the descriptor now (watch / log follow)
                metrics_.record(static_cast<uint32_t>(osal::Thread::get_time_us() - t_start));
                return static_cast<size_t>(-1);
            }
            conn.set_close(); // Kubernetes responses keep the historical close semantics
            if (!resp.is_chunked) conn.send_simple(resp.status_code, resp.content_type, resp.body);
            else conn.mark_responded(resp.status_code);
        } else {
            conn.send_simple(404, "application/json", "{\"error\":\"not_found\"}");
        }
    } else if (!conn.responded()) {
        conn.send_simple(500, "application/json", "{\"error\":\"handler_no_response\"}");
    }

    uint32_t took_us = static_cast<uint32_t>(osal::Thread::get_time_us() - t_start);
    metrics_.record(took_us);
    if (options_.access_log) {
        printf("[HTTP] %s %s %.*s -> %d %u ms %s%s\n", client_ip.c_str(), http_method_str(req.method),
               (int)(req.path.size() > 60 ? 60 : req.path.size()), req.path.data(), conn.status(), (unsigned)(took_us / 1000),
               conn.keep_alive() ? "keep-alive" : "close", conn.responded() ? "" : " (no response)");
    }
    keep_alive = conn.keep_alive();
    return keep_alive ? leftover : static_cast<size_t>(-1);
}

} // namespace kube32::api
