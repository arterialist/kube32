#include "runtime/image_registry.hpp"

#include <cstring>

namespace kube32::runtime {

ImageRegistry& ImageRegistry::instance() {
    static ImageRegistry s_registry;
    return s_registry;
}

void ImageRegistry::add(const EmbeddedImage& image) {
    for (auto& existing : images_) {
        if (std::strcmp(existing.name, image.name) == 0) {
            existing = image;
            return;
        }
    }
    images_.push_back(image);
}

void ImageRegistry::add_all(const EmbeddedImage* images, size_t count) {
    for (size_t i = 0; i < count; ++i) add(images[i]);
}

std::string ImageRegistry::normalize_key(const std::string& image_ref) {
    std::string s = image_ref;
    if (s.rfind("wasm://", 0) == 0) s = s.substr(7);
    size_t slash = s.rfind('/');
    if (slash != std::string::npos) s = s.substr(slash + 1);
    size_t colon = s.find(':');
    if (colon != std::string::npos) s = s.substr(0, colon);
    size_t at = s.find('@');
    if (at != std::string::npos) s = s.substr(0, at);
    return s;
}

const EmbeddedImage* ImageRegistry::resolve(const std::string& image_ref, std::string* out_key) const {
    std::string key = normalize_key(image_ref);
    if (out_key) *out_key = key;
    for (const auto& img : images_) {
        if (key == img.name) return &img;
    }
    return nullptr;
}

std::vector<std::string> ImageRegistry::list() const {
    std::vector<std::string> out;
    for (const auto& img : images_) out.emplace_back(img.name);
    return out;
}

bool base64_decode(const std::string& in, std::vector<uint8_t>& out) {
    static const int8_t table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,62,-1,63, 52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, 15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, 41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    out.clear();
    out.reserve((in.size() / 4) * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int8_t v = table[c];
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return !out.empty();
}

} // namespace kube32::runtime
