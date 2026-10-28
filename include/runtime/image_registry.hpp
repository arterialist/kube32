#pragma once

// Local image registry: WebAssembly modules compiled into the firmware image
// (flash-mapped, zero RAM) plus modules shipped inline in a pod manifest.
//
// Image references accepted by the kubelet:
//   wasm://embedded/<name>[:tag]   -> looked up by <name> in the embedded table
//   wasm://sd/<name>[:tag]         -> /sd/kube32/images/<name>[-<tag>].wasm on the
//                                    node's microSD card (pulled by the kubelet)
//   wasm://<anything>/<name>[:tag] -> same lookup by the last path component
//   any image + annotation "kube32.io/wasm-base64" -> module bytes decoded from
//                                                    the annotation (small modules)

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace kube32::runtime {

struct EmbeddedImage {
    const char* name;      // registry key, e.g. "tessera-core"
    const uint8_t* data;
    size_t len;
};

class ImageRegistry {
public:
    static ImageRegistry& instance();

    void add(const EmbeddedImage& image);
    void add_all(const EmbeddedImage* images, size_t count);

    // Returns nullptr when the image is unknown. `out_key` receives the
    // normalized registry key for log messages.
    const EmbeddedImage* resolve(const std::string& image_ref, std::string* out_key = nullptr) const;
    static std::string normalize_key(const std::string& image_ref);

    std::vector<std::string> list() const;

private:
    ImageRegistry() = default;
    std::vector<EmbeddedImage> images_;
};

// RFC 4648 base64 decoder used for inline module annotations.
bool base64_decode(const std::string& in, std::vector<uint8_t>& out);

// wasm://sd/<name>[:tag]: true when `image_ref` names a card image. Name and
// tag are limited to [A-Za-z0-9._-] (no leading '.'), 48 and 24 bytes.
bool parse_sd_image_ref(const std::string& image_ref, std::string& out_name, std::string& out_tag);
// "kube32/images/<name>[-<tag>].wasm", relative to the card's mount point.
std::string sd_image_relative_path(const std::string& name, const std::string& tag);

} // namespace kube32::runtime
