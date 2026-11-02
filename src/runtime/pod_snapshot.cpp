#include "runtime/pod_snapshot.hpp"
#include "runtime/container_lifecycle.hpp"
#include "models/kube_models.hpp"
#include "models/device.hpp"
#include "osal/sha256.hpp"

#include "wasm3.h"
#include "m3_env.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace kube32::runtime {

namespace {

constexpr uint32_t kNullFunction = 0xFFFFFFFFu;
constexpr uint8_t kTypeI32 = 0x7F, kTypeI64 = 0x7E, kTypeF32 = 0x7D, kTypeF64 = 0x7C, kTypeFuncref = 0x70;
// The small sections are read whole before they apply
constexpr uint32_t kMaxSmallSection = 64 * 1024;

void put16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t get64(const uint8_t* p) { return static_cast<uint64_t>(get32(p)) | (static_cast<uint64_t>(get32(p + 4)) << 32); }

// The first 6 bytes of a digest in hex, for messages
struct Hex12 {
    char text[13];
    explicit Hex12(const uint8_t* d) {
        for (int i = 0; i < 6; ++i) snprintf(text + 2 * i, 3, "%02x", d[i]);
    }
};

void set_why(std::string& why, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void set_why(std::string& why, const char* fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    why = line;
}

uint8_t value_type_code(m3type_t t) {
    switch (t) {
    case c_m3Type_i32: return kTypeI32;
    case c_m3Type_i64: return kTypeI64;
    case c_m3Type_f32: return kTypeF32;
    case c_m3Type_f64: return kTypeF64;
    case c_m3Type_funcref: return kTypeFuncref;
    default: return 0;
    }
}

// A function reference as its index in the module, or kNullFunction
bool function_index(IM3Module module, const void* ref, uint32_t& out) {
    if (!ref) { out = kNullFunction; return true; }
    const M3Function* f = static_cast<const M3Function*>(ref);
    if (f < module->functions || f >= module->functions + module->numFunctions) return false;
    out = static_cast<uint32_t>(f - module->functions);
    return true;
}

IM3Memory memory0(IM3Module module) { return module && module->numMemories ? module->memories[0] : nullptr; }

// The module's own (not imported) mutable globals
bool saved_global(const M3Global& g) { return g.isMutable && !g.imported; }

// Writes through `out` and hashes, counting bytes
struct Emitter {
    SnapshotWrite out;
    osal::Sha256 sha;
    size_t total{0};
    bool ok{true};
    bool put(const void* data, size_t len) {
        if (!ok || len == 0) return ok;
        sha.update(data, len);
        total += len;
        ok = out(static_cast<const uint8_t*>(data), len);
        return ok;
    }
    bool section(uint16_t type, uint32_t len) {
        uint8_t h[6];
        put16(h, type);
        put32(h + 2, len);
        return put(h, sizeof h);
    }
};

} // namespace

size_t write_snapshot(ContainerLifecycleController& ctrl, const uint8_t image_sha[32], const SnapshotExtras& extras, SnapshotWrite out,
                      uint8_t out_digest[32], std::string& why) {
    IM3Module module = static_cast<IM3Module>(ctrl.m3_module_handle());
    if (!module) { why = "the pod has no module"; return 0; }
    IM3Memory mem = memory0(module);
    size_t mem_len = 0;
    uint8_t* mem_bytes = mem ? ctrl.guest_memory(mem_len) : nullptr;
    if (!mem_bytes) mem_len = 0;

    // Globals and tables first: a state that cannot be written fails before
    // anything is sent
    uint32_t globals = 0;
    for (uint32_t i = 0; i < module->numGlobals; ++i) {
        const M3Global& g = module->globals[i];
        if (!saved_global(g)) continue;
        uint32_t idx;
        if (!value_type_code(g.type) || (g.type == c_m3Type_funcref && !function_index(module, g.refValue, idx))) {
            set_why(why, "global %u holds a reference this node cannot name", (unsigned)i);
            return 0;
        }
        ++globals;
    }
    uint32_t tables = 0;
    for (uint32_t t = 0; t < module->numTables; ++t) {
        IM3Table tbl = module->tables[t];
        if (!tbl || tbl->owner != module) continue;
        for (uint32_t i = 0; i < tbl->size; ++i) {
            uint32_t idx;
            if (tbl->type != c_m3Type_funcref ? tbl->elements[i] != nullptr : !function_index(module, tbl->elements[i], idx)) {
                set_why(why, "table %u entry %u holds a reference this node cannot name", (unsigned)t, (unsigned)i);
                return 0;
            }
        }
        ++tables;
    }
    size_t channel_bytes = 0;
    for (const auto& c : extras.channels) channel_bytes += 6 + std::min<size_t>(c.second.size(), 0xFFFF);
    size_t event_bytes = 0;
    if (extras.events) {
        for (const auto& e : *extras.events) event_bytes += 6 + std::min<size_t>(e.topic.size(), 0xFFFF) + e.data.size();
    }
    const uint32_t sections = (mem_len ? 1 : 0) + (globals ? 1 : 0) + tables + (channel_bytes ? 1 : 0) + (event_bytes ? 1 : 0);

    Emitter em{out};
    uint8_t head[kSnapshotHeaderBytes] = {'K', '3', '2', 'S'};
    put16(head + 4, kSnapshotVersion);
    put16(head + 6, 0);
    std::memcpy(head + 8, image_sha, 32);
    put32(head + 40, static_cast<uint32_t>(mem_len));
    put32(head + 44, mem ? static_cast<uint32_t>(mem->numPages) : 0);
    put64(head + 48, extras.pod_time_ms);
    put64(head + 56, extras.unix_ms);
    put32(head + 64, sections);
    em.put(head, sizeof head);

    if (mem_len) {
        em.section(kSectionMemory, static_cast<uint32_t>(mem_len));
        // A slice at a time: the writer may hand each to a socket
        for (size_t off = 0; off < mem_len && em.ok; off += 4096) em.put(mem_bytes + off, std::min<size_t>(4096, mem_len - off));
    }
    if (globals) {
        em.section(kSectionGlobals, globals * 13);
        for (uint32_t i = 0; i < module->numGlobals; ++i) {
            const M3Global& g = module->globals[i];
            if (!saved_global(g)) continue;
            uint8_t rec[13];
            put32(rec, i);
            rec[4] = value_type_code(g.type);
            uint64_t bits = 0;
            if (g.type == c_m3Type_funcref) {
                uint32_t idx = kNullFunction;
                function_index(module, g.refValue, idx);
                bits = idx;
            } else {
                // i32 and f32 in the first 4 bytes of the union, i64 and f64 in 8
                std::memcpy(&bits, &g.i64Value, (g.type == c_m3Type_i32 || g.type == c_m3Type_f32) ? 4 : 8);
            }
            put64(rec + 5, bits);
            em.put(rec, sizeof rec);
        }
    }
    for (uint32_t t = 0; t < module->numTables && em.ok; ++t) {
        IM3Table tbl = module->tables[t];
        if (!tbl || tbl->owner != module) continue;
        em.section(kSectionTable, 8 + tbl->size * 4);
        uint8_t th[8];
        put32(th, t);
        put32(th + 4, tbl->size);
        em.put(th, sizeof th);
        uint8_t chunk[64];
        size_t used = 0;
        for (uint32_t i = 0; i < tbl->size; ++i) {
            uint32_t idx = kNullFunction;
            if (tbl->type == c_m3Type_funcref) function_index(module, tbl->elements[i], idx);
            put32(chunk + used, idx);
            used += 4;
            if (used == sizeof chunk) { em.put(chunk, used); used = 0; }
        }
        em.put(chunk, used);
    }
    if (channel_bytes) {
        em.section(kSectionChannels, static_cast<uint32_t>(channel_bytes));
        for (const auto& c : extras.channels) {
            uint8_t rec[6];
            const uint16_t n = static_cast<uint16_t>(std::min<size_t>(c.second.size(), 0xFFFF));
            put32(rec, static_cast<uint32_t>(c.first));
            put16(rec + 4, n);
            em.put(rec, sizeof rec);
            em.put(c.second.data(), n);
        }
    }
    if (event_bytes) {
        em.section(kSectionEvents, static_cast<uint32_t>(event_bytes));
        for (const auto& e : *extras.events) {
            uint8_t rec[4];
            const uint16_t n = static_cast<uint16_t>(std::min<size_t>(e.topic.size(), 0xFFFF));
            put16(rec, n);
            em.put(rec, 2);
            em.put(e.topic.data(), n);
            put32(rec, static_cast<uint32_t>(e.data.size()));
            em.put(rec, 4);
            em.put(e.data.data(), e.data.size());
        }
    }
    const osal::Sha256::Digest d = em.sha.finish();
    std::memcpy(out_digest, d.data(), 32);
    if (em.ok) em.ok = out(d.data(), d.size());
    if (!em.ok) {
        if (why.empty()) why = "the snapshot could not be written";
        return 0;
    }
    return em.total + d.size();
}

bool parse_snapshot_header(const uint8_t* p, size_t len, SnapshotHeader& out, std::string& why) {
    if (len < kSnapshotHeaderBytes) { why = "the snapshot is shorter than its header"; return false; }
    if (std::memcmp(p, "K32S", 4) != 0) { why = "not a kube32 snapshot (no K32S magic)"; return false; }
    out.version = get16(p + 4);
    std::memcpy(out.image_sha, p + 8, 32);
    out.memory_bytes = get32(p + 40);
    out.memory_pages = get32(p + 44);
    out.pod_time_ms = get64(p + 48);
    out.unix_ms = get64(p + 56);
    out.sections = get32(p + 64);
    return true;
}

// -----------------------------------------------------------------------------
// SnapshotRestorer
// -----------------------------------------------------------------------------
struct SnapshotRestorer::Hash {
    osal::Sha256 sha;
};

SnapshotRestorer::SnapshotRestorer(ContainerLifecycleController& ctrl, const uint8_t image_sha[32]) : ctrl_(ctrl), hash_(new Hash) {
    std::memcpy(image_sha_, image_sha, 32);
}

SnapshotRestorer::~SnapshotRestorer() { delete hash_; }

bool SnapshotRestorer::fail(const char* fmt, ...) {
    if (phase_ != Phase::Failed) {
        char line[160];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        why_ = line;
    }
    phase_ = Phase::Failed;
    std::vector<uint8_t>().swap(buf_);
    return false;
}

bool SnapshotRestorer::begin_memory() {
    IM3Module module = static_cast<IM3Module>(ctrl_.m3_module_handle());
    IM3Runtime rt = static_cast<IM3Runtime>(ctrl_.m3_runtime_handle());
    IM3Memory mem = memory0(module);
    if (!header.memory_bytes) return true;
    if (!mem || !rt) return fail("the snapshot has linear memory and the module declares none");
    if (header.memory_pages != mem->numPages) {
        M3Result r = ResizeMemory(rt, mem, header.memory_pages);
        if (r != m3Err_none) return fail("no memory for the snapshot's %u pages (%u bytes): %s", (unsigned)header.memory_pages, (unsigned)header.memory_bytes, r);
    }
    size_t have = 0;
    memory_ = ctrl_.guest_memory(have);
    if (!memory_ || have < header.memory_bytes) {
        return fail("the pod's memory limit gives %u bytes of linear memory, the snapshot holds %u", (unsigned)have, (unsigned)header.memory_bytes);
    }
    // Bytes past the snapshot's (a higher limit here) read as zero, as fresh memory
    if (have > header.memory_bytes) std::memset(memory_ + header.memory_bytes, 0, have - header.memory_bytes);
    return true;
}

bool SnapshotRestorer::feed(const uint8_t* data, size_t len) {
    PodMemoryScope charged(ctrl_.memory_account());   // memory and tables grown to the snapshot's sizes
    while (len > 0) {
        if (phase_ == Phase::Failed) return false;
        if (phase_ == Phase::Done) return fail("bytes after the snapshot's trailer");
        size_t take = len;
        switch (phase_) {
        case Phase::Header: {
            take = std::min(len, kSnapshotHeaderBytes - head_len_);
            std::memcpy(head_ + head_len_, data, take);
            head_len_ += take;
            data += take;
            len -= take;
            if (head_len_ < kSnapshotHeaderBytes) continue;
            std::string why;
            if (!parse_snapshot_header(head_, head_len_, header, why)) return fail("%s", why.c_str());
            if (header.version != kSnapshotVersion) {
                return fail("snapshot version %u is not known here (this node reads version %u)", (unsigned)header.version, (unsigned)kSnapshotVersion);
            }
            if (std::memcmp(header.image_sha, image_sha_, 32) != 0) {
                return fail("the snapshot is of image sha256 %s..., the pod's image is %s...", Hex12(header.image_sha).text, Hex12(image_sha_).text);
            }
            hash_->sha.update(head_, head_len_);
            total_ += head_len_;
            sections_left_ = header.sections;
            if (!begin_memory()) return false;
            phase_ = sections_left_ ? Phase::SectionHead : Phase::Trailer;
            head_len_ = 0;
            continue;   // already hashed
        }
        case Phase::SectionHead: {
            take = std::min<size_t>(len, 6 - head_len_);
            std::memcpy(head_ + head_len_, data, take);
            head_len_ += take;
            hash_->sha.update(data, take);
            total_ += take;
            data += take;
            len -= take;
            if (head_len_ < 6) continue;
            head_len_ = 0;
            section_type_ = get16(head_);
            section_len_ = get32(head_ + 2);
            section_done_ = 0;
            if (section_type_ == kSectionMemory) {
                if (section_len_ != header.memory_bytes) return fail("the memory section does not match the header's memory size");
            } else if (section_len_ > kMaxSmallSection) {
                return fail("section %u of %u bytes is too large", (unsigned)section_type_, (unsigned)section_len_);
            } else {
                buf_.clear();
                buf_.reserve(section_len_);
            }
            phase_ = Phase::SectionBody;
            if (section_len_ == 0 && !end_section()) return false;
            continue;
        }
        case Phase::SectionBody: {
            take = std::min<size_t>(len, section_len_ - section_done_);
            if (section_type_ == kSectionMemory) {
                std::memcpy(memory_ + section_done_, data, take);
            } else {
                buf_.insert(buf_.end(), data, data + take);
            }
            section_done_ += static_cast<uint32_t>(take);
            hash_->sha.update(data, take);
            total_ += take;
            data += take;
            len -= take;
            if (section_done_ == section_len_ && !end_section()) return false;
            continue;
        }
        case Phase::Trailer: {
            take = std::min(len, kSnapshotTrailerBytes - head_len_);
            std::memcpy(head_ + head_len_, data, take);
            head_len_ += take;
            total_ += take;
            data += take;
            len -= take;
            if (head_len_ < kSnapshotTrailerBytes) continue;
            const osal::Sha256::Digest d = hash_->sha.finish();
            if (std::memcmp(d.data(), head_, 32) != 0) return fail("the snapshot's trailer does not match its contents (damaged in transfer or storage)");
            phase_ = Phase::Done;
            continue;
        }
        default:
            return false;
        }
        data += take;
        len -= take;
    }
    return phase_ != Phase::Failed;
}

bool SnapshotRestorer::end_section() {
    bool ok = true;
    switch (section_type_) {
    case kSectionGlobals: ok = apply_globals(); break;
    case kSectionTable: ok = apply_table(); break;
    case kSectionChannels: ok = read_channels(); break;
    case kSectionEvents: ok = read_events(); break;
    default: break;   // memory is in place; a section this node does not know is skipped
    }
    if (!ok) return false;
    std::vector<uint8_t>().swap(buf_);
    phase_ = --sections_left_ ? Phase::SectionHead : Phase::Trailer;
    return true;
}

bool SnapshotRestorer::apply_globals() {
    IM3Module module = static_cast<IM3Module>(ctrl_.m3_module_handle());
    if (buf_.size() % 13) return fail("the globals section is malformed");
    for (size_t off = 0; off < buf_.size(); off += 13) {
        const uint32_t i = get32(&buf_[off]);
        const uint8_t type = buf_[off + 4];
        const uint64_t bits = get64(&buf_[off + 5]);
        if (i >= module->numGlobals || !saved_global(module->globals[i]) || value_type_code(module->globals[i].type) != type) {
            return fail("global %u of the snapshot does not match the module", (unsigned)i);
        }
        M3Global& g = module->globals[i];
        if (type == kTypeFuncref) {
            const uint32_t idx = static_cast<uint32_t>(bits);
            if (idx != kNullFunction && idx >= module->numFunctions) return fail("global %u names no function of the module", (unsigned)i);
            g.refValue = idx == kNullFunction ? nullptr : &module->functions[idx];
        } else {
            std::memcpy(&g.i64Value, &bits, (type == kTypeI32 || type == kTypeF32) ? 4 : 8);
        }
    }
    return true;
}

bool SnapshotRestorer::apply_table() {
    IM3Module module = static_cast<IM3Module>(ctrl_.m3_module_handle());
    IM3Runtime rt = static_cast<IM3Runtime>(ctrl_.m3_runtime_handle());
    if (buf_.size() < 8) return fail("a table section is malformed");
    const uint32_t t = get32(&buf_[0]);
    const uint32_t size = get32(&buf_[4]);
    if (buf_.size() != 8 + static_cast<size_t>(size) * 4) return fail("a table section is malformed");
    IM3Table tbl = t < module->numTables ? module->tables[t] : nullptr;
    if (!tbl || tbl->owner != module || size < tbl->size || (tbl->maxSize && size > tbl->maxSize)) {
        return fail("table %u of the snapshot (%u entries) does not match the module", (unsigned)t, (unsigned)size);
    }
    if (size > tbl->size) {
        // table.grow, as the guest did it
        void** grown = m3_ReallocArray(void*, tbl->elements, size, tbl->size);
        if (!grown) return fail("no memory for table %u of %u entries", (unsigned)t, (unsigned)size);
        rt->tableElementsUsed += size - tbl->size;
        tbl->elements = grown;
        tbl->size = size;
    }
    for (uint32_t i = 0; i < size; ++i) {
        const uint32_t idx = get32(&buf_[8 + i * 4]);
        if (idx == kNullFunction) { tbl->elements[i] = nullptr; continue; }
        if (tbl->type != c_m3Type_funcref || idx >= module->numFunctions) {
            return fail("table %u entry %u names no function of the module", (unsigned)t, (unsigned)i);
        }
        tbl->elements[i] = &module->functions[idx];
    }
    return true;
}

bool SnapshotRestorer::read_channels() {
    for (size_t off = 0; off < buf_.size();) {
        if (buf_.size() - off < 6) return fail("the channels section is malformed");
        const int32_t handle = static_cast<int32_t>(get32(&buf_[off]));
        const uint16_t n = get16(&buf_[off + 4]);
        off += 6;
        if (buf_.size() - off < n) return fail("the channels section is malformed");
        channels.emplace_back(handle, std::string(reinterpret_cast<const char*>(&buf_[off]), n));
        off += n;
    }
    return true;
}

bool SnapshotRestorer::read_events() {
    for (size_t off = 0; off < buf_.size();) {
        if (buf_.size() - off < 2) return fail("the events section is malformed");
        const uint16_t tn = get16(&buf_[off]);
        off += 2;
        if (buf_.size() - off < static_cast<size_t>(tn) + 4) return fail("the events section is malformed");
        HeldEvent e;
        e.topic.assign(reinterpret_cast<const char*>(&buf_[off]), tn);
        off += tn;
        const uint32_t dn = get32(&buf_[off]);
        off += 4;
        if (buf_.size() - off < dn) return fail("the events section is malformed");
        e.data.assign(reinterpret_cast<const char*>(&buf_[off]), dn);
        off += dn;
        events.push_back(std::move(e));
    }
    return true;
}

bool SnapshotRestorer::finish() {
    if (phase_ == Phase::Failed) return false;
    if (phase_ != Phase::Done) return fail("the snapshot ended early (%u bytes)", (unsigned)total_);
    return true;
}

// -----------------------------------------------------------------------------
// Static blockers
// -----------------------------------------------------------------------------
std::vector<std::string> static_migration_blockers(std::string_view pod_json) {
    std::vector<std::string> out;
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(pod_json, doc, err)) return out;
    auto add = [&](std::string b) {
        if (std::find(out.begin(), out.end(), b) == out.end()) out.push_back(std::move(b));
    };
    const models::JsonValue* meta = doc.get("metadata");
    const models::JsonValue* ann = meta ? meta->get("annotations") : nullptr;
    const models::JsonValue* spec = doc.get("spec");
    const models::JsonValue* containers = spec ? spec->get("containers") : nullptr;
    if (containers && containers->is_array()) {
        for (const auto& c : containers->arr_val) {
            const models::JsonValue* res = c.get("resources");
            for (const char* part : {"requests", "limits"}) {
                const models::JsonValue* m = res ? res->get(part) : nullptr;
                if (!m || !m->is_object()) continue;
                for (const auto& kv : m->obj_val) {
                    const std::string& key = kv.first;
                    if (kv.second.is_string() && kv.second.str_val == "0") continue;
                    const bool hw = key.rfind("hardware.kube32.io/", 0) == 0 && key != "hardware.kube32.io/ipc";
                    if (hw || key.rfind("esp32.io/", 0) == 0 || key.rfind("esp.io/", 0) == 0) add("grant " + key);
                }
            }
            const models::JsonValue* sc = c.get("securityContext");
            const models::JsonValue* caps = sc ? sc->get("capabilities") : nullptr;
            const models::JsonValue* added = caps ? caps->get("add") : nullptr;
            if (added && added->is_array()) {
                for (const auto& cap : added->arr_val) {
                    if (!cap.is_string()) continue;
                    const std::string& n = cap.str_val;
                    if (n == "DISPLAY" || n == "TFT_DISPLAY" || n == "KEYBOARD" || n == "SPEAKER" || n == "AUDIO" || n == "SD" ||
                        n == "SD_CARD" || n == "CHIP_TEMP") {
                        add("grant capability " + n);
                    }
                }
            }
        }
    }
    if (ann && !ann->get_string(models::kAnnotationI2cDevice, "").empty()) add(std::string("grant ") + models::kAnnotationI2cDevice);
    if (const models::JsonValue* vols = spec ? spec->get("volumes") : nullptr; vols && vols->is_array()) {
        for (const auto& v : vols->arr_val) {
            const models::JsonValue* sd = v.get("kube32.io/sd");
            const models::JsonValue* pvc = v.get("persistentVolumeClaim");
            if (!sd && !pvc) continue;
            std::string claim = sd ? sd->get_string("claim", "") : pvc->get_string("claimName", "");
            if (claim.empty()) claim = v.get_string("name", "");
            add("volume claim " + claim + " on node storage");
        }
    }
    if (ann && ann->get_string("kube32.io/migration", "") == "Never") add("annotation kube32.io/migration: Never");
    return out;
}

} // namespace kube32::runtime
