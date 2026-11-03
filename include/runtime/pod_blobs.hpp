#pragma once

// The host side of the k32_blob_* imports (include/runtime/k32_app_abi.h): a
// pod opens the node's blobs (storage/blob_store.hpp) by name and reads them
// in place.
//
// A pod may open only the blobs its annotation kube32.io/blobs names (comma
// separated). It holds up to K32_BLOB_MAX_OPEN at once; the handles are
// closed when the container stops or restarts, and a checkpoint does not
// carry them (k32_restored opens them again). The linear algebra imports read
// a blob that is mapped (flash, or any blob on kube32-sim) through a blob
// address, K32_BLOB_ADDR(handle, offset), in place of a weight pointer.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

typedef struct M3Module* IM3Module;

namespace kube32::storage {
class BlobReader;
}

namespace kube32::runtime {

class PodBlobs {
public:
    static constexpr uint32_t kMaxOpen = 4;

    static PodBlobs& instance();

    // The names kube32.io/blobs grants the pod (set when it is deployed)
    void set_allowed(const std::string& pod_id, std::vector<std::string> names);
    // Closes what the pod left open (container stop or restart)
    void release_pod(const std::string& pod_id);
    // Closes and forgets the pod (it is removed from the node)
    void forget_pod(const std::string& pod_id);

    // All return a value >= 0 or a K32_ERR_* code
    int32_t open(const std::string& pod_id, std::string_view name);
    int32_t size(const std::string& pod_id, int32_t handle);
    int32_t read(const std::string& pod_id, int32_t handle, uint32_t offset, uint8_t* dst, uint32_t len);
    int32_t mapped(const std::string& pod_id, int32_t handle);
    int32_t close(const std::string& pod_id, int32_t handle);

    // Bytes [offset, offset + len) of an open, mapped blob for the native
    // kernels; null with err K32_ERR_BADF (not open), K32_ERR_FAULT (outside
    // the blob) or K32_ERR_NOTSUP (the blob is not mapped). The pointer stays
    // valid until the pod closes the handle.
    const uint8_t* view(const std::string& pod_id, uint32_t handle, uint64_t offset, uint64_t len, int32_t& err) const;

    // "<name> (handle <n>)" for each open blob
    std::vector<std::string> open_blobs(const std::string& pod_id) const;

    // Parses kube32.io/blobs: names separated by commas or whitespace
    static std::vector<std::string> parse_annotation(std::string_view value);

private:
    PodBlobs() = default;
    struct Slot {
        std::shared_ptr<storage::BlobReader> reader;
        std::string name;
    };
    struct Table {
        std::vector<std::string> allowed;
        Slot slots[kMaxOpen];
    };
    std::shared_ptr<storage::BlobReader> reader_for(const std::string& pod_id, int32_t handle, int32_t& err) const;

    mutable std::mutex mutex_;
    std::map<std::string, Table> tables_;
};

// Links k32_blob_open, k32_blob_size, k32_blob_read, k32_blob_mapped and
// k32_blob_close into a pod's module
void link_blob_host_functions(IM3Module module);

} // namespace kube32::runtime
