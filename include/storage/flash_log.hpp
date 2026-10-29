#pragma once

// Log-structured persistence for the object store.
//
// The region (raw NOR flash, or a preallocated file on the node's storage
// root) is a ring of 4 KiB sectors. Each sector starts with a small
// header (magic, sequence number, offset of the first record that begins in
// it); records follow back to back and may continue into the next sector.
// A record is a put (key, value, revision), a delete (key, revision) or a
// revision mark (the highest revision the store may hand out before it writes
// the next mark). Every record carries a CRC32, so a write torn by a power cut
// is detected and ignored on recovery, and the last intact record of a key
// wins. The CRC also covers the sequence number of the sector a record starts
// in, so bytes left over from an earlier pass around the ring never read back
// as records, and a region that cannot erase (a file) needs no erasing.
//
// Space is reclaimed from the oldest sector: records in it that are still the
// latest version of their key are appended again at the head, and the sector
// becomes free. Writes therefore rotate through the whole region and a write
// costs appends rather than rewrites in place.
//
// On the ESP32-S3, internal flash access disables the instruction cache, so
// a region backed by internal flash must be used from a task whose stack is
// in internal RAM (hal/node_storage.hpp).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace kube32::storage {

// Storage behind the log. On NOR flash (erase_required) write() can only
// clear bits, so the log writes erased (0xFF) bytes only and erases a sector
// before reusing it; other regions overwrite in place.
class FlashRegion {
public:
    virtual ~FlashRegion() = default;
    virtual size_t size() const = 0;
    virtual size_t sector_size() const { return 4096; }
    virtual bool erase_required() const { return true; }
    virtual bool read(size_t offset, void* dst, size_t len) = 0;
    virtual bool write(size_t offset, const void* src, size_t len) = 0;
    virtual bool erase_sector(size_t index) = 0;
    // Makes completed writes durable (fsync for a file).
    virtual bool sync() { return true; }
};

// Memory-backed region for tests, NOR flash (`nor`) or a plain overwritable
// image. A write budget simulates a power cut: once `budget` bytes have been
// written, further writes and erases are dropped (partially for the write
// that crosses the limit). -1 means no limit. Reads can be made to fail, and
// syncs are counted.
class RamFlashRegion : public FlashRegion {
public:
    explicit RamFlashRegion(size_t size, size_t sector = 4096, bool nor = true);
    size_t size() const override { return data_.size(); }
    size_t sector_size() const override { return sector_; }
    bool erase_required() const override { return nor_; }
    bool read(size_t offset, void* dst, size_t len) override;
    bool write(size_t offset, const void* src, size_t len) override;
    bool erase_sector(size_t index) override;
    bool sync() override { ++syncs_; return true; }
    void set_write_budget(long long bytes) { budget_ = bytes; }
    // Bytes left before the cut; 0 once it happened
    long long write_budget() const { return budget_; }
    void set_read_error(bool fail) { read_error_ = fail; }
    std::vector<uint8_t>& bytes() { return data_; }
    uint64_t erase_count() const { return erases_; }
    uint64_t sync_count() const { return syncs_; }

private:
    std::vector<uint8_t> data_;
    size_t sector_;
    bool nor_;
    long long budget_{-1};
    bool read_error_{false};
    uint64_t erases_{0};
    uint64_t syncs_{0};
};

// A preallocated file (on the node's storage root, or a host directory). The
// file is extended to `size` without writing it; nothing is erased. On a FAT
// volume every sync also rewrites the file's directory entry.
std::unique_ptr<FlashRegion> create_file_flash_region(const std::string& path, size_t size);

// The data partition named `label` on the board's own flash, written raw: at
// most `size` bytes, NOR semantics, no filesystem between the log and the
// flash, so a reset in the middle of a write can only cut that write. nullptr
// when the partition table has no such partition, and always on the host.
std::unique_ptr<FlashRegion> create_partition_flash_region(const std::string& label, size_t size);

struct FlashLogStats {
    size_t region_bytes{0};
    size_t sector_count{0};
    size_t used_sectors{0};       // sectors between the oldest and the newest
    size_t live_bytes{0};         // bytes of records that are the latest version of their key
    size_t live_keys{0};
    uint64_t records_written{0};  // since boot, including records moved by reclamation
    uint64_t bytes_written{0};
    uint64_t records_moved{0};    // rewritten by reclamation
    uint64_t sectors_erased{0};
    uint32_t head_seq{0};
    uint32_t tail_seq{0};
};

class FlashLog {
public:
    enum class RecordType : uint8_t { Put = 1, Delete = 2, RevisionMark = 3 };

    // `max_record_bytes` bounds the key and value of one record; it sizes the
    // reserve kept free for reclamation.
    FlashLog(FlashRegion& region, size_t max_record_bytes);

    // Scans the region and calls `apply` for every key's latest record, put
    // or delete, in log order. Records torn by a power cut are skipped.
    // Afterwards appends go to a fresh sector. `max_revision` receives the
    // largest revision seen in any record, revision marks included. False
    // when the region could not be read: appends are then refused, since a
    // log started over unreadable sectors would replace what they hold.
    using ApplyFn = std::function<void(RecordType type, const std::string& key, std::string& value, uint64_t revision)>;
    bool recover(const ApplyFn& apply, uint64_t& max_revision);

    // Copies the log in `from` into `to` sector by sector and reads the copy
    // back: moves a journal kept in a file onto a raw partition. Nothing is
    // copied (`copied` false) when `from` holds no log or `to` a newer one; a
    // copy cut short is made again by the next call, since `to` then holds
    // nothing newer. False on a read or write error, or when the two regions
    // differ in size or sector size.
    static bool copy_log(FlashRegion& from, FlashRegion& to, bool& copied);

    // Appends one record. False when the region is full or the flash fails.
    bool append_put(const std::string& key, const std::string& value, uint64_t revision, uint64_t fingerprint);
    bool append_delete(const std::string& key, uint64_t revision);
    bool append_revision_mark(uint64_t revision);
    // Makes the appends so far durable.
    bool sync();

    // True when the key's latest record is a put with this fingerprint
    // (an identical value is already on flash).
    bool has_value(const std::string& key, uint64_t fingerprint) const;
    bool has_key(const std::string& key) const;

    FlashLogStats stats() const;
    size_t live_bytes() const { std::lock_guard<std::recursive_mutex> lock(mutex_); return live_bytes_; }
    // Bytes the live records may occupy before appends fail.
    size_t capacity_bytes() const;

    static uint64_t hash_key(const std::string& key);

private:
    struct IndexEntry {
        uint64_t key_hash;
        uint64_t pos;          // stream position of the record
        uint64_t fingerprint;  // value fingerprint (puts)
        uint32_t bytes;        // record size on flash
        uint8_t type;
    };
    struct SectorHeader {
        uint32_t magic;
        uint32_t seq;
        uint16_t first;
        uint8_t version;
        uint8_t reserved;
        uint32_t crc;
    };
    static constexpr size_t kSectorHeader = 16;
    static constexpr size_t kRecordHeader = 24;

    FlashRegion& region_;
    size_t sector_;
    size_t sectors_;
    size_t payload_;           // bytes per sector after the header
    size_t max_record_;
    size_t reserve_sectors_;
    mutable std::recursive_mutex mutex_;
    bool have_head_{false};
    uint32_t head_seq_{0};     // sector being appended to
    size_t head_off_{0};       // next free byte within it
    uint32_t tail_seq_{0};     // oldest sector holding records
    bool reclaiming_{false};
    bool read_error_{false};   // a region read failed since it was last cleared
    bool unreadable_{false};   // recover() could not read the region
    std::vector<IndexEntry> index_;   // sorted by key_hash
    size_t live_bytes_{0};
    uint64_t revision_mark_{0};
    FlashLogStats counters_;

    size_t phys(uint32_t seq) const { return static_cast<size_t>(seq % sectors_); }
    uint64_t pos_of(uint32_t seq, size_t off) const { return static_cast<uint64_t>(seq) * sector_ + off; }
    bool read_region(size_t offset, void* dst, size_t len);
    bool read_header(size_t index, SectorHeader& out);
    bool newest_sector(uint32_t& head);
    bool open_sector(uint32_t seq, size_t first);
    bool ensure_space(size_t record_bytes);
    bool reclaim_tail();
    bool stream_read(uint32_t& seq, size_t& off, void* dst, size_t len);
    bool stream_write(const void* src, size_t len, size_t record_remaining);
    bool append_record(RecordType type, const std::string& key, const std::string& value, uint64_t revision,
                       uint64_t fingerprint, bool moving);
    bool read_record_at(uint32_t seq, size_t off, RecordType& type, std::string& key, std::string& value,
                        uint64_t& revision, size_t& total, uint32_t& end_seq, size_t& end_off);
    IndexEntry* find(uint64_t key_hash);
    const IndexEntry* find(uint64_t key_hash) const;
    void index_set(const IndexEntry& e);
    void index_erase(uint64_t key_hash);
    size_t used_sectors() const;
};

// FNV-1a over a JSON document with the value of its first "resourceVersion"
// skipped: two writes of the same object differ only there.
uint64_t value_fingerprint(const std::string& json);

} // namespace kube32::storage
