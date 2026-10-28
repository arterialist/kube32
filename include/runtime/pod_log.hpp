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
//
// Every ring has a byte budget (2 KiB on the device by default); the oldest
// records are evicted when a new one does not fit. Records carry a per-ring
// sequence number so followers and the shipping path never duplicate lines.

#include "osal/mutex.hpp"

#include <cstdint>
#include <deque>
#include <map>
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
    explicit PodLogRing(size_t capacity_bytes) : capacity_(capacity_bytes) {}

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
    // process lifetime; a new epoch resets the duplicate filter. Returns true
    // when the record was appended (false for duplicates / replays).
    bool accept_shipped(const std::string& pod_key, uint64_t epoch, const PodLogRecord& rec);

    bool exists(const std::string& pod_key) const;
    bool read_after(const std::string& pod_key, uint64_t after_seq, std::vector<PodLogRecord>& out, size_t max_records = 0) const;
    bool tail(const std::string& pod_key, size_t count, std::vector<PodLogRecord>& out) const;
    uint64_t last_seq(const std::string& pod_key) const;
    uint64_t dropped(const std::string& pod_key) const;
    void remove(const std::string& pod_key);
    std::vector<std::string> pods() const;
    void clear();

private:
    PodLogStore() = default;
    struct Entry {
        PodLogRing ring;
        Entry& operator=(Entry&&) = default;
        Entry(Entry&&) = default;
        uint64_t last_write_ms{0};
        uint64_t shipped_epoch{0};
        uint64_t shipped_seq{0};
        explicit Entry(size_t cap) : ring(cap) {}
    };
    Entry& entry_locked(const std::string& pod_key);

    mutable osal::Mutex mutex_;
    std::map<std::string, Entry> rings_;
    size_t ring_capacity_{2048};
    size_t max_pods_{8};
};

// Renders a log line the way kubectl expects: optional RFC 3339 prefix.
// Without a wall clock the prefix is the node uptime ("+123.456s") so the
// output never carries an invented date.
std::string format_log_line(const PodLogRecord& rec, bool timestamps);

} // namespace kube32::runtime
