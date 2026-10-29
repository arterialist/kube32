#include "runtime/pod_log.hpp"
#include "hal/node_storage.hpp"
#include "models/kube_models.hpp"
#include "osal/condition_variable.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

std::string pod_log_storage_root() {
    return hal::node_storage_root();
}

// ---------------------------------------------------------------------------
// Disk writer: one thread does every file operation, in the order requested
// ---------------------------------------------------------------------------
class PodLogDisk {
public:
    PodLogDisk(std::function<std::string()> root, size_t limit, std::function<void()> collect)
        : root_(std::move(root)), limit_(limit < 1024 ? 1024 : limit), collect_(std::move(collect)) {}

    void append(const std::string& dir, const std::string& file, std::string text) { push(Op{Op::Append, dir, file, std::move(text), nullptr}); }
    void rotate(const std::string& dir, const std::string& file) { push(Op{Op::Rotate, dir, file, {}, nullptr}); }
    void remove_dir(const std::string& dir) { push(Op{Op::RemoveDir, dir, {}, {}, nullptr}); }
    // Starts the writer when it is not running (it collects buffered lines)
    void kick() { push_locked_start(); }
    // The node has a storage root right now
    bool available() const { return root_ && !root_().empty(); }

    // Reads `dir/file` on the writer thread. False when it does not exist.
    bool read(const std::string& dir, const std::string& file, std::string& out) {
        ReadSlot slot;
        push(Op{Op::Read, dir, file, {}, &slot});
        osal::UniqueLock<osal::Mutex> lock(mutex_);
        cv_.wait_for(lock, 5000, [&slot]() { return slot.done; });
        if (!slot.done) { slot.abandoned = true; return false; }
        out = std::move(slot.data);
        return slot.ok;
    }

    // Waits until every queued operation is done
    void sync() {
        osal::UniqueLock<osal::Mutex> lock(mutex_);
        cv_.wait_for(lock, 10000, [this]() { return ops_.empty() && !busy_; });
    }

private:
    struct ReadSlot {
        bool done{false};
        bool ok{false};
        bool abandoned{false};
        std::string data;
    };
    struct Op {
        enum Kind { Append, Rotate, RemoveDir, Read } kind;
        std::string dir;
        std::string file;
        std::string text;
        ReadSlot* slot;
    };

    void push(Op op) {
        {
            osal::LockGuard guard(mutex_);
            ops_.push_back(std::move(op));
        }
        push_locked_start();
        cv_.notify_all();
    }

    void push_locked_start() {
        bool start = false;
        {
            osal::LockGuard guard(mutex_);
            if (!running_) { running_ = true; start = true; }
        }
        if (!start) return;
        osal::Thread::Config cfg;
        cfg.name = "k32_logdisk";
#if defined(ESP_PLATFORM)
        cfg.stack_size = 6144;
        cfg.stack_in_psram = false;   // flash access disables the cache: internal RAM only
#else
        cfg.stack_size = 65536;
#endif
        cfg.priority = 3;
        if (!osal::Thread::create([](void* self) { static_cast<PodLogDisk*>(self)->run(); }, this, cfg)) {
            osal::LockGuard guard(mutex_);
            running_ = false;
            ops_.clear();
        }
    }

    void run() {
        uint64_t idle_since = osal::System::get_uptime_ms();
        for (;;) {
            Op op;
            {
                osal::UniqueLock<osal::Mutex> lock(mutex_);
                cv_.wait_for(lock, 2000, [this]() { return !ops_.empty(); });
                if (ops_.empty()) {
                    lock.unlock();
                    // Lines of quiet pods reach the disk after 10 s
                    collect_();
                    lock.lock();
                    if (ops_.empty()) {
                        if (osal::System::get_uptime_ms() - idle_since > 30000) {
                            running_ = false;
                            cv_.notify_all();
                            return;
                        }
                        continue;
                    }
                }
                op = std::move(ops_.front());
                ops_.pop_front();
                busy_ = true;
            }
            idle_since = osal::System::get_uptime_ms();
            hal::node_storage_call([&]() { execute(op); });
            {
                osal::LockGuard guard(mutex_);
                busy_ = false;
            }
            cv_.notify_all();
            osal::System::feed_watchdog();
        }
    }

    static bool make_dirs(const std::string& root, const std::string& dir) {
        std::string path = root + "/kube32";
        ::mkdir(path.c_str(), 0755);
        path += "/logs";
        ::mkdir(path.c_str(), 0755);
        path += "/" + dir;
        ::mkdir(path.c_str(), 0755);
        struct stat st;
        return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }

    // Keeps the newest half of a file over the limit, from a line start
    void trim(const std::string& path) {
        struct stat st;
        if (::stat(path.c_str(), &st) != 0 || static_cast<size_t>(st.st_size) <= limit_) return;
        const size_t keep = limit_ / 2;
        std::string tail(keep, '\0');
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return;
        ::lseek(fd, static_cast<off_t>(st.st_size - keep), SEEK_SET);
        ssize_t n = ::read(fd, &tail[0], keep);
        ::close(fd);
        if (n <= 0) return;
        tail.resize(static_cast<size_t>(n));
        size_t nl = tail.find('\n');
        if (nl != std::string::npos) tail.erase(0, nl + 1);
        const std::string tmp = path + ".tmp";
        fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return;
        bool ok = ::write(fd, tail.data(), tail.size()) == static_cast<ssize_t>(tail.size());
        ::close(fd);
        if (!ok) { ::unlink(tmp.c_str()); return; }
        ::unlink(path.c_str());
        ::rename(tmp.c_str(), path.c_str());
    }

    void execute(Op& op) {
        const std::string root = root_ ? root_() : std::string();
        const std::string dir = root.empty() ? std::string() : root + "/kube32/logs/" + op.dir;
        const std::string path = dir + "/" + op.file;
        switch (op.kind) {
            case Op::Append: {
                if (root.empty() || !make_dirs(root, op.dir)) return;
                int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd < 0) return;
                ssize_t w = ::write(fd, op.text.data(), op.text.size());
                ::close(fd);
                if (w > 0) trim(path);
                return;
            }
            case Op::Rotate: {
                if (root.empty()) return;
                const std::string prev = path + ".1";
                ::unlink(prev.c_str());
                ::rename(path.c_str(), prev.c_str());
                return;
            }
            case Op::RemoveDir: {
                if (root.empty()) return;
                if (DIR* d = ::opendir(dir.c_str())) {
                    std::vector<std::string> names;
                    while (struct dirent* de = ::readdir(d)) {
                        std::string name = de->d_name;
                        if (name != "." && name != "..") names.push_back(name);
                    }
                    ::closedir(d);
                    for (const auto& n : names) ::unlink((dir + "/" + n).c_str());
                }
                ::rmdir(dir.c_str());
                return;
            }
            case Op::Read: {
                std::string data;
                bool ok = false;
                if (!root.empty()) {
                    int fd = ::open(path.c_str(), O_RDONLY);
                    if (fd >= 0) {
                        ok = true;
                        char buf[512];
                        ssize_t n;
                        while ((n = ::read(fd, buf, sizeof(buf))) > 0 && data.size() < 2 * limit_) data.append(buf, static_cast<size_t>(n));
                        ::close(fd);
                    }
                }
                osal::LockGuard guard(mutex_);
                if (!op.slot->abandoned) {
                    op.slot->data = std::move(data);
                    op.slot->ok = ok;
                    op.slot->done = true;
                }
                return;
            }
        }
    }

    std::function<std::string()> root_;
    size_t limit_;
    std::function<void()> collect_;
    osal::Mutex mutex_;
    osal::ConditionVariable cv_;
    std::deque<Op> ops_;
    bool running_{false};
    bool busy_{false};
};

static std::string log_dir_name(const std::string& pod_key) {
    std::string d = pod_key;
    size_t slash = d.find('/');
    if (slash != std::string::npos) d[slash] = '_';
    return d;
}

static std::string log_file_name(const std::string& pod_key, const std::string& container) {
    if (!container.empty()) return container + ".log";
    size_t slash = pod_key.find('/');
    return (slash == std::string::npos ? pod_key : pod_key.substr(slash + 1)) + ".log";
}

PodLogStore::PodLogStore() = default;
PodLogStore::~PodLogStore() = default;

PodLogStore& PodLogStore::runtime() {
    static PodLogStore s;
    return s;
}

// The API server's mirror is what kubectl reads: it persists to the node's
// storage root when there is one.
PodLogStore& PodLogStore::apiserver() {
    static PodLogStore* s = []() {
        auto* store = new PodLogStore();
        store->enable_persistence(pod_log_storage_root);
        return store;
    }();
    return *s;
}

void PodLogStore::enable_persistence(std::function<std::string()> root, size_t file_limit_bytes) {
    osal::LockGuard guard(mutex_);
    disk_ = std::make_unique<PodLogDisk>(std::move(root), file_limit_bytes, [this]() { collect_pending(10000); });
}

bool PodLogStore::persistence_enabled() const {
    osal::LockGuard guard(mutex_);
    return disk_ != nullptr && disk_->available();
}

void PodLogStore::hand_to_disk_locked(const std::string& pod_key, Entry& e) {
    if (!disk_ || e.disk_pending.empty()) return;
    disk_->append(log_dir_name(pod_key), log_file_name(pod_key, e.container), std::move(e.disk_pending));
    e.disk_pending.clear();
}

void PodLogStore::collect_pending(uint64_t min_age_ms) {
    osal::LockGuard guard(mutex_);
    const uint64_t now = osal::System::get_uptime_ms();
    for (auto& kv : rings_) {
        if (!kv.second.disk_pending.empty() && now - kv.second.disk_pending_since_ms >= min_age_ms) hand_to_disk_locked(kv.first, kv.second);
    }
}

void PodLogStore::flush_persisted() {
    collect_pending(0);
    PodLogDisk* disk = nullptr;
    {
        osal::LockGuard guard(mutex_);
        disk = disk_.get();
    }
    if (disk) disk->sync();
}

void PodLogStore::remove_persisted(const std::string& pod_key) {
    osal::LockGuard guard(mutex_);
    if (!disk_) return;
    auto it = rings_.find(pod_key);
    if (it != rings_.end()) it->second.disk_pending.clear();
    disk_->remove_dir(log_dir_name(pod_key));
}

bool PodLogStore::read_persisted(const std::string& pod_key, const std::string& container, bool previous, size_t tail,
                                 std::vector<PodLogRecord>& out) {
    PodLogDisk* disk = nullptr;
    {
        osal::LockGuard guard(mutex_);
        disk = disk_.get();
        auto it = rings_.find(pod_key);
        if (disk && it != rings_.end()) hand_to_disk_locked(pod_key, it->second);   // the file is complete when read
    }
    if (!disk || !disk->available()) return false;
    std::string data;
    if (!disk->read(log_dir_name(pod_key), log_file_name(pod_key, container) + (previous ? ".1" : ""), data)) return false;
    // Lines as format_log_line(rec, true) wrote them: "<RFC 3339 or +uptime> text"
    std::vector<PodLogRecord> recs;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) nl = data.size();
        std::string_view line(data.data() + pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;
        PodLogRecord r;
        r.seq = recs.size() + 1;
        size_t sp = line.find(' ');
        std::string_view stamp = line.substr(0, sp == std::string_view::npos ? line.size() : sp);
        bool stamped = false;
        if (!stamp.empty() && stamp[0] == '+' && stamp.back() == 's') {
            double secs = std::strtod(std::string(stamp.substr(1, stamp.size() - 2)).c_str(), nullptr);
            r.uptime_ms = static_cast<uint64_t>(secs * 1000.0 + 0.5);
            stamped = true;
        } else {
            stamped = models::parse_rfc3339(stamp, r.unix_ms);
        }
        r.text = std::string(stamped && sp != std::string_view::npos ? line.substr(sp + 1) : (stamped ? std::string_view() : line));
        recs.push_back(std::move(r));
    }
    size_t start = (tail == 0 || tail >= recs.size()) ? 0 : recs.size() - tail;
    for (size_t i = start; i < recs.size(); ++i) out.push_back(std::move(recs[i]));
    return true;
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

void PodLogStore::set_previous_capacity(size_t bytes) {
    osal::LockGuard guard(mutex_);
    previous_capacity_ = bytes < 256 ? 256 : bytes;
}

// The current log becomes the previous container's log (its newest lines up
// to the previous budget); the new current log continues the sequence so log
// followers see one gapless stream.
void PodLogStore::start_instance_locked(const std::string& pod_key, Entry& e, uint32_t instance) {
    if (disk_) {
        hand_to_disk_locked(pod_key, e);
        disk_->rotate(log_dir_name(pod_key), log_file_name(pod_key, e.container));
    }
    auto prev = std::make_unique<PodLogRing>(previous_capacity_);
    std::vector<PodLogRecord> recs;
    e.ring.read_after(0, recs);
    for (const auto& r : recs) prev->append(r.text, r.unix_ms, r.uptime_ms, r.seq);
    e.previous = std::move(prev);
    e.ring = PodLogRing(e.ring.capacity(), e.ring.last_seq() + 1);
    e.instance = instance;
}

bool PodLogStore::accept_shipped(const std::string& pod_key, uint64_t epoch, const PodLogRecord& rec, uint32_t instance, std::string_view pod_uid,
                                 std::string_view container) {
    osal::LockGuard guard(mutex_);
    Entry& e = entry_locked(pod_key);
    if (!container.empty() && e.container.empty()) e.container.assign(container.data(), container.size());
    if (!pod_uid.empty() && e.pod_uid != pod_uid) {
        // A new pod object under this name: nothing kept so far is its log
        if (!e.pod_uid.empty()) {
            e.disk_pending.clear();
            if (disk_) disk_->remove_dir(log_dir_name(pod_key));
            e.ring = PodLogRing(e.ring.capacity(), e.ring.last_seq() + 1);
            e.previous.reset();
            e.shipped_epoch = 0;
            e.shipped_seq = 0;
            e.instance = 0;
        }
        e.pod_uid.assign(pod_uid.data(), pod_uid.size());
    }
    if (e.shipped_epoch == epoch && rec.seq <= e.shipped_seq) return false;
    if (e.shipped_epoch != epoch) {
        // A new kubelet lifetime restarted the container: what came before
        // is the previous container's log.
        if (e.shipped_epoch != 0) start_instance_locked(pod_key, e, instance);
        e.shipped_epoch = epoch;
        e.shipped_seq = 0;
        e.instance = instance;
    } else if (instance > e.instance) {
        start_instance_locked(pod_key, e, instance);
    }
    e.shipped_seq = rec.seq;
    e.last_write_ms = osal::System::get_uptime_ms();
    if (instance < e.instance) {
        // A late line of the container that ran before the restart
        if (e.previous) e.previous->append(rec.text, rec.unix_ms, rec.uptime_ms, e.previous->last_seq() + 1);
        if (disk_ && disk_->available()) disk_->append(log_dir_name(pod_key), log_file_name(pod_key, e.container) + ".1", format_log_line(rec, true));
        return true;
    }
    // The mirror assigns its own sequence so followers see a gapless stream
    e.ring.append(rec.text, rec.unix_ms, rec.uptime_ms);
    if (disk_ && disk_->available()) {
        if (e.disk_pending.empty()) e.disk_pending_since_ms = e.last_write_ms;
        e.disk_pending += format_log_line(rec, true);
        if (e.disk_pending.size() >= 1024) hand_to_disk_locked(pod_key, e);
        else disk_->kick();
    }
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

bool PodLogStore::tail_previous(const std::string& pod_key, size_t count, std::vector<PodLogRecord>& out) const {
    osal::LockGuard guard(mutex_);
    auto it = rings_.find(pod_key);
    if (it == rings_.end() || !it->second.previous) return false;
    it->second.previous->tail(count, out);
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
