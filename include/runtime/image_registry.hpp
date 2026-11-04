#pragma once

// Local image registry: WebAssembly modules compiled into the firmware image
// (flash-mapped, zero RAM), and the bytes of the modules the kubelet reads
// from elsewhere (ModuleBytes, pod memory).
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
    const char* name;      // registry key, e.g. "my-app"
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
    // Whether `data`..`data + len` lies in an embedded image: memory mapped
    // from flash on a board, part of the program in the simulator
    bool holds(const uint8_t* data, size_t len) const;

private:
    ImageRegistry() = default;
    std::vector<EmbeddedImage> images_;
};

// The bytes of a module that is not memory mapped: read from the card,
// from the API server's image store, or decoded from an annotation. They are
// pod memory (kube32_rt_calloc): the pool block first, then the general
// heap above the system reserve on a board without PSRAM, or the PSRAM
// above its reserve on a board with it. They need no contiguous piece of the
// general heap, and the container that keeps them charges them to its pod's
// account (charge_to). One allocation, sized up front where the size is known.
class ModuleBytes {
public:
    // `memory_limit_bytes`: the pod's linear memory. A pod whose linear
    // memory wants the pool block whole keeps its module outside the block
    // while there is room elsewhere, as it does its runtime structures
    // (kube32_pool_wants_block).
    explicit ModuleBytes(uint32_t memory_limit_bytes = 0);
    ~ModuleBytes() { clear(); }
    ModuleBytes(ModuleBytes&& other) noexcept;
    ModuleBytes& operator=(ModuleBytes&& other) noexcept;
    ModuleBytes(const ModuleBytes&) = delete;
    ModuleBytes& operator=(const ModuleBytes&) = delete;

    // Room for `bytes` in one allocation; false when pod memory has none
    bool reserve(size_t bytes);
    // `bytes` long, the bytes added zero
    bool resize(size_t bytes);
    bool append(const void* data, size_t len);
    bool push_back(uint8_t b) {
        if (size_ == capacity_) return append(&b, 1);
        data_[size_++] = b;
        return true;
    }
    // Gives back the room past size()
    void shrink_to_fit();
    // Frees the bytes
    void clear();
    // Charges the bytes to a pod's account from now on (kube32_rt_set_owner)
    void charge_to(int account);

    uint8_t* data() { return data_; }
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }
    bool empty() const { return size_ == 0; }
    const uint8_t* begin() const { return data_; }
    const uint8_t* end() const { return data_ + size_; }

private:
    uint8_t* data_{nullptr};
    size_t size_{0};
    size_t capacity_{0};
    bool outside_block_{false};
};

// Why a module's bytes found no room in pod memory. A pull that fails so
// starts its reason with kModuleNoMemory: the pod waits for memory.
constexpr const char* kModuleNoMemory = "not enough memory for the module";
std::string module_no_memory(size_t bytes);
bool module_error_is_out_of_memory(const std::string& why);

// RFC 4648 base64 decoder used for inline module annotations. Decoding into
// ModuleBytes says why it failed in `why`: no memory (kModuleNoMemory) or
// not base64.
bool base64_decode(const std::string& in, std::vector<uint8_t>& out);
bool base64_decode(const std::string& in, ModuleBytes& out, std::string& why);

// wasm://sd/<name>[:tag]: true when `image_ref` names a card image. Name and
// tag are limited to [A-Za-z0-9._-] (no leading '.'), 48 and 24 bytes.
bool parse_sd_image_ref(const std::string& image_ref, std::string& out_name, std::string& out_tag);
// "kube32/images/<name>[-<tag>].wasm", relative to the card's mount point.
std::string sd_image_relative_path(const std::string& name, const std::string& tag);

// wasm://blob/<name>: true when `image_ref` names a module in the node's
// blob store (storage::valid_blob_name), which the kubelet parses and
// compiles in place from the mapped flash region.
bool parse_blob_image_ref(const std::string& image_ref, std::string& out_name);

} // namespace kube32::runtime
