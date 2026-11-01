#include "api/inline_images.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"
#include "storage/persist_policy.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>

namespace kube32::api {

namespace {

constexpr char kPlainKey[] = "\"kube32.io/wasm-base64\"";
constexpr size_t kPlainLen = sizeof(kPlainKey) - 1;
// The same member inside a JSON string (last-applied-configuration)
constexpr char kEscapedKey[] = "\\\"kube32.io/wasm-base64\\\":\\\"";
constexpr size_t kEscapedLen = sizeof(kEscapedKey) - 1;
constexpr char kRef[] = "sha256:";
constexpr size_t kRefLen = sizeof(kRef) - 1;
constexpr char kDigestKey[] = "\"kube32.io/wasm-sha256\":";
constexpr char kNotBase64[] = "metadata.annotations[kube32.io/wasm-base64]: Invalid value: the module is not base64";
constexpr const char* kImagePrefix = "/images/";
constexpr const char* kTemplateKinds[] = {"/registry/pods/", "/registry/deployments/", "/registry/daemonsets/", "/registry/jobs/"};

std::string image_key(const std::string& digest) { return kImagePrefix + digest; }

std::string piece_key(const std::string& blob, size_t i) {
    char key[64];
    snprintf(key, sizeof key, "%s%s/%u", storage::kFlashOnlyPrefix, blob.c_str(), static_cast<unsigned>(i));
    return key;
}

// A few objects refer to a few images: a linear search is enough
bool contains(const std::vector<std::string>& v, std::string_view s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

bool lower_hex64(std::string_view s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

models::JsonValue jstr(const std::string& v) { return models::JsonValue{models::JsonType::String, false, 0, v, {}, {}}; }

// Calls `fn(value)` for the start of each value of kube32.io/wasm-base64 in
// `json` (plain and JSON-escaped), `value` running to the end of the text
template <typename F>
void for_each_value(std::string_view json, F&& fn) {
    for (size_t at = json.find(kPlainKey); at != std::string_view::npos; at = json.find(kPlainKey, at + kPlainLen)) {
        size_t p = at + kPlainLen;
        while (p < json.size() && is_ws(json[p])) ++p;
        if (p >= json.size() || json[p] != ':') continue;
        ++p;
        while (p < json.size() && is_ws(json[p])) ++p;
        if (p < json.size() && json[p] == '"') fn(json.substr(p + 1), false);
    }
    for (size_t at = json.find(kEscapedKey); at != std::string_view::npos; at = json.find(kEscapedKey, at + kEscapedLen)) {
        fn(json.substr(at + kEscapedLen), true);
    }
}

} // namespace

// =============================================================================
// ImageUpload
// =============================================================================

ImageUpload::ImageUpload(InlineImages& images, bool keep) : images_(images), keep_(keep) {
    if (!keep_) return;
    std::string uid = models::generate_uid();
    uid.erase(std::remove(uid.begin(), uid.end(), '-'), uid.end());
    blob_ = uid.substr(0, 16);
    piece_.reserve(InlineImages::kPieceBytes);
}

ImageUpload::~ImageUpload() {
    if (done_ || !keep_) return;
    for (size_t i = 0; i < pieces_; ++i) images_.store().remove(piece_key(blob_, i));
}

bool ImageUpload::write(const uint8_t* data, size_t len) {
    sha_.update(data, len);
    size_ += len;
    if (!keep_) return true;
    while (len > 0) {
        const size_t n = std::min(len, InlineImages::kPieceBytes - piece_.size());
        piece_.append(reinterpret_cast<const char*>(data), n);
        data += n;
        len -= n;
        if (piece_.size() == InlineImages::kPieceBytes && !store_piece()) return false;
    }
    return true;
}

// One piece to the store and on to flash, so a request holds one piece at most
bool ImageUpload::store_piece() {
    const std::string key = piece_key(blob_, pieces_);
    storage::Status s = images_.store().put_extended(key, piece_, false, true);
    if (s.code == storage::StatusCode::StorageFull && images_.collect_garbage() > 0) {
        s = images_.store().put_extended(key, piece_, false, true);
    }
    if (!s.ok()) {
        error_ = "the image could not be stored: " + (s.message.empty() ? std::string("the object store refused it") : s.message);
        return false;
    }
    ++pieces_;
    piece_.clear();
    storage::Status c = images_.store().commit();
    if (!c.ok()) {
        error_ = "the image could not be written to the object store journal: " + c.message;
        return false;
    }
    return true;
}

bool ImageUpload::finish(std::string& digest) {
    digest = osal::Sha256::hex(sha_.finish());
    if (size_ == 0) {
        error_ = "metadata.annotations[kube32.io/wasm-base64]: Invalid value: it holds no module";
        return false;
    }
    if (!keep_) {
        done_ = true;
        return true;
    }
    InlineImages::Image existing;
    if (images_.claim(digest, existing)) {
        // Stored before: this copy goes
        for (size_t i = 0; i < pieces_; ++i) images_.store().remove(piece_key(blob_, i));
        done_ = true;
        return true;
    }
    if (!piece_.empty() && !store_piece()) return false;
    std::string().swap(piece_);
    char manifest[96];
    snprintf(manifest, sizeof manifest, "{\"size\":%zu,\"blob\":\"%s\",\"chunks\":%zu}", size_, blob_.c_str(), pieces_);
    const std::string key = image_key(digest);
    images_.protect(digest);
    storage::Status s = images_.store().put_extended(key, manifest, false, true);
    if (s.code == storage::StatusCode::AlreadyExists) {
        // The same module, stored by a request that ran alongside
        images_.claim(digest, existing);
        for (size_t i = 0; i < pieces_; ++i) images_.store().remove(piece_key(blob_, i));
        done_ = true;
        return true;
    }
    if (!s.ok()) {
        error_ = "the image could not be stored: " + s.message;
        return false;
    }
    storage::Status c = images_.store().commit();
    if (!c.ok()) {
        images_.store().remove(key);
        error_ = "the image could not be written to the object store journal: " + c.message;
        return false;
    }
    done_ = true;
    return true;
}

// =============================================================================
// InlineImageFilter
// =============================================================================

InlineImageFilter::InlineImageFilter(InlineImages& images, bool keep) : images_(images), keep_(keep) {}

InlineImageFilter::~InlineImageFilter() = default;

bool InlineImageFilter::fail(int status, std::string why) {
    status_ = status;
    error_ = std::move(why);
    upload_.reset();
    return false;
}

void InlineImageFilter::begin_value(bool escaped) {
    state_ = State::Value;
    escaped_value_ = escaped;
    escape_ = Escape::None;
    held_ = 0;
    decided_ = false;
    passthrough_ = false;
    acc_ = 0;
    bits_ = 0;
    padded_ = false;
    out_len_ = 0;
}

bool InlineImageFilter::feed(const char* data, size_t len, std::string& out) {
    if (status_) return false;
    for (size_t i = 0; i < len; ++i) {
        const char c = data[i];
        if (state_ == State::Value) {
            if (!value_char(c, out)) return false;
            continue;
        }
        out.push_back(c);
        if (state_ == State::Colon) {
            if (c == ':') { state_ = State::Open; continue; }
            if (is_ws(c)) continue;
            state_ = State::Scan;
        } else if (state_ == State::Open) {
            if (c == '"') { begin_value(false); continue; }
            if (is_ws(c)) continue;
            state_ = State::Scan;
        }
        plain_ = (c == kPlainKey[plain_]) ? plain_ + 1 : (c == kPlainKey[0] ? 1 : 0);
        escaped_ = (c == kEscapedKey[escaped_]) ? escaped_ + 1 : (c == kEscapedKey[0] ? 1 : 0);
        if (plain_ == kPlainLen) {
            plain_ = escaped_ = 0;
            state_ = State::Colon;
        } else if (escaped_ == kEscapedLen) {
            plain_ = escaped_ = 0;
            begin_value(true);
        }
    }
    return true;
}

bool InlineImageFilter::finish() {
    if (status_) return false;
    if (state_ == State::Value) return fail(400, "the body ends inside the value of kube32.io/wasm-base64");
    return true;
}

// One character of a value. A plain value ends at '"'. An escaped one (in a
// JSON string inside the JSON) ends at \" and has its own escapes doubled.
bool InlineImageFilter::value_char(char c, std::string& out) {
    if (passthrough_) {
        // A reference: copied as it is
        out.push_back(c);
        if (!escaped_value_) {
            if (c == '"') state_ = State::Scan;
        } else if (escape_ == Escape::Outer) {
            escape_ = Escape::None;
            if (c == '"') state_ = State::Scan;
        } else if (c == '\\') {
            escape_ = Escape::Outer;
        }
        return true;
    }
    if (escape_ == Escape::Inner) {
        escape_ = Escape::None;
        if (c == '/') return text('/', out);
        if (c == 'n' || c == 'r' || c == 't') return true;
        return fail(422, kNotBase64);
    }
    if (escape_ == Escape::Outer) {
        escape_ = Escape::None;
        if (escaped_value_ && c == '"') return end_value(out);
        if (escaped_value_ && c == '\\') { escape_ = Escape::Inner; return true; }
        if (c == '/') return text('/', out);
        if (c == 'n' || c == 'r' || c == 't') return true;
        return fail(422, kNotBase64);
    }
    if (c == '\\') { escape_ = Escape::Outer; return true; }
    if (c == '"') {
        if (!escaped_value_) return end_value(out);
        return fail(400, "an escaped kube32.io/wasm-base64 value ends without its escaped quote");
    }
    return text(c, out);
}

// A character of the value's text. The first ones are held until it is clear
// whether the value is a reference ("sha256:<hex>"), which goes through as it
// is; anything else is base64.
bool InlineImageFilter::text(char c, std::string& out) {
    if (!decided_) {
        held_chars_[held_++] = c;
        if (c == kRef[held_ - 1]) {
            if (held_ < kRefLen) return true;
            decided_ = true;
            passthrough_ = true;
            out.append(kRef, kRefLen);
            return true;
        }
        return release_held();
    }
    return decode(c);
}

bool InlineImageFilter::release_held() {
    decided_ = true;
    for (size_t i = 0; i < held_; ++i) {
        if (!decode(held_chars_[i])) return false;
    }
    held_ = 0;
    return true;
}

bool InlineImageFilter::decode(char c) {
    if (c == ' ') return true;
    if (c == '=') { padded_ = true; return true; }
    const int v = b64_value(c);
    if (v < 0 || padded_) {
        char what[128];
        snprintf(what, sizeof what, "%s (character 0x%02x)", kNotBase64, static_cast<unsigned char>(c));
        return fail(422, what);
    }
    acc_ = ((acc_ << 6) | static_cast<uint32_t>(v)) & 0xFFFF;
    bits_ += 6;
    if (bits_ >= 8) {
        bits_ -= 8;
        decoded_[out_len_++] = static_cast<uint8_t>((acc_ >> bits_) & 0xFF);
        if (out_len_ == sizeof decoded_) return flush_decoded();
    }
    return true;
}

bool InlineImageFilter::flush_decoded() {
    if (out_len_ == 0) return true;
    if (!upload_) upload_ = std::make_unique<ImageUpload>(images_, keep_);
    const size_t n = out_len_;
    out_len_ = 0;
    if (!upload_->write(decoded_, n)) return fail(500, upload_->error());
    return true;
}

bool InlineImageFilter::end_value(std::string& out) {
    const char* quote = escaped_value_ ? "\\\"" : "\"";
    if (!decided_ && held_ == 0) {
        // An empty value
        out += quote;
        state_ = State::Scan;
        return true;
    }
    if (!decided_ && !release_held()) return false;
    if (!flush_decoded()) return false;
    if (!upload_) return fail(422, "metadata.annotations[kube32.io/wasm-base64]: Invalid value: it holds no module");
    std::string digest;
    if (!upload_->finish(digest)) return fail(500, upload_->error());
    upload_.reset();
    ++images_taken_;
    out.append(kRef, kRefLen);
    out += digest;
    out += quote;
    state_ = State::Scan;
    return true;
}

// =============================================================================
// InlineImages
// =============================================================================

InlineImages::InlineImages(storage::Store& store) : store_(store) {}

bool InlineImages::carries_module(std::string_view json) {
    bool found = false;
    for_each_value(json, [&](std::string_view v, bool escaped) {
        if (found) return;
        const bool empty = escaped ? v.rfind("\\\"", 0) == 0 : v.rfind('"', 0) == 0;
        if (!empty && v.rfind(kRef, 0) != 0) found = true;
    });
    return found;
}

bool InlineImages::mentions_image(std::string_view json) {
    return json.find(kPlainKey) != std::string_view::npos;
}

bool InlineImages::admit(const std::string& body, bool dry_run, std::string& out, int& status, std::string& why) {
    std::string filtered;
    const std::string* text = &body;
    if (carries_module(body)) {
        InlineImageFilter f(*this, !dry_run);
        filtered.reserve(std::min<size_t>(body.size(), 4096));
        if (!f.feed(body.data(), body.size(), filtered) || !f.finish()) {
            status = f.status();
            why = f.error();
            return false;
        }
        text = &filtered;
    }
    if (!mentions_image(*text)) {
        out = *text;
        return true;
    }
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(*text, doc, err) || !doc.is_object()) {
        // The handler answers the parse error
        out = *text;
        return true;
    }
    if (!resolve(doc, dry_run, why)) {
        status = 422;
        return false;
    }
    out = doc.to_string();
    return true;
}

bool InlineImages::resolve(models::JsonValue& obj, bool dry_run, std::string& why) {
    auto settle = [&](models::JsonValue* annotations) {
        if (!annotations || !annotations->is_object()) return true;
        const models::JsonValue* v = annotations->get("kube32.io/wasm-base64");
        if (!v || !v->is_string()) return true;
        const std::string value = v->str_val;
        if (value.empty()) {
            annotations->remove("kube32.io/wasm-base64");
            return true;
        }
        const std::string digest = value.size() > kRefLen ? value.substr(kRefLen) : std::string();
        if (value.rfind(kRef, 0) != 0 || !lower_hex64(digest)) {
            why = "metadata.annotations[kube32.io/wasm-base64]: Invalid value: the module is not base64";
            return false;
        }
        Image img;
        const bool found = claim(digest, img);
        if (!found && !dry_run) {
            why = "metadata.annotations[kube32.io/wasm-base64]: Invalid value: \"sha256:" + digest +
                  "\": the control plane stores no such image; send the module itself";
            return false;
        }
        annotations->remove("kube32.io/wasm-base64");
        annotations->set("kube32.io/wasm-sha256", jstr(digest));
        if (found) annotations->set("kube32.io/wasm-size", jstr(std::to_string(img.size)));
        return true;
    };
    models::JsonValue* meta = obj.get_mut("metadata");
    if (meta && !settle(meta->get_mut("annotations"))) return false;
    models::JsonValue* spec = obj.get_mut("spec");
    models::JsonValue* tmpl = spec ? spec->get_mut("template") : nullptr;
    models::JsonValue* tmeta = tmpl ? tmpl->get_mut("metadata") : nullptr;
    if (tmeta && !settle(tmeta->get_mut("annotations"))) return false;
    return true;
}

bool InlineImages::lookup(const std::string& digest, Image& out) {
    std::string json;
    if (!store_.get(image_key(digest), json).ok()) return false;
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) return false;
    out.size = static_cast<size_t>(doc.get_int("size", 0));
    out.blob = doc.get_string("blob", "");
    out.pieces = static_cast<size_t>(doc.get_int("chunks", 0));
    return !out.blob.empty();
}

bool InlineImages::claim(const std::string& digest, Image& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!lookup(digest, out)) return false;
    recent_[digest] = osal::System::get_uptime_ms();
    return true;
}

void InlineImages::protect(const std::string& digest) {
    std::lock_guard<std::mutex> lock(mutex_);
    recent_[digest] = osal::System::get_uptime_ms();
}

bool InlineImages::read(const std::string& digest, mem::FunctionRef<bool(const char*, size_t)> sink, std::string& why) {
    Image img;
    if (!lookup(digest, img)) {
        why = "image sha256:" + digest + " not found";
        return false;
    }
    size_t sent = 0;
    std::string piece;
    for (size_t i = 0; i < img.pieces; ++i) {
        if (!store_.get(piece_key(img.blob, i), piece).ok() || sent + piece.size() > img.size) {
            why = "image sha256:" + digest + ": piece " + std::to_string(i) + " cannot be read";
            return false;
        }
        sent += piece.size();
        if (!sink(piece.data(), piece.size())) {
            why = "the client went away";
            return false;
        }
    }
    if (sent != img.size) {
        why = "image sha256:" + digest + " is incomplete";
        return false;
    }
    return true;
}

void InlineImages::remove_image(const Image& img) {
    for (size_t i = 0; i < img.pieces; ++i) store_.remove(piece_key(img.blob, i));
}

size_t InlineImages::collect_garbage() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> images, image_keys;
    store_.list_extended(kImagePrefix, images, &image_keys);
    if (image_keys.empty()) return 0;

    // Digests the objects refer to, read in place
    std::vector<std::string> used;
    for (const char* prefix : kTemplateKinds) {
        std::vector<std::string> objects;
        store_.list(prefix, objects);
        for (const std::string& o : objects) {
            for (size_t at = o.find(kDigestKey); at != std::string::npos; at = o.find(kDigestKey, at + 1)) {
                size_t p = at + sizeof(kDigestKey) - 1;
                while (p < o.size() && is_ws(o[p])) ++p;
                if (p + 66 <= o.size() && o[p] == '"' && o[p + 65] == '"') used.push_back(o.substr(p + 1, 64));
            }
        }
    }

    const uint64_t now = osal::System::get_uptime_ms();
    for (auto it = recent_.begin(); it != recent_.end();) {
        it = (now - it->second >= grace_ms_) ? recent_.erase(it) : std::next(it);
    }
    size_t removed = 0;
    for (size_t i = 0; i < image_keys.size(); ++i) {
        const std::string digest = image_keys[i].substr(std::char_traits<char>::length(kImagePrefix));
        if (contains(used, digest) || recent_.count(digest)) continue;
        Image img;
        if (lookup(digest, img)) remove_image(img);
        store_.remove(image_keys[i]);
        ++removed;
        std::cout << "[API] removed image sha256:" << digest.substr(0, 12) << " (" << img.size << " bytes): no object refers to it" << std::endl;
    }
    return removed;
}

size_t InlineImages::migrate() {
    size_t moved = 0;
    for (const char* prefix : kTemplateKinds) {
        std::vector<std::string> objects, keys;
        store_.list_extended(prefix, objects, &keys);
        for (size_t i = 0; i < objects.size(); ++i) {
            if (!carries_module(objects[i])) continue;
            std::string out, why;
            int status = 0;
            const size_t before = objects[i].size();
            if (admit(objects[i], false, out, status, why) && store_.put(keys[i], out, true).ok()) {
                ++moved;
                std::cout << "[API] " << keys[i] << ": inline image moved to the image store (" << before << " -> " << out.size()
                          << " bytes)" << std::endl;
            } else {
                std::cout << "[API] " << keys[i] << ": inline image kept in the object: " << why << std::endl;
            }
            std::string().swap(objects[i]);
        }
    }
    if (moved) store_.commit();

    // Pieces of a write that a restart cut short belong to no image
    std::vector<std::string> blobs;
    {
        std::vector<std::string> images, image_keys;
        store_.list_extended(kImagePrefix, images, &image_keys);
        for (size_t i = 0; i < images.size(); ++i) {
            models::JsonValue doc;
            std::string err;
            if (models::parse_json(images[i], doc, err) && doc.is_object()) blobs.push_back(doc.get_string("blob", ""));
        }
    }
    std::vector<std::string> pieces, piece_keys;
    store_.list_extended(storage::kFlashOnlyPrefix, pieces, &piece_keys);
    const size_t base = std::char_traits<char>::length(storage::kFlashOnlyPrefix);
    for (const std::string& k : piece_keys) {
        const size_t slash = k.find('/', base);
        if (slash == std::string::npos || !contains(blobs, std::string_view(k).substr(base, slash - base))) store_.remove(k);
    }
    collect_garbage();
    return moved;
}

} // namespace kube32::api
