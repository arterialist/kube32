#pragma once

// Modules that pods carry inline, in the annotation kube32.io/wasm-base64
// (kube32-cli deploy writes it), are kept apart from the objects that carry
// them. When a Pod, Deployment, DaemonSet or Job is written, the API server
// decodes the annotation's value into the object store, keyed by the
// module's SHA-256, and stores the object with
//
//   kube32.io/wasm-sha256   the digest (the reference the kubelet resolves)
//   kube32.io/wasm-size     the module's size in bytes
//
// in place of kube32.io/wasm-base64. Objects stay small, so lists, watches,
// controller passes and the kubelet copy kilobytes, not the module. A
// kubelet reads the module from GET /apis/kube32.io/v1/images/<sha256>.
//
// Store layout:
//   /images/<sha256>     {"size":<bytes>,"blob":"<id>","chunks":<n>}
//   /blobs/<id>/<i>      8 KiB pieces of the module, in order
//                        (storage::PersistClass::FlashOnly: with a flash
//                        journal they leave RAM once they are on flash)
//
// The value is decoded as the request body arrives (InlineImageFilter): a
// request never holds the module, its base64 text or a JSON tree of it.
// The filter leaves "kube32.io/wasm-base64": "sha256:<hex>" in the body;
// admit() turns that into the annotations above. Inside
// kubectl.kubernetes.io/last-applied-configuration the reference stays, so
// a later kubectl apply sends the module again and finds the same digest.
// Images that no object refers to any more are removed by
// collect_garbage().

#include "mem/function_ref.hpp"
#include "osal/sha256.hpp"
#include "storage/store.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace kube32::models {
struct JsonValue;
}

namespace kube32::api {

class InlineImages;

// One module as it is decoded: pieces go to the store as they fill up.
class ImageUpload {
public:
    // `keep` false (dry run): the module is only hashed
    ImageUpload(InlineImages& images, bool keep);
    ~ImageUpload();   // removes what an unfinished upload stored
    ImageUpload(const ImageUpload&) = delete;
    ImageUpload& operator=(const ImageUpload&) = delete;

    bool write(const uint8_t* data, size_t len);
    // Stores the image (or finds it stored already); `digest` gets its SHA-256
    bool finish(std::string& digest);
    size_t size() const { return size_; }
    const std::string& error() const { return error_; }

private:
    bool store_piece();

    InlineImages& images_;
    bool keep_;
    std::string blob_;       // id of this upload's pieces
    std::string piece_;      // the piece being filled
    size_t pieces_{0};       // pieces stored
    size_t size_{0};
    osal::Sha256 sha_;
    bool done_{false};
    std::string error_;
};

// Takes the inline images out of a JSON text as it streams past: every
// "kube32.io/wasm-base64" member (also JSON-escaped, inside a string such as
// kubectl.kubernetes.io/last-applied-configuration) is decoded into the
// store and its value replaced by "sha256:<hex>". Other text is copied.
class InlineImageFilter {
public:
    InlineImageFilter(InlineImages& images, bool keep);
    ~InlineImageFilter();

    // False when a value is not base64 or the image could not be stored:
    // status() and error() say why
    bool feed(const char* data, size_t len, std::string& out);
    // The text ended; false when it ended inside a value
    bool finish();
    int status() const { return status_; }
    const std::string& error() const { return error_; }
    // Images taken out so far
    size_t images() const { return images_taken_; }

private:
    enum class State { Scan, Colon, Open, Value };
    enum class Escape { None, Outer, Inner };
    bool fail(int status, std::string why);
    void begin_value(bool escaped);
    bool value_char(char c, std::string& out);
    bool text(char c, std::string& out);
    bool release_held();
    bool decode(char c);
    bool flush_decoded();
    bool end_value(std::string& out);

    InlineImages& images_;
    bool keep_;
    State state_{State::Scan};
    size_t plain_{0};          // characters of "kube32.io/wasm-base64" matched
    size_t escaped_{0};        // the same, JSON-escaped
    bool escaped_value_{false};
    Escape escape_{Escape::None};
    char held_chars_[8]{};     // the value's first characters, until it is known not to be a reference
    size_t held_{0};
    bool decided_{false};
    bool passthrough_{false};  // the value is a reference
    uint32_t acc_{0};
    int bits_{0};
    bool padded_{false};
    uint8_t decoded_[256];
    size_t out_len_{0};
    std::unique_ptr<ImageUpload> upload_;
    size_t images_taken_{0};
    int status_{0};
    std::string error_;
};

class InlineImages {
public:
    explicit InlineImages(storage::Store& store);

    static constexpr size_t kPieceBytes = 8 * 1024;
    // How long an image stored by a request is safe from collect_garbage()
    // before an object refers to it
    static constexpr uint64_t kGraceMs = 60 * 1000;

    struct Image {
        size_t size{0};
        std::string blob;
        size_t pieces{0};
    };

    // A JSON text with an inline image whose value is not a reference yet
    static bool carries_module(std::string_view json);
    // A JSON text with an inline image annotation of any kind
    static bool mentions_image(std::string_view json);

    // The body of a write as it is stored: inline images in the store,
    // references resolved into kube32.io/wasm-sha256 and kube32.io/wasm-size.
    // False with an HTTP status and the reason when it cannot be.
    bool admit(const std::string& body, bool dry_run, std::string& out, int& status, std::string& why);
    // The references in the annotations of `obj` and of its pod template
    bool resolve(models::JsonValue& obj, bool dry_run, std::string& why);

    bool lookup(const std::string& digest, Image& out);
    // lookup() that also keeps the image from collect_garbage() for kGraceMs:
    // a request that refers to a stored image claims it first
    bool claim(const std::string& digest, Image& out);
    // The module piece by piece; false when it is gone or a piece cannot be read
    bool read(const std::string& digest, mem::FunctionRef<bool(const char*, size_t)> sink, std::string& why);
    // Removes images no Pod, Deployment, DaemonSet or Job refers to. Images
    // stored in the last kGraceMs stay.
    size_t collect_garbage();
    // Takes the inline images out of the objects stored, and drops pieces no
    // image owns (a write cut short by a restart). Run at boot.
    size_t migrate();

    storage::Store& store() { return store_; }
    // How long a stored image is safe before an object refers to it (kGraceMs)
    void set_grace_ms(uint64_t ms) { grace_ms_ = ms; }

private:
    friend class ImageUpload;
    // Keeps `digest` from collect_garbage() for kGraceMs (before its image is written)
    void protect(const std::string& digest);
    void remove_image(const Image& img);

    storage::Store& store_;
    std::mutex mutex_;
    std::map<std::string, uint64_t> recent_;   // digest -> uptime when stored
    uint64_t grace_ms_{kGraceMs};
};

} // namespace kube32::api
