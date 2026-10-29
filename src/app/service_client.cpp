#include "app/service_client.hpp"
#include "runtime/k32_app_abi.h"
#include "runtime/container_lifecycle.hpp"
#include "models/kube_models.hpp"
#include "storage/store.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <strings.h>

namespace kube32::app {

namespace {

bool chain_has(std::string_view chain, std::string_view pod) {
    size_t start = 0;
    while (start <= chain.size()) {
        size_t end = chain.find(',', start);
        if (end == std::string_view::npos) end = chain.size();
        if (chain.substr(start, end - start) == pod) return true;
        start = end + 1;
    }
    return false;
}

size_t chain_length(std::string_view chain) {
    if (chain.empty()) return 0;
    size_t n = 1;
    for (char c : chain) n += (c == ',');
    return n;
}

constexpr size_t kMaxChain = 8;   // pods in one call chain

// ---------------------------------------------------------------------------
// Resolvers
// ---------------------------------------------------------------------------
struct CachedTarget {
    bool ok{false};
    std::string why;
    std::vector<IngressEndpoint> endpoints;
    uint64_t at_ms{0};
};

class StoreResolver : public ServiceResolver {
public:
    explicit StoreResolver(storage::Store& store) : store_(store), dirty_(std::make_shared<std::atomic<bool>>(false)) {
        for (const char* prefix : {"/registry/services/", "/registry/pods/"}) {
            std::shared_ptr<std::atomic<bool>> dirty = dirty_;
            watches_.push_back(store_.watch(prefix, [dirty](const storage::WatchEvent&) { dirty->store(true); }));
        }
    }
    ~StoreResolver() override {
        for (uint32_t id : watches_) store_.unwatch(id);
    }
    bool resolve(const std::string& ns, const std::string& name, uint16_t port, std::vector<IngressEndpoint>& out, std::string& why) override {
        const std::string key = ns + "/" + name + ":" + std::to_string(port);
        {
            osal::LockGuard guard(mutex_);
            if (dirty_->exchange(false)) cache_.clear();
            auto it = cache_.find(key);
            if (it != cache_.end()) {
                out = it->second.endpoints;
                why = it->second.why;
                return it->second.ok;
            }
        }
        std::vector<std::string> services, pods;
        store_.list("/registry/services/" + ns + "/", services);
        store_.list("/registry/pods/" + ns + "/", pods);
        CachedTarget t;
        t.ok = resolve_service_endpoints(ns, name, port, services, pods, t.endpoints, t.why);
        out = t.endpoints;
        why = t.why;
        osal::LockGuard guard(mutex_);
        if (cache_.size() > 32) cache_.clear();
        cache_[key] = std::move(t);
        return out.size() > 0;
    }
private:
    storage::Store& store_;
    std::shared_ptr<std::atomic<bool>> dirty_;
    std::vector<uint32_t> watches_;
    osal::Mutex mutex_;
    std::map<std::string, CachedTarget> cache_;
};

class ApiResolver : public ServiceResolver {
public:
    ApiResolver(std::function<int(const std::string&, std::string&)> get, uint32_t ttl) : get_(std::move(get)), ttl_ms_(ttl) {}
    bool resolve(const std::string& ns, const std::string& name, uint16_t port, std::vector<IngressEndpoint>& out, std::string& why) override {
        const std::string key = ns + "/" + name + ":" + std::to_string(port);
        const uint64_t now = osal::System::get_uptime_ms();
        {
            osal::LockGuard guard(mutex_);
            auto it = cache_.find(key);
            if (it != cache_.end() && now - it->second.at_ms < ttl_ms_) {
                out = it->second.endpoints;
                why = it->second.why;
                return it->second.ok;
            }
        }
        CachedTarget t;
        t.at_ms = now;
        t.ok = fetch(ns, name, port, t.endpoints, t.why);
        out = t.endpoints;
        why = t.why;
        osal::LockGuard guard(mutex_);
        if (cache_.size() > 16) cache_.clear();
        cache_[key] = std::move(t);
        return out.size() > 0;
    }
private:
    // The Service names the port; its Endpoints carry the addresses and the
    // resolved target port under the same port name
    bool fetch(const std::string& ns, const std::string& name, uint16_t port, std::vector<IngressEndpoint>& out, std::string& why) {
        const std::string svc_key = ns + "/" + name;
        std::string body, err;
        int code = get_("/api/v1/namespaces/" + ns + "/services/" + name, body);
        if (code == 404) { why = "service " + svc_key + " not found"; return false; }
        if (code != 200) { why = "cannot read service " + svc_key + " (http " + std::to_string(code) + ")"; return false; }
        std::string port_name;
        bool found = false;
        size_t nports = 0;
        {
            models::JsonValue doc;
            if (!models::parse_json(body, doc, err)) { why = "service " + svc_key + ": " + err; return false; }
            const models::JsonValue* spec = doc.get("spec");
            const models::JsonValue* ports = spec ? spec->get("ports") : nullptr;
            if (ports && ports->is_array()) {
                nports = ports->arr_val.size();
                for (const auto& p : ports->arr_val) {
                    if (port == 0 || p.get_int("port", 0) == port) { port_name = p.get_string("name", ""); found = true; break; }
                }
            }
        }
        if (!found) { why = "service " + svc_key + " has no matching port"; return false; }
        code = get_("/api/v1/namespaces/" + ns + "/endpoints/" + name, body);
        if (code != 200) { why = "no endpoints for service " + svc_key + " (http " + std::to_string(code) + ")"; return false; }
        models::JsonValue doc;
        if (!models::parse_json(body, doc, err)) { why = "endpoints " + svc_key + ": " + err; return false; }
        const models::JsonValue* subsets = doc.get("subsets");
        if (subsets && subsets->is_array()) {
            for (const auto& sub : subsets->arr_val) {
                const models::JsonValue* ports = sub.get("ports");
                int64_t target = 0;
                if (ports && ports->is_array()) {
                    for (const auto& p : ports->arr_val) {
                        if (p.get_string("name", "") == port_name || (nports == 1 && ports->arr_val.size() == 1)) { target = p.get_int("port", 0); break; }
                    }
                }
                const models::JsonValue* addrs = sub.get("addresses");
                if (target <= 0 || target > 65535 || !addrs || !addrs->is_array()) continue;
                for (const auto& a : addrs->arr_val) {
                    IngressEndpoint ep;
                    ep.ip = a.get_string("ip", "");
                    if (const models::JsonValue* ref = a.get("targetRef")) {
                        ep.pod = ref->get_string("namespace", ns) + "/" + ref->get_string("name", "");
                    }
                    ep.port = static_cast<uint16_t>(target);
                    if (!ep.ip.empty()) out.push_back(std::move(ep));
                }
            }
        }
        if (out.empty()) why = "no ready pod selected by service " + svc_key;
        return !out.empty();
    }

    std::function<int(const std::string&, std::string&)> get_;
    uint32_t ttl_ms_;
    osal::Mutex mutex_;
    std::map<std::string, CachedTarget> cache_;
};

// Request headers a guest passes on: "Name: value\r\n" lines minus the ones
// the transport owns
bool header_allowed(std::string_view name) {
    static const char* const kOwned[] = {"host", "content-length", "connection", "transfer-encoding", "keep-alive",
                                         "upgrade", "te", "trailer", "proxy-connection"};
    if (name.size() >= 9 && strncasecmp(name.data(), "x-kube32-", 9) == 0) return false;
    for (const char* o : kOwned) {
        if (name.size() == std::strlen(o) && strncasecmp(name.data(), o, name.size()) == 0) return false;
    }
    return true;
}

template <typename Fn>
void each_header(std::string_view block, Fn&& fn) {
    while (!block.empty()) {
        size_t eol = block.find('\n');
        std::string_view line = block.substr(0, eol);
        block = (eol == std::string_view::npos) ? std::string_view() : block.substr(eol + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0 || line.find('\r') != std::string_view::npos) continue;
        std::string_view name = line.substr(0, colon);
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        fn(name, value);
    }
}

bool name_is(std::string_view a, const char* b) {
    return a.size() == std::strlen(b) && strncasecmp(a.data(), b, a.size()) == 0;
}

// Chunked transfer decoding, fed as bytes arrive
struct ChunkDecoder {
    enum State { Size, Data, DataEnd, Trailer, Done } state{Size};
    size_t remaining{0};
    size_t line{0};          // characters on the current size/trailer line
    bool ext{false};         // inside a chunk extension
    bool bad{false};
    void feed(const char* p, size_t n, runtime::BufferResponseSink& sink) {
        for (size_t i = 0; i < n && state != Done && !bad;) {
            char c = p[i];
            switch (state) {
                case Size:
                    ++i;
                    if (c == '\n') {
                        if (line == 0) { bad = true; break; }
                        state = remaining ? Data : Trailer;
                        line = 0;
                        ext = false;
                    } else if (c == '\r') {
                    } else if (c == ';' || ext) {
                        ext = true;
                    } else {
                        int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                        if (v < 0 || remaining > (SIZE_MAX >> 4)) { bad = true; break; }
                        remaining = (remaining << 4) | static_cast<size_t>(v);
                        ++line;
                    }
                    break;
                case Data: {
                    size_t take = (n - i < remaining) ? n - i : remaining;
                    sink.append(reinterpret_cast<const uint8_t*>(p + i), take);
                    i += take;
                    remaining -= take;
                    if (remaining == 0) state = DataEnd;
                    break;
                }
                case DataEnd:
                    ++i;
                    if (c == '\n') state = Size;
                    break;
                case Trailer:
                    ++i;
                    if (c == '\n') { if (line == 0) state = Done; line = 0; }
                    else if (c != '\r') ++line;
                    break;
                case Done:
                    break;
            }
        }
    }
};

} // namespace

std::unique_ptr<ServiceResolver> make_store_service_resolver(storage::Store& store) {
    return std::make_unique<StoreResolver>(store);
}

std::unique_ptr<ServiceResolver> make_api_service_resolver(std::function<int(const std::string&, std::string&)> api_get, uint32_t ttl_ms) {
    return std::make_unique<ApiResolver>(std::move(api_get), ttl_ms);
}

// ---------------------------------------------------------------------------
// ServiceClient
// ---------------------------------------------------------------------------
ServiceClient::ServiceClient(std::unique_ptr<ServiceResolver> resolver, ServiceClientOptions opts)
    : resolver_(std::move(resolver)), opts_(opts) {}

ServiceClient::~ServiceClient() {
    osal::LockGuard guard(pool_mutex_);
    for (auto& i : idle_) i.sock.close();
}

bool ServiceClient::parse_url(std::string_view url, std::string& service, std::string& ns, uint16_t& port, std::string& path) {
    if (url.rfind("http://", 0) != 0) return false;
    url.remove_prefix(7);
    size_t slash = url.find('/');
    std::string_view authority = url.substr(0, slash);
    path = (slash == std::string_view::npos) ? std::string("/") : std::string(url.substr(slash));
    port = 0;
    size_t colon = authority.find(':');
    if (colon != std::string_view::npos) {
        std::string_view digits = authority.substr(colon + 1);
        if (digits.empty() || digits.size() > 5) return false;
        unsigned v = 0;
        for (char c : digits) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<unsigned>(c - '0');
        }
        if (v == 0 || v > 65535) return false;
        port = static_cast<uint16_t>(v);
        authority = authority.substr(0, colon);
    }
    // <service>[.<namespace>[.svc[.cluster.local]]]
    for (std::string_view suffix : {std::string_view(".svc.cluster.local"), std::string_view(".svc")}) {
        if (authority.size() > suffix.size() && authority.compare(authority.size() - suffix.size(), suffix.size(), suffix) == 0) {
            authority.remove_suffix(suffix.size());
            break;
        }
    }
    size_t dot = authority.find('.');
    service = std::string(authority.substr(0, dot));
    ns = (dot == std::string_view::npos) ? std::string() : std::string(authority.substr(dot + 1));
    if (service.empty() || ns.find('.') != std::string::npos) return false;
    for (char c : service + ns) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return path.find_first_of(" \r\n") == std::string::npos;
}

int32_t ServiceClient::call(const runtime::ServiceCallRequest& req, runtime::BufferResponseSink& sink) {
    stats_.calls.fetch_add(1, std::memory_order_relaxed);
    std::string service, ns, path;
    uint16_t port = 0;
    if (!parse_url(req.url, service, ns, port, path)) {
        stats_.failed.fetch_add(1, std::memory_order_relaxed);
        return K32_ERR_INVAL;
    }
    if (ns.empty()) {
        size_t slash = req.caller_pod.find('/');
        ns = (slash == std::string_view::npos) ? std::string("default") : std::string(req.caller_pod.substr(0, slash));
    }
    if (chain_length(req.call_chain) >= kMaxChain) return K32_ERR_LOOP;

    std::vector<IngressEndpoint> eps;
    std::string why;
    if (!resolver_->resolve(ns, service, port, eps, why)) {
        stats_.failed.fetch_add(1, std::memory_order_relaxed);
        return K32_ERR_NOENT;
    }
    // A pod already serving this chain is never entered again; another
    // replica of the same Service may be
    std::vector<const IngressEndpoint*> candidates;
    for (const auto& e : eps) {
        if (!chain_has(req.call_chain, e.pod)) candidates.push_back(&e);
    }
    if (candidates.empty()) return K32_ERR_LOOP;

    runtime::AppPodHost& host = runtime::AppPodHost::instance();
    const IngressEndpoint* local = nullptr;
    for (const IngressEndpoint* e : candidates) {
        if (host.pod_handles_requests(e->pod)) { local = e; break; }
    }
    if (local) {
        stats_.local.fetch_add(1, std::memory_order_relaxed);
        runtime::AppRequest areq;
        areq.method = req.method;
        areq.path_and_query = path;
        areq.body = req.body;
        areq.call_chain = req.call_chain;
        each_header(req.headers, [&](std::string_view name, std::string_view value) {
            if (name_is(name, "Content-Type")) areq.content_type = value;
            else if (name_is(name, "Cookie")) areq.cookie = value;
            else if (name_is(name, "X-Request-Id")) areq.request_id = value;
            else if (name_is(name, "Accept-Encoding")) areq.accepts_gzip = value.find("gzip") != std::string_view::npos;
        });
        int32_t rc = host.call_nested(local->pod, areq, sink, req.timeout_ms);
        if (rc < 0) stats_.failed.fetch_add(1, std::memory_order_relaxed);
        return rc;
    }
    std::vector<const IngressEndpoint*> remote;
    for (const IngressEndpoint* e : candidates) {
        if (!e->ip.empty()) remote.push_back(e);
    }
    if (remote.empty()) {
        stats_.failed.fetch_add(1, std::memory_order_relaxed);
        return K32_ERR_NOENT;
    }
    const IngressEndpoint& ep = *remote[rr_.fetch_add(1, std::memory_order_relaxed) % remote.size()];
    stats_.remote.fetch_add(1, std::memory_order_relaxed);
    int32_t rc = call_remote(req, ep, service + "." + ns, path, sink);
    if (rc < 0) stats_.failed.fetch_add(1, std::memory_order_relaxed);
    return rc;
}

osal::Socket ServiceClient::take_idle(const std::string& ip, uint16_t port, bool& reused) {
    osal::LockGuard guard(pool_mutex_);
    for (size_t i = 0; i < idle_.size(); ++i) {
        if (idle_[i].ip == ip && idle_[i].port == port) {
            osal::Socket s = std::move(idle_[i].sock);
            idle_.erase(idle_.begin() + static_cast<long>(i));
            reused = true;
            return s;
        }
    }
    reused = false;
    return osal::Socket();
}

void ServiceClient::put_idle(osal::Socket sock, const std::string& ip, uint16_t port) {
    osal::LockGuard guard(pool_mutex_);
    if (idle_.size() >= opts_.pool_size && !idle_.empty()) {
        idle_.front().sock.close();
        idle_.erase(idle_.begin());
    }
    if (opts_.pool_size == 0) { sock.close(); return; }
    idle_.push_back(Idle{std::move(sock), ip, port});
}

int32_t ServiceClient::call_remote(const runtime::ServiceCallRequest& req, const IngressEndpoint& ep, const std::string& host,
                                   const std::string& path, runtime::BufferResponseSink& sink) {
    const uint64_t deadline = osal::System::get_uptime_ms() + req.timeout_ms;
    auto left_ms = [&]() -> uint32_t {
        uint64_t now = osal::System::get_uptime_ms();
        return now >= deadline ? 0 : static_cast<uint32_t>(deadline - now);
    };
    std::string head;
    head.reserve(192 + path.size() + req.headers.size() + req.call_chain.size());
    head.append(req.method.data(), req.method.size()).append(" ").append(path).append(" HTTP/1.1\r\nHost: ").append(host);
    head.append("\r\nX-Kube32-Pod: ").append(ep.pod);
    head.append("\r\nX-Kube32-Call-Chain: ").append(req.call_chain.data(), req.call_chain.size());
    head.append("\r\nContent-Length: ").append(std::to_string(req.body.size())).append("\r\nConnection: keep-alive\r\n");
    each_header(req.headers, [&](std::string_view name, std::string_view value) {
        if (!header_allowed(name)) return;
        head.append(name.data(), name.size()).append(": ").append(value.data(), value.size()).append("\r\n");
    });
    head.append("\r\n");

    constexpr size_t kBuf = 1024;
    std::unique_ptr<char[]> buf(new (std::nothrow) char[kBuf]);
    if (!buf) return K32_ERR_NOMEM;

    for (int attempt = 0; attempt < 2; ++attempt) {
        bool reused = false;
        osal::Socket sock = take_idle(ep.ip, ep.port, reused);
        if (!reused) {
            uint32_t ct = left_ms();
            if (ct == 0) return K32_ERR_TIMEOUT;
            if (!sock.create(osal::SocketType::Tcp) || !sock.connect(ep.ip.c_str(), ep.port, ct < opts_.connect_timeout_ms ? ct : opts_.connect_timeout_ms)) {
                return left_ms() == 0 ? K32_ERR_TIMEOUT : K32_ERR_IO;
            }
            sock.set_tcp_nodelay(true);
        } else {
            stats_.reused.fetch_add(1, std::memory_order_relaxed);
        }
        sock.set_timeout(250, left_ms() ? left_ms() : 1);
        if (!sock.send_all(head.data(), head.size()) || (!req.body.empty() && !sock.send_all(req.body.data(), req.body.size()))) {
            sock.close();
            if (reused) continue;   // the pooled connection had been closed by the node
            return K32_ERR_IO;
        }

        // Head, then the body as it arrives. recv waits at most 250 ms at a
        // time so a stopping caller and the deadline are noticed.
        size_t have = 0;
        size_t head_end = std::string::npos;
        int status = 0;
        long content_length = -1;
        bool chunked = false, server_close = false, http10 = false;
        ChunkDecoder chunks;
        size_t body_read = 0;
        bool done = false;
        int32_t fail = 0;
        while (!done) {
            if (left_ms() == 0) { fail = K32_ERR_TIMEOUT; break; }
            if (runtime::runtime_stop_requested(req.caller_runtime)) { fail = K32_ERR_IO; break; }
            char* dst = (head_end == std::string::npos) ? buf.get() + have : buf.get();
            size_t room = (head_end == std::string::npos) ? kBuf - 1 - have : kBuf;
            if (room == 0) { fail = K32_ERR_IO; break; }   // head over 1 KB
            uint32_t slice = left_ms();
            sock.set_timeout(slice < 250 ? (slice ? slice : 1) : 250, 1000);
            ssize_t r = sock.recv(dst, room);
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
            if (r <= 0) {
                if (head_end == std::string::npos && have == 0 && reused) { fail = 1; break; }   // stale pooled connection: retry
                // Without a length the body ends with the connection
                if (head_end != std::string::npos && !chunked && content_length < 0) { done = true; server_close = true; break; }
                fail = K32_ERR_IO;
                break;
            }
            const char* data = dst;
            size_t n = static_cast<size_t>(r);
            if (head_end == std::string::npos) {
                have += n;
                buf[have] = '\0';
                char* end = std::strstr(buf.get(), "\r\n\r\n");
                if (!end) continue;
                head_end = static_cast<size_t>(end - buf.get()) + 4;
                // Status line and the headers that frame the body
                std::string_view hv(buf.get(), head_end);
                size_t sp = hv.find(' ');
                if (hv.rfind("HTTP/1.", 0) != 0 || sp == std::string_view::npos) { fail = K32_ERR_IO; break; }
                http10 = hv.rfind("HTTP/1.0", 0) == 0;
                status = std::atoi(buf.get() + sp + 1);
                if (status < 100 || status > 599) { fail = K32_ERR_IO; break; }
                size_t line_end = hv.find("\r\n");
                each_header(hv.substr(line_end + 2), [&](std::string_view name, std::string_view value) {
                    if (name_is(name, "Content-Length")) content_length = std::strtol(std::string(value).c_str(), nullptr, 10);
                    else if (name_is(name, "Transfer-Encoding")) chunked = value.find("chunked") != std::string_view::npos;
                    else if (name_is(name, "Connection")) server_close = (value.size() >= 5 && strncasecmp(value.data(), "close", 5) == 0);
                });
                sink.begin(status, std::string_view());
                data = buf.get() + head_end;
                n = have - head_end;
                if ((!chunked && content_length == 0) || status == 204 || status == 304 || req.method == "HEAD") done = true;
            }
            if (n > 0 && !done) {
                if (chunked) {
                    chunks.feed(data, n, sink);
                    if (chunks.bad) { fail = K32_ERR_IO; break; }
                    done = chunks.state == ChunkDecoder::Done;
                } else {
                    size_t take = n;
                    if (content_length >= 0 && body_read + take > static_cast<size_t>(content_length)) take = static_cast<size_t>(content_length) - body_read;
                    sink.append(reinterpret_cast<const uint8_t*>(data), take);
                    body_read += take;
                    done = content_length >= 0 && body_read >= static_cast<size_t>(content_length);
                }
            }
        }
        if (fail == 1) { sock.close(); continue; }
        if (fail != 0) { sock.close(); return fail; }
        sink.end();
        if (!server_close && !http10 && !sink.overflow()) put_idle(std::move(sock), ep.ip, ep.port);
        else sock.close();
        return status;
    }
    return K32_ERR_IO;
}

void install_service_client(ServiceClient* client) {
    runtime::AppPlatformHooks hooks = runtime::app_platform_hooks();
    if (client) {
        hooks.service_call = [client](const runtime::ServiceCallRequest& req, runtime::BufferResponseSink& sink) {
            return client->call(req, sink);
        };
    } else {
        hooks.service_call = nullptr;
    }
    runtime::set_app_platform_hooks(hooks);
}

} // namespace kube32::app
