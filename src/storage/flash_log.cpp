#include "storage/flash_log.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(KUBE32_TARGET_ESP32)
#include "esp_partition.h"
#endif

namespace kube32::storage {

namespace {

constexpr uint32_t kSectorMagic = 0x4C32334Bu;   // "K32L"
constexpr uint8_t kLogVersion = 1;
constexpr uint16_t kRecordMagic = 0x4B52u;
constexpr uint16_t kNoRecordStart = 0xFFFFu;
constexpr size_t kMaxKey = 1024;
const std::string kRevisionKey("\x01revision", 9);

// CRC-32 (IEEE 802.3), four bits at a time: a 64-byte table instead of 1 KB.
uint32_t crc32_update(uint32_t crc, const void* data, size_t len) {
    static constexpr uint32_t kTable[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        crc = (crc >> 4) ^ kTable[crc & 0x0F];
        crc = (crc >> 4) ^ kTable[crc & 0x0F];
    }
    return ~crc;
}

void put16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) { uint32_t v = 0; for (int i = 3; i >= 0; --i) v = (v << 8) | p[i]; return v; }
uint64_t get64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; }

size_t align4(size_t n) { return (n + 3) & ~static_cast<size_t>(3); }

uint64_t fnv1a(uint64_t h, const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) { h ^= static_cast<unsigned char>(p[i]); h *= 1099511628211ull; }
    return h;
}

} // namespace

uint64_t value_fingerprint(const std::string& json) {
    uint64_t h = 1469598103934665603ull;
    size_t at = json.find("\"resourceVersion\"");
    if (at == std::string::npos) return fnv1a(h, json.data(), json.size());
    size_t p = at + 17;
    while (p < json.size() && (json[p] == ' ' || json[p] == ':')) ++p;
    size_t end = p;
    if (end < json.size() && json[end] == '"') {
        ++end;
        while (end < json.size() && json[end] != '"') { if (json[end] == '\\') ++end; ++end; }
        if (end < json.size()) ++end;
    } else {
        while (end < json.size() && json[end] != ',' && json[end] != '}') ++end;
    }
    h = fnv1a(h, json.data(), p);
    return fnv1a(h, json.data() + end, json.size() - end);
}

uint64_t FlashLog::hash_key(const std::string& key) {
    return fnv1a(1469598103934665603ull, key.data(), key.size());
}

// ---------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------

RamFlashRegion::RamFlashRegion(size_t size, size_t sector, bool nor) : data_(size, 0xFF), sector_(sector), nor_(nor) {}

bool RamFlashRegion::read(size_t offset, void* dst, size_t len) {
    if (read_error_ || offset + len > data_.size()) return false;
    std::memcpy(dst, data_.data() + offset, len);
    return true;
}

bool RamFlashRegion::write(size_t offset, const void* src, size_t len) {
    if (offset + len > data_.size()) return false;
    size_t n = len;
    if (budget_ >= 0) {
        n = std::min<size_t>(len, static_cast<size_t>(budget_));
        budget_ -= static_cast<long long>(n);
    }
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; ++i) data_[offset + i] = nor_ ? (data_[offset + i] & s[i]) : s[i];
    return true;
}

bool RamFlashRegion::erase_sector(size_t index) {
    if ((index + 1) * sector_ > data_.size()) return false;
    if (budget_ == 0 || !nor_) return true;
    std::memset(data_.data() + index * sector_, 0xFF, sector_);
    ++erases_;
    return true;
}

namespace {

class FileFlashRegion : public FlashRegion {
public:
    FileFlashRegion(int fd, size_t size) : fd_(fd), size_(size) {}
    ~FileFlashRegion() override { if (fd_ >= 0) ::close(fd_); }
    size_t size() const override { return size_; }
    bool erase_required() const override { return false; }
    bool read(size_t offset, void* dst, size_t len) override {
        if (offset + len > size_) return false;
        return ::pread(fd_, dst, len, static_cast<off_t>(offset)) == static_cast<ssize_t>(len);
    }
    bool write(size_t offset, const void* src, size_t len) override {
        if (offset + len > size_) return false;
        return ::pwrite(fd_, src, len, static_cast<off_t>(offset)) == static_cast<ssize_t>(len);
    }
    bool erase_sector(size_t) override { return true; }
    bool sync() override { return ::fsync(fd_) == 0; }

private:
    int fd_;
    size_t size_;
};

} // namespace

std::unique_ptr<FlashRegion> create_file_flash_region(const std::string& path, size_t size) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return nullptr;
    struct stat st;
    if (::fstat(fd, &st) != 0) { ::close(fd); return nullptr; }
    // Seeking past the end and writing the last byte allocates the space
    // without writing it (ftruncate would zero-fill: a megabyte of flash
    // writes); what the gap reads back is never taken for log data
    if (static_cast<size_t>(st.st_size) < size) {
        const char zero = 0;
        if (::lseek(fd, static_cast<off_t>(size - 1), SEEK_SET) != static_cast<off_t>(size - 1) || ::write(fd, &zero, 1) != 1 ||
            ::fsync(fd) != 0) {
            ::close(fd);
            return nullptr;
        }
    }
    return std::make_unique<FileFlashRegion>(fd, size);
}

#if defined(KUBE32_TARGET_ESP32)

namespace {

class PartitionFlashRegion : public FlashRegion {
public:
    PartitionFlashRegion(const esp_partition_t* part, size_t size) : part_(part), size_(size) {}
    size_t size() const override { return size_; }
    bool read(size_t offset, void* dst, size_t len) override {
        return offset + len <= size_ && esp_partition_read(part_, offset, dst, len) == ESP_OK;
    }
    bool write(size_t offset, const void* src, size_t len) override {
        return offset + len <= size_ && esp_partition_write(part_, offset, src, len) == ESP_OK;
    }
    bool erase_sector(size_t index) override {
        const size_t sector = sector_size();
        return (index + 1) * sector <= size_ && esp_partition_erase_range(part_, index * sector, sector) == ESP_OK;
    }

private:
    const esp_partition_t* part_;
    size_t size_;
};

} // namespace

#endif

std::unique_ptr<FlashRegion> create_partition_flash_region(const std::string& label, size_t size) {
#if defined(KUBE32_TARGET_ESP32)
    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label.c_str());
    // An encrypted partition takes 16-byte aligned writes only
    if (!part || part->encrypted || part->erase_size != 4096) return nullptr;
    const size_t bytes = std::min<size_t>(size, part->size) / 4096 * 4096;
    if (bytes == 0) return nullptr;
    return std::make_unique<PartitionFlashRegion>(part, bytes);
#else
    (void)label;
    (void)size;
    return nullptr;
#endif
}


// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

FlashLog::FlashLog(FlashRegion& region, size_t max_record_bytes)
    : region_(region), sector_(region.sector_size()), sectors_(region.size() / region.sector_size()) {
    payload_ = sector_ - kSectorHeader;
    max_record_ = max_record_bytes;
    // Reclaiming a sector may move a record of up to max_record_bytes before
    // the sector it frees can be reused
    reserve_sectors_ = (max_record_bytes + kRecordHeader + payload_ - 1) / payload_ + 2;
    counters_.region_bytes = region.size();
    counters_.sector_count = sectors_;
}

FlashLog::IndexEntry* FlashLog::find(uint64_t key_hash) {
    auto it = std::lower_bound(index_.begin(), index_.end(), key_hash,
                               [](const IndexEntry& e, uint64_t h) { return e.key_hash < h; });
    return (it != index_.end() && it->key_hash == key_hash) ? &*it : nullptr;
}

const FlashLog::IndexEntry* FlashLog::find(uint64_t key_hash) const {
    auto it = std::lower_bound(index_.begin(), index_.end(), key_hash,
                               [](const IndexEntry& e, uint64_t h) { return e.key_hash < h; });
    return (it != index_.end() && it->key_hash == key_hash) ? &*it : nullptr;
}

void FlashLog::index_set(const IndexEntry& e) {
    auto it = std::lower_bound(index_.begin(), index_.end(), e.key_hash,
                               [](const IndexEntry& x, uint64_t h) { return x.key_hash < h; });
    if (it != index_.end() && it->key_hash == e.key_hash) {
        if (it->type != static_cast<uint8_t>(RecordType::Delete)) live_bytes_ -= it->bytes;
        *it = e;
    } else {
        index_.insert(it, e);
    }
    if (e.type != static_cast<uint8_t>(RecordType::Delete)) live_bytes_ += e.bytes;
}

void FlashLog::index_erase(uint64_t key_hash) {
    auto it = std::lower_bound(index_.begin(), index_.end(), key_hash,
                               [](const IndexEntry& x, uint64_t h) { return x.key_hash < h; });
    if (it == index_.end() || it->key_hash != key_hash) return;
    if (it->type != static_cast<uint8_t>(RecordType::Delete)) live_bytes_ -= it->bytes;
    index_.erase(it);
}

size_t FlashLog::used_sectors() const {
    return have_head_ ? static_cast<size_t>(head_seq_ - tail_seq_ + 1) : 0;
}

size_t FlashLog::capacity_bytes() const {
    // Half of what is left after the reserve: reclamation then moves at most
    // one live byte per byte it frees, on average.
    size_t usable = sectors_ > reserve_sectors_ + 2 ? sectors_ - reserve_sectors_ - 2 : 0;
    return usable * payload_ / 2;
}

bool FlashLog::read_region(size_t offset, void* dst, size_t len) {
    if (region_.read(offset, dst, len)) return true;
    read_error_ = true;
    return false;
}

bool FlashLog::read_header(size_t index, SectorHeader& out) {
    uint8_t b[kSectorHeader];
    if (!read_region(index * sector_, b, sizeof(b))) return false;
    out.magic = get32(b);
    out.seq = get32(b + 4);
    out.first = get16(b + 8);
    out.version = b[10];
    out.reserved = b[11];
    out.crc = get32(b + 12);
    return out.magic == kSectorMagic && out.version == kLogVersion && out.crc == crc32_update(0, b, 12) &&
           phys(out.seq) == index;
}

bool FlashLog::open_sector(uint32_t seq, size_t first) {
    const size_t index = phys(seq);
    if (region_.erase_required()) {
        // Reclaimed sectors are erased already; anything else is erased now
        bool erased = true;
        uint8_t buf[256];
        for (size_t off = 0; off < sector_ && erased; off += sizeof(buf)) {
            if (!region_.read(index * sector_ + off, buf, sizeof(buf))) return false;
            for (uint8_t c : buf) if (c != 0xFF) { erased = false; break; }
        }
        if (!erased) {
            if (!region_.erase_sector(index)) return false;
            counters_.sectors_erased++;
        }
    }
    uint8_t h[kSectorHeader];
    put32(h, kSectorMagic);
    put32(h + 4, seq);
    put16(h + 8, static_cast<uint16_t>(first));
    h[10] = kLogVersion;
    h[11] = 0xFF;
    put32(h + 12, crc32_update(0, h, 12));
    if (!region_.write(index * sector_, h, sizeof(h))) return false;
    if (!have_head_) { tail_seq_ = seq; have_head_ = true; }
    head_seq_ = seq;
    head_off_ = kSectorHeader;
    return true;
}

bool FlashLog::ensure_space(size_t record_bytes) {
    for (size_t guard = 0; guard <= sectors_; ++guard) {
        size_t avail = have_head_ ? sector_ - head_off_ : 0;
        size_t extra = record_bytes > avail ? (record_bytes - avail + payload_ - 1) / payload_ : 0;
        size_t free_sectors = sectors_ - used_sectors();
        size_t want = extra + (reclaiming_ ? 0 : reserve_sectors_);
        if (free_sectors >= want && free_sectors >= extra) return true;
        if (reclaiming_ || !reclaim_tail()) return false;
    }
    return false;
}

bool FlashLog::stream_read(uint32_t& seq, size_t& off, void* dst, size_t len) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    while (len > 0) {
        if (off >= sector_) {
            if (!have_head_ || seq + 1 > head_seq_) return false;
            ++seq;
            off = kSectorHeader;
        }
        size_t n = std::min(len, sector_ - off);
        if (!read_region(phys(seq) * sector_ + off, d, n)) return false;
        d += n;
        off += n;
        len -= n;
    }
    return true;
}

bool FlashLog::stream_write(const void* src, size_t len, size_t record_remaining) {
    const uint8_t* s = static_cast<const uint8_t*>(src);
    while (len > 0) {
        if (head_off_ >= sector_) {
            // The record continues here: the next record starts after its tail
            size_t first = record_remaining < payload_ ? kSectorHeader + record_remaining : kNoRecordStart;
            if (!open_sector(head_seq_ + 1, first)) return false;
        }
        size_t n = std::min(len, sector_ - head_off_);
        if (!region_.write(phys(head_seq_) * sector_ + head_off_, s, n)) return false;
        s += n;
        head_off_ += n;
        len -= n;
        record_remaining -= n;
    }
    return true;
}

bool FlashLog::append_record(RecordType type, const std::string& key, const std::string& value, uint64_t revision,
                             uint64_t fingerprint, bool moving) {
    if (unreadable_ || key.size() > kMaxKey || key.size() + value.size() > max_record_) return false;
    const size_t body = kRecordHeader + key.size() + value.size();
    const size_t total = align4(body);
    if (!moving && type == RecordType::Put) {
        const IndexEntry* old = find(hash_key(key));
        size_t old_bytes = (old && old->type != static_cast<uint8_t>(RecordType::Delete)) ? old->bytes : 0;
        if (live_bytes_ - old_bytes + total > capacity_bytes()) return false;
    }
    if (!ensure_space(total)) return false;
    // A record starts at the beginning of a sector when the head one is full
    if (!have_head_ || head_off_ >= sector_) {
        if (!open_sector(have_head_ ? head_seq_ + 1 : 1, kSectorHeader)) return false;
    }
    const uint32_t start_seq = head_seq_;
    const size_t start_off = head_off_;

    uint8_t h[kRecordHeader];
    put16(h, kRecordMagic);
    h[2] = static_cast<uint8_t>(type);
    h[3] = 0;
    put16(h + 4, static_cast<uint16_t>(key.size()));
    put16(h + 6, 0);
    put32(h + 8, static_cast<uint32_t>(value.size()));
    put64(h + 12, revision);
    uint8_t seqb[4];
    put32(seqb, start_seq);
    uint32_t crc = crc32_update(0, seqb, 4);
    crc = crc32_update(crc, h, 20);
    crc = crc32_update(crc, key.data(), key.size());
    crc = crc32_update(crc, value.data(), value.size());
    put32(h + 20, crc);

    size_t remaining = total;
    if (!stream_write(h, sizeof(h), remaining)) return false;
    remaining -= sizeof(h);
    if (!stream_write(key.data(), key.size(), remaining)) return false;
    remaining -= key.size();
    if (!value.empty()) {
        if (!stream_write(value.data(), value.size(), remaining)) return false;
        remaining -= value.size();
    }
    if (remaining > 0) {
        static const uint8_t kPad[4] = {0, 0, 0, 0};
        if (!stream_write(kPad, remaining, remaining)) return false;
    }

    IndexEntry e;
    e.key_hash = hash_key(key);
    e.pos = pos_of(start_seq, start_off);
    e.fingerprint = fingerprint;
    e.bytes = static_cast<uint32_t>(total);
    e.type = static_cast<uint8_t>(type);
    index_set(e);
    counters_.records_written++;
    counters_.bytes_written += total;
    if (moving) counters_.records_moved++;
    return true;
}

bool FlashLog::read_record_at(uint32_t seq, size_t off, RecordType& type, std::string& key, std::string& value,
                              uint64_t& revision, size_t& total, uint32_t& end_seq, size_t& end_off) {
    uint8_t seqb[4];
    put32(seqb, seq);
    uint8_t h[kRecordHeader];
    if (!stream_read(seq, off, h, sizeof(h))) return false;
    if (get16(h) != kRecordMagic) return false;
    const uint8_t t = h[2];
    if (t < 1 || t > 3) return false;
    const size_t klen = get16(h + 4);
    const size_t vlen = get32(h + 8);
    if (klen > kMaxKey || klen + vlen > max_record_) return false;
    key.resize(klen);
    value.resize(vlen);
    if (klen && !stream_read(seq, off, &key[0], klen)) return false;
    if (vlen && !stream_read(seq, off, &value[0], vlen)) return false;
    uint32_t crc = crc32_update(0, seqb, 4);
    crc = crc32_update(crc, h, 20);
    crc = crc32_update(crc, key.data(), klen);
    crc = crc32_update(crc, value.data(), vlen);
    if (crc != get32(h + 20)) return false;
    total = align4(kRecordHeader + klen + vlen);
    size_t pad = total - (kRecordHeader + klen + vlen);
    uint8_t padb[4];
    if (pad && !stream_read(seq, off, padb, pad)) return false;
    if (off >= sector_ && seq < head_seq_) { ++seq; off = kSectorHeader; }
    type = static_cast<RecordType>(t);
    revision = get64(h + 12);
    end_seq = seq;
    end_off = off;
    return true;
}

bool FlashLog::reclaim_tail() {
    if (!have_head_ || tail_seq_ >= head_seq_) return false;
    reclaiming_ = true;
    read_error_ = false;
    const uint32_t seq = tail_seq_;
    bool moved = false;
    SectorHeader hdr;
    if (read_header(phys(seq), hdr) && hdr.seq == seq && hdr.first != kNoRecordStart) {
        size_t off = hdr.first;
        std::string key, value;
        while (off < sector_) {
            RecordType type;
            uint64_t revision = 0;
            size_t total = 0;
            uint32_t end_seq = 0;
            size_t end_off = 0;
            if (!read_record_at(seq, off, type, key, value, revision, total, end_seq, end_off)) break;
            const uint64_t kh = hash_key(key);
            const IndexEntry* e = find(kh);
            if (e && e->pos == pos_of(seq, off)) {
                if (type == RecordType::Delete) {
                    // Nothing older than the oldest sector remains to be shadowed
                    index_erase(kh);
                } else if (!append_record(type, key, value, revision, e->fingerprint, true)) {
                    reclaiming_ = false;
                    return false;
                } else {
                    moved = true;
                }
            }
            if (end_seq != seq) break;   // the record ran into the next sector
            off = end_off;
        }
    }
    // A record that could not be read was not moved: the sector stays
    if (read_error_) { reclaiming_ = false; return false; }
    // The copies are durable before the sector they replace can be erased or
    // written over (a file may reach the disk out of order)
    if (moved && !region_.sync()) { reclaiming_ = false; return false; }
    // On NOR flash a reclaimed sector is erased at once, so recovery never
    // finds it; elsewhere its stale records are older than every copy made
    if (region_.erase_required()) {
        if (!region_.erase_sector(phys(seq))) { reclaiming_ = false; return false; }
        counters_.sectors_erased++;
    }
    tail_seq_ = seq + 1;
    // Sectors that only continue a record that started earlier hold nothing to keep
    while (tail_seq_ < head_seq_) {
        SectorHeader next;
        if (read_header(phys(tail_seq_), next) && next.first != kNoRecordStart) break;
        if (region_.erase_required()) {
            if (!region_.erase_sector(phys(tail_seq_))) break;
            counters_.sectors_erased++;
        }
        ++tail_seq_;
    }
    reclaiming_ = false;
    return true;
}

bool FlashLog::newest_sector(uint32_t& head) {
    bool found = false;
    head = 0;
    for (size_t i = 0; i < sectors_; ++i) {
        SectorHeader h;
        if (!read_header(i, h)) continue;
        if (!found || h.seq > head) head = h.seq;
        found = true;
    }
    return found;
}

bool FlashLog::recover(const ApplyFn& apply, uint64_t& max_revision) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    index_.clear();
    live_bytes_ = 0;
    have_head_ = false;
    max_revision = 0;
    revision_mark_ = 0;
    read_error_ = false;
    unreadable_ = false;

    // The newest valid sector and the run of consecutive sequence numbers
    // that ends there are the log; any other valid sector is left over from a
    // broken run and gets erased when its slot is reused.
    uint32_t head = 0;
    const bool found = newest_sector(head);
    if (read_error_) { unreadable_ = true; return false; }
    if (!found) return true;
    uint32_t tail = head;
    while (tail > 0 && head - tail + 1 < sectors_) {
        SectorHeader h;
        if (!read_header(phys(tail - 1), h) || h.seq != tail - 1) break;
        --tail;
    }
    have_head_ = true;
    head_seq_ = head;
    tail_seq_ = tail;

    std::string key, value;
    uint32_t seq = tail;
    size_t off = 0;
    bool positioned = false;
    while (seq <= head) {
        if (!positioned) {
            SectorHeader h;
            if (!read_header(phys(seq), h) || h.first == kNoRecordStart) { ++seq; continue; }
            off = h.first;
            positioned = true;
        }
        if (off >= sector_) { ++seq; positioned = false; continue; }
        RecordType type;
        uint64_t revision = 0;
        size_t total = 0;
        uint32_t end_seq = 0;
        size_t end_off = 0;
        if (!read_record_at(seq, off, type, key, value, revision, total, end_seq, end_off)) {
            // Erased space or a torn write: this run of records ends here and
            // the next one starts in a later sector
            ++seq;
            positioned = false;
            continue;
        }
        if (revision > max_revision) max_revision = revision;
        IndexEntry e;
        e.key_hash = hash_key(key);
        e.pos = pos_of(seq, off);
        e.bytes = static_cast<uint32_t>(total);
        e.type = static_cast<uint8_t>(type);
        e.fingerprint = (type == RecordType::Put) ? value_fingerprint(value) : 0;
        if (type == RecordType::RevisionMark) {
            if (revision > revision_mark_) revision_mark_ = revision;
            index_set(e);
        } else {
            index_set(e);
            apply(type, key, value, revision);
        }
        seq = end_seq;
        off = end_off;
        if (end_off >= sector_) { ++seq; positioned = false; }
    }
    if (read_error_) {
        index_.clear();
        live_bytes_ = 0;
        unreadable_ = true;
        return false;
    }
    // Appends continue in a fresh sector: the tail end of the newest one may
    // hold a torn record that must not be extended
    head_off_ = sector_;
    return true;
}

bool FlashLog::copy_log(FlashRegion& from, FlashRegion& to, bool& copied) {
    copied = false;
    if (from.size() != to.size() || from.sector_size() != to.sector_size()) return false;
    FlashLog src(from, 0);
    FlashLog dst(to, 0);
    uint32_t src_head = 0, dst_head = 0;
    const bool src_log = src.newest_sector(src_head);
    const bool dst_log = dst.newest_sector(dst_head);
    if (src.read_error_ || dst.read_error_) return false;
    if (!src_log || (dst_log && dst_head > src_head)) return true;
    auto erased = [](const std::vector<uint8_t>& b) {
        return std::all_of(b.begin(), b.end(), [](uint8_t c) { return c == 0xFF; });
    };
    std::vector<uint8_t> buf(from.sector_size()), back(from.sector_size());
    for (size_t i = 0; i < src.sectors_; ++i) {
        const size_t at = i * buf.size();
        if (!from.read(at, buf.data(), buf.size()) || !to.read(at, back.data(), back.size())) return false;
        if (back == buf) continue;   // copied by an earlier call, or both erased
        if (to.erase_required() && !erased(back) && !to.erase_sector(i)) return false;
        if (to.erase_required() && erased(buf)) continue;
        if (!to.write(at, buf.data(), buf.size())) return false;
        if (!to.read(at, back.data(), back.size()) || back != buf) return false;
    }
    if (!to.sync()) return false;
    copied = true;
    return true;
}

bool FlashLog::append_put(const std::string& key, const std::string& value, uint64_t revision, uint64_t fingerprint) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return append_record(RecordType::Put, key, value, revision, fingerprint, false);
}

bool FlashLog::append_delete(const std::string& key, uint64_t revision) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const IndexEntry* e = find(hash_key(key));
    if (!e || e->type == static_cast<uint8_t>(RecordType::Delete)) return true;   // nothing on flash to shadow
    return append_record(RecordType::Delete, key, std::string(), revision, 0, false);
}

bool FlashLog::append_revision_mark(uint64_t revision) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!append_record(RecordType::RevisionMark, kRevisionKey, std::string(), revision, 0, false)) return false;
    revision_mark_ = revision;
    return true;
}

bool FlashLog::sync() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return region_.sync();
}

bool FlashLog::has_value(const std::string& key, uint64_t fingerprint) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const IndexEntry* e = find(hash_key(key));
    return e && e->type == static_cast<uint8_t>(RecordType::Put) && e->fingerprint == fingerprint;
}

bool FlashLog::has_key(const std::string& key) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const IndexEntry* e = find(hash_key(key));
    return e && e->type == static_cast<uint8_t>(RecordType::Put);
}

FlashLogStats FlashLog::stats() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    FlashLogStats s = counters_;
    s.used_sectors = used_sectors();
    s.live_bytes = live_bytes_;
    size_t keys = 0;
    for (const auto& e : index_) if (e.type == static_cast<uint8_t>(RecordType::Put)) ++keys;
    s.live_keys = keys;
    s.head_seq = head_seq_;
    s.tail_seq = tail_seq_;
    return s;
}

} // namespace kube32::storage
