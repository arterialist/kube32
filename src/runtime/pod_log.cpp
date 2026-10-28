#include "runtime/pod_log.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"

#include <cstdio>

namespace kube32::runtime {

void PodLogRing::append(std::string_view text, uint64_t unix_ms, uint64_t uptime_ms, uint64_t seq) {
    size_t need = text.size() + kRecordOverhead;
    if (need > capacity_) {
        // Keep the tail of an oversized line; the ring must still hold it
        size_t keep = capacity_ > kRecordOverhead ? capacity_ - kRecordOverhead : 0;
        text = text.substr(text.size() - keep);
        need = text.size() + kRecordOverhead;
    }
    while (!records_.empty() && used_ + need > capacity_) {
        used_ -= records_.front().text.size() + kRecordOverhead;
        records_.pop_front();
        ++dropped_;
    }
    PodLogRecord rec;
    rec.seq = (seq != 0) ? seq : next_seq_;
    if (rec.seq >= next_seq_) next_seq_ = rec.seq + 1;
    rec.unix_ms = unix_ms;
    rec.uptime_ms = uptime_ms;
    rec.text.assign(text.data(), text.size());
    used_ += need;
    records_.push_back(std::move(rec));
}

void PodLogRing::read_after(uint64_t after_seq, std::vector<PodLogRecord>& out, size_t max_records) const {
    for (const auto& r : records_) {
        if (r.seq <= after_seq) continue;
        out.push_back(r);
        if (max_records != 0 && out.size() >= max_records) break;
    }
}

void PodLogRing::tail(size_t count, std::vector<PodLogRecord>& out) const {
    size_t start = (count == 0 || count >= records_.size()) ? 0 : records_.size() - count;
    for (size_t i = start; i < records_.size(); ++i) out.push_back(records_[i]);
}

PodLogStore& PodLogStore::runtime() {
    static PodLogStore s;
    return s;
}

PodLogStore& PodLogStore::apiserver() {
    static PodLogStore s;
    return s;
}

void PodLogStore::set_ring_capacity(size_t bytes) {
    osal::LockGuard guard(mutex_);
    ring_capacity_ = bytes < 256 ? 256 : bytes;
}

size_t PodLogStore::ring_capacity() const {
    osal::LockGuard guard(mutex_);
    return ring_capacity_;
}

void PodLogStore::set_max_pods(size_t n) {
    osal::LockGuard guard(mutex_);
    max_pods_ = n == 0 ? 1 : n;
}

PodLogStore::Entry& PodLogStore::entry_locked(const std::string& pod_key) {
    auto it = rings_.find(pod_key);
    if (it == rings_.end()) {
        if (rings_.size() >= max_pods_) {
            auto victim = rings_.begin();
            for (auto e = rings_.begin(); e != rings_.end(); ++e) {
                if (e->second.last_write_ms < victim->second.last_write_ms) victim = e;
            }
            rings_.erase(victim);
        }
        it = rings_.emplace(pod_key, Entry(ring_capacity_)).first;
    }
    return it->second;
}

void PodLogStore::append(const std::string& pod_key, std::string_view text) {
    append(pod_key, text, osal::System::get_unix_time_ms(), osal::System::get_uptime_ms());
}

void PodLogStore::append(const std::string& pod_key, std::string_view text, uint64_t unix_ms, uint64_t uptime_ms) {
    osal::LockGuard guard(mutex_);
    Entry& e = entry_locked(pod_key);
    e.last_write_ms = osal::System::get_uptime_ms();
    e.ring.append(text, unix_ms, uptime_ms);
}

bool PodLogStore::accept_shipped(const std::string& pod_key, uint64_t epoch, const PodLogRecord& rec) {
    osal::LockGuard guard(mutex_);
    Entry& e = entry_locked(pod_key);
    if (e.shipped_epoch == epoch && rec.seq <= e.shipped_seq) return false;
    if (e.shipped_epoch != epoch) {
        // A new kubelet lifetime (reboot or a new pod with the same name):
        // the previous incarnation's lines are not this container's log.
        if (e.shipped_epoch != 0) e.ring = PodLogRing(e.ring.capacity());
        e.shipped_epoch = epoch;
        e.shipped_seq = 0;
    }
    e.shipped_seq = rec.seq;
    e.last_write_ms = osal::System::get_uptime_ms();
    // The mirror assigns its own sequence so followers see a gapless stream
    e.ring.append(rec.text, rec.unix_ms, rec.uptime_ms);
    return true;
}

bool PodLogStore::exists(const std::string& pod_key) const {
    osal::LockGuard guard(mutex_);
    return rings_.find(pod_key) != rings_.end();
}

bool PodLogStore::read_after(const std::string& pod_key, uint64_t after_seq, std::vector<PodLogRecord>& out, size_t max_records) const {
    osal::LockGuard guard(mutex_);
    auto it = rings_.find(pod_key);
    if (it == rings_.end()) return false;
    it->second.ring.read_after(after_seq, out, max_records);
    return true;
}

bool PodLogStore::tail(const std::string& pod_key, size_t count, std::vector<PodLogRecord>& out) const {
    osal::LockGuard guard(mutex_);
    auto it = rings_.find(pod_key);
    if (it == rings_.end()) return false;
    it->second.ring.tail(count, out);
    return true;
}

uint64_t PodLogStore::last_seq(const std::string& pod_key) const {
    osal::LockGuard guard(mutex_);
    auto it = rings_.find(pod_key);
    return it == rings_.end() ? 0 : it->second.ring.last_seq();
}

uint64_t PodLogStore::dropped(const std::string& pod_key) const {
    osal::LockGuard guard(mutex_);
    auto it = rings_.find(pod_key);
    return it == rings_.end() ? 0 : it->second.ring.dropped();
}

void PodLogStore::remove(const std::string& pod_key) {
    osal::LockGuard guard(mutex_);
    rings_.erase(pod_key);
}

std::vector<std::string> PodLogStore::pods() const {
    osal::LockGuard guard(mutex_);
    std::vector<std::string> out;
    out.reserve(rings_.size());
    for (const auto& kv : rings_) out.push_back(kv.first);
    return out;
}

void PodLogStore::clear() {
    osal::LockGuard guard(mutex_);
    rings_.clear();
}

std::string format_log_line(const PodLogRecord& rec, bool timestamps) {
    std::string line;
    line.reserve(rec.text.size() + 40);
    if (timestamps) {
        if (rec.unix_ms != 0) {
            // RFC 3339 with milliseconds, as the kubelet prints them
            std::string ts = models::format_rfc3339(rec.unix_ms);
            char frac[8];
            snprintf(frac, sizeof(frac), ".%03u", static_cast<unsigned>(rec.unix_ms % 1000ULL));
            ts.insert(ts.size() - 1, frac);
            line += ts;
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "+%llu.%03us", static_cast<unsigned long long>(rec.uptime_ms / 1000ULL),
                     static_cast<unsigned>(rec.uptime_ms % 1000ULL));
            line += buf;
        }
        line += ' ';
    }
    line += rec.text;
    line += '\n';
    return line;
}

} // namespace kube32::runtime
