#include "app/ingress_controller.hpp"
#include "app/pod_server.hpp"
#include "embedded/embedded_blobs.hpp"
#include "ipc/channel.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <strings.h>

namespace kube32::app {

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
    if (running_.load()) {
        running_.store(false);
        if (thread_) { thread_->join(); thread_.reset(); }
    }
    osal::LockGuard guard(mutex_);
    for (auto& c : clients_) c.sock.close();
    clients_.clear();
    pending_.clear();
}

void SseHub::add(osal::Socket sock, Subscription sub) {
    osal::LockGuard guard(mutex_);
    if (clients_.size() >= max_clients_ && !clients_.empty()) {
        // Drop the oldest viewer so the newest tab always gets a stream
        clients_.front().sock.close();
        clients_.erase(clients_.begin());
    }
    sock.set_timeout(200, 300);
    clients_.push_back(Client{std::move(sock), std::move(sub)});
}

void SseHub::broadcast(uint32_t topic_hash, std::string_view event, std::string_view data) {
    Frame f;
    f.topic_hash = topic_hash;
    f.text.reserve(event.size() + data.size() + 16);
    if (!event.empty()) f.text.append("event: ").append(event.data(), event.size()).append("\n");
    f.text.append("data: ").append(data.data(), data.size()).append("\n\n");
    osal::LockGuard guard(mutex_);
    if (clients_.empty()) return;
    if (pending_.size() < 64) pending_.push_back(std::move(f));
}

void SseHub::retain_streams(const std::vector<uint32_t>& stream_ids) {
    osal::LockGuard guard(mutex_);
    for (size_t i = 0; i < clients_.size();) {
        if (std::find(stream_ids.begin(), stream_ids.end(), clients_[i].sub.stream_id) == stream_ids.end()) {
            clients_[i].sock.close();
            clients_.erase(clients_.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
}

size_t SseHub::count() const {
    osal::LockGuard guard(mutex_);
    return clients_.size();
}

size_t SseHub::count_streams(const std::vector<uint32_t>& stream_ids) const {
    osal::LockGuard guard(mutex_);
    size_t n = 0;
    for (const auto& c : clients_) {
        if (std::find(stream_ids.begin(), stream_ids.end(), c.sub.stream_id) != stream_ids.end()) n++;
    }
    return n;
}

bool SseHub::has_listeners(uint32_t topic_hash) const {
    osal::LockGuard guard(mutex_);
    for (const auto& c : clients_) {
        if (std::find(c.sub.topics.begin(), c.sub.topics.end(), topic_hash) != c.sub.topics.end()) return true;
    }
    return false;
}

void SseHub::send_to(uint32_t topic_hash, const std::string& frame) {
    // Caller holds mutex_
    for (size_t i = 0; i < clients_.size();) {
        Client& c = clients_[i];
        bool wants = topic_hash == 0 || std::find(c.sub.topics.begin(), c.sub.topics.end(), topic_hash) != c.sub.topics.end();
        if (wants && !c.sock.send_all(frame.data(), frame.size())) {
            c.sock.close();
            clients_.erase(clients_.begin() + static_cast<long>(i));
            continue;
        }
        ++i;
    }
}

void SseHub::send_presence() {
    // Caller holds mutex_. One "presence" event per stream whose viewer count changed.
    std::map<uint32_t, size_t> counts;
    for (const auto& c : clients_) counts[c.sub.stream_id]++;
    for (auto it = presence_sent_.begin(); it != presence_sent_.end();) {
        if (counts.find(it->first) == counts.end()) it = presence_sent_.erase(it); else ++it;
    }
    for (const auto& kv : counts) {
        size_t& last = presence_sent_[kv.first];
        if (last == kv.second) continue;
        last = kv.second;
        char buf[64];
        int l = snprintf(buf, sizeof(buf), "event: presence\ndata: {\"viewers\":%zu}\n\n", kv.second);
        for (size_t i = 0; i < clients_.size();) {
            Client& c = clients_[i];
            if (c.sub.stream_id == kv.first && c.sub.presence && !c.sock.send_all(buf, static_cast<size_t>(l))) {
                c.sock.close();
                clients_.erase(clients_.begin() + static_cast<long>(i));
                continue;
            }
            ++i;
        }
    }
}

void SseHub::run() {
    uint64_t last_ping = 0;
    while (running_.load()) {
        std::vector<Frame> batch;
        {
            osal::LockGuard guard(mutex_);
            batch.swap(pending_);
        }
        uint64_t now = osal::System::get_uptime_ms();
        {
            osal::LockGuard guard(mutex_);
            for (const auto& f : batch) {
                if (clients_.empty()) break;
                send_to(f.topic_hash, f.text);
                events_sent_.fetch_add(1, std::memory_order_relaxed);
            }
            if (now - last_ping >= 15000) {
                last_ping = now;
                static const std::string ping = ": ping\n\n";
                send_to(0, ping);
            }
            send_presence();
        }
        osal::System::feed_watchdog();
        osal::Thread::sleep_ms(50);
    }
}

// =============================================================================
// IngressController
// =============================================================================
static uint32_t topic_hash(const std::string& topic) { return ipc::fnv1a_32(topic); }

// SSE event name of a channel message: its "type" (or "t") field, else none
static std::string_view event_name(std::string_view payload) {
    size_t klen = 8;
    size_t pos = payload.find("\"type\":\"");
    if (pos == std::string_view::npos) { pos = payload.find("\"t\":\""); klen = 5; }
    if (pos == std::string_view::npos) return {};
    size_t start = pos + klen;
    size_t end = payload.find('"', start);
    if (end == std::string_view::npos || end - start > 32) return {};
    return payload.substr(start, end - start);
}

static std::string_view request_path(const api::HttpRequest& req) {
    std::string_view raw = req.raw_uri;
    if (raw.empty() || raw.front() != '/') return req.path;
    size_t q = raw.find('?');
    return q == std::string_view::npos ? raw : raw.substr(0, q);
}

IngressController::IngressController(IngressControllerOptions opts)
    : opts_(std::move(opts)), hub_(std::make_shared<SseHub>()), dirty_(std::make_shared<std::atomic<bool>>(true)) {
    hub_->set_max_clients(opts_.max_stream_clients);
}

IngressController::~IngressController() {
    stop();
}

bool IngressController::start() {
    if (opts_.store) {
        for (const char* prefix : {"/registry/ingresses/", "/registry/services/", "/registry/pods/"}) {
            std::shared_ptr<std::atomic<bool>> dirty = dirty_;
            store_watches_.push_back(opts_.store->watch(prefix, [dirty](const storage::WatchEvent&) { dirty->store(true); }));
        }
    }

    api::HttpServerOptions o;
    o.host = "0.0.0.0";
    o.port = opts_.port;
    o.backlog = 8;
    o.rx_buffer_size = opts_.rx_buffer;
    o.max_body_size = 16 * 1024;
    o.workers = opts_.workers;
    o.worker_stack_bytes = opts_.worker_stack;
    o.worker_core_affinity = opts_.core_affinity;
    o.worker_stack_in_psram = opts_.stacks_in_psram;
    o.keepalive_timeout_ms = 8000;
    o.accept_queue_depth = 8;
    o.name = "k32_ing";
    server_ = std::make_unique<api::HttpServer>(o, static_cast<storage::Store*>(nullptr), nullptr);
    server_->set_handler(this);
    if (!server_->start()) {
        std::cout << "[INGRESS] failed to bind port " << opts_.port << std::endl;
        return false;
    }
    hub_->start();
    std::cout << "[INGRESS] controller on :" << opts_.port << " (" << o.workers << " workers), routes from networking.k8s.io/v1 Ingress" << std::endl;
    return true;
}

void IngressController::stop() {
    if (opts_.store) {
        for (uint32_t id : store_watches_) opts_.store->unwatch(id);
    }
    store_watches_.clear();
    {
        osal::LockGuard guard(build_mutex_);
        for (const auto& kv : topic_subs_) ipc::ChannelManager::instance().unsubscribe(kv.second);
        topic_subs_.clear();
    }
    hub_->stop();
    if (server_) server_->stop();
    osal::LockGuard guard(pool_mutex_);
    for (auto& u : pool_) u->sock.close();
    pool_.clear();
}

// ---------------------------------------------------------------------------
// Routing table
// ---------------------------------------------------------------------------
std::shared_ptr<const RouteTable> IngressController::routes() {
    osal::LockGuard guard(table_mutex_);
    return table_;
}

void IngressController::refresh_routes() {
    osal::LockGuard guard(build_mutex_);
    rebuild_locked();
}

std::shared_ptr<const RouteTable> IngressController::current_routes() {
    std::shared_ptr<const RouteTable> t = routes();
    uint64_t now = osal::System::get_uptime_ms();
    if (!t || (dirty_->load() && now - built_at_ms_.load() >= 250)) {
        // One worker rebuilds; the others keep using the table they have
        if (build_mutex_.try_lock(t ? 0 : 2000)) {
            if (!t || dirty_->load()) rebuild_locked();
            build_mutex_.unlock();
            t = routes();
        }
    }
    return t;
}

void IngressController::rebuild_locked() {
    if (!opts_.store) return;
    dirty_->store(false);
    std::shared_ptr<RouteTable> table;
    try {
        std::vector<std::string> ingresses, services, pods;
        opts_.store->list("/registry/ingresses/", ingresses);
        opts_.store->list("/registry/services/", services);
        opts_.store->list("/registry/pods/", pods);
        std::shared_ptr<const RouteTable> previous = routes();
        table = build_route_table(ingresses, services, pods, previous.get());
    } catch (const std::exception& e) {
        // Out of memory: keep serving the previous table and retry later
        std::cout << "[INGRESS] routing table rebuild skipped: " << e.what() << std::endl;
        dirty_->store(true);
        built_at_ms_.store(osal::System::get_uptime_ms());
        return;
    }
    sync_subscriptions_locked(*table);
    std::vector<uint32_t> streams;
    for (const auto& r : table->routes) {
        if (r.kind == BackendKind::EventStream) streams.push_back(r.stream_id);
    }
    hub_->retain_streams(streams);
    {
        osal::LockGuard guard(table_mutex_);
        table_ = table;
    }
    built_at_ms_.store(osal::System::get_uptime_ms());
}

void IngressController::sync_subscriptions_locked(const RouteTable& table) {
    for (auto it = topic_subs_.begin(); it != topic_subs_.end();) {
        if (std::find(table.channel_topics.begin(), table.channel_topics.end(), it->first) == table.channel_topics.end()) {
            ipc::ChannelManager::instance().unsubscribe(it->second);
            it = topic_subs_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& topic : table.channel_topics) {
        if (topic_subs_.count(topic)) continue;
        std::shared_ptr<SseHub> hub = hub_;
        uint32_t hash = topic_hash(topic);
        topic_subs_[topic] = ipc::ChannelManager::instance().subscribe(topic, [hub, hash](std::string_view, uint32_t, const uint8_t* data, size_t len) {
            std::string_view payload(reinterpret_cast<const char*>(data), len);
            hub->broadcast(hash, event_name(payload), payload);
        });
    }
}

// Records the controller's address in status.loadBalancer of every served
// Ingress (kubectl's ADDRESS column). Skips the write when it is current.
void IngressController::sync_status() {
    if (!opts_.store || !opts_.address) return;
    std::string addr = opts_.address();
    std::shared_ptr<const RouteTable> t = routes();
    if (addr.empty() || !t) return;
    for (const auto& info : t->ingresses) {
        if (info.address_ip == addr) continue;
        size_t slash = info.key.find('/');
        std::string key = "/registry/ingresses/" + info.key.substr(0, slash) + "/" + info.key.substr(slash + 1);
        std::string json;
        uint64_t version = 0;
        if (!opts_.store->get_extended(key, json, &version).ok()) continue;
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(json, doc, err) || !doc.is_object()) continue;
        models::JsonValue ip_entry;
        ip_entry.type = models::JsonType::Object;
        ip_entry.set("ip", models::JsonValue{models::JsonType::String, false, 0, addr, {}, {}});
        models::JsonValue list;
        list.type = models::JsonType::Array;
        list.arr_val.push_back(std::move(ip_entry));
        models::JsonValue lb;
        lb.type = models::JsonType::Object;
        lb.set("ingress", std::move(list));
        models::JsonValue status;
        status.type = models::JsonType::Object;
        status.set("loadBalancer", std::move(lb));
        doc.set("status", std::move(status));
        // A concurrent update wins; the next tick retries
        opts_.store->put_extended(key, doc.to_string(), true, false, version, nullptr);
    }
}

void IngressController::tick() {
    if (!opts_.store) return;
    if (dirty_->load()) refresh_routes();
    sync_status();
    std::shared_ptr<const RouteTable> t = routes();
    if (!t) return;

    // kube32.io/stats-topic: one line per Ingress per tick
    for (size_t i = 0; i < t->ingresses.size(); ++i) {
        const IngressInfo& info = t->ingresses[i];
        uint64_t total = info.counters->requests.load();
        uint64_t delta = total - info.counters->requests_at_last_tick;
        info.counters->requests_at_last_tick = total;
        if (info.stats_topic.empty()) continue;
        std::vector<uint32_t> streams;
        for (const auto& r : t->routes) {
            if (r.kind == BackendKind::EventStream && r.ingress == i) streams.push_back(r.stream_id);
        }
        char line[160];
        int l = snprintf(line, sizeof(line), "{\"t\":\"stats\",\"ingress\":\"%s\",\"rps\":%llu,\"viewers\":%zu}",
                         info.key.c_str(), static_cast<unsigned long long>(delta), hub_->count_streams(streams));
        if (l > 0 && static_cast<size_t>(l) < sizeof(line)) {
            ipc::ChannelManager::instance().publish("kube32/ingress", info.stats_topic, reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(l));
        }
    }

    // kube32.cluster streams: telemetry every 2 s while someone listens
    uint64_t now = osal::System::get_uptime_ms();
    static const uint32_t kClusterHash = topic_hash(kTopicClusterTelemetry);
    if (now - last_telemetry_ms_ >= 2000 && hub_->has_listeners(kClusterHash)) {
        last_telemetry_ms_ = now;
        // Telemetry is best effort: an allocation failure skips one update
        // instead of aborting the node
        try {
            hub_->broadcast(kClusterHash, "cluster", build_cluster_json());
        } catch (const std::exception& e) {
            std::cout << "[INGRESS] telemetry skipped: " << e.what() << std::endl;
        }
    }
}

// ---------------------------------------------------------------------------
// Request dispatch
// ---------------------------------------------------------------------------
bool IngressController::handle(api::HttpConnection& conn) {
    const api::HttpRequest& req = conn.request();
    std::string_view path = request_path(req);
    std::shared_ptr<const RouteTable> table = current_routes();
    const IngressRoute* route = table ? table->match(req.host, path) : nullptr;
    if (!route) {
        stats_.not_found.fetch_add(1, std::memory_order_relaxed);
        return conn.send_simple(404, "application/json", "{\"error\":\"not_found\",\"message\":\"no Ingress rule matches this host and path\"}");
    }
    table->ingresses[route->ingress].counters->requests.fetch_add(1, std::memory_order_relaxed);
    switch (route->kind) {
        case BackendKind::Service:
            return serve_service(conn, *route);
        case BackendKind::StaticBundle:
            return serve_bundle(conn, *route, path);
        case BackendKind::EventStream:
            return serve_stream(conn, *route);
        case BackendKind::Telemetry:
            if (req.method != api::HttpMethod::Get && req.method != api::HttpMethod::Head) {
                return conn.send_simple(405, "application/json", "{\"error\":\"method_not_allowed\"}");
            }
            return conn.send_simple(200, "application/json", build_cluster_json(), "Cache-Control: no-store\r\n");
    }
    return conn.send_simple(500, "application/json", "{\"error\":\"internal\"}");
}

bool IngressController::serve_bundle(api::HttpConnection& conn, const IngressRoute& route, std::string_view path) {
    const api::HttpRequest& req = conn.request();
    if (req.method != api::HttpMethod::Get && req.method != api::HttpMethod::Head) {
        return conn.send_simple(405, "application/json", "{\"error\":\"method_not_allowed\"}");
    }
    std::string file = bundle_relative_path(route, path);
    if (file.back() == '/') file += "index.html";
    size_t count = 0;
    const embedded::EmbeddedAsset* assets = embedded::assets(count);
    const embedded::EmbeddedAsset* match = nullptr;
    const embedded::EmbeddedAsset* index = nullptr;
    // A file stored both plain and gzip-compressed: the variant the client takes
    auto better = [&](const embedded::EmbeddedAsset* have, const embedded::EmbeddedAsset* cand) {
        return !have || (cand->gzip == req.accepts_gzip && have->gzip != req.accepts_gzip);
    };
    for (size_t i = 0; i < count; ++i) {
        if (route.bundle != assets[i].bundle) continue;
        if (file == assets[i].path) { if (better(match, &assets[i])) match = &assets[i]; continue; }
        if (std::strcmp(assets[i].path, "/index.html") == 0 && better(index, &assets[i])) index = &assets[i];
    }
    if (!match) {
        // Single-page app fallback: client-side routes have no file extension
        std::string_view last = std::string_view(file).substr(file.rfind('/') + 1);
        if (last.find('.') == std::string_view::npos) match = index;
    }
    if (!match) {
        return conn.send_simple(404, "application/json", "{\"error\":\"not_found\",\"message\":\"no such file in bundle " + route.bundle + "\"}");
    }
    stats_.static_hits.fetch_add(1, std::memory_order_relaxed);
    if (match->gzip && !req.accepts_gzip) {
        // Files are stored compressed and cannot be inflated here; browsers
        // accept gzip, curl needs --compressed
        return conn.send_simple(406, "text/plain", "file is stored gzip-compressed; send Accept-Encoding: gzip\n");
    }
    char etag[24];
    snprintf(etag, sizeof(etag), "%zx-%x", match->len, static_cast<unsigned>(reinterpret_cast<uintptr_t>(match->data) & 0xffff));
    // index.html is revalidated on every load; other files are cached for an
    // hour (bundle file names carry no content hash)
    const char* cache = (std::strcmp(match->path, "/index.html") == 0) ? "no-cache" : "public, max-age=3600";
    return conn.send_blob(match->content_type, match->data, match->len, match->gzip, etag, cache);
}

bool IngressController::serve_stream(api::HttpConnection& conn, const IngressRoute& route) {
    const api::HttpRequest& req = conn.request();
    if (req.method != api::HttpMethod::Get) return conn.send_simple(405, "text/plain", "GET only\n");
    SseHub::Subscription sub;
    sub.stream_id = route.stream_id;
    for (const auto& t : route.topics) {
        if (t == kTopicPresence) sub.presence = true;
        else sub.topics.push_back(topic_hash(t));
    }
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
    hub_->add(conn.take_socket(), std::move(sub));
    return true;
}

bool IngressController::serve_service(api::HttpConnection& conn, const IngressRoute& route) {
    if (route.endpoints.empty()) {
        stats_.proxy_errors.fetch_add(1, std::memory_order_relaxed);
        std::string body = "{\"error\":\"unavailable\",\"message\":\"" + route.unavailable + "\"}";
        return conn.send_simple(503, "application/json", body, "Retry-After: 2\r\n");
    }
    // A pod on this node is called directly; otherwise round robin over the others
    const IngressEndpoint* ep = nullptr;
    for (const auto& e : route.endpoints) {
        if (runtime::AppPodHost::instance().pod_handles_requests(e.pod)) { ep = &e; break; }
    }
    if (!ep) {
        const IngressEndpoint& remote = route.endpoints[rr_.fetch_add(1, std::memory_order_relaxed) % route.endpoints.size()];
        if (remote.ip.empty()) {
            stats_.proxy_errors.fetch_add(1, std::memory_order_relaxed);
            return conn.send_simple(503, "application/json", "{\"error\":\"unavailable\",\"message\":\"pod " + remote.pod + " has no IP yet\"}", "Retry-After: 2\r\n");
        }
        return proxy_upstream(conn, remote);
    }

    stats_.local_calls.fetch_add(1, std::memory_order_relaxed);
    runtime::AppRequest areq = app_request_from(conn.request());
    HttpAppSink sink(conn);
    uint64_t t_call = osal::Thread::get_time_us();
    int32_t rc = runtime::AppPodHost::instance().handle(ep->pod, areq, sink);
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

// ---------------------------------------------------------------------------
// Proxy to a pod HTTP server on another node
// ---------------------------------------------------------------------------
IngressController::Upstream* IngressController::acquire_upstream(const std::string& ip, uint16_t port) {
    osal::LockGuard guard(pool_mutex_);
    for (auto& u : pool_) {
        if (!u->in_use && u->ip == ip && u->port == port) {
            u->in_use = true;
            return u.get();
        }
    }
    if (pool_.size() < opts_.upstream_pool_size) {
        auto u = std::make_unique<Upstream>();
        u->ip = ip;
        u->port = port;
        u->in_use = true;
        pool_.push_back(std::move(u));
        return pool_.back().get();
    }
    // Full: reuse an idle slot that belongs to another target
    for (auto& u : pool_) {
        if (!u->in_use) {
            u->sock.close();
            u->ip = ip;
            u->port = port;
            u->in_use = true;
            return u.get();
        }
    }
    return nullptr;
}

void IngressController::release_upstream(Upstream* u, bool reusable) {
    osal::LockGuard guard(pool_mutex_);
    if (!reusable) u->sock.close();
    u->in_use = false;
}

// Finds "\r\n\r\n" in a buffer; returns offset of body or npos
static size_t find_head_end(const char* buf, size_t len) {
    for (size_t i = 3; i < len; ++i) {
        if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n') return i + 1;
    }
    return std::string::npos;
}

bool IngressController::proxy_upstream(api::HttpConnection& conn, const IngressEndpoint& ep) {
    const api::HttpRequest& req = conn.request();
    char* buf = conn.scratch();
    const size_t cap = conn.scratch_capacity();
    const std::string& ip = ep.ip;
    const uint16_t port = ep.port;

    for (int attempt = 0; attempt < 2; ++attempt) {
        Upstream* up = acquire_upstream(ip, port);
        if (!up) {
            return conn.send_simple(503, "application/json", "{\"error\":\"busy\",\"message\":\"upstream pool exhausted\"}", "Retry-After: 1\r\n");
        }
        bool fresh = false;
        if (!up->sock.is_valid()) {
            up->sock = osal::Socket();
            if (!up->sock.create(osal::SocketType::Tcp) || !up->sock.connect(ip.c_str(), port, opts_.upstream_timeout_ms)) {
                release_upstream(up, false);
                stats_.proxy_errors.fetch_add(1, std::memory_order_relaxed);
                return conn.send_simple(502, "application/json", "{\"error\":\"bad_gateway\",\"message\":\"cannot reach the pod's node\"}");
            }
            up->sock.set_tcp_nodelay(true);
            fresh = true;
            if (attempt > 0) stats_.upstream_reconnects.fetch_add(1, std::memory_order_relaxed);
        }
        up->sock.set_timeout(opts_.upstream_timeout_ms, opts_.upstream_timeout_ms);

        // ---- Send request --------------------------------------------------
        int n = snprintf(buf, cap,
                         "%s %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "X-Kube32-Pod: %s\r\n"
                         "X-Forwarded-For: %s\r\n"
                         "Content-Length: %zu\r\n"
                         "Connection: keep-alive\r\n",
                         api::http_method_str(req.method), req.raw_uri.c_str(), ip.c_str(),
                         ep.pod.c_str(), req.client_ip.c_str(), req.body.size());
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
        add_header("Accept-Encoding", req.accepts_gzip ? std::string("gzip") : std::string());
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

// ---------------------------------------------------------------------------
// Cluster telemetry
// ---------------------------------------------------------------------------
std::string IngressController::build_cluster_json() {
    std::string out;
    out.reserve(1024);
    out += "{\"nodes\":[";
    bool first = true;
    if (opts_.store) {
        std::vector<std::string> nodes;
        opts_.store->list("/registry/nodes/", nodes);
        // Pods are listed and parsed once, not once per node
        std::vector<models::KubePod> all_pods;
        {
            std::vector<std::string> pod_jsons;
            opts_.store->list("/registry/pods/", pod_jsons);
            all_pods.reserve(pod_jsons.size());
            for (const auto& pj : pod_jsons) all_pods.push_back(models::parse_pod_manifest(pj));
        }
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
            if (name == opts_.local_node_name) {
                const size_t min_free = osal::System::get_min_free_heap_bytes();   // 0: not tracked (host)
                if (min_free > 0) out += ",\"min_free_heap\":" + std::to_string(min_free);
                out += ",\"uptime_ms\":" + std::to_string(osal::System::get_uptime_ms());
                out += ",\"free_heap_live\":" + std::to_string(osal::System::get_free_heap_bytes());
            }
            // Pods on this node
            out += ",\"pods\":[";
            bool pf = true;
            for (const auto& p : all_pods) {
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
        out += "\"requests\":" + std::to_string(reqs);
        out += ",\"errors\":" + std::to_string(m.errors.load());
        out += ",\"active\":" + std::to_string(m.active.load());
        out += ",\"p50_us\":" + std::to_string(p50) + ",\"p99_us\":" + std::to_string(p99);
        out += ",\"avg_us\":" + std::to_string(reqs ? m.total_us.load() / reqs : 0);
        out += ",\"workers\":" + std::to_string(server_->options().workers);
        out += ",\"uptime_ms\":" + std::to_string(osal::System::get_uptime_ms());
        out += ",";
    }
    out += "\"active_sse\":" + std::to_string(hub_->count());
    out += ",\"sse_events\":" + std::to_string(hub_->events_sent());
    out += ",\"proxied\":" + std::to_string(stats_.proxied.load());
    out += ",\"proxy_errors\":" + std::to_string(stats_.proxy_errors.load());
    out += ",\"local_calls\":" + std::to_string(stats_.local_calls.load());
    out += ",\"static_hits\":" + std::to_string(stats_.static_hits.load());
    out += ",\"not_found\":" + std::to_string(stats_.not_found.load());
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

} // namespace kube32::app
