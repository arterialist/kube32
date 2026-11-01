#pragma once

// Pod snapshots: the state of a request pod between two host calls, in the
// K32S format (docs/migration.md).
//
// Between two host calls no Wasm frame is live, so a request pod's state is
// its linear memory (size and bytes), its mutable globals and its tables.
// A snapshot holds those, plus what the host keeps for the pod: its clock,
// the channels it opened and the events that arrived while it was frozen.
// wasm3's compiled code is never part of it: the destination instantiates
// the same module again and writes the state back.
//
// Layout, little-endian, no host pointers:
//
//   header   "K32S", u16 version (1), u16 flags (0), image sha256[32],
//            u32 memory bytes, u32 memory pages, u64 pod time ms,
//            u64 unix ms (0: clock unset), u32 section count        68 bytes
//   section  u16 type, u32 length, length bytes
//              1 MEMORY    the linear memory, `memory bytes` long
//              2 GLOBALS   {u32 index, u8 value type, u64 bits} per mutable global
//              3 TABLE     u32 table index, u32 size, u32 function index per
//                          entry (0xFFFFFFFF: null); one section per table
//              4 CHANNELS  {u32 handle, u16 length, name} per open channel
//              5 EVENTS    {u16 length, topic, u32 length, payload} per event
//   trailer  sha256[32] of every byte before it

#include "mem/function_ref.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kube32::runtime {

class ContainerLifecycleController;

constexpr uint16_t kSnapshotVersion = 1;
constexpr size_t kSnapshotHeaderBytes = 68;
constexpr size_t kSnapshotTrailerBytes = 32;
enum SnapshotSection : uint16_t {
    kSectionMemory = 1,
    kSectionGlobals = 2,
    kSectionTable = 3,
    kSectionChannels = 4,
    kSectionEvents = 5,
};
// Events a frozen pod holds; later ones are dropped and counted
constexpr size_t kMaxHeldEvents = 8;

struct HeldEvent {
    std::string topic;
    std::string data;
};

// What a snapshot carries besides the module's own state
struct SnapshotExtras {
    uint64_t pod_time_ms{0};
    uint64_t unix_ms{0};
    std::vector<std::pair<int32_t, std::string>> channels;   // handle, name
    const std::vector<HeldEvent>* events{nullptr};
};

// Receives snapshot bytes in order; false stops the snapshot
using SnapshotWrite = mem::FunctionRef<bool(const uint8_t*, size_t)>;

// Writes the snapshot of the container's module through `out`, linear
// memory straight from where it lives (no copy). Returns the bytes written,
// 0 when the state cannot be captured (`why`) or `out` refused bytes. The
// trailer's digest goes to `out_digest`. The module must not run meanwhile.
size_t write_snapshot(ContainerLifecycleController& ctrl, const uint8_t image_sha[32], const SnapshotExtras& extras, SnapshotWrite out,
                      uint8_t out_digest[32], std::string& why);

// The header fields of a snapshot
struct SnapshotHeader {
    uint16_t version{0};
    uint8_t image_sha[32]{};
    uint32_t memory_bytes{0};
    uint32_t memory_pages{0};
    uint64_t pod_time_ms{0};
    uint64_t unix_ms{0};
    uint32_t sections{0};
};
// Reads the header; false (with `why`) for a short header or another magic
bool parse_snapshot_header(const uint8_t* data, size_t len, SnapshotHeader& out, std::string& why);

// Writes a snapshot back into a freshly instantiated module as its bytes
// arrive: linear memory straight into the module's memory, the small
// sections through a bounded buffer. The trailer is checked at the end;
// a snapshot that fails leaves the module unusable (the caller discards it).
class SnapshotRestorer {
public:
    SnapshotRestorer(ContainerLifecycleController& ctrl, const uint8_t image_sha[32]);
    // The next bytes of the stream; false once the snapshot is refused (why())
    bool feed(const uint8_t* data, size_t len);
    // The stream ended: true when the snapshot was whole and its digest matched
    bool finish();
    const std::string& why() const { return why_; }
    size_t bytes() const { return total_; }

    // Read from the snapshot, applied by the caller (the host keeps them)
    SnapshotHeader header;
    std::vector<std::pair<int32_t, std::string>> channels;
    std::vector<HeldEvent> events;

private:
    enum class Phase { Header, SectionHead, SectionBody, Trailer, Done, Failed };
    bool fail(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    bool begin_memory();
    bool end_section();
    bool apply_globals();
    bool apply_table();
    bool read_channels();
    bool read_events();

    ContainerLifecycleController& ctrl_;
    uint8_t image_sha_[32];
    Phase phase_{Phase::Header};
    uint8_t head_[kSnapshotHeaderBytes];
    size_t head_len_{0};
    uint16_t section_type_{0};
    uint32_t section_len_{0};
    uint32_t section_done_{0};
    uint32_t sections_left_{0};
    uint8_t* memory_{nullptr};
    std::vector<uint8_t> buf_;     // the current small section
    uint8_t digest_[32];
    size_t total_{0};
    std::string why_;
    struct Hash;
    Hash* hash_;
public:
    ~SnapshotRestorer();
    SnapshotRestorer(const SnapshotRestorer&) = delete;
    SnapshotRestorer& operator=(const SnapshotRestorer&) = delete;
};

// What keeps a pod from moving that its spec tells, each reason as a short
// line: "grant esp32.io/gpio-4", "grant kube32.io/i2c-device",
// "grant capability DISPLAY", "volume claim notes on node storage",
// "annotation kube32.io/migration: Never". An entry point is found in the
// module (module_entry_point, wasm_runtime.hpp). Empty for a movable pod.
std::vector<std::string> static_migration_blockers(std::string_view pod_json);

} // namespace kube32::runtime
