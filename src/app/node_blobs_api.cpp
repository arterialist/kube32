#include "app/node_blobs_api.hpp"
#include "app/node_storage_api.hpp"
#include "api/selectors.hpp"
#include "osal/system.hpp"
#include "runtime/k32_app_abi.h"
#include "storage/blob_store.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kube32::app {

bool is_node_blobs_path(std::string_view path) {
    return path == "/blobs" || path.rfind("/blobs/", 0) == 0;
}

namespace {

std::string json_escape(std::string_view in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
    }
    return out;
}

bool reply(api::HttpConnection& conn, int status, const std::string& message) {
    return conn.send_simple(status, "application/json", "{\"error\":\"" + json_escape(message) + "\"}");
}

int http_status(int32_t err) {
    switch (err) {
        case K32_ERR_INVAL: return 400;
        case K32_ERR_PERM: return 403;
        case K32_ERR_NOENT: return 404;
        case K32_ERR_BUSY: return 409;
        case K32_ERR_NODEV: return 503;
        case K32_ERR_NOSPC: return 507;
        default: return 500;
    }
}

const char* err_text(int32_t err) {
    switch (err) {
        case K32_ERR_INVAL: return "invalid request";
        case K32_ERR_NOENT: return "no such blob";
        case K32_ERR_BUSY: return "busy";
        case K32_ERR_NODEV: return "no blob store";
        case K32_ERR_NOSPC: return "no space";
        case K32_ERR_MFILE: return "too many open files";
        default: return "I/O error";
    }
}

storage::BlobStore* store_named(std::string_view medium) {
    if (medium == "flash") return storage::node_flash_blobs();
    if (medium == "sd") return storage::node_card_blobs();
    return nullptr;
}

bool list_stores(api::HttpConnection& conn) {
    std::string body = "{\"stores\":[";
    bool first = true;
    for (storage::BlobStore* store : {static_cast<storage::BlobStore*>(storage::node_flash_blobs()), storage::node_card_blobs()}) {
        if (!store) continue;
        std::string why;
        const bool up = store->available(&why);
        if (!first) body += ",";
        first = false;
        body += "{\"medium\":\"" + std::string(store->medium()) + "\",\"location\":\"" + json_escape(store->location()) + "\"";
        body += ",\"available\":" + std::string(up ? "true" : "false");
        if (!up) body += ",\"reason\":\"" + json_escape(why) + "\"";
        body += ",\"capacity\":" + std::to_string(store->capacity_bytes()) + ",\"free\":" + std::to_string(up ? store->free_bytes() : 0);
        body += ",\"blobs\":[";
        bool first_blob = true;
        for (const storage::BlobInfo& b : store->list()) {
            if (!first_blob) body += ",";
            first_blob = false;
            char crc[16];
            std::snprintf(crc, sizeof(crc), "0x%08x", (unsigned)b.crc32);
            body += "{\"name\":\"" + json_escape(b.name) + "\",\"size\":" + std::to_string(b.size);
            if (std::strcmp(store->medium(), "flash") == 0) body += ",\"crc32\":\"" + std::string(crc) + "\"";
            body += "}";
        }
        body += "]}";
    }
    body += "]}";
    return conn.send_simple(200, "application/json", body);
}

bool get_blob(api::HttpConnection& conn, const std::string& name, storage::BlobStore* only) {
    std::shared_ptr<storage::BlobReader> reader;
    int32_t err = K32_ERR_NOENT;
    if (only) {
        reader = only->open(name, err);
    } else {
        for (storage::BlobStore* store : storage::node_blob_stores()) {
            reader = store->open(name, err);
            if (reader) break;
        }
    }
    if (!reader) return reply(conn, http_status(err), "blob " + name + ": " + err_text(err));
    const uint64_t size = reader->size();
    bool ok = conn.begin_fixed(200, "application/octet-stream", static_cast<size_t>(size));
    char* buf = conn.scratch();
    const size_t cap = conn.scratch_capacity();
    uint64_t sent = 0;
    while (ok && sent < size && conn.request().method != api::HttpMethod::Head) {
        const int32_t n = reader->read(sent, buf, static_cast<uint32_t>(cap));
        if (n <= 0) break;
        ok = conn.send_raw(buf, static_cast<size_t>(n));
        sent += static_cast<uint64_t>(n);
    }
    if (sent != size && conn.request().method != api::HttpMethod::Head) conn.set_close();
    return true;
}

bool put_blob(api::HttpConnection& conn, const std::string& name, storage::BlobStore* store) {
    const api::HttpRequest& req = conn.request();
    uint64_t size = req.content_length;
    std::string size_param;
    if (conn.body_chunked()) {
        if (!api::query_param(req.query_string, "size", size_param) || size_param.empty()) {
            conn.set_close();
            return reply(conn, 411, "a chunked upload names its length: ?size=<bytes>");
        }
        char* end = nullptr;
        size = std::strtoull(size_param.c_str(), &end, 10);
        if (!end || *end) {
            conn.set_close();
            return reply(conn, 400, "size=" + size_param + " is not a number of bytes");
        }
    }
    bool existed = false;
    for (const storage::BlobInfo& b : store->list()) existed = existed || b.name == name;
    int32_t err = K32_OK;
    std::string why;
    std::unique_ptr<storage::BlobWriter> writer = store->create(name, size, err, why);
    if (!writer) {
        conn.set_close();
        return reply(conn, http_status(err), why.empty() ? err_text(err) : why);
    }
    const uint64_t t0 = osal::System::get_uptime_ms();
    char* buf = conn.scratch();
    const size_t cap = conn.scratch_capacity();
    uint64_t received = 0;
    while (true) {
        long n = conn.read_body(buf, cap);
        if (n == 0 && conn.body_remaining() == 0) break;
        if (n <= 0) {
            conn.set_close();
            return reply(conn, 400, "the upload ended after " + std::to_string(received) + " bytes");
        }
        err = writer->write(buf, static_cast<size_t>(n), why);
        if (err != K32_OK) {
            conn.set_close();
            return reply(conn, http_status(err), why);
        }
        received += static_cast<uint64_t>(n);
    }
    err = writer->commit(why);
    writer.reset();
    if (err != K32_OK) {
        conn.set_close();
        return reply(conn, http_status(err), why);
    }
    const uint64_t ms = osal::System::get_uptime_ms() - t0;
    uint32_t crc = 0;
    for (const storage::BlobInfo& b : store->list()) {
        if (b.name == name) crc = b.crc32;
    }
    std::printf("[BLOBS] %s: %llu bytes to %s in %u ms\n", name.c_str(), (unsigned long long)received, store->location().c_str(), (unsigned)ms);
    char tail[96];
    if (std::strcmp(store->medium(), "flash") == 0) {
        std::snprintf(tail, sizeof(tail), ",\"bytes\":%llu,\"ms\":%llu,\"crc32\":\"0x%08x\"}", (unsigned long long)received,
                      (unsigned long long)ms, (unsigned)crc);
    } else {
        std::snprintf(tail, sizeof(tail), ",\"bytes\":%llu,\"ms\":%llu}", (unsigned long long)received, (unsigned long long)ms);
    }
    return conn.send_simple(existed ? 200 : 201, "application/json",
                            "{\"name\":\"" + json_escape(name) + "\",\"medium\":\"" + store->medium() + "\"" + tail);
}

} // namespace

bool serve_node_blobs(api::HttpConnection& conn, std::string_view path, const std::string& token) {
    const api::HttpRequest& req = conn.request();
    if (!bearer_token_matches(req.authorization, token)) {
        conn.set_close();
        return reply(conn, 401, "Unauthorized");
    }
    if (!is_node_blobs_path(path)) return reply(conn, 404, "no such path on this node");
    path.remove_prefix(std::strlen("/blobs"));
    while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    std::string medium;
    api::query_param(req.query_string, "medium", medium);
    storage::BlobStore* chosen = nullptr;
    if (!medium.empty()) {
        chosen = store_named(medium);
        if (!chosen) {
            conn.set_close();
            return reply(conn, medium == "flash" || medium == "sd" ? 503 : 400,
                         medium == "flash" || medium == "sd" ? "this node has no " + medium + " blob store" : "medium is flash or sd");
        }
    }
    if (path.empty()) {
        if (req.method == api::HttpMethod::Get || req.method == api::HttpMethod::Head) return list_stores(conn);
        conn.set_close();
        return reply(conn, 405, "GET /blobs lists the stores; PUT and DELETE name a blob");
    }
    const std::string name(path);
    if (!storage::valid_blob_name(name)) {
        conn.set_close();
        return reply(conn, 400, "a blob name is 1 to 47 characters of [A-Za-z0-9._-]");
    }
    switch (req.method) {
        case api::HttpMethod::Get:
        case api::HttpMethod::Head:
            return get_blob(conn, name, chosen);
        case api::HttpMethod::Put: {
            storage::BlobStore* store = chosen ? chosen : storage::node_flash_blobs();
            if (!store) store = storage::node_card_blobs();
            if (!store) {
                conn.set_close();
                return reply(conn, 503, "this node keeps no blobs");
            }
            return put_blob(conn, name, store);
        }
        case api::HttpMethod::Delete: {
            int32_t err = K32_ERR_NOENT;
            std::string why;
            for (storage::BlobStore* store : {static_cast<storage::BlobStore*>(storage::node_flash_blobs()), storage::node_card_blobs()}) {
                if (!store || (chosen && store != chosen)) continue;
                err = store->remove(name, why);
                if (err != K32_ERR_NOENT) break;
            }
            if (err != K32_OK) return reply(conn, http_status(err), why.empty() ? "blob " + name + ": " + err_text(err) : why);
            return conn.send_simple(200, "application/json", "{\"name\":\"" + json_escape(name) + "\",\"removed\":true}");
        }
        default:
            conn.set_close();
            return reply(conn, 405, "GET, PUT or DELETE");
    }
}

} // namespace kube32::app
