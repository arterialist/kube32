#include "app/ingress.hpp"
#include "embedded/embedded_blobs.hpp"
#include "ipc/channel.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"

#include <cstring>
#include <cstdio>
#include <algorithm>

namespace kube32::app {

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

bool HttpAppSink::flush_head() {
    if (head_sent_) return true;
    head_sent_ = true;
    return conn_.begin_chunked(status_ > 0 ? status_ : 200, content_type_.empty() ? "application/json" : content_type_, extra_headers_);
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
    flush_buffer();
    conn_.end_chunked();
}

// =============================================================================
// SseHub
// =============================================================================
SseHub::SseHub() = default;
SseHub::~SseHub() { stop(); }

bool SseHub::start() {
    if (running_.load()) return true;
    running_.store(true);
    thread_ = std::make_unique<osal::Thread>();
    osal::Thread::Config cfg;
    cfg.name = "k32_sse";
    cfg.stack_size = 4096;
    cfg.priority = 4;
    bool ok = thread_->start([](void* p) { static_cast<SseHub*>(p)->run(); }, this, cfg);
    if (!ok) running_.store(false);
    return ok;
}

void SseHub::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (thread_) { thread_->join(); thread_.reset(); }
    osal::LockGuard guard(mutex_);
    for (auto& c : clients_) c.close();
    clients_.clear();
}

void SseHub::add(osal::Socket sock) {
    osal::LockGuard guard(mutex_);
    if (clients_.size() >= max_clients_) {
        // Drop the oldest viewer so the newest tab always gets a stream
        clients_.front().close();
        clients_.erase(clients_.begin());
    }
    sock.set_timeout(200, 300);
    clients_.push_back(std::move(sock));
}

void SseHub::broadcast(std::string_view event, std::string_view data) {
    std::string frame;
    frame.reserve(event.size() + data.size() + 16);
    frame.append("event: ").append(event.data(), event.size()).append("\n");
    frame.append("data: ").append(data.data(), data.size()).append("\n\n");
    osal::LockGuard guard(mutex_);
    if (pending_.size() < 64) pending_.push_back(std::move(frame));
}

size_t SseHub::count() const {
    osal::LockGuard guard(mutex_);
    return clients_.size();
}

bool SseHub::send_to_all(const char* data, size_t len) {
    // Caller holds mutex_
    bool any_dropped = false;
    for (size_t i = 0; i < clients_.size();) {
        if (!clients_[i].send_all(data, len)) {
            clients_[i].close();
            clients_.erase(clients_.begin() + static_cast<long>(i));
            any_dropped = true;
        } else {
            ++i;
        }
    }
    return any_dropped;
}

void SseHub::run() {
    uint64_t last_ping = 0;
    uint64_t last_cluster = 0;
    size_t last_count = 0;
    while (running_.load()) {
        std::vector<std::string> batch;
        {
            osal::LockGuard guard(mutex_);
            batch.swap(pending_);
        }
        uint64_t now = osal::System::get_uptime_ms();
        if (now - last_cluster >= 2000 && cluster_provider_) {
            last_cluster = now;
            std::string json = cluster_provider_();
            if (!json.empty()) batch.push_back("event: cluster\ndata: " + json + "\n\n");
        }
        if (now - last_ping >= 15000) {
            last_ping = now;
            batch.push_back(": ping\n\n");
        }
        if (!batch.empty()) {
            osal::LockGuard guard(mutex_);
            for (const auto& f : batch) {
                if (clients_.empty()) break;
                send_to_all(f.data(), f.size());
                events_sent_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        size_t n = count();
        if (n != last_count) {
            last_count = n;
            char buf[48];
            int l = snprintf(buf, sizeof(buf), "event: presence\ndata: {\"viewers\":%zu}\n\n", n);
            osal::LockGuard guard(mutex_);
            send_to_all(buf, static_cast<size_t>(l));
        }
        osal::System::feed_watchdog();
        osal::Thread::sleep_ms(50);
    }
}

// =============================================================================
// Ingress
// =============================================================================
Ingress::Ingress(IngressConfig cfg) : cfg_(std::move(cfg)) {
    sse_.set_cluster_provider([this]() { return build_cluster_json(); });
}

Ingress::~Ingress() {
    sse_.stop();
}

static bool starts_with(std::string_view s, const char* prefix) {
    size_t n = std::strlen(prefix);
    return s.size() >= n && std::memcmp(s.data(), prefix, n) == 0;
}

bool Ingress::handle(api::HttpConnection& conn) {
    const api::HttpRequest& req = conn.request();
    std::string_view path = req.path;

    if (path == "/events") return serve_events(conn);
    if (path == "/api/cluster") {
        std::string json = build_cluster_json();
        return conn.send_simple(200, "application/json", json, "Cache-Control: no-store\r\n");
    }
    if (path == "/healthz") return conn.send_simple(200, "text/plain", "ok\n");
    if (starts_with(path, "/api/")) return serve_api(conn);
    return serve_static(conn);
}

bool Ingress::serve_static(api::HttpConnection& conn) {
    const api::HttpRequest& req = conn.request();
    if (req.method != api::HttpMethod::Get && req.method != api::HttpMethod::Head) {
        return conn.send_simple(405, "application/json", "{\"error\":\"method_not_allowed\"}");
    }
    size_t count = 0;
    const embedded::EmbeddedAsset* assets = embedded::assets(count);
    const embedded::EmbeddedAsset* match = nullptr;
    const embedded::EmbeddedAsset* index = nullptr;
    for (size_t i = 0; i < count; ++i) {
        if (cfg_.app_name != assets[i].app) continue;
        if (req.path == assets[i].path) { match = &assets[i]; break; }
        if (std::strcmp(assets[i].path, "/") == 0) index = &assets[i];
    }
    if (!match) {
        // Single-page app fallback for client-side routes (no file extension)
        bool looks_like_file = req.path.find('.') != std::string::npos;
        if (!looks_like_file && index) match = index;
    }
    if (!match) return conn.send_simple(404, "application/json", "{\"error\":\"not_found\"}");
    stats_.static_hits.fetch_add(1, std::memory_order_relaxed);
    char etag[24];
    snprintf(etag, sizeof(etag), "%zx-%x", match->len, static_cast<unsigned>(reinterpret_cast<uintptr_t>(match->data) & 0xffff));
    if (match->gzip && !req.accepts_gzip) {
        // We cannot inflate on-device; every browser accepts gzip, curl needs --compressed
        return conn.send_simple(406, "text/plain", "asset is stored gzip-compressed; send Accept-Encoding: gzip\n");
    }
    const char* cache = (std::strcmp(match->path, "/") == 0) ? "no-cache" : "public, max-age=86400, immutable";
    return conn.send_blob(match->content_type, match->data, match->len, match->gzip, etag, cache);
}

bool Ingress::serve_events(api::HttpConnection& conn) {
    const api::HttpRequest& req = conn.request();
    if (req.method != api::HttpMethod::Get) return conn.send_simple(405, "text/plain", "GET only\n");
    // Head for an unbounded stream: no framing, connection stays open
    const char* head =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "X-Accel-Buffering: no\r\n"
        "\r\n"
        "retry: 2000\n\n";
    if (!conn.send_raw(head, std::strlen(head))) return true;
    sse_.add(conn.take_socket());
    return true;
}

bool Ingress::resolve_upstream(std::string& out_ip, uint16_t& out_port, bool& out_local) {
    // Local pod: zero network hops
    if (runtime::AppPodHost::instance().pod_handles_requests(cfg_.core_namespace + "/" + cfg_.core_pod)) {
        out_local = true;
        return true;
    }
    out_local = false;
    uint64_t now = osal::System::get_uptime_ms();
    {
        osal::LockGuard guard(resolve_mutex_);
        if (!cached_ip_.empty() && now - cached_at_ms_ < 5000) {
            out_ip = cached_ip_;
            out_port = cached_port_;
            return true;
        }
    }
    if (!cfg_.upstream_override_ip.empty()) {
        out_ip = cfg_.upstream_override_ip;
        out_port = cfg_.upstream_port;
    } else if (cfg_.store) {
        std::string pod_json;
        if (!cfg_.store->get("/registry/pods/" + cfg_.core_namespace + "/" + cfg_.core_pod, pod_json).ok()) return false;
        models::KubePod pod = models::parse_pod_manifest(pod_json);
        if (pod.node_name.empty() || pod.node_name == "<none>") return false;
        std::string node_json;
        if (!cfg_.store->get("/registry/nodes/" + pod.node_name, node_json).ok()) return false;
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(node_json, doc, err)) return false;
        const models::JsonValue* status = doc.get("status");
        const models::JsonValue* addrs = status ? status->get("addresses") : nullptr;
        if (!addrs || !addrs->is_array()) return false;
        for (const auto& a : addrs->arr_val) {
            if (a.get_string("type", "") == "InternalIP") { out_ip = a.get_string("address", ""); break; }
        }
        if (out_ip.empty()) return false;
        out_port = cfg_.upstream_port;
    } else {
        return false;
    }
    osal::LockGuard guard(resolve_mutex_);
    cached_ip_ = out_ip;
    cached_port_ = out_port;
    cached_at_ms_ = now;
    return true;
}

Ingress::Upstream* Ingress::acquire_upstream(const std::string& ip, uint16_t port) {
    osal::LockGuard guard(pool_mutex_);
    if (pool_ip_ != ip || pool_port_ != port) {
        for (auto& u : pool_) u->sock.close();
        pool_.clear();
        pool_ip_ = ip;
        pool_port_ = port;
    }
    for (auto& u : pool_) {
        if (!u->in_use) {
            u->in_use = true;
            return u.get();
        }
    }
    if (pool_.size() < cfg_.upstream_pool_size) {
        auto u = std::make_unique<Upstream>();
        u->in_use = true;
        pool_.push_back(std::move(u));
        return pool_.back().get();
    }
    return nullptr;
}

void Ingress::release_upstream(Upstream* u, bool reusable) {
    osal::LockGuard guard(pool_mutex_);
    if (!reusable) u->sock.close();
    u->last_used_ms = osal::System::get_uptime_ms();
    u->in_use = false;
}

bool Ingress::serve_api(api::HttpConnection& conn) {
    std::string ip;
    uint16_t port = 0;
    bool local = false;
    if (!resolve_upstream(ip, port, local)) {
        stats_.proxy_errors.fetch_add(1, std::memory_order_relaxed);
        return conn.send_simple(503, "application/json",
            "{\"error\":\"unavailable\",\"message\":\"no running pod serves this app yet\"}", "Retry-After: 2\r\n");
    }
    const api::HttpRequest& req = conn.request();
    if (local) {
        stats_.local_calls.fetch_add(1, std::memory_order_relaxed);
        runtime::AppRequest areq;
        areq.method = api::http_method_str(req.method);
        areq.path_and_query = req.raw_uri;
        areq.cookie = req.cookie;
        areq.content_type = req.content_type;
        areq.request_id = req.request_id;
        areq.forwarded_for = req.client_ip;
        areq.body = req.body;
        HttpAppSink sink(conn);
        uint64_t t_call = osal::Thread::get_time_us();
        int32_t rc = runtime::AppPodHost::instance().handle(cfg_.core_namespace + "/" + cfg_.core_pod, areq, sink);
        {
            // Where a large response spends its time: inside the guest or in
            // socket writes made on its behalf. Bounded diagnostic.
            static int logged = 0;
            uint32_t total_us = static_cast<uint32_t>(osal::Thread::get_time_us() - t_call);
            if (sink.bytes() >= 16384 && logged < 20) {
                logged++;
                printf("[INGRESS] %.*s: %u bytes in %u ms, %u ms of it in %u socket writes\n",
                       (int)areq.path_and_query.size(), areq.path_and_query.data(), (unsigned)sink.bytes(),
                       (unsigned)(total_us / 1000), (unsigned)(sink.socket_us() / 1000), (unsigned)sink.flushes());
            }
        }
        if (rc < 0 && !conn.responded()) {
            return conn.send_simple(502, "application/json", "{\"error\":\"pod_failure\"}");
        }
        return true;
    }
    return proxy_upstream(conn, ip, port);
}

// Finds "\r\n\r\n" in a buffer; returns offset of body or npos
static size_t find_head_end(const char* buf, size_t len) {
    for (size_t i = 3; i < len; ++i) {
        if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n') return i + 1;
    }
    return std::string::npos;
}

bool Ingress::proxy_upstream(api::HttpConnection& conn, const std::string& ip, uint16_t port) {
    const api::HttpRequest& req = conn.request();
    char* buf = conn.scratch();
    const size_t cap = conn.scratch_capacity();

    for (int attempt = 0; attempt < 2; ++attempt) {
        Upstream* up = acquire_upstream(ip, port);
        if (!up) {
            return conn.send_simple(503, "application/json", "{\"error\":\"busy\",\"message\":\"upstream pool exhausted\"}", "Retry-After: 1\r\n");
        }
        bool fresh = false;
        if (!up->sock.is_valid()) {
            up->sock = osal::Socket();
            if (!up->sock.create(osal::SocketType::Tcp) || !up->sock.connect(ip.c_str(), port, cfg_.upstream_timeout_ms)) {
                release_upstream(up, false);
                stats_.proxy_errors.fetch_add(1, std::memory_order_relaxed);
                return conn.send_simple(502, "application/json", "{\"error\":\"bad_gateway\",\"message\":\"cannot reach the pod's node\"}");
            }
            up->sock.set_tcp_nodelay(true);
            fresh = true;
            if (attempt > 0) stats_.upstream_reconnects.fetch_add(1, std::memory_order_relaxed);
        }
        up->sock.set_timeout(cfg_.upstream_timeout_ms, cfg_.upstream_timeout_ms);

        // ---- Send request --------------------------------------------------
        int n = snprintf(buf, cap,
                         "%s %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "X-Kube32-Pod: %s/%s\r\n"
                         "X-Forwarded-For: %s\r\n"
                         "Content-Length: %zu\r\n"
                         "Connection: keep-alive\r\n",
                         api::http_method_str(req.method), req.raw_uri.c_str(), ip.c_str(),
                         cfg_.core_namespace.c_str(), cfg_.core_pod.c_str(), req.client_ip.c_str(), req.body.size());
        if (n <= 0 || static_cast<size_t>(n) >= cap - 512) {
            release_upstream(up, true);
            return conn.send_simple(414, "application/json", "{\"error\":\"uri_too_long\"}");
        }
        size_t off = static_cast<size_t>(n);
        auto add_header = [&](const char* name, const std::string& value) {
            if (value.empty()) return;
            int m = snprintf(buf + off, cap - off, "%s: %s\r\n", name, value.c_str());
            if (m > 0 && off + static_cast<size_t>(m) < cap) off += static_cast<size_t>(m);
        };
        add_header("Cookie", req.cookie);
        add_header("Content-Type", req.content_type);
        add_header("X-Request-Id", req.request_id);
        if (off + 2 < cap) { buf[off++] = '\r'; buf[off++] = '\n'; }

        bool sent = up->sock.send_all(buf, off) && (req.body.empty() || up->sock.send_all(req.body.data(), req.body.size()));
        if (!sent) {
            release_upstream(up, false);
            if (!fresh && attempt == 0) continue; // stale keep-alive socket: reconnect once
            stats_.proxy_errors.fetch_add(1, std::memory_order_relaxed);
            return conn.send_simple(502, "application/json", "{\"error\":\"bad_gateway\",\"message\":\"upstream write failed\"}");
        }

        // ---- Read response head ---------------------------------------------
        size_t have = 0;
        size_t head_end = std::string::npos;
        while (have < cap - 1) {
            ssize_t r = up->sock.recv(buf + have, cap - 1 - have);
            if (r <= 0) break;
            have += static_cast<size_t>(r);
            head_end = find_head_end(buf, have);
            if (head_end != std::string::npos) break;
        }
        if (head_end == std::string::npos) {
            release_upstream(up, false);
            if (!fresh && attempt == 0 && have == 0) continue; // upstream closed idle keep-alive
            stats_.proxy_errors.fetch_add(1, std::memory_order_relaxed);
            return conn.send_simple(504, "application/json", "{\"error\":\"gateway_timeout\",\"message\":\"pod did not answer in time\"}");
        }

        // ---- Parse what we need from the head ---------------------------------
        std::string_view head(buf, head_end);
        bool chunked = false;
        bool upstream_close = false;
        long content_length = -1;
        size_t line_start = head.find("\r\n");
        std::string_view status_line = head.substr(0, line_start == std::string_view::npos ? 0 : line_start);
        std::string rebuilt;
        rebuilt.reserve(head_end + 32);
        rebuilt.append(status_line.data(), status_line.size()).append("\r\n");
        size_t cur = (line_start == std::string_view::npos) ? head.size() : line_start + 2;
        while (cur < head.size()) {
            size_t eol = head.find("\r\n", cur);
            if (eol == std::string_view::npos || eol == cur) break;
            std::string_view line = head.substr(cur, eol - cur);
            cur = eol + 2;
            size_t colon = line.find(':');
            if (colon == std::string_view::npos) continue;
            std::string_view key = line.substr(0, colon);
            std::string_view val = line.substr(colon + 1);
            while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
            if (key.size() == 10 && strncasecmp(key.data(), "Connection", 10) == 0) {
                if (val.size() >= 5 && strncasecmp(val.data(), "close", 5) == 0) upstream_close = true;
                continue; // rewritten below
            }
            if (key.size() == 17 && strncasecmp(key.data(), "Transfer-Encoding", 17) == 0 && val.find("chunked") != std::string_view::npos) chunked = true;
            if (key.size() == 14 && strncasecmp(key.data(), "Content-Length", 14) == 0) content_length = std::strtol(std::string(val).c_str(), nullptr, 10);
            rebuilt.append(line.data(), line.size()).append("\r\n");
        }
        rebuilt.append(conn.keep_alive() ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
        rebuilt.append("\r\n");
        if (!conn.send_raw(rebuilt.data(), rebuilt.size())) {
            release_upstream(up, false);
            return true;
        }

        // ---- Relay body -------------------------------------------------------
        size_t body_have = have - head_end;
        const char* body_ptr = buf + head_end;
        bool ok = true;
        if (chunked) {
            // Pass bytes through while tracking chunk framing to know where the body ends
            size_t need_chunk_hdr = 1;      // state: 1 = reading size line, 0 = reading data
            size_t remaining = 0;           // bytes left in current chunk including trailing CRLF
            bool done = false;
            std::string pending(body_ptr, body_have);
            while (!done && ok) {
                size_t pos = 0;
                while (pos < pending.size() && !done) {
                    if (need_chunk_hdr) {
                        size_t eol = pending.find("\r\n", pos);
                        if (eol == std::string::npos) break;
                        size_t size = std::strtoul(pending.c_str() + pos, nullptr, 16);
                        pos = eol + 2;
                        if (size == 0) {
                            // trailer: expect final CRLF
                            size_t t = pending.find("\r\n", pos);
                            if (t == std::string::npos) { remaining = 2; need_chunk_hdr = 0; done = false; break; }
                            pos = t + 2;
                            done = true;
                            break;
                        }
                        remaining = size + 2;
                        need_chunk_hdr = 0;
                    } else {
                        size_t take = std::min(remaining, pending.size() - pos);
                        pos += take;
                        remaining -= take;
                        if (remaining == 0) need_chunk_hdr = 1;
                    }
                }
                if (pos > 0) {
                    ok = conn.send_raw(pending.data(), pos);
                    pending.erase(0, pos);
                }
                if (done) {
                    if (!pending.empty()) ok = ok && conn.send_raw(pending.data(), pending.size());
                    break;
                }
                if (!ok) break;
                ssize_t r = up->sock.recv(buf, cap);
                if (r <= 0) { ok = false; break; }
                pending.append(buf, static_cast<size_t>(r));
            }
        } else if (content_length >= 0) {
            size_t remaining = static_cast<size_t>(content_length);
            size_t take = std::min(remaining, body_have);
            if (take > 0) ok = conn.send_raw(body_ptr, take);
            remaining -= take;
            while (ok && remaining > 0) {
                ssize_t r = up->sock.recv(buf, std::min(remaining, cap));
                if (r <= 0) { ok = false; break; }
                ok = conn.send_raw(buf, static_cast<size_t>(r));
                remaining -= static_cast<size_t>(r);
            }
        } else {
            // Unknown length: relay until upstream closes
            if (body_have > 0) ok = conn.send_raw(body_ptr, body_have);
            while (ok) {
                ssize_t r = up->sock.recv(buf, cap);
                if (r <= 0) break;
                ok = conn.send_raw(buf, static_cast<size_t>(r));
            }
            upstream_close = true;
            conn.set_close();
        }
        stats_.proxied.fetch_add(1, std::memory_order_relaxed);
        if (!ok) { stats_.proxy_errors.fetch_add(1, std::memory_order_relaxed); conn.set_close(); }
        release_upstream(up, ok && !upstream_close);
        return true;
    }
    return conn.send_simple(502, "application/json", "{\"error\":\"bad_gateway\"}");
}

std::string Ingress::build_cluster_json() {
    std::string out;
    out.reserve(1024);
    out += "{\"nodes\":[";
    bool first = true;
    if (cfg_.store) {
        std::vector<std::string> nodes;
        cfg_.store->list("/registry/nodes/", nodes);
        for (const auto& nj : nodes) {
            models::JsonValue doc;
            std::string err;
            if (!models::parse_json(nj, doc, err)) continue;
            const models::JsonValue* meta = doc.get("metadata");
            std::string name = meta ? meta->get_string("name", "") : "";
            const models::JsonValue* status = doc.get("status");
            bool ready = false;
            std::string free_mem, free_psram, cap_psram;
            if (status) {
                const models::JsonValue* conds = status->get("conditions");
                if (conds && conds->is_array()) {
                    for (const auto& c : conds->arr_val) {
                        if (c.get_string("type", "") == "Ready") ready = (c.get_string("status", "") == "True");
                    }
                }
                const models::JsonValue* alloc = status->get("allocatable");
                if (alloc) {
                    free_mem = alloc->get_string("memory", "");
                    free_psram = alloc->get_string("esp32.io/psram", alloc->get_string("esp.io/psram", ""));
                }
                const models::JsonValue* capv = status->get("capacity");
                if (capv) cap_psram = capv->get_string("esp32.io/psram", capv->get_string("esp.io/psram", ""));
            }
            bool is_cp = false;
            if (meta) {
                const models::JsonValue* labels = meta->get("labels");
                if (labels && labels->get("node-role.kubernetes.io/control-plane")) is_cp = true;
            }
            if (!first) out += ",";
            first = false;
            out += "{\"name\":\"" + name + "\",\"role\":\"" + (is_cp ? "control-plane" : "worker") + "\",\"ready\":" + (ready ? "true" : "false");
            out += ",\"free_heap\":" + std::to_string(models::parse_quantity(free_mem));
            out += ",\"free_psram\":" + std::to_string(models::parse_quantity(free_psram));
            out += ",\"total_psram\":" + std::to_string(models::parse_quantity(cap_psram));
            if (name == cfg_.local_node_name) {
                out += ",\"min_free_heap\":" + std::to_string(osal::System::get_min_free_heap_bytes());
                out += ",\"uptime_ms\":" + std::to_string(osal::System::get_uptime_ms());
                out += ",\"free_heap_live\":" + std::to_string(osal::System::get_free_heap_bytes());
            }
            if (node_telemetry_) out += node_telemetry_(name);
            // Pods on this node
            out += ",\"pods\":[";
            std::vector<std::string> pods;
            cfg_.store->list("/registry/pods/", pods);
            bool pf = true;
            for (const auto& pj : pods) {
                models::KubePod p = models::parse_pod_manifest(pj);
                if (p.node_name != name) continue;
                if (!pf) out += ",";
                pf = false;
                out += "{\"name\":\"" + p.name + "\",\"phase\":\"" + p.status + "\",\"restarts\":" + std::to_string(p.restart_count) + "}";
            }
            out += "]}";
        }
    }
    out += "],\"ingress\":{";
    if (server_) {
        auto& m = server_->metrics();
        uint32_t p50 = 0, p99 = 0;
        m.percentiles(p50, p99);
        uint64_t reqs = m.requests.load();
        uint64_t up = osal::System::get_uptime_ms();
        out += "\"requests\":" + std::to_string(reqs);
        out += ",\"errors\":" + std::to_string(m.errors.load());
        out += ",\"active\":" + std::to_string(m.active.load());
        out += ",\"p50_us\":" + std::to_string(p50) + ",\"p99_us\":" + std::to_string(p99);
        out += ",\"avg_us\":" + std::to_string(reqs ? m.total_us.load() / reqs : 0);
        out += ",\"workers\":" + std::to_string(server_->options().workers);
        out += ",\"uptime_ms\":" + std::to_string(up);
    }
    out += ",\"active_sse\":" + std::to_string(sse_.count());
    out += ",\"sse_events\":" + std::to_string(sse_.events_sent());
    out += ",\"proxied\":" + std::to_string(stats_.proxied.load());
    out += ",\"proxy_errors\":" + std::to_string(stats_.proxy_errors.load());
    out += ",\"local_calls\":" + std::to_string(stats_.local_calls.load());
    out += ",\"static_hits\":" + std::to_string(stats_.static_hits.load());
    out += "},\"link\":{";
    auto& cs = ipc::ChannelManager::instance().stats();
    out += "\"espnow_frames_rx\":" + std::to_string(cs.frames_received.load());
    out += ",\"espnow_frames_tx\":" + std::to_string(cs.frames_sent.load());
    out += ",\"delivered\":" + std::to_string(cs.frames_delivered.load());
    out += ",\"crc_errors\":" + std::to_string(cs.crc_errors.load());
    out += ",\"send_errors\":" + std::to_string(cs.send_errors.load());
    out += ",\"reassembled\":" + std::to_string(cs.reassembled_msgs.load());
    out += ",\"hello_rx\":" + std::to_string(cs.hello_rx.load());
    out += ",\"peers\":[";
    bool pf = true;
    for (const auto& p : ipc::ChannelManager::instance().peers()) {
        if (!pf) out += ",";
        pf = false;
        char mac[24];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", p.endpoint.mac_address[0], p.endpoint.mac_address[1], p.endpoint.mac_address[2],
                 p.endpoint.mac_address[3], p.endpoint.mac_address[4], p.endpoint.mac_address[5]);
        out += "{\"node\":\"" + p.node_name + "\",\"id\":" + std::to_string(p.node_id) + ",\"mac\":\"" + mac + "\",\"last_seen_ms\":" + std::to_string(p.last_seen_ms) + ",\"hellos\":" + std::to_string(p.hello_count) + "}";
    }
    out += "]},\"now\":" + std::to_string(osal::System::get_uptime_ms()) + "}";
    return out;
}

// =============================================================================
// PodServerHandler
// =============================================================================
bool PodServerHandler::handle(api::HttpConnection& conn) {
    const api::HttpRequest& req = conn.request();
    if (req.path == "/healthz") return conn.send_simple(200, "text/plain", "ok\n");
    std::string pod = req.target_pod;
    if (pod.empty()) pod = runtime::AppPodHost::instance().default_request_pod();
    if (pod.empty() || !runtime::AppPodHost::instance().pod_handles_requests(pod)) {
        return conn.send_simple(503, "application/json", "{\"error\":\"unavailable\",\"message\":\"no request-handling pod on this node\"}", "Retry-After: 2\r\n");
    }
    runtime::AppRequest areq;
    areq.method = api::http_method_str(req.method);
    areq.path_and_query = req.raw_uri;
    areq.cookie = req.cookie;
    areq.content_type = req.content_type;
    areq.request_id = req.request_id;
    areq.forwarded_for = req.client_ip;
    areq.body = req.body;
    HttpAppSink sink(conn);
    int32_t rc = runtime::AppPodHost::instance().handle(pod, areq, sink);
    if (rc < 0 && !conn.responded()) {
        return conn.send_simple(502, "application/json", "{\"error\":\"pod_failure\"}");
    }
    return true;
}

} // namespace kube32::app
