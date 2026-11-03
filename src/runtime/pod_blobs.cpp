#include "runtime/pod_blobs.hpp"
#include "runtime/app_pod.hpp"
#include "runtime/k32_app_abi.h"
#include "storage/blob_store.hpp"

#include "wasm3.h"
#include "m3_env.h"

#include <cstdio>

namespace kube32::runtime {

PodBlobs& PodBlobs::instance() {
    static PodBlobs s_blobs;
    return s_blobs;
}

std::vector<std::string> PodBlobs::parse_annotation(std::string_view value) {
    std::vector<std::string> out;
    std::string name;
    for (size_t i = 0; i <= value.size(); ++i) {
        const char c = i < value.size() ? value[i] : ',';
        if (c == ',' || c == ' ' || c == '\t' || c == '\n') {
            if (!name.empty()) out.push_back(name);
            name.clear();
        } else {
            name.push_back(c);
        }
    }
    return out;
}

void PodBlobs::set_allowed(const std::string& pod_id, std::vector<std::string> names) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (names.empty() && tables_.find(pod_id) == tables_.end()) return;
    tables_[pod_id].allowed = std::move(names);
}

void PodBlobs::release_pod(const std::string& pod_id) {
    // Readers are let go outside the lock: a card reader closes its file
    std::vector<std::shared_ptr<storage::BlobReader>> drop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tables_.find(pod_id);
        if (it == tables_.end()) return;
        for (Slot& s : it->second.slots) {
            if (s.reader) drop.push_back(std::move(s.reader));
            s.name.clear();
        }
    }
}

void PodBlobs::forget_pod(const std::string& pod_id) {
    std::vector<std::shared_ptr<storage::BlobReader>> drop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tables_.find(pod_id);
        if (it == tables_.end()) return;
        for (Slot& s : it->second.slots) {
            if (s.reader) drop.push_back(std::move(s.reader));
        }
        tables_.erase(it);
    }
}

int32_t PodBlobs::open(const std::string& pod_id, std::string_view name) {
    if (!storage::valid_blob_name(name)) return K32_ERR_INVAL;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tables_.find(pod_id);
        bool allowed = false;
        if (it != tables_.end()) {
            for (const std::string& a : it->second.allowed) allowed = allowed || a == name;
        }
        if (!allowed) {
            std::printf("[BLOBS] denied %s: blob %.*s is not in its kube32.io/blobs annotation\n", pod_id.c_str(), (int)name.size(), name.data());
            return K32_ERR_PERM;
        }
        bool free_slot = false;
        for (const Slot& s : it->second.slots) free_slot = free_slot || !s.reader;
        if (!free_slot) return K32_ERR_MFILE;
    }
    // The stores are searched outside the lock: a card open reads its directory
    std::shared_ptr<storage::BlobReader> reader;
    int32_t err = K32_ERR_NOENT;
    const std::vector<storage::BlobStore*> stores = storage::node_blob_stores();
    for (storage::BlobStore* store : stores) {
        int32_t e = K32_ERR_NOENT;
        reader = store->open(name, e);
        if (reader) break;
        if (e != K32_ERR_NOENT) err = e;
    }
    if (!reader) return stores.empty() ? K32_ERR_NODEV : err;
    if (reader->size() > 0x7FFFFFFFu) return K32_ERR_TOOBIG;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tables_.find(pod_id);
    if (it == tables_.end()) return K32_ERR_PERM;   // forgotten meanwhile
    for (uint32_t h = 0; h < kMaxOpen; ++h) {
        Slot& s = it->second.slots[h];
        if (s.reader) continue;
        s.reader = std::move(reader);
        s.name.assign(name.data(), name.size());
        return static_cast<int32_t>(h);
    }
    return K32_ERR_MFILE;
}

std::shared_ptr<storage::BlobReader> PodBlobs::reader_for(const std::string& pod_id, int32_t handle, int32_t& err) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tables_.find(pod_id);
    if (handle < 0 || handle >= static_cast<int32_t>(kMaxOpen) || it == tables_.end() || !it->second.slots[handle].reader) {
        err = K32_ERR_BADF;
        return nullptr;
    }
    err = K32_OK;
    return it->second.slots[handle].reader;
}

int32_t PodBlobs::size(const std::string& pod_id, int32_t handle) {
    int32_t err = K32_OK;
    auto r = reader_for(pod_id, handle, err);
    return r ? static_cast<int32_t>(r->size()) : err;
}

int32_t PodBlobs::read(const std::string& pod_id, int32_t handle, uint32_t offset, uint8_t* dst, uint32_t len) {
    int32_t err = K32_OK;
    auto r = reader_for(pod_id, handle, err);
    if (!r) return err;
    if (len > 0x7FFFFFFFu) len = 0x7FFFFFFFu;
    return r->read(offset, dst, len);
}

int32_t PodBlobs::mapped(const std::string& pod_id, int32_t handle) {
    int32_t err = K32_OK;
    auto r = reader_for(pod_id, handle, err);
    return r ? (r->view() ? 1 : 0) : err;
}

int32_t PodBlobs::close(const std::string& pod_id, int32_t handle) {
    std::shared_ptr<storage::BlobReader> drop;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tables_.find(pod_id);
    if (handle < 0 || handle >= static_cast<int32_t>(kMaxOpen) || it == tables_.end() || !it->second.slots[handle].reader) {
        return K32_ERR_BADF;
    }
    drop = std::move(it->second.slots[handle].reader);
    it->second.slots[handle].name.clear();
    return K32_OK;
}

const uint8_t* PodBlobs::view(const std::string& pod_id, uint32_t handle, uint64_t offset, uint64_t len, int32_t& err) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tables_.find(pod_id);
    if (handle >= kMaxOpen || it == tables_.end() || !it->second.slots[handle].reader) {
        err = K32_ERR_BADF;
        return nullptr;
    }
    const storage::BlobReader& r = *it->second.slots[handle].reader;
    if (!r.view()) {
        err = K32_ERR_NOTSUP;
        return nullptr;
    }
    if (offset > r.size() || len > r.size() - offset) {
        err = K32_ERR_FAULT;
        return nullptr;
    }
    err = K32_OK;
    return r.view() + offset;
}

std::vector<std::string> PodBlobs::open_blobs(const std::string& pod_id) const {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tables_.find(pod_id);
    if (it == tables_.end()) return out;
    for (uint32_t h = 0; h < kMaxOpen; ++h) {
        if (it->second.slots[h].reader) out.push_back(it->second.slots[h].name + " (handle " + std::to_string(h) + ")");
    }
    return out;
}

// ---- the imports -------------------------------------------------------------------

static inline uint32_t arg(const uint64_t* sp, int i) { return static_cast<uint32_t>(sp[i]); }
static const void* result(uint64_t* sp, int32_t rc) {
    *reinterpret_cast<int32_t*>(sp) = rc;
    return m3Err_none;
}
static bool in_memory(void* mem, uint32_t off, uint32_t len) {
    return static_cast<uint64_t>(off) + len <= m3_GetMemorySizeAt(mem);
}

// k32_blob_open(name, name_len) -> handle or K32_ERR_*
static const void* raw_blob_open(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
    const uint32_t off = arg(sp, 1), len = arg(sp, 2);
    if (len == 0 || len > storage::kBlobNameMax) return result(sp, len ? K32_ERR_NAMETOOLONG : K32_ERR_INVAL);
    if (!in_memory(mem, off, len)) return result(sp, K32_ERR_FAULT);
    return result(sp, PodBlobs::instance().open(app_pod_id(rt), std::string_view(static_cast<const char*>(mem) + off, len)));
}

static const void* raw_blob_size(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void*) {
    return result(sp, PodBlobs::instance().size(app_pod_id(rt), static_cast<int32_t>(arg(sp, 1))));
}

// k32_blob_read(blob, offset, buf, len) -> bytes copied or K32_ERR_*
static const void* raw_blob_read(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void* mem) {
    const uint32_t buf = arg(sp, 3), len = arg(sp, 4);
    if (!in_memory(mem, buf, len)) return result(sp, K32_ERR_FAULT);
    return result(sp, PodBlobs::instance().read(app_pod_id(rt), static_cast<int32_t>(arg(sp, 1)), arg(sp, 2),
                                                static_cast<uint8_t*>(mem) + buf, len));
}

static const void* raw_blob_mapped(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void*) {
    return result(sp, PodBlobs::instance().mapped(app_pod_id(rt), static_cast<int32_t>(arg(sp, 1))));
}

static const void* raw_blob_close(IM3Runtime rt, IM3ImportContext, uint64_t* sp, void*) {
    return result(sp, PodBlobs::instance().close(app_pod_id(rt), static_cast<int32_t>(arg(sp, 1))));
}

void link_blob_host_functions(IM3Module module) {
    m3_LinkRawFunction(module, "env", "k32_blob_open", "i(ii)", &raw_blob_open);
    m3_LinkRawFunction(module, "env", "k32_blob_size", "i(i)", &raw_blob_size);
    m3_LinkRawFunction(module, "env", "k32_blob_read", "i(iiii)", &raw_blob_read);
    m3_LinkRawFunction(module, "env", "k32_blob_mapped", "i(i)", &raw_blob_mapped);
    m3_LinkRawFunction(module, "env", "k32_blob_close", "i(i)", &raw_blob_close);
}

} // namespace kube32::runtime
