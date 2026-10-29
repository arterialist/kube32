#include "runtime/pod_fs.hpp"
#include "runtime/k32_app_abi.h"
#include "hal/capability_firewall.hpp"
#include "hal/node_storage.hpp"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kube32::runtime {

PodFs& PodFs::instance() {
    static PodFs s_fs;
    return s_fs;
}

int32_t PodFs::errno_to_k32(int err) {
    switch (err) {
        case ENOENT: case ENOTDIR: return K32_ERR_NOENT;
        case EIO: return K32_ERR_IO;
        case EBADF: return K32_ERR_BADF;
        case ENOMEM: return K32_ERR_NOMEM;
        case EEXIST: return K32_ERR_EXIST;
        case ENODEV: case ENXIO: return K32_ERR_NODEV;
        case EISDIR: return K32_ERR_ISDIR;
        case EINVAL: case ENOTEMPTY: return K32_ERR_INVAL;
        case EMFILE: case ENFILE: return K32_ERR_MFILE;
        case ENOSPC: case EFBIG: return K32_ERR_NOSPC;
        case ENAMETOOLONG: return K32_ERR_NAMETOOLONG;
        case EACCES: case EPERM: case EROFS: return K32_ERR_PERM;
        default: return K32_ERR_IO;
    }
}

bool PodFs::validate_relative_path(std::string_view path, bool allow_empty, std::string& out) {
    out.clear();
    if (path.size() > K32_FS_MAX_PATH) return false;
    if (path.empty()) return allow_empty;
    if (path.front() == '/' || path.back() == '/') return false;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        std::string_view comp = path.substr(start, end - start);
        if (comp.empty() || comp == "." || comp == "..") return false;
        for (char c : comp) {
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            if (!ok) return false;
        }
        if (!out.empty()) out.push_back('/');
        out.append(comp.data(), comp.size());
        start = end + 1;
    }
    return true;
}

// The volume's directory on the node storage root, "" without a claim, a
// root, or the card the claim insists on.
static std::string claim_root(const std::string& pod_id) {
    const hal::CapabilityFirewall& fw = hal::CapabilityFirewall::instance();
    std::string claim = fw.sd_claim(pod_id);
    if (claim.empty()) return std::string();
    hal::NodeStorageInfo si = hal::node_storage_info();
    if (si.root.empty()) return std::string();
    if (fw.volume_needs_card(pod_id) && si.medium != hal::StorageMedium::SdCard) return std::string();
    return si.root + "/kube32/volumes/" + claim;
}

std::string PodFs::volume_root(const std::string& pod_id) const {
    return claim_root(pod_id);
}

int32_t PodFs::resolve_root(const std::string& pod_id, std::string& out_root) {
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::SdCard, 0, hal::AccessMode::ReadWrite)) {
        return K32_ERR_PERM;
    }
    if (hal::CapabilityFirewall::instance().sd_claim(pod_id).empty()) return K32_ERR_PERM;
    out_root = claim_root(pod_id);
    if (out_root.empty()) return K32_ERR_NODEV;

    std::lock_guard<std::mutex> lock(mutex_);
    Table& t = tables_[pod_id];
    if (!t.dir_ready) {
        if (!hal::node_storage_make_dirs(out_root)) return K32_ERR_IO;
        t.dir_ready = true;
    }
    return 0;
}

int32_t PodFs::open(const std::string& pod_id, std::string_view path, uint32_t flags) {
    std::string rel;
    if (!validate_relative_path(path, false, rel)) return path.size() > K32_FS_MAX_PATH ? K32_ERR_NAMETOOLONG : K32_ERR_INVAL;
    const uint32_t known = K32_FS_O_RDONLY | K32_FS_O_WRONLY | K32_FS_O_RDWR | K32_FS_O_CREAT | K32_FS_O_EXCL | K32_FS_O_TRUNC | K32_FS_O_APPEND;
    if ((flags & ~known) != 0 || (flags & 3) == 3) return K32_ERR_INVAL;

    std::string root;
    int32_t rc = resolve_root(pod_id, root);
    if (rc != 0) return rc;

    // ABI flag values are Linux's; the platform's O_* constants differ on newlib
    int oflags = 0;
    uint32_t acc = flags & 3;
    oflags |= (acc == K32_FS_O_WRONLY) ? O_WRONLY : (acc == K32_FS_O_RDWR) ? O_RDWR : O_RDONLY;
    if (flags & K32_FS_O_CREAT) oflags |= O_CREAT;
    if (flags & K32_FS_O_EXCL) oflags |= O_EXCL;
    if (flags & K32_FS_O_TRUNC) oflags |= O_TRUNC;
    if (flags & K32_FS_O_APPEND) oflags |= O_APPEND;

    std::lock_guard<std::mutex> lock(mutex_);
    Table& t = tables_[pod_id];
    int slot = -1;
    for (int i = 0; i < K32_FS_MAX_FDS; ++i) {
        if (t.slots[i].host_fd < 0) { slot = i; break; }
    }
    if (slot < 0) return K32_ERR_MFILE;

    std::string full = root + "/" + rel;
    int fd = -1, err = 0;
    hal::node_storage_call([&]() { fd = ::open(full.c_str(), oflags, 0666); err = errno; });
    if (fd < 0) return errno_to_k32(err);
    t.slots[slot].host_fd = fd;
    t.slots[slot].readable = (acc != K32_FS_O_WRONLY);
    t.slots[slot].writable = (acc != K32_FS_O_RDONLY);
    return slot;
}

int32_t PodFs::read(const std::string& pod_id, int32_t fd, uint8_t* buf, uint32_t len) {
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::SdCard, 0, hal::AccessMode::Read)) return K32_ERR_PERM;
    if (fd < 0 || fd >= K32_FS_MAX_FDS) return K32_ERR_BADF;
    if (len > K32_FS_MAX_IO) len = K32_FS_MAX_IO;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tables_.find(pod_id);
    if (it == tables_.end()) return K32_ERR_BADF;
    Slot& s = it->second.slots[fd];
    if (s.host_fd < 0 || !s.readable) return K32_ERR_BADF;
    if (len == 0) return 0;
    ssize_t n = -1;
    int err = 0;
    hal::node_storage_call([&]() { n = staged_read(s.host_fd, buf, len); err = errno; });
    if (n < 0) return errno_to_k32(err);
    return static_cast<int32_t>(n);
}

ssize_t PodFs::staged_read(int host_fd, uint8_t* buf, uint32_t len) {
    uint32_t done = 0;
    while (done < len) {
        const uint32_t want = (len - done) < sizeof(stage_) ? (len - done) : static_cast<uint32_t>(sizeof(stage_));
        const ssize_t n = ::read(host_fd, stage_, want);
        if (n < 0) return done > 0 ? static_cast<ssize_t>(done) : -1;
        std::memcpy(buf + done, stage_, static_cast<size_t>(n));
        done += static_cast<uint32_t>(n);
        if (static_cast<uint32_t>(n) < want) break;   // end of file
    }
    return static_cast<ssize_t>(done);
}

ssize_t PodFs::staged_write(int host_fd, const uint8_t* buf, uint32_t len) {
    uint32_t done = 0;
    while (done < len) {
        const uint32_t want = (len - done) < sizeof(stage_) ? (len - done) : static_cast<uint32_t>(sizeof(stage_));
        std::memcpy(stage_, buf + done, want);
        const ssize_t n = ::write(host_fd, stage_, want);
        if (n < 0) return done > 0 ? static_cast<ssize_t>(done) : -1;
        done += static_cast<uint32_t>(n);
        if (static_cast<uint32_t>(n) < want) break;   // volume full
    }
    return static_cast<ssize_t>(done);
}

int32_t PodFs::write(const std::string& pod_id, int32_t fd, const uint8_t* buf, uint32_t len) {
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::SdCard, 0, hal::AccessMode::Write)) return K32_ERR_PERM;
    if (fd < 0 || fd >= K32_FS_MAX_FDS) return K32_ERR_BADF;
    if (len > K32_FS_MAX_IO) len = K32_FS_MAX_IO;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tables_.find(pod_id);
    if (it == tables_.end()) return K32_ERR_BADF;
    Slot& s = it->second.slots[fd];
    if (s.host_fd < 0 || !s.writable) return K32_ERR_BADF;
    if (len == 0) return 0;
    ssize_t n = -1;
    int err = 0;
    hal::node_storage_call([&]() { n = staged_write(s.host_fd, buf, len); err = errno; });
    if (n < 0) return errno_to_k32(err);
    return static_cast<int32_t>(n);
}

int32_t PodFs::close(const std::string& pod_id, int32_t fd) {
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::SdCard, 0, hal::AccessMode::ReadWrite)) return K32_ERR_PERM;
    if (fd < 0 || fd >= K32_FS_MAX_FDS) return K32_ERR_BADF;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tables_.find(pod_id);
    if (it == tables_.end()) return K32_ERR_BADF;
    Slot& s = it->second.slots[fd];
    if (s.host_fd < 0) return K32_ERR_BADF;
    int rc = -1, err = 0;
    hal::node_storage_call([&]() { rc = ::close(s.host_fd); err = errno; });
    s = Slot{};
    return rc == 0 ? 0 : errno_to_k32(err);
}

int32_t PodFs::size(const std::string& pod_id, int32_t fd) {
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::SdCard, 0, hal::AccessMode::Read)) return K32_ERR_PERM;
    if (fd < 0 || fd >= K32_FS_MAX_FDS) return K32_ERR_BADF;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tables_.find(pod_id);
    if (it == tables_.end()) return K32_ERR_BADF;
    Slot& s = it->second.slots[fd];
    if (s.host_fd < 0) return K32_ERR_BADF;
    struct stat st;
    int rc = -1, err = 0;
    hal::node_storage_call([&]() { rc = ::fstat(s.host_fd, &st); err = errno; });
    if (rc != 0) return errno_to_k32(err);
    if (st.st_size > 0x7FFFFFFF) return 0x7FFFFFFF;
    return static_cast<int32_t>(st.st_size);
}

int32_t PodFs::list(const std::string& pod_id, std::string_view dir, char* out, uint32_t cap) {
    std::string rel;
    if (!validate_relative_path(dir, true, rel)) return dir.size() > K32_FS_MAX_PATH ? K32_ERR_NAMETOOLONG : K32_ERR_INVAL;
    std::string root;
    int32_t rc = resolve_root(pod_id, root);
    if (rc != 0) return rc;
    if (cap > K32_FS_MAX_IO) cap = K32_FS_MAX_IO;

    std::string full = rel.empty() ? root : root + "/" + rel;
    int32_t result = 0;
    hal::node_storage_call([&]() {
    DIR* d = ::opendir(full.c_str());
    if (!d) { result = errno_to_k32(errno); return; }
    uint32_t written = 0;
    while (struct dirent* e = ::readdir(d)) {
        const char* name = e->d_name;
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
        size_t n = std::strlen(name);
        bool is_dir = false;
#if defined(DT_DIR)
        if (e->d_type == DT_DIR) is_dir = true;
        else if (e->d_type == DT_UNKNOWN)
#endif
        {
            struct stat st;
            std::string child = full + "/" + name;
            if (::stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) is_dir = true;
        }
        size_t need = n + (is_dir ? 1 : 0) + 1;
        if (written + need > cap) break;   // the rest does not fit: stop at a whole entry
        std::memcpy(out + written, name, n);
        written += static_cast<uint32_t>(n);
        if (is_dir) out[written++] = '/';
        out[written++] = '\n';
    }
    ::closedir(d);
    result = static_cast<int32_t>(written);
    });
    return result;
}

int32_t PodFs::remove(const std::string& pod_id, std::string_view path) {
    std::string rel;
    if (!validate_relative_path(path, false, rel)) return path.size() > K32_FS_MAX_PATH ? K32_ERR_NAMETOOLONG : K32_ERR_INVAL;
    std::string root;
    int32_t rc = resolve_root(pod_id, root);
    if (rc != 0) return rc;
    std::string full = root + "/" + rel;
    int r = -1, err = 0;
    hal::node_storage_call([&]() {
        struct stat st;
        if (::stat(full.c_str(), &st) != 0) { err = errno; return; }
        r = S_ISDIR(st.st_mode) ? ::rmdir(full.c_str()) : ::unlink(full.c_str());
        err = errno;
    });
    return r == 0 ? 0 : errno_to_k32(err);
}

void PodFs::release_pod(const std::string& pod_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tables_.find(pod_id);
    if (it == tables_.end()) return;
    for (auto& s : it->second.slots) {
        if (s.host_fd >= 0) {
            int fd = s.host_fd;
            hal::node_storage_call([fd]() { ::close(fd); });
        }
        s = Slot{};
    }
    tables_.erase(it);
}

uint32_t PodFs::open_count(const std::string& pod_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tables_.find(pod_id);
    if (it == tables_.end()) return 0;
    uint32_t n = 0;
    for (const auto& s : it->second.slots) if (s.host_fd >= 0) n++;
    return n;
}

} // namespace kube32::runtime
