#pragma once

// Blobs: named, read-only byte strings a node keeps for its pods, such as
// model weights and lookup tables. A pod reads a blob in place (k32_blob_*,
// include/runtime/k32_app_abi.h) instead of loading it into its memory, so a
// blob may be larger than any pod's memory limit.
//
// A node has up to two blob stores, looked up in this order:
//
//   flash  a raw region of the board's own flash: the data partition labelled
//          "blobs" when the partition table has one, otherwise the passive app
//          slot (the slot a firmware update writes). The region is mapped into
//          the address space whole, so the native kernels read a blob there as
//          memory. kube32-sim keeps the region in a file.
//   sd     files in <card>/kube32/blobs/ (the KUBE32_SD_ROOT directory on
//          kube32-sim), read with file reads. kube32-sim maps them as well.
//
// Flash region layout (NOR flash, 4 KiB erase sectors):
//
//   0x00000  directory, copy A (one sector)
//   0x01000  directory, copy B
//   0x10000  data: each blob starts on a 64 KiB boundary (an MMU page)
//
// A directory copy is a 32-byte header {"K32BLOB1", sequence, count, CRC-32}
// and `count` 64-byte entries {name, offset, size, CRC-32 of the data}. A
// change writes the copy that is not current with the next sequence number;
// at start the intact copy with the higher number is the directory. A new
// blob's data is written before the directory that names it, so a power cut
// leaves either the old directory or the new one, never a blob that is only
// partly there.
//
// When the region is the passive app slot, firmware comes first: staging a
// firmware image there erases the blobs (refused while a pod has one open),
// and blobs are written only while the slot holds nothing the firmware still
// needs (set_firmware_claim).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace kube32::storage {

constexpr size_t kBlobNameMax = 47;                // bytes of [A-Za-z0-9._-]
constexpr size_t kBlobPage = 64 * 1024;            // a flash blob starts on an MMU page
constexpr size_t kBlobDirSector = 4096;
constexpr size_t kBlobDataStart = kBlobPage;       // directory copies live in the first page
constexpr size_t kMaxFlashBlobs = 63;              // entries a directory sector holds

// 1 to kBlobNameMax characters of [A-Za-z0-9._-], not "." or "..", not
// ending in ".part" (an upload in progress on a card)
bool valid_blob_name(std::string_view name);

struct BlobInfo {
    std::string name;
    uint64_t size{0};
    uint32_t crc32{0};      // of the data (flash); 0 on a card
    uint64_t offset{0};     // flash: where the data starts in the region
};

// An open blob. It stays readable as long as it is held, also when the blob
// is replaced or removed in the meantime (flash keeps the old bytes until the
// last reader lets go; a card refuses to replace a blob that is open).
class BlobReader {
public:
    virtual ~BlobReader() = default;
    uint64_t size() const { return size_; }
    // The whole blob in the address space, or null when it can only be read
    // with read() (a file on the board's card)
    const uint8_t* view() const { return view_; }
    // Copies bytes [offset, offset + len), clamped to the blob's end, to
    // dst: bytes copied (0 at or past the end) or a negative K32_ERR_* code
    virtual int32_t read(uint64_t offset, void* dst, uint32_t len) = 0;

protected:
    uint64_t size_{0};
    const uint8_t* view_{nullptr};
};

// A blob being written. The bytes arrive in order; commit() makes the blob
// visible under its name (replacing an older one). Destroying an uncommitted
// writer drops what it wrote.
class BlobWriter {
public:
    virtual ~BlobWriter() = default;
    virtual int32_t write(const void* data, size_t len, std::string& why) = 0;   // K32_OK or K32_ERR_*
    virtual int32_t commit(std::string& why) = 0;
};

class BlobStore {
public:
    virtual ~BlobStore() = default;
    virtual const char* medium() const = 0;            // "flash" or "sd"
    virtual std::string location() const = 0;         // where the store lives, for listings and logs
    // Whether the store can be used now (a card that is mounted, a region
    // the firmware does not hold); why not otherwise
    virtual bool available(std::string* why = nullptr) const = 0;
    virtual uint64_t capacity_bytes() const = 0;
    virtual uint64_t free_bytes() const = 0;           // the largest blob a write can take now
    virtual std::vector<BlobInfo> list() const = 0;
    // K32_ERR_NOENT when there is no such blob, K32_ERR_INVAL for a bad name
    virtual std::shared_ptr<BlobReader> open(std::string_view name, int32_t& err) = 0;
    // A writer for `size` bytes, or null with err and why
    virtual std::unique_ptr<BlobWriter> create(std::string_view name, uint64_t size, int32_t& err, std::string& why) = 0;
    virtual int32_t remove(std::string_view name, std::string& why) = 0;
};

// Raw flash for a FlashBlobStore. write() programs erased bytes; erase()
// takes 4 KiB-aligned ranges and leaves 0xFF bytes.
class BlobRegion {
public:
    virtual ~BlobRegion() = default;
    virtual size_t size() const = 0;
    virtual bool read(size_t offset, void* dst, size_t len) = 0;
    virtual bool write(size_t offset, const void* src, size_t len) = 0;
    virtual bool erase(size_t offset, size_t len) = 0;
    // The whole region mapped read-only, or null
    virtual const uint8_t* data() const = 0;
    virtual std::string name() const = 0;
};

// A file of `size` bytes standing in for flash (kube32-sim): created filled
// with 0xFF, mapped read-only. Null when it cannot be created or mapped.
std::unique_ptr<BlobRegion> create_file_blob_region(const std::string& path, size_t size);
// Memory standing in for flash, for tests
std::unique_ptr<BlobRegion> create_ram_blob_region(size_t size);
// The board's region: the "blobs" data partition, else the passive app slot
// (`is_app_slot` true). Null on the host and when neither exists.
std::unique_ptr<BlobRegion> create_board_blob_region(bool& is_app_slot);

class FlashBlobStore : public BlobStore {
public:
    // `firmware_slot`: the region is the passive app slot that firmware
    // updates write
    FlashBlobStore(std::unique_ptr<BlobRegion> region, bool firmware_slot);
    ~FlashBlobStore() override;

    const char* medium() const override { return "flash"; }
    std::string location() const override;
    bool available(std::string* why = nullptr) const override;
    uint64_t capacity_bytes() const override;
    uint64_t free_bytes() const override;
    std::vector<BlobInfo> list() const override;
    std::shared_ptr<BlobReader> open(std::string_view name, int32_t& err) override;
    std::unique_ptr<BlobWriter> create(std::string_view name, uint64_t size, int32_t& err, std::string& why) override;
    int32_t remove(std::string_view name, std::string& why) override;

    // What the firmware still keeps in the slot, or "" when blobs may take it
    // (a staged image, the image an unconfirmed update falls back to)
    void set_firmware_claim(std::function<std::string()> claim);
    // A firmware image is about to be written over the region: false while a
    // pod has a blob open; otherwise the blobs are gone from here on
    bool yield_to_firmware(std::string& why);
    bool firmware_slot() const { return firmware_slot_; }
    // Directory changes since the store was opened, and blobs open now
    uint32_t directory_sequence() const;
    size_t open_readers() const;

private:
    class Reader;
    class Writer;
    struct Entry {
        std::string name;
        uint32_t offset{0};
        uint32_t size{0};
        uint32_t crc32{0};
    };

    bool load_directory_locked();
    bool write_directory_locked(const std::vector<Entry>& entries, std::string& why);
    // Makes the region a blob store (an empty directory) when it is not one
    // yet; false with why when the firmware keeps it
    bool claim_locked(std::string& why);
    // First free run of `bytes` (whole pages) in the data area, 0 when none
    uint32_t find_extent_locked(uint64_t bytes) const;
    uint64_t largest_gap_locked() const;
    void release_extent(uint32_t offset);

    std::unique_ptr<BlobRegion> region_;
    const bool firmware_slot_;
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    bool formatted_{false};       // the region holds a directory
    bool retired_{false};         // given to the firmware
    uint32_t seq_{0};
    int current_copy_{0};         // 0: A, 1: B
    bool writing_{false};
    uint32_t write_offset_{0};    // extent of the write in progress
    uint32_t write_bytes_{0};
    std::map<uint32_t, std::pair<uint32_t, int>> open_extents_;   // offset -> (bytes, readers)
    std::function<std::string()> firmware_claim_;
};

// Files in <card>/kube32/blobs/; the card is looked up at every call
class CardBlobStore : public BlobStore {
public:
    CardBlobStore() = default;
    const char* medium() const override { return "sd"; }
    std::string location() const override;
    bool available(std::string* why = nullptr) const override;
    uint64_t capacity_bytes() const override;
    uint64_t free_bytes() const override;
    std::vector<BlobInfo> list() const override;
    std::shared_ptr<BlobReader> open(std::string_view name, int32_t& err) override;
    std::unique_ptr<BlobWriter> create(std::string_view name, uint64_t size, int32_t& err, std::string& why) override;
    int32_t remove(std::string_view name, std::string& why) override;

private:
    class Reader;
    class Writer;
    std::string directory() const;
    void release(const std::string& name);

    mutable std::mutex mutex_;
    std::map<std::string, int> open_;   // name -> readers
};

// The node's stores, set once at boot. Either may be null.
void set_node_blob_stores(FlashBlobStore* flash, BlobStore* card);
FlashBlobStore* node_flash_blobs();
BlobStore* node_card_blobs();
// The stores that are available now, in lookup order (flash, then card)
std::vector<BlobStore*> node_blob_stores();

} // namespace kube32::storage
