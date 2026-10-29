#include "app/node_storage_api.hpp"
#include "hal/node_storage.hpp"
#include "runtime/pod_fs.hpp"
#include "osal/system.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kube32::app {

bool is_node_storage_path(std::string_view path) {
    return path == "/storage" || path.rfind("/storage/", 0) == 0;
}

bool bearer_token_matches(std::string_view h, const std::string& token) {
    if (token.empty()) return true;
    while (!h.empty() && h.front() == ' ') h.remove_prefix(1);
    static const char kScheme[] = "bearer ";
    if (h.size() < 7) return false;
    for (size_t i = 0; i < 7; ++i) {
        char c = h[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != kScheme[i]) return false;
    }
    h.remove_prefix(7);
    while (!h.empty() && h.front() == ' ') h.remove_prefix(1);
    while (!h.empty() && h.back() == ' ') h.remove_suffix(1);
    unsigned diff = static_cast<unsigned>(h.size() ^ token.size());
    for (size_t i = 0; i < token.size(); ++i) {
        diff |= static_cast<unsigned>(static_cast<unsigned char>(token[i]) ^ (i < h.size() ? static_cast<unsigned char>(h[i]) : 0));
    }
    return diff == 0;
}

namespace {

std::string json_escape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (char c : in) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out.push_back(c);
    }
    return out;
}

bool reply(api::HttpConnection& conn, int status, const std::string& message) {
    return conn.send_simple(status, "application/json", "{\"error\":\"" + json_escape(message) + "\"}");
}

// GET of a directory: one entry per file or subdirectory
bool list_directory(api::HttpConnection& conn, const std::string& full, const std::string& rel) {
    std::string body = "{\"path\":\"" + json_escape(rel) + "\",\"entries\":[";
    bool ok = false;
    hal::node_storage_call([&]() {
        DIR* d = opendir(full.c_str());
        if (!d) return;
        ok = true;
        bool first = true;
        int budget = 256;
        while (budget-- > 0) {
            struct dirent* e = readdir(d);
            if (!e) break;
            if (e->d_name[0] == '.') continue;
            struct stat st;
            std::string child = full + "/" + e->d_name;
            if (stat(child.c_str(), &st) != 0) continue;
            if (!first) body += ",";
            first = false;
            body += "{\"name\":\"" + json_escape(e->d_name) + "\"";
            body += S_ISDIR(st.st_mode) ? ",\"dir\":true}" : ",\"size\":" + std::to_string(static_cast<long long>(st.st_size)) + "}";
        }
        closedir(d);
    });
    if (!ok) return reply(conn, 500, "cannot read directory " + rel);
    body += "]}";
    return conn.send_simple(200, "application/json", body);
}

bool send_file(api::HttpConnection& conn, const std::string& full, size_t size) {
    int fd = -1;
    hal::node_storage_call([&]() { fd = ::open(full.c_str(), O_RDONLY); });
    if (fd < 0) return reply(conn, 500, "open failed");
    bool ok = conn.begin_fixed(200, "application/octet-stream", size);
    char* buf = conn.scratch();
    const size_t cap = conn.scratch_capacity();
    size_t sent = 0;
    while (ok && sent < size && conn.request().method != api::HttpMethod::Head) {
        ssize_t n = 0;
        hal::node_storage_call([&]() { n = ::read(fd, buf, cap); });
        if (n <= 0) break;
        ok = conn.send_raw(buf, static_cast<size_t>(n));
        sent += static_cast<size_t>(n);
    }
    hal::node_storage_call([&]() { ::close(fd); });
    if (sent != size && conn.request().method != api::HttpMethod::Head) conn.set_close();
    return true;
}

// PUT: the body goes to <file>.part a buffer at a time and replaces <file>
// once complete, so a failed upload never leaves a truncated file behind.
bool put_file(api::HttpConnection& conn, const std::string& full, const std::string& rel, size_t max_upload) {
    const api::HttpRequest& req = conn.request();
    // A chunked body (kubectl replace --raw) has no length up front: the
    // limit is enforced as it arrives
    const bool chunked = conn.body_chunked();
    const size_t len = req.content_length;
    if (!chunked && len > max_upload) {
        conn.set_close();
        return reply(conn, 413, "upload of " + std::to_string(len) + " bytes is over this node's limit of " + std::to_string(max_upload));
    }
    hal::NodeStorageInfo si = hal::node_storage_info();
    if (si.free_bytes < static_cast<uint64_t>(chunked ? 0 : len) + 64 * 1024) {
        conn.set_close();
        return reply(conn, 507, "not enough free space on the node's " + std::string(hal::storage_medium_name(si.medium)));
    }
    const size_t slash = full.rfind('/');
    if (slash != std::string::npos && !hal::node_storage_make_dirs(full.substr(0, slash))) {
        conn.set_close();
        return reply(conn, 500, "cannot create the directory of " + rel);
    }
    bool target_existed = false;
    hal::node_storage_call([&]() {
        struct stat st;
        target_existed = (stat(full.c_str(), &st) == 0);
        if (target_existed && S_ISDIR(st.st_mode)) target_existed = false;
    });
    const std::string part = full + ".part";
    int fd = -1;
    int open_errno = 0;
    hal::node_storage_call([&]() {
        fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        open_errno = errno;
    });
    if (fd < 0) {
        conn.set_close();
        return reply(conn, 500, "cannot create " + rel + ".part (errno " + std::to_string(open_errno) + ")");
    }
    const uint64_t t0 = osal::System::get_uptime_ms();
    char* buf = conn.scratch();
    const size_t cap = conn.scratch_capacity();
    size_t written = 0;
    const char* failure = nullptr;
    int failure_status = 500;
    for (;;) {
        long n = conn.read_body(buf, cap);
        if (n == 0 && conn.body_remaining() == 0) break;
        if (n <= 0) { failure = "upload interrupted"; break; }
        if (written + static_cast<size_t>(n) > max_upload) {
            failure = "upload over this node's limit";
            failure_status = 413;
            break;
        }
        ssize_t w = 0;
        hal::node_storage_call([&]() { w = ::write(fd, buf, static_cast<size_t>(n)); });
        if (w != n) { failure = "write failed (storage full or removed)"; break; }
        written += static_cast<size_t>(n);
    }
    int rc = 0;
    hal::node_storage_call([&]() {
        rc = ::close(fd);
        if (failure || rc != 0) { ::unlink(part.c_str()); return; }
        // FAT cannot rename over an existing file
        if (target_existed) ::unlink(full.c_str());
        rc = ::rename(part.c_str(), full.c_str());
        if (rc != 0) ::unlink(part.c_str());
    });
    if (failure || rc != 0) {
        conn.set_close();
        return reply(conn, failure ? failure_status : 500, std::string(failure ? failure : "rename failed") + " after " + std::to_string(written) + " bytes");
    }
    const uint64_t ms = osal::System::get_uptime_ms() - t0;
    std::printf("[STORAGE] %s: %u bytes written in %u ms\n", rel.c_str(), (unsigned)written, (unsigned)ms);
    return conn.send_simple(target_existed ? 200 : 201, "application/json",
                            "{\"path\":\"" + json_escape(rel) + "\",\"bytes\":" + std::to_string(written) + ",\"ms\":" + std::to_string(ms) + "}");
}

bool remove_path(api::HttpConnection& conn, const std::string& full, const std::string& rel, bool is_dir) {
    int rc = 0, err = 0;
    hal::node_storage_call([&]() {
        rc = is_dir ? ::rmdir(full.c_str()) : ::unlink(full.c_str());
        err = errno;
    });
    if (rc != 0) {
        if (err == ENOTEMPTY || err == EEXIST || err == EACCES) return reply(conn, 409, rel + " is not empty");
        return reply(conn, 500, "cannot remove " + rel + " (errno " + std::to_string(err) + ")");
    }
    return conn.send_simple(200, "application/json", "{\"path\":\"" + json_escape(rel) + "\",\"removed\":true}");
}

} // namespace

bool serve_node_storage(api::HttpConnection& conn, std::string_view path, const std::string& token, size_t max_upload) {
    const api::HttpRequest& req = conn.request();
    if (!bearer_token_matches(req.authorization, token)) {
        conn.set_close();
        return reply(conn, 401, "Unauthorized");
    }
    if (!is_node_storage_path(path)) return reply(conn, 404, "no such path on this node");
    path.remove_prefix(std::strlen("/storage"));
    while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    std::string rel;
    if (!runtime::PodFs::validate_relative_path(path, true, rel)) {
        conn.set_close();
        return reply(conn, 400, "bad path: components of [A-Za-z0-9._-], no '.' or '..', at most 96 bytes");
    }
    const std::string full = hal::node_storage_path(rel);
    if (full.empty()) {
        conn.set_close();
        return reply(conn, 503, "no storage mounted on this node");
    }

    struct stat st;
    bool exists = false;
    hal::node_storage_call([&]() { exists = (stat(full.c_str(), &st) == 0); });
    const bool is_dir = exists && S_ISDIR(st.st_mode);

    switch (req.method) {
        case api::HttpMethod::Get:
        case api::HttpMethod::Head:
            if (rel.empty() && !exists) return conn.send_simple(200, "application/json", "{\"path\":\"\",\"entries\":[]}");
            if (!exists) return reply(conn, 404, rel + " not found");
            return is_dir ? list_directory(conn, full, rel) : send_file(conn, full, static_cast<size_t>(st.st_size));
        case api::HttpMethod::Put:
            if (rel.empty() || is_dir) {
                conn.set_close();
                return reply(conn, 409, "a PUT names a file");
            }
            return put_file(conn, full, rel, max_upload);
        case api::HttpMethod::Delete:
            if (rel.empty()) return reply(conn, 409, "the storage root cannot be removed");
            if (!exists) return reply(conn, 404, rel + " not found");
            return remove_path(conn, full, rel, is_dir);
        default:
            conn.set_close();
            return reply(conn, 405, "GET, PUT or DELETE");
    }
}

} // namespace kube32::app
