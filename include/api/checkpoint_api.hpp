#pragma once

// The data of kube32.io/v1 Checkpoints (docs/migration.md) on the API server:
//
//   GET /apis/kube32.io/v1/namespaces/<ns>/checkpoints/<name>/data
//       the snapshot (application/octet-stream, Content-Length); 404 while
//       the Checkpoint has none
//   PUT /apis/kube32.io/v1/namespaces/<ns>/checkpoints/<name>/data
//       stores the body (Content-Length required) as the Checkpoint's
//       snapshot. Its K32S magic, version and trailer are checked (422 when
//       they are wrong), and the Checkpoint's status gets sizeBytes,
//       snapshotSha256 (the trailer), imageSha256 and podTimeMs from it.
//
// Snapshots live outside the object store, in memory (PSRAM on the board),
// within a total cap: storing one that does not fit drops the oldest others
// first; one larger than the cap gets 413. Deleting the Checkpoint drops its
// data. The data is not persisted: after a control plane restart the
// Checkpoint objects remain without it. Requests are authenticated like
// every API call. Other requests go to `next`.

#include "api/http_server.hpp"
#include "osal/mutex.hpp"
#include "storage/store.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace kube32::api {

class CheckpointApi : public HttpHandler {
public:
    CheckpointApi(storage::Store& store, RouteHandler& routes, size_t cap_bytes, HttpHandler* next = nullptr);
    ~CheckpointApi() override;

    bool handle(HttpConnection& conn) override;
    bool streams_body(const HttpRequest& req) override;

    // Bytes of snapshot data held, and how many snapshots
    size_t used_bytes() const;
    size_t count() const;

    // "/apis/kube32.io/v1/namespaces/<ns>/checkpoints/<name>/data"
    static bool match(std::string_view path, std::string& ns, std::string& name);

private:
    struct Blob;
    bool put(HttpConnection& conn, const std::string& key, const std::string& ns, const std::string& name);
    bool get(HttpConnection& conn, const std::string& key, const std::string& name);
    void drop(const std::string& key);
    // Writes the snapshot's facts into the Checkpoint's status
    void record(const std::string& store_key, size_t size, const uint8_t* data);

    storage::Store& store_;
    RouteHandler& routes_;
    size_t cap_;
    HttpHandler* next_;
    uint32_t watch_id_{0};
    mutable osal::Mutex mu_;
    std::map<std::string, std::shared_ptr<Blob>> blobs_;   // by "<ns>/<name>"
    size_t used_{0};
    uint64_t seq_{0};
};

} // namespace kube32::api
