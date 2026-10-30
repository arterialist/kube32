#include "api/firmware_api.hpp"
#include "api/route_handler.hpp"
#include "api/selectors.hpp"
#include "models/kube_models.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace kube32::api {

namespace {

constexpr std::string_view kStatusPath = "/apis/kube32.io/v1/firmware";
constexpr std::string_view kImagePath = "/apis/kube32.io/v1/firmware/image";

// Notes kept with the slots for the staged image
constexpr const char* kNoteSha = "stg_sha";
constexpr const char* kNoteSize = "stg_size";
constexpr const char* kNoteVersion = "stg_ver";

bool parse_size(const std::string& query, const char* key, size_t& out) {
    std::string v;
    if (!query_param(query, key, v) || v.empty() || v.size() > 10) return false;
    for (char c : v) if (c < '0' || c > '9') return false;
    out = static_cast<size_t>(std::strtoull(v.c_str(), nullptr, 10));
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out + "\"";
}

} // namespace

FirmwareApi::FirmwareApi(hal::FirmwareSlots& slots, storage::Store& store, RouteHandler& routes, HttpHandler* next)
    : slots_(slots), store_(store), routes_(routes), next_(next) {
    const std::string sha = slots_.note(kNoteSha);
    if (!osal::Sha256::valid_hex(sha)) return;
    if (sha == slots_.running().sha256) {
        // The control plane switched to the staged image: the passive slot
        // now holds the image it ran before
        store_notes(StagedFirmware{});
        return;
    }
    st_.phase = "Staged";
    st_.sha256 = sha;
    st_.size = static_cast<size_t>(std::strtoull(slots_.note(kNoteSize).c_str(), nullptr, 10));
    st_.received = st_.size;
    st_.version = slots_.note(kNoteVersion);
}

void FirmwareApi::store_notes(const StagedFirmware& st) {
    const bool staged = st.phase == "Staged";
    slots_.set_note(kNoteSha, staged ? st.sha256 : std::string());
    slots_.set_note(kNoteSize, staged ? std::to_string(st.size) : std::string());
    slots_.set_note(kNoteVersion, staged ? st.version : std::string());
}

StagedFirmware FirmwareApi::staged() const {
    osal::LockGuard<osal::Mutex> lock(mu_);
    return st_;
}

std::string FirmwareApi::status_json() const {
    const StagedFirmware st = staged();
    return "{\"kind\":\"FirmwareImage\",\"apiVersion\":\"kube32.io/v1\",\"metadata\":{\"name\":\"staged\"},\"status\":{"
           "\"phase\":\"" + st.phase + "\",\"sha256\":\"" + st.sha256 + "\",\"size\":" + std::to_string(st.size) +
           ",\"received\":" + std::to_string(st.received) + ",\"version\":" + json_string(st.version) +
           ",\"slot\":\"" + slots_.passive_slot() + "\",\"slotSize\":" + std::to_string(slots_.slot_size()) +
           ",\"message\":" + json_string(st.message) + "}}";
}

bool FirmwareApi::streams_body(const HttpRequest& req) {
    if (req.path == kImagePath) return req.method == HttpMethod::Put;
    return next_ && next_->streams_body(req);
}

bool FirmwareApi::reply(HttpConnection& conn, int code, const std::string& body) {
    return conn.send_simple(code, "application/json", body);
}

bool FirmwareApi::fail(HttpConnection& conn, int code, const char* reason, const std::string& message) {
    HttpResponse r = RouteHandler::make_status(code, reason, message);
    if (conn.body_remaining() > 0) conn.set_close();
    return conn.send_simple(r.status_code, r.content_type, r.body);
}

bool FirmwareApi::handle(HttpConnection& conn) {
    const HttpRequest& req = conn.request();
    if (req.path != kStatusPath && req.path != kImagePath) return next_ && next_->handle(conn);
    if (!routes_.authenticated(req)) return fail(conn, 401, "Unauthorized", "Unauthorized");
    if (req.path == kStatusPath) {
        if (req.method != HttpMethod::Get) return fail(conn, 405, "MethodNotAllowed", "only GET is allowed on " + req.path);
        return reply(conn, 200, status_json());
    }
    if (req.method == HttpMethod::Put) return upload(conn);
    if (req.method == HttpMethod::Get) return download(conn);
    return fail(conn, 405, "MethodNotAllowed", "GET and PUT are allowed on " + req.path);
}

bool FirmwareApi::rollout_active(std::string& name) {
    std::vector<std::string> rollouts;
    store_.list("/registry/firmwarerollouts/", rollouts);
    for (const auto& json : rollouts) {
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(json, doc, err)) continue;
        const models::JsonValue* st = doc.get("status");
        const std::string phase = st ? st->get_string("phase", "") : "";
        if (phase == "Complete" || phase == "Failed") continue;
        const models::JsonValue* meta = doc.get("metadata");
        name = meta ? meta->get_string("name", "") : "";
        return true;
    }
    return false;
}

bool FirmwareApi::upload(HttpConnection& conn) {
    const HttpRequest& req = conn.request();
    size_t offset = 0, size = 0;
    std::string sha;
    if (!parse_size(req.query_string, "offset", offset) || !parse_size(req.query_string, "size", size) ||
        !query_param(req.query_string, "sha256", sha) || !osal::Sha256::valid_hex(sha)) {
        return fail(conn, 400, "BadRequest", "offset, size and sha256 (64 hex digits) are required");
    }
    sha = lower(sha);
    if (size == 0 || size > slots_.slot_size()) {
        return fail(conn, 413, "RequestEntityTooLarge", "an image of " + std::to_string(size) + " bytes does not fit the " +
                                                          std::to_string(slots_.slot_size()) + "-byte app slot");
    }
    if (offset > size || (!conn.body_chunked() && conn.body_remaining() > size - offset)) {
        return fail(conn, 400, "BadRequest", "the range ends past the image size");
    }
    {
        osal::LockGuard<osal::Mutex> lock(mu_);
        if (busy_) return fail(conn, 409, "Conflict", "another upload is being written");
        if (offset > 0 && (st_.phase != "Receiving" || st_.sha256 != sha || st_.size != size || st_.received != offset)) {
            const std::string expected = st_.phase == "Receiving" && st_.sha256 == sha ? std::to_string(st_.received) : std::string("0");
            if (conn.body_remaining() > 0) conn.set_close();
            HttpResponse r = RouteHandler::make_status(409, "Conflict", "the upload continues at offset " + expected);
            return conn.send_simple(r.status_code, r.content_type, r.body);
        }
        busy_ = true;
    }
    // Ends the request: `failure` marks the upload Failed with that message
    auto settle = [&](const std::string& failure) {
        osal::LockGuard<osal::Mutex> lock(mu_);
        if (!failure.empty()) {
            st_.phase = "Failed";
            st_.message = failure;
        }
        busy_ = false;
    };
    if (offset == 0) {
        std::string active, why;
        if (rollout_active(active)) {
            settle(std::string());
            return fail(conn, 409, "Conflict", "FirmwareRollout " + active + " is in progress: delete it before staging another image");
        }
        // Nothing is erased before the first write: a refusal keeps what is staged
        if (!slots_.begin(why)) {
            settle(std::string());
            return fail(conn, 409, "Conflict", "the update slot cannot be written: " + why);
        }
        store_notes(StagedFirmware{});
        sha_.reset();
        osal::LockGuard<osal::Mutex> lock(mu_);
        st_ = StagedFirmware{};
        st_.phase = "Receiving";
        st_.sha256 = sha;
        st_.size = size;
    }

    // The body, through the connection's buffer into the slot
    char* buf = conn.scratch();
    const size_t cap = conn.scratch_capacity();
    size_t received = offset;
    std::string why;
    bool broken = false;
    while (conn.body_remaining() > 0) {
        const long n = conn.read_body(buf, cap);
        if (n == 0 && conn.body_remaining() == 0) break;
        if (n <= 0) { broken = true; break; }
        if (received + static_cast<size_t>(n) > size) {
            why = "the body runs past the image size";
            break;
        }
        if (!slots_.write(buf, static_cast<size_t>(n), why)) break;
        sha_.update(buf, static_cast<size_t>(n));
        received += static_cast<size_t>(n);
        osal::LockGuard<osal::Mutex> lock(mu_);
        st_.received = received;
    }
    if (!why.empty()) {
        slots_.abort();
        settle(why);
        return fail(conn, 422, "Invalid", why);
    }
    if (broken) {
        // Kept as it is: the client resumes at `received`
        settle(std::string());
        conn.set_close();
        return fail(conn, 400, "BadRequest", "the request body ended early; the upload continues at offset " + std::to_string(received));
    }
    if (received == size) {
        const std::string digest = osal::Sha256::hex(sha_.finish());
        if (digest != sha) {
            slots_.abort();
            why = "the image's SHA-256 is " + digest + ", not " + sha;
            settle(why);
            return fail(conn, 422, "Invalid", why);
        }
        if (!slots_.finish(why)) {
            settle(why);
            return fail(conn, 422, "Invalid", why);
        }
        StagedFirmware done;
        done.phase = "Staged";
        done.sha256 = sha;
        done.size = size;
        done.received = size;
        done.version = hal::find_image_version(size, [&](size_t off, void* dst, size_t len) { return slots_.read(off, dst, len); });
        store_notes(done);
        {
            osal::LockGuard<osal::Mutex> lock(mu_);
            st_ = done;
        }
        printf("[FIRMWARE] staged image %s (%s, %u bytes) in %s\n", sha.substr(0, 12).c_str(),
               done.version.empty() ? "no version" : done.version.c_str(), (unsigned)size, slots_.passive_slot().c_str());
    }
    settle(std::string());
    return reply(conn, 200, status_json());
}

bool FirmwareApi::download(HttpConnection& conn) {
    const HttpRequest& req = conn.request();
    std::string sha;
    size_t offset = 0, length = kMaxRange;
    if (!query_param(req.query_string, "sha256", sha) || !osal::Sha256::valid_hex(sha) || !parse_size(req.query_string, "offset", offset)) {
        return fail(conn, 400, "BadRequest", "sha256 (64 hex digits) and offset are required");
    }
    std::string length_text;
    if (query_param(req.query_string, "length", length_text) && !parse_size(req.query_string, "length", length)) {
        return fail(conn, 400, "BadRequest", "length must be a number of bytes");
    }
    const StagedFirmware st = staged();
    if (st.phase != "Staged") return fail(conn, 409, "Conflict", "no firmware image is staged");
    if (st.sha256 != lower(sha)) return fail(conn, 409, "Conflict", "the staged firmware image is " + st.sha256);
    if (offset >= st.size) return fail(conn, 416, "RequestedRangeNotSatisfiable", "the image is " + std::to_string(st.size) + " bytes");
    const size_t len = std::min({length, st.size - offset, kMaxRange});
    conn.set_close();
    if (!conn.begin_fixed(200, "application/octet-stream", len)) return true;
    char* buf = conn.scratch();
    const size_t cap = conn.scratch_capacity();
    for (size_t sent = 0; sent < len;) {
        const size_t n = std::min(cap, len - sent);
        // A short body tells the kubelet the range failed
        if (!slots_.read(offset + sent, buf, n) || !conn.send_raw(buf, n)) break;
        sent += n;
    }
    return true;
}

} // namespace kube32::api
