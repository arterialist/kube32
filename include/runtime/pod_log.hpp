#pragma once

// Per-pod bounded log rings.
//
// Two stores exist in every process:
//   * PodLogStore::runtime()   - fed by the k32_log host function and by the
//     container lifecycle (started / trapped / stopped) on the node that runs
//     the pod. The kubelet ships new records from here to the API server.
//   * PodLogStore::apiserver() - the control plane's mirror of every pod's
//     ring, filled by the kubelet log shipping endpoint and served by
//     GET /api/v1/namespaces/{ns}/pods/{name}/log (and follow streams).
//     Shipped records name the container instance (restart count) that wrote
//     them; when a new instance starts, the mirror keeps the tail of the last
//     one as the previous container's log (?previous=true).
//     With a storage root (see enable_persistence) the mirror also writes each
//     pod's log to <root>/kube32/logs/<namespace>_<pod>/<container>.log, moves
//     it to <container>.log.1 when a new container instance starts, and serves
//     the previous container's log from that file.
//
// Every ring has a byte budget (2 KiB on the device by default); the oldest
// records are evicted when a new one does not fit. Records carry a per-ring
// sequence number so followers and the shipping path never duplicate lines.

#include "osal/mutex.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kube32::runtime {

struct PodLogRecord {
    uint64_t seq{0};
    uint64_t unix_ms{0};    // 0 when the wall clock was unknown at write time
    uint64_t uptime_ms{0};
    std::string text;       // one line, without the trailing newline
};

class PodLogRing {
public:
    explicit PodLogRing(size_t capacity_bytes, uint64_t first_seq = 1) : capacity_(capacity_bytes), next_seq_(first_seq) {}

    // Appends a record. `seq` 0 means "assign the next sequence number".
    void append(std::string_view text, uint64_t unix_ms, uint64_t uptime_ms, uint64_t seq = 0);
    // Copies records with seq > after_seq (oldest first); max_records 0 = all.
    void read_after(uint64_t after_seq, std::vector<PodLogRecord>& out, size_t max_records = 0) const;
    // Copies the newest `count` records (all when count is 0).
    void tail(size_t count, std::vector<PodLogRecord>& out) const;

    uint64_t last_seq() const { return next_seq_ - 1; }
    uint64_t dropped() const { return dropped_; }
    size_t bytes_used() const { return used_; }
    size_t capacity() const { return capacity_; }
    size_t size() const { return records_.size(); }

private:
    static constexpr size_t kRecordOverhead = sizeof(PodLogRecord);
    size_t capacity_;
    size_t used_{0};
    uint64_t next_seq_{1};
    uint64_t dropped_{0};
    std::deque<PodLogRecord> records_;
};

class PodLogDisk;

// The node's storage root for container logs (hal::node_storage_root(): the
// microSD card, else the internal flash partition). Empty when it has none.
std::string pod_log_storage_root();

class PodLogStore {
public:
    static PodLogStore& runtime();
    static PodLogStore& apiserver();

    // Byte budget applied to rings created afterwards.
    void set_ring_capacity(size_t bytes);
    size_t ring_capacity() const;
    // Rings kept at most; the least recently written ring is evicted beyond this.
    void set_max_pods(size_t n);

    // Appends on the local ring. Timestamps default to "now".
    void append(const std::string& pod_key, std::string_view text);
    void append(const std::string& pod_key, std::string_view text, uint64_t unix_ms, uint64_t uptime_ms);

    // Mirror side: accepts a shipped record. `epoch` identifies the sender's
    // process lifetime; a new epoch resets the duplicate filter. `instance` is
    // the container instance (restart count) that wrote the line and
    // `pod_uid` the pod object it belongs to: a higher instance, or a new
    // epoch for the same pod, starts a new current log and keeps the old one
    // as the previous log; a different pod_uid drops both. Returns true when
    // the record was appended (false for duplicates / replays).
    bool accept_shipped(const std::string& pod_key, uint64_t epoch, const PodLogRecord& rec,
                        uint32_t instance = 0, std::string_view pod_uid = {}, std::string_view container = {});
    // Byte budget of a previous container's log (the tail is kept).
    void set_previous_capacity(size_t bytes);

    bool exists(const std::string& pod_key) const;
    bool read_after(const std::string& pod_key, uint64_t after_seq, std::vector<PodLogRecord>& out, size_t max_records = 0) const;
    bool tail(const std::string& pod_key, size_t count, std::vector<PodLogRecord>& out) const;
    // The previous container instance's log; false when there is none.
    bool tail_previous(const std::string& pod_key, size_t count, std::vector<PodLogRecord>& out) const;
    uint64_t last_seq(const std::string& pod_key) const;
    uint64_t dropped(const std::string& pod_key) const;
    void remove(const std::string& pod_key);
    std::vector<std::string> pods() const;
    void clear();

    // Persists shipped lines under `root()` (evaluated on every write; empty
    // means no storage, nothing is written). Each file is kept under
    // `file_limit_bytes` by dropping its oldest half. Lines are written in
    // batches (1 KiB or 10 s). All disk I/O runs on one writer thread whose
    // stack is in internal RAM, as flash access requires on the ESP32-S3.
    void enable_persistence(std::function<std::string()> root, size_t file_limit_bytes = 32 * 1024);
    bool persistence_enabled() const;
    // Lines of a container's log file: the running instance's, or with
    // `previous` the one before it. False when there is no such file.
    bool read_persisted(const std::string& pod_key, const std::string& container, bool previous, size_t tail,
                        std::vector<PodLogRecord>& out);
    // Deletes the pod's log directory (the pod object was deleted).
    void remove_persisted(const std::string& pod_key);
    // Writes every buffered line and waits for the writer to finish.
    void flush_persisted();

    ~PodLogStore();

private:
    PodLogStore();
    struct Entry {
        PodLogRing ring;
        Entry& operator=(Entry&&) = default;
        Entry(Entry&&) = default;
        uint64_t last_write_ms{0};
        uint64_t shipped_epoch{0};
        uint64_t shipped_seq{0};
        uint32_t instance{0};
        std::string pod_uid;
        std::unique_ptr<PodLogRing> previous;
        std::string container;             // file name of the persisted log
        std::string disk_pending;          // lines not yet handed to the writer
        uint64_t disk_pending_since_ms{0};
        explicit Entry(size_t cap) : ring(cap) {}
    };
    Entry& entry_locked(const std::string& pod_key);
    void start_instance_locked(const std::string& pod_key, Entry& e, uint32_t instance);
    void hand_to_disk_locked(const std::string& pod_key, Entry& e);
    // Hands lines buffered for longer than `min_age_ms` to the writer.
    void collect_pending(uint64_t min_age_ms);

    mutable osal::Mutex mutex_;
    std::map<std::string, Entry> rings_;
    size_t ring_capacity_{2048};
    size_t previous_capacity_{1024};
    size_t max_pods_{8};
    std::unique_ptr<PodLogDisk> disk_;
};

// Renders a log line the way kubectl expects: optional RFC 3339 prefix.
// Without a wall clock the prefix is the node uptime ("+123.456s") so the
// output never carries an invented date.
std::string format_log_line(const PodLogRecord& rec, bool timestamps);

} // namespace kube32::runtime
