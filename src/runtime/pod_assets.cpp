// k32_resp_asset: files of a pod's image bundle sent as a whole response.
// Embedded bundles are streamed straight from flash; a bundle directory on
// node storage is read with one small buffer.

#include "runtime/app_pod.hpp"
#include "runtime/pod_fs.hpp"
#include "runtime/k32_app_abi.h"
#include "embedded/embedded_blobs.hpp"
#include "hal/node_storage.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/stat.h>

namespace kube32::runtime {

const char* content_type_for_path(std::string_view path) {
    // Same table as tools/embed_assets.py
    static const struct { const char* ext; const char* type; } kTypes[] = {
        {".html", "text/html; charset=utf-8"}, {".js", "application/javascript; charset=utf-8"},
        {".mjs", "application/javascript; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
        {".json", "application/json"}, {".map", "application/json"}, {".svg", "image/svg+xml"},
        {".png", "image/png"}, {".ico", "image/x-icon"}, {".txt", "text/plain; charset=utf-8"},
        {".webmanifest", "application/manifest+json"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
        {".webp", "image/webp"}, {".gif", "image/gif"}, {".wasm", "application/wasm"},
        {".woff", "font/woff"}, {".woff2", "font/woff2"},
    };
    size_t dot = path.rfind('.');
    size_t slash = path.rfind('/');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return "application/octet-stream";
    std::string_view ext = path.substr(dot);
    for (const auto& t : kTypes) {
        if (ext.size() != std::strlen(t.ext)) continue;
        bool same = true;
        for (size_t i = 0; i < ext.size() && same; ++i) {
            char c = ext[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            same = (c == t.ext[i]);
        }
        if (same) return t.type;
    }
    return "application/octet-stream";
}

namespace {

void send_head(AppResponseSink& sink, int status, const char* ctype, bool gzip, size_t len,
               const std::vector<std::pair<std::string, std::string>>& headers) {
    sink.begin(status, ctype);
    for (const auto& h : headers) sink.header(h.first, h.second);
    if (gzip) {
        sink.header("Content-Encoding", "gzip");
        sink.header("Vary", "Accept-Encoding");
    }
    sink.body_length(len);
}

void send_not_acceptable(AppResponseSink& sink) {
    static const char kBody[] = "file is stored gzip-compressed; send Accept-Encoding: gzip\n";
    sink.begin(406, "text/plain");
    sink.body_length(sizeof(kBody) - 1);
    sink.write(reinterpret_cast<const uint8_t*>(kBody), sizeof(kBody) - 1);
    sink.end();
}

int32_t send_embedded(const std::string& bundle, const std::string& rel, int status, bool accepts_gzip,
                      const std::vector<std::pair<std::string, std::string>>& headers, AppResponseSink& sink) {
    size_t count = 0;
    const embedded::EmbeddedAsset* assets = embedded::assets(count);
    const embedded::EmbeddedAsset* plain = nullptr;
    const embedded::EmbeddedAsset* gz = nullptr;
    for (size_t i = 0; i < count; ++i) {
        const embedded::EmbeddedAsset& a = assets[i];
        if (bundle != a.bundle || a.path[0] != '/' || rel != a.path + 1) continue;
        (a.gzip ? gz : plain) = &a;
    }
    const embedded::EmbeddedAsset* pick = (gz && (accepts_gzip || !plain)) ? gz : plain;
    if (!pick) return K32_ERR_NOENT;
    if (pick->gzip && !accepts_gzip) {
        send_not_acceptable(sink);
        return 0;
    }
    send_head(sink, status, pick->content_type, pick->gzip, pick->len, headers);
    // Straight from flash, in slices the sink can pass on without copying
    const size_t slice = 8192;
    for (size_t off = 0; off < pick->len; off += slice) {
        size_t n = (pick->len - off < slice) ? pick->len - off : slice;
        if (!sink.write(pick->data + off, n)) break;
    }
    sink.end();
    return 0;
}

bool regular_file_size(const std::string& path, size_t& out) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    out = static_cast<size_t>(st.st_size);
    return true;
}

int32_t send_from_directory(const std::string& dir, const std::string& rel, int status, bool accepts_gzip,
                            const std::vector<std::pair<std::string, std::string>>& headers, AppResponseSink& sink) {
    const std::string plain_path = dir + "/" + rel;
    const std::string gz_path = plain_path + ".gz";
    size_t plain_len = 0, gz_len = 0;
    bool have_plain = false, have_gz = false;
    hal::node_storage_call([&]() {
        have_gz = regular_file_size(gz_path, gz_len);
        if (!have_gz || !accepts_gzip) have_plain = regular_file_size(plain_path, plain_len);
    });
    if (!have_plain && !have_gz) return K32_ERR_NOENT;
    const bool use_gz = have_gz && (accepts_gzip || !have_plain);
    if (use_gz && !accepts_gzip) {
        send_not_acceptable(sink);
        return 0;
    }
    const std::string& path = use_gz ? gz_path : plain_path;
    const size_t len = use_gz ? gz_len : plain_len;
    FILE* f = nullptr;
    hal::node_storage_call([&]() { f = std::fopen(path.c_str(), "rb"); });
    if (!f) return K32_ERR_NOENT;
    send_head(sink, status, content_type_for_path(rel), use_gz, len, headers);
    // One small buffer: the file never sits in RAM as a whole
    constexpr size_t kBuf = 2048;
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kBuf]);
    size_t sent = 0;
    while (buf && sent < len) {
        size_t n = 0;
        hal::node_storage_call([&]() { n = std::fread(buf.get(), 1, kBuf, f); });
        if (n == 0 || !sink.write(buf.get(), n)) break;
        sent += n;
    }
    hal::node_storage_call([&]() { std::fclose(f); });
    sink.end();
    return 0;
}

} // namespace

int32_t send_bundle_file(const AppAssetSource& src, std::string_view path, int status, bool accepts_gzip,
                         const std::vector<std::pair<std::string, std::string>>& headers, AppResponseSink& sink) {
    while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    std::string rel;
    if (!PodFs::validate_relative_path(path, false, rel)) return K32_ERR_NOENT;
    if (!src.bundle.empty()) return send_embedded(src.bundle, rel, status, accepts_gzip, headers, sink);
    if (!src.directory.empty()) return send_from_directory(src.directory, rel, status, accepts_gzip, headers, sink);
    return K32_ERR_NOENT;
}

} // namespace kube32::runtime
