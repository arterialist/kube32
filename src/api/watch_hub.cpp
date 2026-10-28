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
// Only queues; never touches a socket.
void WatchHub::on_store_event(const storage::WatchEvent& ev) {
    auto qe = std::make_shared<const QueuedEvent>(QueuedEvent{ev.type, ev.key, ev.json, ev.resource_version});
    osal::LockGuard guard(mutex_);
    if (options_.replay_ring > 0) {
        if (replay_.size() >= options_.replay_ring) {
            oldest_evicted_version_ = replay_.front()->version;
            replay_.pop_front();
        }
        replay_.push_back(qe);
    }
    for (auto& s : streams_) {
        if (s->is_log) continue;
        if (ev.key.compare(0, s->watch.prefix.size(), s->watch.prefix) != 0) continue;
        if (s->queue.size() >= options_.queue_depth) {
            s->overflow = true;
            continue;
        }
        s->queue.push_back(qe);
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

bool WatchHub::send_watch_event(Stream& s, const char* type, const std::string& object_json) {
    if (s.watch.wrap) return send_frame(s, frame(type, s.watch.wrap(object_json)));
    return send_frame(s, frame(type, object_json));
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
        return true;
    }
    std::vector<EventPtr> history;
    bool expired = false;
    {
        osal::LockGuard guard(mutex_);
        // Events evicted from the ring that the client has not seen: it must relist
        if (oldest_evicted_version_ > s.watch.since_version) expired = true;
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
    return true;
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

void WatchHub::run() {
    while (running_.load()) {
        uint64_t now = osal::System::get_uptime_ms();

        // Snapshot the stream pointers and their pending events under the lock;
        // socket I/O happens outside it so store writers never wait on a client.
        std::vector<Stream*> active;
        std::vector<std::vector<EventPtr>> pending;
        std::vector<bool> overflowed;
        {
            osal::LockGuard guard(mutex_);
            active.reserve(streams_.size());
            for (auto& s : streams_) {
                active.push_back(s.get());
                std::vector<EventPtr> batch(s->queue.begin(), s->queue.end());
                s->queue.clear();
                pending.push_back(std::move(batch));
                overflowed.push_back(s->overflow);
                s->overflow = false;
            }
        }

        std::vector<Stream*> closed;
        for (size_t i = 0; i < active.size(); ++i) {
            Stream& s = *active[i];
            bool alive = true;
            if (s.is_log) {
                alive = poll_log(s, now);
            } else {
                if (!s.primed) {
                    s.primed = true;
                    alive = prime(s);
                }
                if (alive && overflowed[i]) {
                    send_frame(s, frame("ERROR", expired_status(s.watch.since_version)));
                    alive = false;
                }
                if (alive && !pending[i].empty()) alive = flush(s, pending[i]);
                if (alive && s.watch.bookmarks && now >= s.next_bookmark_ms) {
                    s.next_bookmark_ms = now + options_.bookmark_interval_ms;
                    std::string obj = "{\"kind\":\"" + s.watch.kind + "\",\"apiVersion\":\"" + s.watch.api_version +
                                      "\",\"metadata\":{\"resourceVersion\":\"" + std::to_string(store_.current_version()) + "\"}}";
                    alive = send_frame(s, frame("BOOKMARK", obj));
                }
            }
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
