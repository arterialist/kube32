#include "app/service_client.hpp"
#include "runtime/k32_app_abi.h"
#include "runtime/container_lifecycle.hpp"
#include "models/kube_models.hpp"
#include "storage/store.hpp"
#include "osal/condition_variable.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"
#include "osal/tls.hpp"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <new>
#include <strings.h>

namespace kube32::app {

// Where an answer goes as it is read: the caller's sink, the body counted
// against the caller's limit
struct ResponseBody {
    runtime::AppResponseSink& sink;
    size_t limit;
    size_t total{0};
    void begin(int status) { sink.begin(status, std::string_view()); }
    void append(const char* data, size_t n) {
        total += n;
        sink.write(reinterpret_cast<const uint8_t*>(data), n);
    }
    bool overflow() const { return total > limit; }
    void end() { sink.end(); }
};

// Threads that run https calls: at most `size` at once, each started when it
// is first needed and kept
class TlsSessions {
public:
    TlsSessions(size_t size, size_t stack_bytes, bool stack_in_psram)
        : size_(size ? size : 1), stack_bytes_(stack_bytes), stack_in_psram_(stack_in_psram) {}
    ~TlsSessions() {
        osal::UniqueLock<osal::Mutex> lock(mutex_);
        stopping_ = true;
        work_cv_.notify_all();
        idle_cv_.wait(lock, [this] { return threads_ == 0; });
    }
    // False when `size` calls are already running, or no thread is free and
    // none can be started
    bool start(std::function<void()> job) {
        osal::LockGuard guard(mutex_);
        if (stopping_ || jobs_.size() + running_ >= size_) return false;
        if (jobs_.size() + running_ >= threads_) {
            osal::Thread::Config cfg;
            cfg.name = "k32_tls";
            cfg.stack_size = stack_bytes_;
            cfg.stack_in_psram = stack_in_psram_;
            cfg.priority = 4;
            if (!osal::Thread::create(&TlsSessions::run, this, cfg)) return false;
            ++threads_;
        }
        jobs_.push_back(std::move(job));
        work_cv_.notify_one();
        return true;
    }

private:
    static void run(void* self) { static_cast<TlsSessions*>(self)->loop(); }
    void loop() {
        osal::UniqueLock<osal::Mutex> lock(mutex_);
        for (;;) {
            work_cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty()) break;
            std::function<void()> job = std::move(jobs_.front());
            jobs_.pop_front();
            ++running_;
            lock.unlock();
            try {
                job();
            } catch (const std::exception&) {
                // the job reports its own failures; nothing may leave the thread
            }
            job = nullptr;
            lock.lock();
            --running_;
        }
        --threads_;
        idle_cv_.notify_all();
    }

    const size_t size_;
    const size_t stack_bytes_;
    const bool stack_in_psram_;
    osal::Mutex mutex_;
    osal::ConditionVariable work_cv_;
    osal::ConditionVariable idle_cv_;
    std::deque<std::function<void()>> jobs_;
    size_t threads_{0};
    size_t running_{0};
    bool stopping_{false};
};

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
    bool no_service{false};
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
    bool resolve(const std::string& ns, const std::string& name, uint16_t port, std::vector<IngressEndpoint>& out, std::string& why,
                 bool* no_service) override {
        const std::string key = ns + "/" + name + ":" + std::to_string(port);
        {
            osal::LockGuard guard(mutex_);
            if (dirty_->exchange(false)) cache_.clear();
            auto it = cache_.find(key);
            if (it != cache_.end()) {
                out = it->second.endpoints;
                why = it->second.why;
                if (no_service) *no_service = it->second.no_service;
                return it->second.ok;
            }
        }
        std::vector<std::string> services, pods;
        store_.list("/registry/services/" + ns + "/", services);
        store_.list("/registry/pods/" + ns + "/", pods);
        CachedTarget t;
        t.ok = resolve_service_endpoints(ns, name, port, services, pods, t.endpoints, t.why);
        if (!t.ok) {
            std::string unused;
            t.no_service = !store_.get("/registry/services/" + ns + "/" + name, unused).ok();
        }
        out = t.endpoints;
        why = t.why;
        if (no_service) *no_service = t.no_service;
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
    bool resolve(const std::string& ns, const std::string& name, uint16_t port, std::vector<IngressEndpoint>& out, std::string& why,
                 bool* no_service) override {
        const std::string key = ns + "/" + name + ":" + std::to_string(port);
        const uint64_t now = osal::System::get_uptime_ms();
        {
            osal::LockGuard guard(mutex_);
            auto it = cache_.find(key);
            if (it != cache_.end() && now - it->second.at_ms < ttl_ms_) {
                out = it->second.endpoints;
                why = it->second.why;
                if (no_service) *no_service = it->second.no_service;
                return it->second.ok;
            }
        }
        CachedTarget t;
        t.at_ms = now;
        t.ok = fetch(ns, name, port, t.endpoints, t.why, t.no_service);
        out = t.endpoints;
        why = t.why;
        if (no_service) *no_service = t.no_service;
        osal::LockGuard guard(mutex_);
        if (cache_.size() > 16) cache_.clear();
        cache_[key] = std::move(t);
        return out.size() > 0;
    }
private:
    // The Service names the port; its Endpoints carry the addresses and the
    // resolved target port under the same port name
    bool fetch(const std::string& ns, const std::string& name, uint16_t port, std::vector<IngressEndpoint>& out, std::string& why,
               bool& no_service) {
        const std::string svc_key = ns + "/" + name;
        std::string body, err;
        int code = get_("/api/v1/namespaces/" + ns + "/services/" + name, body);
        if (code == 404) { why = "service " + svc_key + " not found"; no_service = true; return false; }
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
// the transport owns (the body is always sent with the request: no Expect)
bool header_allowed(std::string_view name) {
    static const char* const kOwned[] = {"host", "content-length", "connection", "transfer-encoding", "keep-alive",
                                         "upgrade", "te", "trailer", "proxy-connection", "expect"};
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
    void feed(const char* p, size_t n, ResponseBody& out) {
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
                    out.append(p + i, take);
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

constexpr size_t kRecvBuf = 1024;
constexpr int32_t kStale = 1;   // a pooled connection the node had closed: try another

// The egress gateway's answer to a forwarded call that it did not relay from
// the host: no X-Kube32-Egress-Result header, so it comes from the API
// server itself (authentication, or a control plane without a gateway)
int32_t gateway_refusal(int status) {
    if (status == 401 || status == 403) return K32_ERR_PERM;
    if (status == 404 || status == 405 || status == 501) return K32_ERR_NOTSUP;
    return K32_ERR_IO;
}

// The calling pod is being stopped, or nobody waits for the answer any more
bool call_stopped(const runtime::ServiceCallRequest& req) {
    return runtime::runtime_stop_requested(req.caller_runtime) || (req.abandoned && req.abandoned());
}

// Reads one HTTP/1.x response from `sock` into `out`: the head, which must
// fit `buf` (`buf_size` bytes), then the body as it arrives. recv waits at
// most 250 ms at a time, so a stopping caller and the deadline are noticed.
// With `stop_when_full` reading ends as soon as the body no longer fits.
// Returns the status or a K32_ERR_*, or kStale when `reused` and the
// connection had been closed; `reusable` tells whether another request may
// follow on the connection. With `from_gateway` the answer is the egress
// gateway's: its X-Kube32-Egress-Result header says whether the body that
// follows is the host's.
int32_t read_response(osal::Socket& sock, const runtime::ServiceCallRequest& req, ResponseBody& out, uint64_t deadline,
                      bool reused, bool stop_when_full, char* buf, size_t buf_size, bool& reusable, bool from_gateway = false) {
    auto left_ms = [&]() -> uint32_t {
        uint64_t now = osal::System::get_uptime_ms();
        return now >= deadline ? 0 : static_cast<uint32_t>(deadline - now);
    };
    reusable = false;
    size_t have = 0;
    size_t head_end = std::string::npos;
    int status = 0;
    long content_length = -1;
    bool chunked = false, server_close = false, http10 = false, truncated = false;
    ChunkDecoder chunks;
    size_t body_read = 0;
    bool done = false;
    while (!done) {
        if (left_ms() == 0) return K32_ERR_TIMEOUT;
        if (call_stopped(req)) return K32_ERR_IO;
        char* dst = (head_end == std::string::npos) ? buf + have : buf;
        size_t room = (head_end == std::string::npos) ? buf_size - 1 - have : buf_size;
        if (room == 0) return K32_ERR_IO;   // the head does not fit
        uint32_t slice = left_ms();
        sock.set_timeout(slice < 250 ? (slice ? slice : 1) : 250, 1000);
        ssize_t r = sock.recv(dst, room);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (r <= 0) {
            if (head_end == std::string::npos && have == 0 && reused) return kStale;
            // Without a length the body ends with the connection: closed,
            // not reset
            if (r == 0 && head_end != std::string::npos && !chunked && content_length < 0) { server_close = true; break; }
            return K32_ERR_IO;
        }
        const char* data = dst;
        size_t n = static_cast<size_t>(r);
        if (head_end == std::string::npos) {
            have += n;
            buf[have] = '\0';
            // Interim answers (100 Continue, 103 Early Hints) come before the
            // final one, which may already be in the buffer
            char* end;
            while ((end = std::strstr(buf, "\r\n\r\n")) != nullptr) {
                const size_t len = static_cast<size_t>(end - buf) + 4;
                const char* sp = std::strchr(buf, ' ');
                const int interim = (std::strncmp(buf, "HTTP/1.", 7) == 0 && sp && sp < end) ? std::atoi(sp + 1) : 0;
                if (interim < 100 || interim > 199 || interim == 101) { head_end = len; break; }
                have -= len;
                std::memmove(buf, buf + len, have);
                buf[have] = '\0';
            }
            if (head_end == std::string::npos) continue;
            // Status line and the headers that frame the body
            std::string_view hv(buf, head_end);
            size_t sp = hv.find(' ');
            if (hv.rfind("HTTP/1.", 0) != 0 || sp == std::string_view::npos) return K32_ERR_IO;
            http10 = hv.rfind("HTTP/1.0", 0) == 0;
            status = std::atoi(buf + sp + 1);
            if (status < 100 || status > 599) return K32_ERR_IO;
            size_t line_end = hv.find("\r\n");
            long egress_result = LONG_MIN;
            each_header(hv.substr(line_end + 2), [&](std::string_view name, std::string_view value) {
                if (name_is(name, "Content-Length")) content_length = std::strtol(std::string(value).c_str(), nullptr, 10);
                else if (name_is(name, "Transfer-Encoding")) chunked = value.find("chunked") != std::string_view::npos;
                else if (name_is(name, "Connection")) server_close = (value.size() >= 5 && strncasecmp(value.data(), "close", 5) == 0);
                else if (name_is(name, "X-Kube32-Egress-Result")) egress_result = std::strtol(std::string(value).c_str(), nullptr, 10);
            });
            if (from_gateway) {
                if (egress_result == LONG_MIN) return gateway_refusal(status);
                if (egress_result < 0) return egress_result < -4095 ? K32_ERR_IO : static_cast<int32_t>(egress_result);
            }
            out.begin(status);
            data = buf + head_end;
            n = have - head_end;
            if ((!chunked && content_length == 0) || status == 204 || status == 304 || req.method == "HEAD") done = true;
        }
        if (n > 0 && !done) {
            if (chunked) {
                chunks.feed(data, n, out);
                if (chunks.bad) return K32_ERR_IO;
                done = chunks.state == ChunkDecoder::Done;
            } else {
                size_t take = n;
                if (content_length >= 0 && body_read + take > static_cast<size_t>(content_length)) take = static_cast<size_t>(content_length) - body_read;
                out.append(data, take);
                body_read += take;
                done = content_length >= 0 && body_read >= static_cast<size_t>(content_length);
            }
        }
        // The rest would only be counted: the connection goes, the caller
        // learns K32_ERR_TOOBIG with what fits
        if (!done && stop_when_full && out.overflow()) truncated = done = true;
    }
    out.end();
    reusable = !server_close && !http10 && !truncated && !out.overflow();
    return status;
}

// A DNS-1123 label, as Service and namespace names are
bool is_label(std::string_view s) {
    if (s.empty() || s.size() > 63 || s.front() == '-' || s.back() == '-') return false;
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

int32_t connect_error(int err) {
    switch (err) {
        case ECONNREFUSED:
        case ECONNRESET:      // lwIP reports a refused connect as reset
            return K32_ERR_CONNREFUSED;
        case EHOSTUNREACH:
        case ENETUNREACH:
#ifdef EHOSTDOWN
        case EHOSTDOWN:
#endif
#ifdef ENETDOWN
        case ENETDOWN:
#endif
            return K32_ERR_HOSTUNREACH;
        case ETIMEDOUT:
        case ECONNABORTED:    // lwIP gave up on the handshake
            return K32_ERR_TIMEOUT;
        default:
            return K32_ERR_IO;
    }
}

// A method token (RFC 9110 tchar): it goes into the request line as it is
bool is_method(std::string_view m) {
    if (m.empty() || m.size() > 16) return false;
    for (char c : m) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c && std::strchr("!#$%&'*+-.^_`|~", c)))) return false;
    }
    return true;
}

std::string_view namespace_of(std::string_view pod) {
    size_t slash = pod.find('/');
    return slash == std::string_view::npos ? std::string_view("default") : pod.substr(0, slash);
}

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
ServiceClient::ServiceClient(std::unique_ptr<ServiceResolver> resolver, ServiceClientOptions opts, std::unique_ptr<EgressPolicySource> egress)
    : resolver_(std::move(resolver)), opts_(std::move(opts)), egress_(std::move(egress)) {}

ServiceClient::~ServiceClient() {
    {
        osal::LockGuard guard(sessions_mutex_);
        sessions_.reset();   // waits for the calls running on them
    }
    osal::LockGuard guard(pool_mutex_);
    for (auto& i : idle_) i.sock.close();
}

bool ServiceClient::parse_target(std::string_view url, Target& out) {
    out = Target();
    if (url.rfind("https://", 0) == 0) {
        out.https = true;
        url.remove_prefix(8);
    } else if (url.rfind("http://", 0) == 0) {
        url.remove_prefix(7);
    } else {
        return false;
    }
    const size_t end = url.find_first_of("/?#");
    std::string_view authority = url.substr(0, end);
    std::string_view rest = (end == std::string_view::npos) ? std::string_view() : url.substr(end);
    rest = rest.substr(0, rest.find('#'));
    out.path = (rest.empty() || rest.front() != '/') ? "/" + std::string(rest) : std::string(rest);
    if (out.path.find_first_of(" \r\n") != std::string::npos) return false;
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
        out.port = static_cast<uint16_t>(v);
        authority = authority.substr(0, colon);
    }
    // Host names and IPv4 addresses only (no user info, no IPv6 literals)
    if (authority.empty() || authority.size() > 253) return false;
    for (char c : authority) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    }
    out.host = std::string(authority);
    if (net::parse_ipv4(authority, out.ip)) {
        if (out.ip == 0) return false;
        out.external = true;
        return true;
    }
    // Pods serve plain HTTP: an https:// host is outside the cluster
    if (out.https) {
        out.external = true;
        return true;
    }
    // <service>[.<namespace>[.svc[.cluster.local]]]
    std::string_view name = authority;
    bool cluster_suffix = false;
    for (std::string_view suffix : {std::string_view(".svc.cluster.local"), std::string_view(".svc")}) {
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            name.remove_suffix(suffix.size());
            cluster_suffix = true;
            break;
        }
    }
    const size_t dot = name.find('.');
    const std::string_view first = name.substr(0, dot);
    const std::string_view second = (dot == std::string_view::npos) ? std::string_view() : name.substr(dot + 1);
    if (dot == std::string_view::npos && is_label(first)) {
        out.service = std::string(first);
        return true;
    }
    if (dot != std::string_view::npos && second.find('.') == std::string_view::npos && is_label(first) && is_label(second)) {
        out.service = std::string(first);
        out.ns = std::string(second);
        out.or_external = !cluster_suffix;
        return true;
    }
    out.external = true;
    return true;
}

int32_t ServiceClient::call(const runtime::ServiceCallRequest& req, runtime::BufferResponseSink& sink) {
    stats_.calls.fetch_add(1, std::memory_order_relaxed);
    ResponseBody out{sink, sink.capacity()};
    Target t;
    if (!is_method(req.method) || !parse_target(req.url, t)) {
        stats_.failed.fetch_add(1, std::memory_order_relaxed);
        return K32_ERR_INVAL;
    }
    if (t.https) return call_https(req, t, out);
    if (t.external) return call_external(req, t, out);

    const std::string ns = t.ns.empty() ? std::string(namespace_of(req.caller_pod)) : t.ns;
    if (chain_length(req.call_chain) >= kMaxChain) return K32_ERR_LOOP;
    std::vector<IngressEndpoint> eps;
    std::string why;
    bool no_service = false;
    if (!resolver_->resolve(ns, t.service, t.port, eps, why, &no_service)) {
        // <name>.<name> that is no Service: the search path ends with the name itself
        if (t.or_external && no_service) return call_external(req, t, out);
        stats_.failed.fetch_add(1, std::memory_order_relaxed);
        return K32_ERR_NOENT;
    }
    return call_service(req, t, ns, eps, out);
}

bool ServiceClient::start_tls_session(std::function<void()> job) {
    if (!opts_.tls) return false;
    osal::LockGuard guard(sessions_mutex_);
    if (!sessions_) sessions_ = std::make_unique<TlsSessions>(opts_.tls_sessions, opts_.tls_stack_bytes, opts_.tls_stack_in_psram);
    if (sessions_->start(std::move(job))) return true;
    stats_.busy.fetch_add(1, std::memory_order_relaxed);
    return false;
}

int32_t ServiceClient::call_https_here(const runtime::ServiceCallRequest& req, runtime::AppResponseSink& sink, size_t limit) {
    stats_.calls.fetch_add(1, std::memory_order_relaxed);
    ResponseBody out{sink, limit};
    Target t;
    if (!is_method(req.method) || !parse_target(req.url, t) || !t.https) {
        stats_.failed.fetch_add(1, std::memory_order_relaxed);
        return K32_ERR_INVAL;
    }
    return call_external(req, t, out);
}

int32_t ServiceClient::call_https(const runtime::ServiceCallRequest& req, const Target& t, ResponseBody& out) {
    if (!opts_.tls) {
        if (!opts_.gateway_host.empty()) return call_gateway(req, t, out);
        stats_.failed.fetch_add(1, std::memory_order_relaxed);
        return K32_ERR_NOTSUP;
    }
    // On a TLS session thread; this one waits for it. The call ends at its
    // deadline or when the pod stops, so the wait does too.
    struct Done {
        osal::Mutex mutex;
        osal::ConditionVariable cv;
        bool finished{false};
        int32_t rc{K32_ERR_IO};
    };
    auto done = std::make_shared<Done>();
    const bool started = start_tls_session([this, &req, &t, &out, done]() {
        int32_t rc;
        try {
            rc = call_external(req, t, out);
        } catch (const std::bad_alloc&) {
            rc = K32_ERR_NOMEM;
        } catch (const std::exception&) {
            rc = K32_ERR_IO;
        }
        osal::LockGuard guard(done->mutex);
        done->rc = rc;
        done->finished = true;
        done->cv.notify_all();
    });
    if (!started) {
        stats_.failed.fetch_add(1, std::memory_order_relaxed);
        log_tls(req.caller_pod, t.host + ":" + std::to_string(t.port ? t.port : 443), "every TLS session is in use");
        return K32_ERR_BUSY;
    }
    osal::UniqueLock<osal::Mutex> lock(done->mutex);
    done->cv.wait(lock, [&done] { return done->finished; });
    return done->rc;
}

bool ServiceClient::egress_scope(const runtime::ServiceCallRequest& req, EgressScope& out, const std::string& target) {
    std::string why;
    if (!egress_ || egress_->scope(req.caller_pod, out, why)) return true;
    log_denied(req.caller_pod, target, why);
    stats_.denied.fetch_add(1, std::memory_order_relaxed);
    stats_.failed.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// A denial is logged once a second at most: a pod retrying in a loop does
// not flood the console
void ServiceClient::log_denied(std::string_view caller, const std::string& target, const std::string& why) {
    const uint64_t now = osal::System::get_uptime_ms() + 1;
    uint64_t last = last_denial_log_ms_.load();
    if (last != 0 && now - last < 1000) return;
    if (!last_denial_log_ms_.compare_exchange_strong(last, now)) return;
    printf("[NETPOL] %.*s -> %s denied: %s\n", static_cast<int>(caller.size()), caller.data(), target.c_str(), why.c_str());
}

// TLS failures, at most once a second as well
void ServiceClient::log_tls(std::string_view caller, const std::string& target, const std::string& what) {
    const uint64_t now = osal::System::get_uptime_ms() + 1;
    uint64_t last = last_tls_log_ms_.load();
    if (last != 0 && now - last < 1000) return;
    if (!last_tls_log_ms_.compare_exchange_strong(last, now)) return;
    printf("[EGRESS] %.*s -> https://%s: %s\n", static_cast<int>(caller.size()), caller.data(), target.c_str(), what.c_str());
}

int32_t ServiceClient::call_service(const runtime::ServiceCallRequest& req, const Target& t, const std::string& ns,
                                    std::vector<IngressEndpoint>& eps, ResponseBody& out) {
    // A pod already serving this chain is never entered again; another
    // replica of the same Service may be
    std::vector<const IngressEndpoint*> candidates;
    for (const auto& e : eps) {
        if (!chain_has(req.call_chain, e.pod)) candidates.push_back(&e);
    }
    if (candidates.empty()) return K32_ERR_LOOP;

    // NetworkPolicy, per backing pod: its address, its target port, and for
    // selectors and named ports the pod itself
    if (egress_) {
        EgressScope scope;
        const std::string service = "service " + ns + "/" + t.service;
        if (!egress_scope(req, scope, service)) return K32_ERR_PERM;
        if (!scope.selecting.empty()) {
            const std::string_view caller_ns = namespace_of(req.caller_pod);
            std::vector<const IngressEndpoint*> allowed;
            for (const IngressEndpoint* e : candidates) {
                net::EgressTarget target;
                net::parse_ipv4(e->ip, target.ip);
                target.port = e->port;
                target.pod_namespace = namespace_of(e->pod);
                net::PeerPod pod;
                auto peer = [&]() -> const net::PeerPod* { return egress_->peer(e->pod, pod) ? &pod : nullptr; };
                if (net::egress_allowed(scope.selecting, caller_ns, target, peer)) allowed.push_back(e);
            }
            if (allowed.empty()) {
                log_denied(req.caller_pod, service, "no NetworkPolicy egress rule allows its pods");
                stats_.denied.fetch_add(1, std::memory_order_relaxed);
                stats_.failed.fetch_add(1, std::memory_order_relaxed);
                return K32_ERR_PERM;
            }
            candidates.swap(allowed);
        }
    }

    runtime::AppPodHost& host = runtime::AppPodHost::instance();
    const IngressEndpoint* local = nullptr;
    for (const IngressEndpoint* e : candidates) {
        if (host.pod_handles_requests(e->pod)) { local = e; break; }
    }
    if (local) {
        stats_.local.fetch_add(1, std::memory_order_relaxed);
        runtime::AppRequest areq;
        areq.method = req.method;
        areq.path_and_query = t.path;
        areq.body = req.body;
        areq.call_chain = req.call_chain;
        each_header(req.headers, [&](std::string_view name, std::string_view value) {
            if (name_is(name, "Content-Type")) areq.content_type = value;
            else if (name_is(name, "Cookie")) areq.cookie = value;
            else if (name_is(name, "X-Request-Id")) areq.request_id = value;
            else if (name_is(name, "Accept-Encoding")) areq.accepts_gzip = value.find("gzip") != std::string_view::npos;
        });
        int32_t rc = host.call_nested(local->pod, areq, out.sink, req.timeout_ms);
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
    int32_t rc = call_remote(req, ep, t.service + "." + ns, t.path, out);
    if (rc < 0) stats_.failed.fetch_add(1, std::memory_order_relaxed);
    return rc;
}

int32_t ServiceClient::call_external(const runtime::ServiceCallRequest& req, const Target& t, ResponseBody& out) {
    stats_.external.fetch_add(1, std::memory_order_relaxed);
    const uint64_t deadline = osal::System::get_uptime_ms() + req.timeout_ms;
    auto left_ms = [&]() -> uint32_t {
        uint64_t now = osal::System::get_uptime_ms();
        return now >= deadline ? 0 : static_cast<uint32_t>(deadline - now);
    };
    auto stop = [&req]() { return call_stopped(req); };
    auto fail = [&](int32_t rc) {
        stats_.failed.fetch_add(1, std::memory_order_relaxed);
        return rc;
    };
    const uint16_t default_port = t.https ? 443 : 80;
    const uint16_t port = t.port ? t.port : default_port;

    uint32_t ip = t.ip;
    if (ip == 0) {
        const int rc = opts_.resolve_host ? opts_.resolve_host(t.host, left_ms(), ip)
                                          : osal::Socket::resolve_ipv4(t.host.c_str(), left_ms(), stop, ip);
        if (rc == ETIMEDOUT) return fail(K32_ERR_TIMEOUT);
        if (rc == ECANCELED) return fail(K32_ERR_IO);
        if (rc != 0 || ip == 0) return fail(K32_ERR_NOENT);
    }
    std::string where = t.host + ":" + std::to_string(port);
    if (t.ip == 0) where += " (" + net::format_ipv4(ip) + ")";
    if (egress_) {
        EgressScope scope;
        if (!egress_scope(req, scope, where)) return K32_ERR_PERM;
        net::EgressTarget target;
        target.ip = ip;
        target.port = port;
        if (!net::egress_allowed(scope.selecting, namespace_of(req.caller_pod), target, []() -> const net::PeerPod* { return nullptr; })) {
            log_denied(req.caller_pod, where, "no NetworkPolicy egress rule allows it");
            stats_.denied.fetch_add(1, std::memory_order_relaxed);
            return fail(K32_ERR_PERM);
        }
    }

    osal::Socket sock;
    if (!sock.create(osal::SocketType::Tcp)) return fail(K32_ERR_IO);
    const uint32_t ct = left_ms();
    if (ct == 0) return fail(K32_ERR_TIMEOUT);
    const int err = sock.connect_within(ip, port, ct, stop);
    if (err != 0) return fail(err == ECANCELED ? K32_ERR_IO : connect_error(err));
    sock.set_tcp_nodelay(true);

    if (t.https) {
        if (!opts_.tls) return fail(K32_ERR_NOTSUP);
        stats_.https.fetch_add(1, std::memory_order_relaxed);
        const uint64_t started = osal::System::get_uptime_ms();
        std::string why;
        const osal::TlsStatus st = sock.tls_connect(*opts_.tls, t.host, left_ms(), stop, &why);
        switch (st) {
            case osal::TlsStatus::Ok:
                break;
            case osal::TlsStatus::Certificate:
                log_tls(req.caller_pod, where, "certificate rejected: " + why);
                return fail(K32_ERR_CERT);
            case osal::TlsStatus::Handshake:
                log_tls(req.caller_pod, where, "handshake failed: " + why);
                return fail(K32_ERR_TLS);
            case osal::TlsStatus::Timeout:
                return fail(K32_ERR_TIMEOUT);
            case osal::TlsStatus::Cancelled:
                return fail(K32_ERR_IO);
        }
        // The first sessions say what one costs
        if (sessions_logged_.fetch_add(1, std::memory_order_relaxed) < 2) {
            printf("[EGRESS] TLS session to %s in %llu ms (free: internal %u bytes, PSRAM %u bytes)\n", where.c_str(),
                   static_cast<unsigned long long>(osal::System::get_uptime_ms() - started),
                   static_cast<unsigned>(osal::System::get_free_heap_bytes()), static_cast<unsigned>(osal::System::get_free_psram_bytes()));
        }
    }

    std::string head;
    head.reserve(128 + t.path.size() + t.host.size() + req.headers.size());
    head.append(req.method.data(), req.method.size()).append(" ").append(t.path).append(" HTTP/1.1\r\nHost: ").append(t.host);
    if (t.port && t.port != default_port) head.append(":").append(std::to_string(t.port));
    head.append("\r\nContent-Length: ").append(std::to_string(req.body.size())).append("\r\nConnection: close\r\n");
    each_header(req.headers, [&](std::string_view name, std::string_view value) {
        if (!header_allowed(name)) return;
        head.append(name.data(), name.size()).append(": ").append(value.data(), value.size()).append("\r\n");
    });
    head.append("\r\n");
    const size_t buf_size = opts_.response_head_bytes > kRecvBuf ? opts_.response_head_bytes : kRecvBuf;
    std::unique_ptr<char[]> buf(new (std::nothrow) char[buf_size]);
    if (!buf) return fail(K32_ERR_NOMEM);
    sock.set_timeout(250, left_ms() ? left_ms() : 1);
    if (!sock.send_all(head.data(), head.size()) || (!req.body.empty() && !sock.send_all(req.body.data(), req.body.size()))) {
        return fail(left_ms() == 0 ? K32_ERR_TIMEOUT : K32_ERR_IO);
    }
    bool reusable = false;
    const int32_t rc = read_response(sock, req, out, deadline, false, true, buf.get(), buf_size, reusable);
    sock.close();
    return rc < 0 ? fail(rc) : rc;
}

// The call as an HTTP message (the request line with the absolute URL, the
// caller's headers, the body) to the control plane's egress gateway, which
// answers with the host's response or an X-Kube32-Egress-Result error
int32_t ServiceClient::call_gateway(const runtime::ServiceCallRequest& req, const Target& t, ResponseBody& out) {
    stats_.forwarded.fetch_add(1, std::memory_order_relaxed);
    const uint64_t deadline = osal::System::get_uptime_ms() + req.timeout_ms;
    auto left_ms = [&]() -> uint32_t {
        uint64_t now = osal::System::get_uptime_ms();
        return now >= deadline ? 0 : static_cast<uint32_t>(deadline - now);
    };
    auto fail = [&](int32_t rc) {
        stats_.failed.fetch_add(1, std::memory_order_relaxed);
        return rc;
    };
    // The URL as parsed: no fragment, nothing parse_target did not check
    std::string origin = "https://" + t.host;
    if (t.port) origin.append(":").append(std::to_string(t.port));
    std::string message;
    message.reserve(32 + origin.size() + t.path.size() + req.headers.size());
    message.append(req.method.data(), req.method.size()).append(" ").append(origin).append(t.path).append(" HTTP/1.1\r\n");
    each_header(req.headers, [&](std::string_view name, std::string_view value) {
        if (!header_allowed(name)) return;
        message.append(name.data(), name.size()).append(": ").append(value.data(), value.size()).append("\r\n");
    });
    message.append("\r\n");

    osal::Socket sock;
    const uint32_t ct = left_ms() < opts_.connect_timeout_ms ? left_ms() : opts_.connect_timeout_ms;
    if (ct == 0) return fail(K32_ERR_TIMEOUT);
    if (!sock.create(osal::SocketType::Tcp) || !sock.connect(opts_.gateway_host.c_str(), opts_.gateway_port, ct)) {
        return fail(left_ms() == 0 ? K32_ERR_TIMEOUT : K32_ERR_IO);
    }
    sock.set_tcp_nodelay(true);
    std::string head;
    head.reserve(256 + req.caller_pod.size() + opts_.gateway_token.size());
    head.append("POST /apis/kube32.io/v1/egress?pod=").append(req.caller_pod.data(), req.caller_pod.size());
    // The gateway ends the call a little before this node does, so its
    // answer (a timeout included) arrives while the pod still waits
    const uint32_t left = left_ms();
    head.append("&timeout=").append(std::to_string(left > 400 ? left - 200 : left)).append("&limit=").append(std::to_string(out.limit));
    head.append(" HTTP/1.1\r\nHost: ").append(opts_.gateway_host).append(":").append(std::to_string(opts_.gateway_port)).append("\r\n");
    if (!opts_.gateway_token.empty()) head.append("Authorization: Bearer ").append(opts_.gateway_token).append("\r\n");
    head.append("Content-Type: message/http\r\nContent-Length: ").append(std::to_string(message.size() + req.body.size()));
    head.append("\r\nConnection: close\r\n\r\n");
    std::unique_ptr<char[]> buf(new (std::nothrow) char[kRecvBuf]);
    if (!buf) return fail(K32_ERR_NOMEM);
    sock.set_timeout(250, left_ms() ? left_ms() : 1);
    if (!sock.send_all(head.data(), head.size()) || !sock.send_all(message.data(), message.size()) ||
        (!req.body.empty() && !sock.send_all(req.body.data(), req.body.size()))) {
        return fail(left_ms() == 0 ? K32_ERR_TIMEOUT : K32_ERR_IO);
    }
    bool reusable = false;
    const int32_t rc = read_response(sock, req, out, deadline, false, true, buf.get(), kRecvBuf, reusable, true);
    sock.close();
    if (rc == K32_ERR_PERM) log_denied(req.caller_pod, origin, "refused by the egress gateway");
    return rc < 0 ? fail(rc) : rc;
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
                                   const std::string& path, ResponseBody& out) {
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

    std::unique_ptr<char[]> buf(new (std::nothrow) char[kRecvBuf]);
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
        bool reusable = false;
        const int32_t rc = read_response(sock, req, out, deadline, reused, false, buf.get(), kRecvBuf, reusable);
        if (rc == kStale) { sock.close(); continue; }
        if (rc < 0) { sock.close(); return rc; }
        if (reusable) put_idle(std::move(sock), ep.ip, ep.port);
        else sock.close();
        return rc;
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
