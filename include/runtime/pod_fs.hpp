#pragma once

// Pod storage: the host side of the k32_fs_* imports (see k32_app_abi.h).
//
// A pod with a volume claim (spec.volumes: persistentVolumeClaim, or
// kube32.io/sd for one that must be on the microSD card) owns one directory
// on the node's storage root, <root>/kube32/volumes/<claim>/ (see
// hal/node_storage.hpp). Every call checks the CapabilityFirewall, then runs
// plain POSIX file operations through the VFS (FATFS on the board, host
// directories in the simulator), so firmware and simulator share this code.

#include <cstdint>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace kube32::runtime {

class PodFs {
public:
    static PodFs& instance();

    // All return a value >= 0 or a K32_ERR_* code from k32_app_abi.h.
    int32_t open(const std::string& pod_id, std::string_view path, uint32_t flags);
    int32_t read(const std::string& pod_id, int32_t fd, uint8_t* buf, uint32_t len);
    int32_t write(const std::string& pod_id, int32_t fd, const uint8_t* buf, uint32_t len);
    int32_t close(const std::string& pod_id, int32_t fd);
    int32_t size(const std::string& pod_id, int32_t fd);
    int32_t list(const std::string& pod_id, std::string_view dir, char* out, uint32_t cap);
    int32_t remove(const std::string& pod_id, std::string_view path);

    // Closes whatever the pod left open (container stop, restart, deploy).
    void release_pod(const std::string& pod_id);
    uint32_t open_count(const std::string& pod_id) const;

    // Checks a guest path: relative, no "..", no empty or "." components,
    // only [A-Za-z0-9._-] characters, at most K32_FS_MAX_PATH bytes. On
    // success `out` is the cleaned path ("" allowed only when allow_empty).
    static bool validate_relative_path(std::string_view path, bool allow_empty, std::string& out);

    // Absolute directory of a pod's volume, or "" without a claim, a storage
    // root, or the card a kube32.io/sd volume needs.
    std::string volume_root(const std::string& pod_id) const;

private:
    PodFs() = default;
    struct Slot {
        int host_fd{-1};
        bool readable{false};
        bool writable{false};
    };
    struct Table {
        Slot slots[4];
        bool dir_ready{false};
    };

    // Returns 0 and the volume root, or K32_ERR_PERM / K32_ERR_NODEV.
    int32_t resolve_root(const std::string& pod_id, std::string& out_root);
    static int32_t errno_to_k32(int err);

    mutable std::mutex mutex_;
    std::map<std::string, Table> tables_;
};

} // namespace kube32::runtime
