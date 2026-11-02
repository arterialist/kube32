#include "api/checkpoint_api.hpp"
#include "api/route_handler.hpp"
#include "models/kube_models.hpp"
#include "osal/sha256.hpp"
#include "runtime/pod_memory_pool.h"
#include "runtime/pod_snapshot.hpp"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kube32::api {

namespace {

constexpr std::string_view kPrefix = "/apis/kube32.io/v1/namespaces/";
constexpr std::string_view kStorePrefix = "/registry/checkpoints/";

// Snapshot data is pod state: it goes to the pod pool block where there is
// one, so admission and allocatable memory count it and the system's own
// memory stays whole. When the block is full, PSRAM above the system's
// reserve (kube32_pool_psram_reserve), or the heap without PSRAM.
uint8_t* blob_alloc(size_t len) {
    if (void* p = kube32_pool_sub_alloc(len)) return static_cast<uint8_t*>(p);
#if defined(ESP_PLATFORM)
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < len + kube32_pool_psram_reserve()) return nullptr;
        return static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
#endif
    return static_cast<uint8_t*>(std::malloc(len));
}

void blob_free(uint8_t* p) {
    if (!kube32_pool_sub_free(p)) std::free(p);
}

bool fail(HttpConnection& conn, int code, const char* reason, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
bool fail(HttpConnection& conn, int code, const char* reason, const char* fmt, ...) {
    char message[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    HttpResponse r = RouteHandler::make_status(code, reason, message);
    if (conn.body_remaining() > 0) conn.set_close();
    return conn.send_simple(r.status_code, r.content_type, r.body);
}

} // namespace

struct CheckpointApi::Blob {
    uint8_t* data{nullptr};
    size_t len{0};
    uint64_t seq{0};
    ~Blob() { blob_free(data); }
};

CheckpointApi::CheckpointApi(storage::Store& store, RouteHandler& routes, size_t cap_bytes, HttpHandler* next)
    : store_(store), routes_(routes), cap_(cap_bytes), next_(next) {
    // A deleted Checkpoint takes its data with it
    watch_id_ = store_.watch(std::string(kStorePrefix), [this](const storage::WatchEvent& ev) {
        if (ev.type == storage::WatchEventType::Deleted && ev.key.size() > kStorePrefix.size()) drop(ev.key.substr(kStorePrefix.size()));
    });
}

CheckpointApi::~CheckpointApi() {
    store_.unwatch(watch_id_);
}

size_t CheckpointApi::used_bytes() const {
    osal::LockGuard<osal::Mutex> lock(mu_);
    return used_;
}

size_t CheckpointApi::count() const {
    osal::LockGuard<osal::Mutex> lock(mu_);
    return blobs_.size();
}

void CheckpointApi::drop(const std::string& key) {
    std::shared_ptr<Blob> gone;   // freed once a download still sending it is done
    osal::LockGuard<osal::Mutex> lock(mu_);
    auto it = blobs_.find(key);
    if (it == blobs_.end()) return;
    used_ -= it->second->len;
    gone = std::move(it->second);
    blobs_.erase(it);
}

bool CheckpointApi::match(std::string_view path, std::string& ns, std::string& name) {
    if (path.rfind(kPrefix, 0) != 0) return false;
    std::string_view rest = path.substr(kPrefix.size());
    const size_t a = rest.find('/');
    if (a == std::string_view::npos || a == 0) return false;
    ns.assign(rest.substr(0, a));
    rest.remove_prefix(a + 1);
    constexpr std::string_view kKind = "checkpoints/";
    if (rest.rfind(kKind, 0) != 0) return false;
    rest.remove_prefix(kKind.size());
    const size_t b = rest.find('/');
    if (b == std::string_view::npos || b == 0 || rest.substr(b) != "/data") return false;
    name.assign(rest.substr(0, b));
    return true;
}

bool CheckpointApi::streams_body(const HttpRequest& req) {
    std::string ns, name;
    if (match(req.path, ns, name)) return req.method == HttpMethod::Put;
    return next_ && next_->streams_body(req);
}

bool CheckpointApi::handle(HttpConnection& conn) {
    const HttpRequest& req = conn.request();
    std::string ns, name;
    if (!match(req.path, ns, name)) return next_ && next_->handle(conn);
    if (!routes_.authenticated(req)) return fail(conn, 401, "Unauthorized", "Unauthorized");
    const std::string key = ns + "/" + name;
    std::string json;
    if (!store_.get(std::string(kStorePrefix) + key, json).ok()) {
        drop(key);
        return fail(conn, 404, "NotFound", "checkpoints.kube32.io \"%s\" not found", name.c_str());
    }
    if (req.method == HttpMethod::Put) return put(conn, key, ns, name);
    if (req.method == HttpMethod::Get || req.method == HttpMethod::Head) return get(conn, key, name);
    return fail(conn, 405, "MethodNotAllowed", "GET and PUT are allowed on checkpoint data");
}

bool CheckpointApi::get(HttpConnection& conn, const std::string& key, const std::string& name) {
    std::shared_ptr<Blob> blob;
    {
        osal::LockGuard<osal::Mutex> lock(mu_);
        auto it = blobs_.find(key);
        if (it != blobs_.end()) blob = it->second;
    }
    if (!blob) return fail(conn, 404, "NotFound", "checkpoint %s has no data", name.c_str());
    if (!conn.begin_fixed(200, "application/octet-stream", blob->len)) return true;
    if (conn.request().method == HttpMethod::Head) return true;
    for (size_t off = 0; off < blob->len;) {
        const size_t n = std::min<size_t>(8192, blob->len - off);
        if (!conn.send_raw(blob->data + off, n)) {
            conn.set_close();
            break;
        }
        off += n;
    }
    return true;
}

bool CheckpointApi::put(HttpConnection& conn, const std::string& key, const std::string& ns, const std::string& name) {
    if (conn.body_chunked()) return fail(conn, 411, "LengthRequired", "the snapshot is uploaded with a Content-Length");
    const size_t len = conn.body_remaining();
    if (len < runtime::kSnapshotHeaderBytes + runtime::kSnapshotTrailerBytes) return fail(conn, 422, "Invalid", "the body is not a K32S snapshot");
    if (len > cap_) {
        return fail(conn, 413, "RequestEntityTooLarge", "a snapshot of %u bytes is larger than the %u bytes kept for checkpoint data",
                    (unsigned)len, (unsigned)cap_);
    }
    // Room first: the oldest other snapshots go
    {
        std::vector<std::shared_ptr<Blob>> gone;
        osal::LockGuard<osal::Mutex> lock(mu_);
        auto mine = blobs_.find(key);
        size_t used = used_ - (mine != blobs_.end() ? mine->second->len : 0);
        while (used + len > cap_) {
            auto oldest = blobs_.end();
            for (auto it = blobs_.begin(); it != blobs_.end(); ++it) {
                if (it->first != key && (oldest == blobs_.end() || it->second->seq < oldest->second->seq)) oldest = it;
            }
            if (oldest == blobs_.end()) break;
            printf("[CHECKPOINT] dropping the data of %s (%u bytes) for %s\n", oldest->first.c_str(), (unsigned)oldest->second->len, key.c_str());
            used -= oldest->second->len;
            used_ -= oldest->second->len;
            gone.push_back(std::move(oldest->second));
            blobs_.erase(oldest);
        }
    }
    auto blob = std::make_shared<Blob>();
    blob->data = blob_alloc(len);
    if (!blob->data) return fail(conn, 507, "InsufficientStorage", "no memory for a snapshot of %u bytes besides the system's reserve", (unsigned)len);
    blob->len = len;
    size_t got = 0;
    while (got < len) {
        const long n = conn.read_body(blob->data + got, len - got);
        if (n <= 0) {
            conn.set_close();
            return fail(conn, 400, "BadRequest", "the request body ended early");
        }
        got += static_cast<size_t>(n);
    }
    // Checked before it is kept: a damaged upload is refused here, not at the restore
    runtime::SnapshotHeader h;
    std::string why;
    if (!runtime::parse_snapshot_header(blob->data, len, h, why)) return fail(conn, 422, "Invalid", "%s", why.c_str());
    if (h.version != runtime::kSnapshotVersion) return fail(conn, 422, "Invalid", "snapshot version %u is not known here", (unsigned)h.version);
    const size_t body = len - runtime::kSnapshotTrailerBytes;
    const osal::Sha256::Digest digest = osal::Sha256::of(blob->data, body);
    if (std::memcmp(digest.data(), blob->data + body, 32) != 0) return fail(conn, 422, "Invalid", "the snapshot's trailer does not match its contents");
    {
        std::shared_ptr<Blob> old;
        osal::LockGuard<osal::Mutex> lock(mu_);
        blob->seq = ++seq_;
        auto& slot = blobs_[key];
        if (slot) used_ -= slot->len;
        old = std::move(slot);
        slot = blob;
        used_ += len;
    }
    record(std::string(kStorePrefix) + key, len, blob->data);
    std::string json;
    store_.get(std::string(kStorePrefix) + key, json);
    printf("[CHECKPOINT] %s/%s: %u bytes stored (%u held in all)\n", ns.c_str(), name.c_str(), (unsigned)len, (unsigned)used_bytes());
    return conn.send_simple(200, "application/json", json);
}

void CheckpointApi::record(const std::string& store_key, size_t size, const uint8_t* data) {
    runtime::SnapshotHeader h;
    std::string why;
    runtime::parse_snapshot_header(data, size, h, why);
    osal::Sha256::Digest trailer, image;
    std::memcpy(trailer.data(), data + size - runtime::kSnapshotTrailerBytes, 32);
    std::memcpy(image.data(), h.image_sha, 32);
    char text[256];
    snprintf(text, sizeof(text), "{\"status\":{\"sizeBytes\":%u,\"snapshotSha256\":\"%s\",\"imageSha256\":\"%s\",\"podTimeMs\":%llu}}",
             (unsigned)size, osal::Sha256::hex(trailer).c_str(), osal::Sha256::hex(image).c_str(), (unsigned long long)h.pod_time_ms);
    models::JsonValue patch;
    std::string err;
    models::parse_json(text, patch, err);
    // A concurrent status write wins the race once; the next attempt merges with it
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string json;
        uint64_t version = 0;
        models::JsonValue doc;
        if (!store_.get_extended(store_key, json, &version).ok() || !models::parse_json(json, doc, err)) return;
        models::merge_json(doc, patch);
        if (store_.put_extended(store_key, doc.to_string(), true, true, version).code != storage::StatusCode::VersionConflict) return;
    }
}

} // namespace kube32::api
