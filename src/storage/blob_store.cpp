#include "storage/blob_store.hpp"
#include "storage/flash_log.hpp"
#include "hal/node_storage.hpp"
#include "hal/sd_card.hpp"
#include "runtime/k32_app_abi.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(KUBE32_TARGET_ESP32)
#include "esp_ota_ops.h"
#include "esp_partition.h"
#else
#include <sys/mman.h>
#endif

namespace kube32::storage {

namespace {

constexpr char kDirMagic[8] = {'K', '3', '2', 'B', 'L', 'O', 'B', '1'};
constexpr size_t kDirHeader = 32;
constexpr size_t kDirEntry = 64;
constexpr size_t kEntryName = 48;
static_assert(kDirHeader + kMaxFlashBlobs * kDirEntry <= kBlobDirSector, "a directory copy fits one sector");

void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

uint64_t round_up_page(uint64_t bytes) { return (bytes + kBlobPage - 1) / kBlobPage * kBlobPage; }

int32_t errno_to_k32(int err) {
    switch (err) {
        case ENOENT: return K32_ERR_NOENT;
        case EMFILE:
        case ENFILE: return K32_ERR_MFILE;
        case ENOSPC: return K32_ERR_NOSPC;
        case ENOMEM: return K32_ERR_NOMEM;
        case EEXIST: return K32_ERR_EXIST;
        case EISDIR: return K32_ERR_ISDIR;
        default: return K32_ERR_IO;
    }
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ---- regions ------------------------------------------------------------------

class RamBlobRegion : public BlobRegion {
public:
    explicit RamBlobRegion(size_t size) : bytes_(size, 0xFF) {}
    size_t size() const override { return bytes_.size(); }
    bool read(size_t offset, void* dst, size_t len) override {
        if (offset + len > bytes_.size()) return false;
        std::memcpy(dst, bytes_.data() + offset, len);
        return true;
    }
    // NOR flash: programming clears bits only
    bool write(size_t offset, const void* src, size_t len) override {
        if (offset + len > bytes_.size()) return false;
        const uint8_t* s = static_cast<const uint8_t*>(src);
        for (size_t i = 0; i < len; ++i) bytes_[offset + i] &= s[i];
        return true;
    }
    bool erase(size_t offset, size_t len) override {
        if (offset % kBlobDirSector || len % kBlobDirSector || offset + len > bytes_.size()) return false;
        std::memset(bytes_.data() + offset, 0xFF, len);
        return true;
    }
    const uint8_t* data() const override { return bytes_.data(); }
    std::string name() const override { return "memory"; }

private:
    std::vector<uint8_t> bytes_;
};

#if !defined(KUBE32_TARGET_ESP32)

class FileBlobRegion : public BlobRegion {
public:
    FileBlobRegion(std::string path, int fd, size_t size, const uint8_t* map) : path_(std::move(path)), fd_(fd), size_(size), map_(map) {}
    ~FileBlobRegion() override {
        if (map_) ::munmap(const_cast<uint8_t*>(map_), size_);
        if (fd_ >= 0) ::close(fd_);
    }
    size_t size() const override { return size_; }
    bool read(size_t offset, void* dst, size_t len) override {
        if (offset + len > size_) return false;
        std::memcpy(dst, map_ + offset, len);
        return true;
    }
    bool write(size_t offset, const void* src, size_t len) override {
        return offset + len <= size_ && ::pwrite(fd_, src, len, static_cast<off_t>(offset)) == static_cast<ssize_t>(len);
    }
    bool erase(size_t offset, size_t len) override {
        if (offset % kBlobDirSector || len % kBlobDirSector || offset + len > size_) return false;
        std::vector<uint8_t> ones(kBlobDirSector, 0xFF);
        for (size_t done = 0; done < len; done += ones.size()) {
            if (::pwrite(fd_, ones.data(), ones.size(), static_cast<off_t>(offset + done)) != static_cast<ssize_t>(ones.size())) return false;
        }
        return true;
    }
    const uint8_t* data() const override { return map_; }
    std::string name() const override { return path_; }

private:
    std::string path_;
    int fd_;
    size_t size_;
    const uint8_t* map_;
};

#endif

} // namespace

bool valid_blob_name(std::string_view name) {
    if (name.empty() || name.size() > kBlobNameMax || name == "." || name == ".." || ends_with(name, ".part")) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::unique_ptr<BlobRegion> create_ram_blob_region(size_t size) {
    return std::make_unique<RamBlobRegion>(size);
}

std::unique_ptr<BlobRegion> create_file_blob_region(const std::string& path, size_t size) {
#if defined(KUBE32_TARGET_ESP32)
    (void)path;
    (void)size;
    return nullptr;
#else
    if (size < kBlobDataStart + kBlobPage || size % kBlobPage) return nullptr;
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) return nullptr;
    struct stat st;
    if (::fstat(fd, &st) != 0) { ::close(fd); return nullptr; }
    // A new file reads as erased flash; an existing one keeps its blobs
    if (static_cast<size_t>(st.st_size) < size) {
        std::vector<uint8_t> ones(kBlobPage, 0xFF);
        for (size_t off = static_cast<size_t>(st.st_size) / kBlobPage * kBlobPage; off < size; off += ones.size()) {
            if (::pwrite(fd, ones.data(), ones.size(), static_cast<off_t>(off)) != static_cast<ssize_t>(ones.size())) {
                ::close(fd);
                return nullptr;
            }
        }
    }
    void* map = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { ::close(fd); return nullptr; }
    return std::make_unique<FileBlobRegion>(path, fd, size, static_cast<const uint8_t*>(map));
#endif
}

#if defined(KUBE32_TARGET_ESP32)

namespace {

// Reads come from the mapping (no cache switch-off, any stack); writes and
// erases switch the cache off and run where flash may be written
// (hal::node_storage_call). ESP-IDF invalidates the cached lines of a mapped
// range that it writes or erases.
class PartitionBlobRegion : public BlobRegion {
public:
    PartitionBlobRegion(const esp_partition_t* part, const uint8_t* map, esp_partition_mmap_handle_t handle)
        : part_(part), map_(map), handle_(handle) {}
    ~PartitionBlobRegion() override { esp_partition_munmap(handle_); }
    size_t size() const override { return part_->size; }
    bool read(size_t offset, void* dst, size_t len) override {
        if (offset + len > part_->size) return false;
        std::memcpy(dst, map_ + offset, len);
        return true;
    }
    bool write(size_t offset, const void* src, size_t len) override {
        esp_err_t err = ESP_FAIL;
        hal::node_storage_call([&] { err = esp_partition_write(part_, offset, src, len); });
        return err == ESP_OK;
    }
    bool erase(size_t offset, size_t len) override {
        esp_err_t err = ESP_FAIL;
        hal::node_storage_call([&] { err = esp_partition_erase_range(part_, offset, len); });
        return err == ESP_OK;
    }
    const uint8_t* data() const override { return map_; }
    std::string name() const override { return part_->label; }

private:
    const esp_partition_t* part_;
    const uint8_t* map_;
    esp_partition_mmap_handle_t handle_;
};

} // namespace

std::unique_ptr<BlobRegion> create_board_blob_region(bool& is_app_slot) {
    is_app_slot = false;
    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "blobs");
    if (!part) {
        part = esp_ota_get_next_update_partition(nullptr);
        is_app_slot = part != nullptr;
    }
    if (!part || part->encrypted || part->size % kBlobPage || part->size < kBlobDataStart + kBlobPage) return nullptr;
    const void* map = nullptr;
    esp_partition_mmap_handle_t handle = 0;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &map, &handle) != ESP_OK) return nullptr;
    return std::make_unique<PartitionBlobRegion>(part, static_cast<const uint8_t*>(map), handle);
}

#else

std::unique_ptr<BlobRegion> create_board_blob_region(bool& is_app_slot) {
    is_app_slot = false;
    return nullptr;
}

#endif

// ---- flash store ------------------------------------------------------------------

class FlashBlobStore::Reader : public BlobReader {
public:
    Reader(FlashBlobStore* store, const Entry& e) : store_(store), offset_(e.offset) {
        size_ = e.size;
        const uint8_t* base = store->region_->data();
        view_ = base ? base + e.offset : nullptr;
    }
    ~Reader() override { store_->release_extent(offset_); }
    int32_t read(uint64_t offset, void* dst, uint32_t len) override {
        if (offset >= size_) return 0;
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(len, size_ - offset));
        if (view_) {
            std::memcpy(dst, view_ + offset, n);
            return static_cast<int32_t>(n);
        }
        return store_->region_->read(offset_ + offset, dst, n) ? static_cast<int32_t>(n) : K32_ERR_IO;
    }

private:
    FlashBlobStore* store_;
    uint32_t offset_;
};

class FlashBlobStore::Writer : public BlobWriter {
public:
    Writer(FlashBlobStore* store, std::string name, uint32_t offset, uint32_t size)
        : store_(store), name_(std::move(name)), offset_(offset), size_(size), erased_(offset) {}
    ~Writer() override {
        std::lock_guard<std::mutex> lock(store_->mutex_);
        store_->writing_ = false;
    }
    int32_t write(const void* data, size_t len, std::string& why) override {
        if (len > size_ - written_) {
            why = "more bytes than the " + std::to_string(size_) + " announced";
            return K32_ERR_INVAL;
        }
        // Pages are erased as the data reaches them, so a long upload pays
        // for erasing as it goes
        while (offset_ + written_ + len > erased_) {
            if (!store_->region_->erase(erased_, kBlobPage)) {
                why = "erasing flash at offset " + std::to_string(erased_) + " failed";
                return K32_ERR_IO;
            }
            erased_ += kBlobPage;
        }
        if (!store_->region_->write(offset_ + written_, data, len)) {
            why = "writing flash at offset " + std::to_string(offset_ + written_) + " failed";
            return K32_ERR_IO;
        }
        crc_ = crc32_update(crc_, data, len);
        written_ += static_cast<uint32_t>(len);
        return K32_OK;
    }
    int32_t commit(std::string& why) override {
        if (written_ != size_) {
            why = "the upload ended after " + std::to_string(written_) + " of " + std::to_string(size_) + " bytes";
            return K32_ERR_IO;
        }
        // Read back what the flash holds before the directory names it
        uint32_t check = 0;
        uint8_t buf[256];
        for (uint32_t off = 0; off < size_; off += sizeof(buf)) {
            const uint32_t n = std::min<uint32_t>(sizeof(buf), size_ - off);
            if (!store_->region_->read(offset_ + off, buf, n)) {
                why = "reading back flash failed";
                return K32_ERR_IO;
            }
            check = crc32_update(check, buf, n);
        }
        if (check != crc_) {
            why = "the data read back from flash differs from what was written";
            return K32_ERR_IO;
        }
        std::lock_guard<std::mutex> lock(store_->mutex_);
        std::vector<Entry> next;
        for (const Entry& e : store_->entries_) {
            if (e.name != name_) next.push_back(e);
        }
        if (next.size() >= kMaxFlashBlobs) {
            why = "the directory holds " + std::to_string(kMaxFlashBlobs) + " blobs already";
            return K32_ERR_NOSPC;
        }
        next.push_back(Entry{name_, offset_, size_, crc_});
        if (!store_->write_directory_locked(next, why)) return K32_ERR_IO;
        return K32_OK;
    }

private:
    FlashBlobStore* store_;
    std::string name_;
    uint32_t offset_;
    uint32_t size_;
    uint32_t written_{0};
    uint32_t erased_;
    uint32_t crc_{0};
};

FlashBlobStore::FlashBlobStore(std::unique_ptr<BlobRegion> region, bool firmware_slot)
    : region_(std::move(region)), firmware_slot_(firmware_slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    load_directory_locked();
}

FlashBlobStore::~FlashBlobStore() = default;

std::string FlashBlobStore::location() const {
    return region_->name() + (firmware_slot_ ? " (passive app slot)" : "");
}

void FlashBlobStore::set_firmware_claim(std::function<std::string()> claim) {
    std::lock_guard<std::mutex> lock(mutex_);
    firmware_claim_ = std::move(claim);
}

bool FlashBlobStore::load_directory_locked() {
    struct Copy {
        bool ok{false};
        uint32_t seq{0};
        std::vector<Entry> entries;
    };
    auto read_copy = [&](int index) {
        Copy c;
        const size_t base = static_cast<size_t>(index) * kBlobDirSector;
        uint8_t head[kDirHeader];
        if (!region_->read(base, head, sizeof(head)) || std::memcmp(head, kDirMagic, sizeof(kDirMagic)) != 0) return c;
        const uint32_t seq = get32(head + 8), count = get32(head + 12), crc = get32(head + 16);
        if (count > kMaxFlashBlobs) return c;
        std::vector<uint8_t> body(8 + count * kDirEntry);
        std::memcpy(body.data(), head + 8, 8);
        if (count && !region_->read(base + kDirHeader, body.data() + 8, count * kDirEntry)) return c;
        if (crc32_update(0, body.data(), body.size()) != crc) return c;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* p = body.data() + 8 + i * kDirEntry;
            Entry e;
            e.name.assign(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), kEntryName));
            e.offset = get32(p + 48);
            e.size = get32(p + 52);
            e.crc32 = get32(p + 56);
            if (!valid_blob_name(e.name) || e.offset < kBlobDataStart || e.offset % kBlobPage ||
                static_cast<uint64_t>(e.offset) + e.size > region_->size()) {
                return c;
            }
            c.entries.push_back(std::move(e));
        }
        c.ok = true;
        c.seq = seq;
        return c;
    };
    Copy a = read_copy(0), b = read_copy(1);
    formatted_ = a.ok || b.ok;
    const bool use_b = b.ok && (!a.ok || b.seq > a.seq);
    Copy& cur = use_b ? b : a;
    entries_ = std::move(cur.entries);
    seq_ = cur.seq;
    current_copy_ = use_b ? 1 : 0;
    return formatted_;
}

bool FlashBlobStore::write_directory_locked(const std::vector<Entry>& entries, std::string& why) {
    const int target = formatted_ ? 1 - current_copy_ : 0;
    const uint32_t seq = formatted_ ? seq_ + 1 : 1;
    std::vector<uint8_t> sector(kDirHeader + entries.size() * kDirEntry, 0);
    std::memcpy(sector.data(), kDirMagic, sizeof(kDirMagic));
    put32(sector.data() + 8, seq);
    put32(sector.data() + 12, static_cast<uint32_t>(entries.size()));
    for (size_t i = 0; i < entries.size(); ++i) {
        uint8_t* p = sector.data() + kDirHeader + i * kDirEntry;
        std::memcpy(p, entries[i].name.data(), std::min(entries[i].name.size(), kEntryName - 1));
        put32(p + 48, entries[i].offset);
        put32(p + 52, entries[i].size);
        put32(p + 56, entries[i].crc32);
    }
    // The CRC covers sequence, count and entries, read back as one run
    std::vector<uint8_t> body(8 + entries.size() * kDirEntry);
    std::memcpy(body.data(), sector.data() + 8, 8);
    if (!entries.empty()) std::memcpy(body.data() + 8, sector.data() + kDirHeader, entries.size() * kDirEntry);
    put32(sector.data() + 16, crc32_update(0, body.data(), body.size()));
    const size_t base = static_cast<size_t>(target) * kBlobDirSector;
    if (!region_->erase(base, kBlobDirSector) || !region_->write(base, sector.data(), sector.size())) {
        why = "writing the blob directory failed";
        return false;
    }
    entries_ = entries;
    seq_ = seq;
    current_copy_ = target;
    formatted_ = true;
    return true;
}

bool FlashBlobStore::claim_locked(std::string& why) {
    if (formatted_ && !retired_) return true;
    if (firmware_slot_ && firmware_claim_) {
        std::string reason = firmware_claim_();
        if (!reason.empty()) {
            why = reason;
            return false;
        }
    }
    // Whatever the slot held before (an image, a store given to the
    // firmware) is no directory from here on
    if (!region_->erase(0, kBlobDataStart)) {
        why = "erasing the blob directory failed";
        return false;
    }
    formatted_ = false;
    retired_ = false;
    entries_.clear();
    seq_ = 0;
    current_copy_ = 0;
    return write_directory_locked({}, why);
}

uint32_t FlashBlobStore::find_extent_locked(uint64_t bytes) const {
    std::vector<std::pair<uint64_t, uint64_t>> used;   // [start, end)
    for (const Entry& e : entries_) used.emplace_back(e.offset, e.offset + round_up_page(e.size));
    for (const auto& kv : open_extents_) used.emplace_back(kv.first, kv.first + kv.second.first);
    if (writing_) used.emplace_back(write_offset_, write_offset_ + round_up_page(write_bytes_));
    std::sort(used.begin(), used.end());
    const uint64_t need = round_up_page(bytes);
    uint64_t at = kBlobDataStart;
    for (const auto& u : used) {
        if (u.first >= at + need) break;
        at = std::max(at, u.second);
    }
    return at + need <= region_->size() ? static_cast<uint32_t>(at) : 0;
}

uint64_t FlashBlobStore::largest_gap_locked() const {
    std::vector<std::pair<uint64_t, uint64_t>> used;
    for (const Entry& e : entries_) used.emplace_back(e.offset, e.offset + round_up_page(e.size));
    for (const auto& kv : open_extents_) used.emplace_back(kv.first, kv.first + kv.second.first);
    if (writing_) used.emplace_back(write_offset_, write_offset_ + round_up_page(write_bytes_));
    std::sort(used.begin(), used.end());
    uint64_t at = kBlobDataStart, best = 0;
    for (const auto& u : used) {
        if (u.first > at) best = std::max(best, u.first - at);
        at = std::max(at, u.second);
    }
    if (region_->size() > at) best = std::max<uint64_t>(best, region_->size() - at);
    return best;
}

void FlashBlobStore::release_extent(uint32_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = open_extents_.find(offset);
    if (it != open_extents_.end() && --it->second.second <= 0) open_extents_.erase(it);
}

bool FlashBlobStore::available(std::string* why) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (retired_) {
        if (why) *why = "a firmware image is being written to " + region_->name();
        return false;
    }
    if (!formatted_ && firmware_slot_ && firmware_claim_) {
        std::string reason = firmware_claim_();
        if (!reason.empty()) {
            if (why) *why = reason;
            return false;
        }
    }
    return true;
}

uint64_t FlashBlobStore::capacity_bytes() const { return region_->size() - kBlobDataStart; }

uint64_t FlashBlobStore::free_bytes() const {
    if (!available()) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    return largest_gap_locked();
}

std::vector<BlobInfo> FlashBlobStore::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<BlobInfo> out;
    if (retired_) return out;
    for (const Entry& e : entries_) out.push_back(BlobInfo{e.name, e.size, e.crc32, e.offset});
    return out;
}

std::shared_ptr<BlobReader> FlashBlobStore::open(std::string_view name, int32_t& err) {
    if (!valid_blob_name(name)) {
        err = K32_ERR_INVAL;
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (retired_) {
        err = K32_ERR_NOENT;
        return nullptr;
    }
    for (const Entry& e : entries_) {
        if (e.name != name) continue;
        auto& slot = open_extents_[e.offset];
        slot.first = static_cast<uint32_t>(round_up_page(e.size));
        slot.second++;
        err = K32_OK;
        return std::make_shared<Reader>(this, e);
    }
    err = K32_ERR_NOENT;
    return nullptr;
}

std::unique_ptr<BlobWriter> FlashBlobStore::create(std::string_view name, uint64_t size, int32_t& err, std::string& why) {
    if (!valid_blob_name(name)) {
        err = K32_ERR_INVAL;
        why = "a blob name is 1 to 47 characters of [A-Za-z0-9._-]";
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (writing_) {
        err = K32_ERR_BUSY;
        why = "another blob is being written";
        return nullptr;
    }
    if (size == 0 || size > capacity_bytes()) {
        err = size ? K32_ERR_NOSPC : K32_ERR_INVAL;
        why = size ? "a blob of " + std::to_string(size) + " bytes is larger than the " + std::to_string(capacity_bytes()) +
                         "-byte flash region"
                   : "a blob holds at least one byte";
        return nullptr;
    }
    if (!claim_locked(why)) {
        err = K32_ERR_BUSY;
        return nullptr;
    }
    size_t named = 0;
    for (const Entry& e : entries_) named += e.name != name;
    if (named >= kMaxFlashBlobs) {
        err = K32_ERR_NOSPC;
        why = "the directory holds " + std::to_string(kMaxFlashBlobs) + " blobs already";
        return nullptr;
    }
    const uint32_t offset = find_extent_locked(size);
    if (offset == 0) {
        err = K32_ERR_NOSPC;
        why = "no free run of " + std::to_string(round_up_page(size)) + " bytes in the flash region (largest " +
              std::to_string(largest_gap_locked()) + ")";
        return nullptr;
    }
    writing_ = true;
    write_offset_ = offset;
    write_bytes_ = static_cast<uint32_t>(size);
    err = K32_OK;
    return std::make_unique<Writer>(this, std::string(name), offset, static_cast<uint32_t>(size));
}

int32_t FlashBlobStore::remove(std::string_view name, std::string& why) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (retired_ || !formatted_) return K32_ERR_NOENT;
    std::vector<Entry> next;
    for (const Entry& e : entries_) {
        if (e.name != name) next.push_back(e);
    }
    if (next.size() == entries_.size()) return K32_ERR_NOENT;
    return write_directory_locked(next, why) ? K32_OK : K32_ERR_IO;
}

bool FlashBlobStore::yield_to_firmware(std::string& why) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_extents_.empty()) {
        size_t readers = 0;
        for (const auto& kv : open_extents_) readers += static_cast<size_t>(kv.second.second);
        why = std::to_string(readers) + " blob reader(s) hold " + region_->name() + ": stop the pods that read blobs first";
        return false;
    }
    if (writing_) {
        why = "a blob is being written to " + region_->name();
        return false;
    }
    if (formatted_ && !entries_.empty()) {
        std::printf("[BLOBS] %s goes to a firmware image: %u blob(s) dropped\n", region_->name().c_str(), (unsigned)entries_.size());
    }
    retired_ = true;
    formatted_ = false;
    entries_.clear();
    return true;
}

uint32_t FlashBlobStore::directory_sequence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seq_;
}

size_t FlashBlobStore::open_readers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& kv : open_extents_) n += static_cast<size_t>(kv.second.second);
    return n;
}

// ---- card store ------------------------------------------------------------------

class CardBlobStore::Reader : public BlobReader {
public:
    Reader(CardBlobStore* store, std::string name, int fd, uint64_t size, const uint8_t* map)
        : store_(store), name_(std::move(name)), fd_(fd), map_(map) {
        size_ = size;
        view_ = map;
    }
    ~Reader() override {
#if !defined(KUBE32_TARGET_ESP32)
        if (map_) ::munmap(const_cast<uint8_t*>(map_), static_cast<size_t>(size_));
#endif
        const int fd = fd_;
        hal::node_storage_call([fd] { ::close(fd); });
        store_->release(name_);
    }
    int32_t read(uint64_t offset, void* dst, uint32_t len) override {
        if (offset >= size_) return 0;
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(len, size_ - offset));
        if (view_) {
            std::memcpy(dst, view_ + offset, n);
            return static_cast<int32_t>(n);
        }
        ssize_t got = 0;
        int err = 0;
        hal::node_storage_call([&] {
            got = ::pread(fd_, dst, n, static_cast<off_t>(offset));
            err = errno;
        });
        if (got < 0) return errno_to_k32(err);
        return static_cast<int32_t>(got);
    }

private:
    CardBlobStore* store_;
    std::string name_;
    int fd_;
    const uint8_t* map_;
};

class CardBlobStore::Writer : public BlobWriter {
public:
    Writer(CardBlobStore* store, std::string name, std::string path, int fd, uint64_t size)
        : store_(store), name_(std::move(name)), path_(std::move(path)), fd_(fd), size_(size) {}
    ~Writer() override {
        if (fd_ < 0) return;
        const std::string part = path_ + ".part";
        const int fd = fd_;
        hal::node_storage_call([&] {
            ::close(fd);
            ::unlink(part.c_str());
        });
    }
    int32_t write(const void* data, size_t len, std::string& why) override {
        if (len > size_ - written_) {
            why = "more bytes than the " + std::to_string(size_) + " announced";
            return K32_ERR_INVAL;
        }
        ssize_t n = 0;
        int err = 0;
        hal::node_storage_call([&] {
            n = ::write(fd_, data, len);
            err = errno;
        });
        if (n != static_cast<ssize_t>(len)) {
            why = "writing the card failed (full or removed)";
            return n < 0 ? errno_to_k32(err) : K32_ERR_NOSPC;
        }
        written_ += len;
        return K32_OK;
    }
    int32_t commit(std::string& why) override {
        if (written_ != size_) {
            why = "the upload ended after " + std::to_string(written_) + " of " + std::to_string(size_) + " bytes";
            return K32_ERR_IO;
        }
        {
            std::lock_guard<std::mutex> lock(store_->mutex_);
            auto it = store_->open_.find(name_);
            if (it != store_->open_.end() && it->second > 0) {
                why = "blob " + name_ + " is open in a pod";
                return K32_ERR_BUSY;
            }
        }
        const std::string part = path_ + ".part";
        int rc = 0;
        hal::node_storage_call([&] {
            rc = ::close(fd_);
            fd_ = -1;
            if (rc != 0) { ::unlink(part.c_str()); return; }
            ::unlink(path_.c_str());   // FAT cannot rename over a file
            rc = ::rename(part.c_str(), path_.c_str());
            if (rc != 0) ::unlink(part.c_str());
        });
        if (rc != 0) {
            why = "storing the blob on the card failed";
            return K32_ERR_IO;
        }
        return K32_OK;
    }

private:
    CardBlobStore* store_;
    std::string name_;
    std::string path_;
    int fd_;
    uint64_t size_;
    uint64_t written_{0};
};

std::string CardBlobStore::directory() const {
    const std::string root = hal::sd_root_path();
    return root.empty() ? std::string() : root + "/kube32/blobs";
}

std::string CardBlobStore::location() const {
    const std::string dir = directory();
    return dir.empty() ? std::string("kube32/blobs on the microSD card") : dir;
}

bool CardBlobStore::available(std::string* why) const {
    if (hal::sd_is_mounted()) return true;
    if (why) *why = "no microSD card is mounted";
    return false;
}

uint64_t CardBlobStore::capacity_bytes() const { return hal::sd_is_mounted() ? hal::sd_info().total_bytes : 0; }
uint64_t CardBlobStore::free_bytes() const { return hal::sd_is_mounted() ? hal::sd_info().free_bytes : 0; }

std::vector<BlobInfo> CardBlobStore::list() const {
    std::vector<BlobInfo> out;
    const std::string dir = directory();
    if (dir.empty()) return out;
    hal::node_storage_call([&] {
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        for (int budget = 256; budget > 0; --budget) {
            struct dirent* e = readdir(d);
            if (!e) break;
            if (!valid_blob_name(e->d_name)) continue;
            struct stat st;
            const std::string path = dir + "/" + e->d_name;
            if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            out.push_back(BlobInfo{e->d_name, static_cast<uint64_t>(st.st_size), 0, 0});
        }
        closedir(d);
    });
    std::sort(out.begin(), out.end(), [](const BlobInfo& a, const BlobInfo& b) { return a.name < b.name; });
    return out;
}

std::shared_ptr<BlobReader> CardBlobStore::open(std::string_view name, int32_t& err) {
    if (!valid_blob_name(name)) {
        err = K32_ERR_INVAL;
        return nullptr;
    }
    const std::string dir = directory();
    if (dir.empty()) {
        err = K32_ERR_NOENT;
        return nullptr;
    }
    const std::string path = dir + "/" + std::string(name);
    int fd = -1, open_err = 0;
    struct stat st{};
    hal::node_storage_call([&] {
        fd = ::open(path.c_str(), O_RDONLY);
        open_err = errno;
        if (fd >= 0 && (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))) {
            ::close(fd);
            fd = -1;
            open_err = EISDIR;
        }
    });
    if (fd < 0) {
        err = errno_to_k32(open_err);
        return nullptr;
    }
    const uint8_t* map = nullptr;
#if !defined(KUBE32_TARGET_ESP32)
    if (st.st_size > 0) {
        void* m = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) map = static_cast<const uint8_t*>(m);
    }
#endif
    {
        std::lock_guard<std::mutex> lock(mutex_);
        open_[std::string(name)]++;
    }
    err = K32_OK;
    return std::make_shared<Reader>(this, std::string(name), fd, static_cast<uint64_t>(st.st_size), map);
}

void CardBlobStore::release(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = open_.find(name);
    if (it != open_.end() && --it->second <= 0) open_.erase(it);
}

std::unique_ptr<BlobWriter> CardBlobStore::create(std::string_view name, uint64_t size, int32_t& err, std::string& why) {
    if (!valid_blob_name(name)) {
        err = K32_ERR_INVAL;
        why = "a blob name is 1 to 47 characters of [A-Za-z0-9._-]";
        return nullptr;
    }
    const std::string dir = directory();
    if (dir.empty()) {
        err = K32_ERR_NODEV;
        why = "no microSD card is mounted";
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = open_.find(std::string(name));
        if (it != open_.end() && it->second > 0) {
            err = K32_ERR_BUSY;
            why = "blob " + std::string(name) + " is open in a pod";
            return nullptr;
        }
    }
    if (size + 64 * 1024 > free_bytes()) {
        err = K32_ERR_NOSPC;
        why = "not enough free space on the card";
        return nullptr;
    }
    if (!hal::node_storage_make_dirs(dir)) {
        err = K32_ERR_IO;
        why = "cannot create " + dir;
        return nullptr;
    }
    const std::string path = dir + "/" + std::string(name);
    const std::string part = path + ".part";
    int fd = -1, open_err = 0;
    hal::node_storage_call([&] {
        fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        open_err = errno;
    });
    if (fd < 0) {
        err = errno_to_k32(open_err);
        why = "cannot create " + part;
        return nullptr;
    }
    err = K32_OK;
    return std::make_unique<Writer>(this, std::string(name), path, fd, size);
}

int32_t CardBlobStore::remove(std::string_view name, std::string& why) {
    if (!valid_blob_name(name)) return K32_ERR_INVAL;
    const std::string dir = directory();
    if (dir.empty()) return K32_ERR_NOENT;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = open_.find(std::string(name));
        if (it != open_.end() && it->second > 0) {
            why = "blob " + std::string(name) + " is open in a pod";
            return K32_ERR_BUSY;
        }
    }
    const std::string path = dir + "/" + std::string(name);
    int rc = 0, err = 0;
    hal::node_storage_call([&] {
        rc = ::unlink(path.c_str());
        err = errno;
    });
    return rc == 0 ? K32_OK : errno_to_k32(err);
}

// ---- the node's stores ------------------------------------------------------------

namespace {
FlashBlobStore* s_flash = nullptr;
BlobStore* s_card = nullptr;
}

void set_node_blob_stores(FlashBlobStore* flash, BlobStore* card) {
    s_flash = flash;
    s_card = card;
}

FlashBlobStore* node_flash_blobs() { return s_flash; }
BlobStore* node_card_blobs() { return s_card; }

std::vector<BlobStore*> node_blob_stores() {
    std::vector<BlobStore*> out;
    if (s_flash && s_flash->available()) out.push_back(s_flash);
    if (s_card && s_card->available()) out.push_back(s_card);
    return out;
}

} // namespace kube32::storage
