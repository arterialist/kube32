#include "api/watch_hub.hpp"
#include <iostream>
#include "osal/system.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/select.h>
#include <sys/time.h>

namespace kube32::api {

WatchHub::WatchHub(storage::Store& store, WatchHubOptions options)
    : store_(store), options_(options) {
    if (options_.max_streams == 0) options_.max_streams = 1;
    if (options_.queue_depth == 0) options_.queue_depth = 1;
}

WatchHub::~WatchHub() {
    stop();
}

bool WatchHub::start() {
    if (running_.load()) return true;
    running_.store(true);
    {
        // History before this hub started (an earlier boot of a persistent
        // store included) cannot be replayed: such watches must relist
        osal::LockGuard guard(mutex_);
        if (store_.current_version() > oldest_evicted_version_) oldest_evicted_version_ = store_.current_version();
    }
    store_watch_id_ = store_.watch("/registry/", [this](const storage::WatchEvent& ev) { on_store_event(ev); });
    thread_ = std::make_unique<osal::Thread>();
    osal::Thread::Config cfg;
    cfg.name = options_.thread_name;
    cfg.stack_size = options_.stack_bytes;
    cfg.priority = 4;
    cfg.core_affinity = options_.core_affinity;
    cfg.stack_in_psram = options_.stack_in_psram;
    bool ok = thread_->start([](void* p) { static_cast<WatchHub*>(p)->run(); }, this, cfg);
    if (!ok) {
        running_.store(false);
        store_.unwatch(store_watch_id_);
        store_watch_id_ = 0;
    }
    return ok;
}

void WatchHub::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (store_watch_id_) {
        store_.unwatch(store_watch_id_);
        store_watch_id_ = 0;
    }
    if (thread_) {
        thread_->join();
        thread_.reset();
    }
    osal::LockGuard guard(mutex_);
    for (auto& s : streams_) {
        s->sock.send_all("0\r\n\r\n", 5);
        s->sock.close();
    }
    streams_.clear();
    replay_.clear();
    replay_bytes_ = 0;
}

bool WatchHub::has_capacity() const {
    osal::LockGuard guard(mutex_);
    return streams_.size() < options_.max_streams;
}

size_t WatchHub::active_streams() const {
    osal::LockGuard guard(mutex_);
    return streams_.size();
}

bool WatchHub::add_watch(osal::Socket sock, WatchSpec spec) {
    auto s = std::make_unique<Stream>();
    s->sock = std::move(sock);
    s->is_log = false;
    s->watch = std::move(spec);
    uint64_t now = osal::System::get_uptime_ms();
    uint32_t timeout_s = s->watch.timeout_s ? s->watch.timeout_s : options_.default_timeout_s;
    s->deadline_ms = now + static_cast<uint64_t>(timeout_s) * 1000ULL;
    s->next_bookmark_ms = now + options_.bookmark_interval_ms;
    s->sock.set_timeout(50, options_.send_timeout_ms);
    osal::LockGuard guard(mutex_);
    if (!running_.load() || streams_.size() >= options_.max_streams) {
        s->sock.close();
        return false;
    }
    streams_.push_back(std::move(s));
    std::cout << "[API] watch stream opened (" << streams_.size() << "/" << options_.max_streams << " in use)" << std::endl;
    return true;
}

bool WatchHub::add_log_follow(osal::Socket sock, LogFollowSpec spec) {
    auto s = std::make_unique<Stream>();
    s->sock = std::move(sock);
    s->is_log = true;
    s->log = std::move(spec);
    s->primed = true;
    uint64_t now = osal::System::get_uptime_ms();
    uint32_t timeout_s = s->log.timeout_s ? s->log.timeout_s : options_.default_timeout_s;
    s->deadline_ms = now + static_cast<uint64_t>(timeout_s) * 1000ULL;
    s->log_seen = runtime::PodLogStore::apiserver().exists(s->log.pod_key);
    s->sock.set_timeout(50, options_.send_timeout_ms);
    osal::LockGuard guard(mutex_);
    if (!running_.load() || streams_.size() >= options_.max_streams) {
        s->sock.close();
        return false;
    }
    streams_.push_back(std::move(s));
    return true;
}

std::string WatchHub::frame(const char* type, const std::string& object_json) {
    std::string out;
    out.reserve(object_json.size() + 24);
    out += "{\"type\":\"";
    out += type;
    out += "\",\"object\":";
    out += object_json;
    out += "}\n";
    return out;
}

std::string WatchHub::expired_status(uint64_t resource_version) {
    return "{\"kind\":\"Status\",\"apiVersion\":\"v1\",\"metadata\":{},\"status\":\"Failure\","
           "\"message\":\"too old resource version: " + std::to_string(resource_version) + "\","
           "\"reason\":\"Expired\",\"code\":410}";
}

// Runs on the thread that mutated the store (API worker, controller, kubelet).
// Only queues; never touches a socket. It does not throw: the change is in
// the store already, so an event that cannot be queued for lack of memory
// makes the streams that wanted it start over (410, the client relists)
// rather than miss it.
void WatchHub::on_store_event(const storage::WatchEvent& ev) {
    EventPtr qe;
    try {
        qe = std::make_shared<const QueuedEvent>(QueuedEvent{ev.type, ev.key, ev.json, ev.resource_version});
    } catch (const std::bad_alloc&) {
    }
    osal::LockGuard guard(mutex_);
    bool ring_kept = false;
    if (qe && options_.replay_ring > 0) {
        try {
            replay_.push_back(qe);
            replay_bytes_ += qe->json.size();
            ring_kept = true;
        } catch (const std::bad_alloc&) {
        }
        while (!replay_.empty() && (replay_.size() > options_.replay_ring || (replay_bytes_ > options_.replay_ring_bytes && replay_.size() > 1))) {
            oldest_evicted_version_ = replay_.front()->version;
            replay_bytes_ -= replay_.front()->json.size();
            replay_.pop_front();
        }
    }
    // A resourceVersion watch from before this event cannot be served from the ring
    if (!ring_kept && ev.resource_version > oldest_evicted_version_) oldest_evicted_version_ = ev.resource_version;
    for (auto& s : streams_) {
        if (s->is_log) continue;
        if (ev.key.compare(0, s->watch.prefix.size(), s->watch.prefix) != 0) continue;
        if (!qe || s->queue.size() >= options_.queue_depth) {
            s->overflow = true;
            continue;
        }
        try {
            s->queue.push_back(qe);
        } catch (const std::bad_alloc&) {
            s->overflow = true;
        }
    }
}

bool WatchHub::send_frame(Stream& s, const std::string& payload) {
    char head[16];
    int n = snprintf(head, sizeof(head), "%zx\r\n", payload.size());
    std::string wire;
    wire.reserve(payload.size() + static_cast<size_t>(n) + 2);
    wire.append(head, static_cast<size_t>(n));
    wire += payload;
    wire += "\r\n";
    if (!s.sock.send_all(wire.data(), wire.size())) return false;
    frames_sent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// One chunk {"type":"<type>","object":<object>}\n, sent in pieces: the
// object is not copied for each stream
bool WatchHub::send_watch_event(Stream& s, const char* type, const std::string& object_json) {
    if (s.watch.wrap) return send_frame(s, frame(type, s.watch.wrap(object_json)));
    char head[64];
    const size_t type_len = std::strlen(type);
    const size_t payload = 20 + type_len + object_json.size() + 2;   // {"type":"","object": and }\n
    const int n = snprintf(head, sizeof(head), "%zx\r\n{\"type\":\"%s\",\"object\":", payload, type);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(head)) return false;
    if (!s.sock.send_all(head, static_cast<size_t>(n))) return false;
    if (!s.sock.send_all(object_json.data(), object_json.size())) return false;
    if (!s.sock.send_all("}\n\r\n", 4)) return false;
    frames_sent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void WatchHub::finish(Stream& s) {
    s.sock.send_all("0\r\n\r\n", 5);
    s.sock.close();
}

// Initial state for a new watch: either every current object (no
// resourceVersion) or the events after the client's resourceVersion.
bool WatchHub::prime(Stream& s) {
    if (s.watch.since_version == 0) {
        std::vector<std::string> jsons;
        std::vector<std::string> keys;
        store_.list_extended(s.watch.prefix, jsons, &keys);
        for (size_t i = 0; i < jsons.size(); ++i) {
            if (!s.watch.selector.matches_json(jsons[i])) continue;
            if (!send_watch_event(s, "ADDED", jsons[i])) return false;
            osal::Thread::yield();
        }
        return !s.watch.send_initial_events || send_initial_events_end(s);
    }
    std::vector<EventPtr> history;
    bool expired = false;
    {
        osal::LockGuard guard(mutex_);
        // Events evicted from the ring that the client has not seen: it must relist
        if (oldest_evicted_version_ > s.watch.since_version) expired = true;
        // A version this store never handed out (from before a restart of a
        // store without persistence): relist rather than wait for it
        if (s.watch.since_version > store_.current_version()) expired = true;
        for (const auto& e : replay_) {
            if (e->version > s.watch.since_version &&
                e->key.compare(0, s.watch.prefix.size(), s.watch.prefix) == 0) {
                history.push_back(e);
            }
        }
    }
    if (expired) {
        send_frame(s, frame("ERROR", expired_status(s.watch.since_version)));
        return false;
    }
    for (const auto& e : history) {
        if (!s.watch.selector.matches_json(e->json)) continue;
        const char* type = (e->type == storage::WatchEventType::Added) ? "ADDED"
                         : (e->type == storage::WatchEventType::Deleted) ? "DELETED" : "MODIFIED";
        if (!send_watch_event(s, type, e->json)) return false;
    }
    return !s.watch.send_initial_events || send_initial_events_end(s);
}

// WatchList (sendInitialEvents=true): client-go treats the stream as its LIST
// and waits for a BOOKMARK carrying the initial-events-end annotation before
// it considers the cache synced. k9s uses this for every view.
bool WatchHub::send_initial_events_end(Stream& s) {
    std::string obj = "{\"kind\":\"" + s.watch.kind + "\",\"apiVersion\":\"" + s.watch.api_version +
                      "\",\"metadata\":{\"resourceVersion\":\"" + std::to_string(store_.current_version()) +
                      "\",\"annotations\":{\"k8s.io/initial-events-end\":\"true\"}}}";
    return send_frame(s, frame("BOOKMARK", obj));
}

bool WatchHub::flush(Stream& s, std::vector<EventPtr>& batch) {
    for (const auto& e : batch) {
        // Queued before priming finished may duplicate the initial list; informers tolerate it
        if (!s.watch.selector.matches_json(e->json)) continue;
        const char* type = (e->type == storage::WatchEventType::Added) ? "ADDED"
                         : (e->type == storage::WatchEventType::Deleted) ? "DELETED" : "MODIFIED";
        if (!send_watch_event(s, type, e->json)) return false;
        osal::Thread::yield();
    }
    return true;
}

bool WatchHub::poll_log(Stream& s, uint64_t now_ms) {
    (void)now_ms;
    std::vector<runtime::PodLogRecord> recs;
    bool exists = runtime::PodLogStore::apiserver().read_after(s.log.pod_key, s.log.after_seq, recs, 32);
    if (exists) s.log_seen = true;
    if (!recs.empty()) {
        std::string payload;
        for (const auto& r : recs) {
            payload += runtime::format_log_line(r, s.log.timestamps);
            s.log.after_seq = r.seq;
        }
        if (!send_frame(s, payload)) return false;
    }
    // Like the kubelet closing a log stream: once the pod object is gone and
    // its last lines are delivered, the follow ends.
    size_t slash = s.log.pod_key.find('/');
    std::string pod_store_key = "/registry/pods/" + s.log.pod_key.substr(0, slash) + "/" + s.log.pod_key.substr(slash + 1);
    std::string unused;
    if (!store_.get(pod_store_key, unused).ok()) {
        std::vector<runtime::PodLogRecord> rest;
        if (runtime::PodLogStore::apiserver().read_after(s.log.pod_key, s.log.after_seq, rest, 0) && !rest.empty()) {
            std::string payload;
            for (const auto& r : rest) { payload += runtime::format_log_line(r, s.log.timestamps); s.log.after_seq = r.seq; }
            send_frame(s, payload);
        }
        return false;
    }
    return true;
}

// One pass over one stream: false when it ends
bool WatchHub::serve(Stream& s, uint64_t now, std::vector<EventPtr>& pending, bool overflowed) {
    if (s.is_log) return poll_log(s, now);
    bool alive = true;
    if (!s.primed) {
        s.primed = true;
        alive = prime(s);
    }
    if (alive && overflowed) {
        send_frame(s, frame("ERROR", expired_status(s.watch.since_version)));
        alive = false;
    }
    if (alive && !pending.empty()) alive = flush(s, pending);
    if (alive && s.watch.bookmarks && now >= s.next_bookmark_ms) {
        s.next_bookmark_ms = now + options_.bookmark_interval_ms;
        std::string obj = "{\"kind\":\"" + s.watch.kind + "\",\"apiVersion\":\"" + s.watch.api_version +
                          "\",\"metadata\":{\"resourceVersion\":\"" + std::to_string(store_.current_version()) + "\"}}";
        alive = send_frame(s, frame("BOOKMARK", obj));
    }
    return alive;
}

void WatchHub::run() {
    while (running_.load()) {
        uint64_t now = osal::System::get_uptime_ms();

        // Snapshot the stream pointers and their pending events under the lock;
        // socket I/O happens outside it so store writers never wait on a client.
        // Out of memory, a stream whose events cannot be taken is told to
        // start over (410) rather than miss them.
        std::vector<Stream*> active;
        std::vector<std::vector<EventPtr>> pending;
        std::vector<bool> overflowed;
        bool reserved = true;
        {
            osal::LockGuard guard(mutex_);
            try {
                active.reserve(streams_.size());
                pending.reserve(streams_.size());
                overflowed.reserve(streams_.size());
            } catch (const std::bad_alloc&) {
                reserved = false;
            }
            for (size_t i = 0; reserved && i < streams_.size(); ++i) {
                Stream& s = *streams_[i];
                std::vector<EventPtr> batch;
                try {
                    batch.assign(s.queue.begin(), s.queue.end());
                    s.queue.clear();
                } catch (const std::bad_alloc&) {
                    s.overflow = true;
                }
                active.push_back(&s);
                pending.push_back(std::move(batch));
                overflowed.push_back(s.overflow);
                s.overflow = false;
            }
        }
        if (!reserved) {
            osal::Thread::sleep_ms(options_.poll_interval_ms);
            continue;
        }

        std::vector<Stream*> closed;
        for (size_t i = 0; i < active.size(); ++i) {
            Stream& s = *active[i];
            bool alive = false;
            try {
                alive = serve(s, now, pending[i], overflowed[i]);
            } catch (const std::exception& e) {
                // Out of memory: the stream ends and its client watches again
                std::cout << "[API] watch stream ended: " << e.what() << std::endl;
            }
            pending[i].clear();
            if (alive && now >= s.deadline_ms) alive = false;   // client re-watches, as with kube-apiserver timeouts
            if (!alive) closed.push_back(&s);
        }

        // Hang-up detection: a readable stream socket means EOF or stray bytes
        if (!active.empty()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            int maxfd = -1;
            for (Stream* s : active) {
                int fd = s->sock.native_handle();
                if (fd < 0) continue;
                FD_SET(fd, &rfds);
                if (fd > maxfd) maxfd = fd;
            }
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = static_cast<int>(options_.poll_interval_ms) * 1000;
            int ready = (maxfd >= 0) ? ::select(maxfd + 1, &rfds, nullptr, nullptr, &tv) : 0;
            if (ready > 0) {
                for (Stream* s : active) {
                    int fd = s->sock.native_handle();
                    if (fd < 0 || !FD_ISSET(fd, &rfds)) continue;
                    char scratch[64];
                    ssize_t n = s->sock.recv(scratch, sizeof(scratch));
                    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                        closed.push_back(s);
                    }
                }
            }
        } else {
            osal::Thread::sleep_ms(options_.poll_interval_ms);
        }

        if (!closed.empty()) {
            osal::LockGuard guard(mutex_);
            for (size_t i = 0; i < streams_.size();) {
                bool drop = false;
                for (Stream* c : closed) if (c == streams_[i].get()) { drop = true; break; }
                if (drop) {
                    finish(*streams_[i]);
                    streams_.erase(streams_.begin() + static_cast<long>(i));
                    dropped_.fetch_add(1, std::memory_order_relaxed);
                    std::cout << "[API] watch stream closed (" << streams_.size() << "/" << options_.max_streams << " in use)" << std::endl;
                } else {
                    ++i;
                }
            }
        }
        osal::System::feed_watchdog();
    }
}

} // namespace kube32::api
