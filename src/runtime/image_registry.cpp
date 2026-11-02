#include "runtime/image_registry.hpp"
#include "runtime/pod_memory_pool.h"

#include <algorithm>
#include <cstdio>
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

bool ImageRegistry::holds(const uint8_t* data, size_t len) const {
    const uintptr_t p = reinterpret_cast<uintptr_t>(data);
    for (const auto& img : images_) {
        const uintptr_t start = reinterpret_cast<uintptr_t>(img.data);
        if (p >= start && len <= img.len && p - start <= img.len - len) return true;
    }
    return false;
}

namespace {
// While alive, pod allocations try outside the pool block first
struct OutsideBlock {
    bool on;
    explicit OutsideBlock(bool o) : on(o) { if (on) kube32_pool_prefer_general(1); }
    ~OutsideBlock() { if (on) kube32_pool_prefer_general(0); }
};
} // namespace

ModuleBytes::ModuleBytes(uint32_t memory_limit_bytes) : outside_block_(kube32_pool_wants_block(memory_limit_bytes) != 0) {}

ModuleBytes::ModuleBytes(ModuleBytes&& other) noexcept
    : data_(other.data_), size_(other.size_), capacity_(other.capacity_), outside_block_(other.outside_block_) {
    other.data_ = nullptr;
    other.size_ = other.capacity_ = 0;
}

ModuleBytes& ModuleBytes::operator=(ModuleBytes&& other) noexcept {
    if (this != &other) {
        clear();
        data_ = other.data_;
        size_ = other.size_;
        capacity_ = other.capacity_;
        outside_block_ = other.outside_block_;
        other.data_ = nullptr;
        other.size_ = other.capacity_ = 0;
    }
    return *this;
}

bool ModuleBytes::reserve(size_t bytes) {
    if (bytes <= capacity_) return true;
    OutsideBlock outside(outside_block_);
    void* p = data_ ? kube32_rt_realloc(data_, bytes) : kube32_rt_calloc(bytes);
    if (!p) return false;
    data_ = static_cast<uint8_t*>(p);
    capacity_ = bytes;
    return true;
}

bool ModuleBytes::resize(size_t bytes) {
    if (!reserve(bytes)) return false;
    if (bytes > size_) std::memset(data_ + size_, 0, bytes - size_);
    size_ = bytes;
    return true;
}

bool ModuleBytes::append(const void* data, size_t len) {
    // Grown twice as large when the size was not known up front, else to fit
    if (size_ + len > capacity_ && !reserve(std::max({size_ + len, capacity_ * 2, static_cast<size_t>(256)})) && !reserve(size_ + len)) {
        return false;
    }
    if (len) std::memcpy(data_ + size_, data, len);
    size_ += len;
    return true;
}

void ModuleBytes::shrink_to_fit() {
    if (!data_ || capacity_ == size_) return;
    if (size_ == 0) {
        clear();
        return;
    }
    if (!kube32_rt_shrink(data_, size_)) {
        if (void* p = kube32_rt_realloc(data_, size_)) data_ = static_cast<uint8_t*>(p);
    }
    capacity_ = size_;
}

void ModuleBytes::clear() {
    if (data_) kube32_rt_free(data_);
    data_ = nullptr;
    size_ = capacity_ = 0;
}

void ModuleBytes::charge_to(int account) {
    if (data_) kube32_rt_set_owner(data_, account);
}

std::string module_no_memory(size_t bytes) {
    char why[160];
    const int n = snprintf(why, sizeof why, "%s: %u bytes in one piece", kModuleNoMemory, static_cast<unsigned>(bytes));
    if (kube32_pool_block_size() > 0 && n > 0 && static_cast<size_t>(n) < sizeof why) {
        snprintf(why + n, sizeof why - n, " (pool block: %u bytes free, in pieces of at most %u)", static_cast<unsigned>(kube32_pool_available(nullptr)),
                 static_cast<unsigned>(kube32_pool_largest_free()));
    }
    return why;
}

bool module_error_is_out_of_memory(const std::string& why) {
    return why.find(kModuleNoMemory) != std::string::npos;
}

static bool valid_image_token(const std::string& s, size_t max_len) {
    if (s.empty() || s.size() > max_len || s[0] == '.') return false;
    for (char c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool parse_sd_image_ref(const std::string& image_ref, std::string& out_name, std::string& out_tag) {
    out_name.clear();
    out_tag.clear();
    std::string s = image_ref;
    if (s.rfind("wasm://", 0) == 0) s = s.substr(7);
    if (s.rfind("sd/", 0) != 0) return false;
    s = s.substr(3);
    if (s.find('/') != std::string::npos || s.find('@') != std::string::npos) return false;
    size_t colon = s.find(':');
    std::string name = (colon == std::string::npos) ? s : s.substr(0, colon);
    std::string tag = (colon == std::string::npos) ? std::string() : s.substr(colon + 1);
    if (!valid_image_token(name, 48)) return false;
    if (colon != std::string::npos && !valid_image_token(tag, 24)) return false;
    out_name = name;
    out_tag = tag;
    return true;
}

std::string sd_image_relative_path(const std::string& name, const std::string& tag) {
    return "kube32/images/" + name + (tag.empty() ? std::string() : "-" + tag) + ".wasm";
}

namespace {
// Calls `put` with each byte `in` decodes to; false when `in` is not base64
// or `put` returns false
template <class Put>
bool base64_each(const std::string& in, Put put) {
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
            if (!put(static_cast<uint8_t>((acc >> bits) & 0xFF))) return false;
        }
    }
    return true;
}
} // namespace

bool base64_decode(const std::string& in, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve((in.size() / 4) * 3);
    return base64_each(in, [&](uint8_t b) { out.push_back(b); return true; }) && !out.empty();
}

bool base64_decode(const std::string& in, ModuleBytes& out, std::string& why) {
    out.clear();
    const size_t most = (in.size() * 3 + 3) / 4;
    if (!out.reserve(most)) {
        why = module_no_memory(most);
        return false;
    }
    if (!base64_each(in, [&](uint8_t b) { return out.push_back(b); }) || out.empty()) {
        out.clear();
        why = "kube32.io/wasm-base64 is not a base64 module";
        return false;
    }
    out.shrink_to_fit();
    return true;
}

} // namespace kube32::runtime
