#pragma once

// Long-lived API streams (watch and log follow) without pinning HTTP workers.
//
// The route handler writes the chunked response head, moves the socket here
// and returns. One hub thread owns every stream: it drains bounded per-stream
// event queues fed by the store's change hook, replays recent history for
// resourceVersion watches, sends BOOKMARK frames, polls pod log rings for
// follow streams, and detects client hang-ups with select().

#include "api/selectors.hpp"
#include "osal/mutex.hpp"
#include "osal/socket.hpp"
#include "osal/thread.hpp"
#include "runtime/pod_log.hpp"
#include "storage/store.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace kube32::api {

struct WatchHubOptions {
    size_t max_streams{8};            // watches + log follows; beyond it the handler answers 429
    size_t queue_depth{16};           // events buffered per watch before it is told to relist (410)
    size_t replay_ring{32};           // recent events kept for resourceVersion=N watches
    uint32_t bookmark_interval_ms{30000};
    uint32_t default_timeout_s{1800}; // kube-apiserver's upper bound when the client sends none
    uint32_t poll_interval_ms{250};
    uint32_t send_timeout_ms{1500};   // a stalled client is dropped, never waited for
    size_t stack_bytes{16384};
    bool stack_in_psram{false};
    int core_affinity{-1};
    const char* thread_name{"k32_watch"};
};

struct WatchSpec {
    std::string prefix;               // store prefix, e.g. "/registry/pods/default/"
    std::string kind;                 // "Pod", used by BOOKMARK frames
    std::string api_version;          // "v1"
    ListSelector selector;
    uint64_t since_version{0};        // 0: send ADDED for every existing object first
    bool bookmarks{false};
    uint32_t timeout_s{0};            // 0: hub default
    // Optional transform applied to every object before framing (Table rows).
    std::function<std::string(const std::string& object_json)> wrap;
};

struct LogFollowSpec {
    std::string pod_key;              // "namespace/name"
    uint64_t after_seq{0};            // records already sent by the handler
    bool timestamps{false};
    uint32_t timeout_s{0};
};

class WatchHub {
public:
    WatchHub(storage::Store& store, WatchHubOptions options = {});
    ~WatchHub();

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    // The chunked HTTP head must already be on the wire. Returns false (socket
    // closed) when the hub is at capacity.
    bool add_watch(osal::Socket sock, WatchSpec spec);
    bool add_log_follow(osal::Socket sock, LogFollowSpec spec);

    bool has_capacity() const;
    size_t active_streams() const;
    uint64_t frames_sent() const { return frames_sent_.load(); }
    uint64_t streams_dropped() const { return dropped_.load(); }
    const WatchHubOptions& options() const { return options_; }

    // {"type":"ADDED","object":{...}}\n  (one line, as kube-apiserver frames it)
    static std::string frame(const char* type, const std::string& object_json);
    // Status object carried by an ERROR frame (410 Gone / Expired).
    static std::string expired_status(uint64_t resource_version);

private:
    struct QueuedEvent {
        storage::WatchEventType type;
        std::string key;
        std::string json;
        uint64_t version;
    };
    using EventPtr = std::shared_ptr<const QueuedEvent>;

    struct Stream {
        osal::Socket sock;
        bool is_log{false};
        WatchSpec watch;
        LogFollowSpec log;
        std::deque<EventPtr> queue;   // guarded by mutex_
        bool overflow{false};         // guarded by mutex_
        bool primed{false};           // initial list / replay done
        uint64_t deadline_ms{0};
        uint64_t next_bookmark_ms{0};
        bool log_seen{false};
    };

    void run();
    void on_store_event(const storage::WatchEvent& ev);
    bool prime(Stream& s);
    bool flush(Stream& s, std::vector<EventPtr>& batch);
    bool send_frame(Stream& s, const std::string& payload);
    bool send_watch_event(Stream& s, const char* type, const std::string& object_json);
    bool poll_log(Stream& s, uint64_t now_ms);
    void finish(Stream& s);

    storage::Store& store_;
    WatchHubOptions options_;
    mutable osal::Mutex mutex_;
    std::vector<std::unique_ptr<Stream>> streams_;
    std::deque<EventPtr> replay_;
    uint64_t oldest_evicted_version_{0};
    uint32_t store_watch_id_{0};
    std::unique_ptr<osal::Thread> thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_sent_{0};
    std::atomic<uint64_t> dropped_{0};
};

} // namespace kube32::api
