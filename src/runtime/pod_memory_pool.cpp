#include "runtime/pod_memory_pool.h"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

static void* s_block = nullptr;
static size_t s_block_size = 0;
static std::atomic<bool> s_in_use{false};
static bool s_external = false;
static const size_t k_threshold = 48 * 1024;
static void* s_stack = nullptr;
static size_t s_stack_size = 0;
static std::atomic<bool> s_stack_in_use{false};

// The block's two uses can share it: one large linear memory (a pod whose
// memory wants most of the block) at its start, and pod allocations the
// general heap cannot place (it keeps a reserve for the system, and after
// Wi-Fi it has no large blocks) as a small heap of its own, placed from the
// end down so the start stays free for a large memory as long as possible.
// The small heap is gone again once its last allocation is freed.
namespace {
struct SubChunk {
    uint32_t size;   // whole chunk, header included, multiple of 8
    uint32_t used;
};
constexpr size_t kSubAlign = 8;
constexpr size_t kMinChunk = 4 * sizeof(SubChunk);
std::mutex s_sub_mutex;
bool s_sub_active = false;
size_t s_sub_allocs = 0;
uint8_t* s_sub_base = nullptr;    // start of the small heap (above a large memory)
size_t s_whole_bytes = 0;         // the large memory's size while one is placed

size_t align8(size_t n) { return (n + kSubAlign - 1) & ~(kSubAlign - 1); }
uint8_t* sub_end() { return static_cast<uint8_t*>(s_block) + (s_block_size & ~(kSubAlign - 1)); }
SubChunk* sub_first() { return reinterpret_cast<SubChunk*>(s_sub_base); }
SubChunk* sub_next(SubChunk* c) { return reinterpret_cast<SubChunk*>(reinterpret_cast<uint8_t*>(c) + c->size); }
bool sub_in_range(const void* p) {
    const uint8_t* b = static_cast<const uint8_t*>(s_block);
    return s_block && p > b && p < b + s_block_size;
}

void sub_coalesce() {
    for (SubChunk* c = sub_first(); reinterpret_cast<uint8_t*>(c) < sub_end(); c = sub_next(c)) {
        while (!c->used) {
            SubChunk* n = sub_next(c);
            if (reinterpret_cast<uint8_t*>(n) >= sub_end() || n->used) break;
            c->size += n->size;
        }
    }
}

// Caller holds s_sub_mutex
void* sub_alloc_locked(size_t bytes) {
    if (!s_block || bytes == 0 || bytes > s_block_size) return nullptr;
    if (!s_sub_active) {
        s_sub_base = static_cast<uint8_t*>(s_block) + (s_in_use.load() ? align8(s_whole_bytes) : 0);
        if (sub_end() - s_sub_base < static_cast<ptrdiff_t>(kMinChunk)) return nullptr;
        SubChunk* c = sub_first();
        c->size = static_cast<uint32_t>(sub_end() - s_sub_base);
        c->used = 0;
        s_sub_active = true;
        s_sub_allocs = 0;
    }
    const size_t need = align8(bytes) + sizeof(SubChunk);
    // The highest free chunk that fits, allocated from its top
    SubChunk* fit = nullptr;
    for (SubChunk* c = sub_first(); reinterpret_cast<uint8_t*>(c) < sub_end(); c = sub_next(c)) {
        if (!c->used && c->size >= need) fit = c;
    }
    if (!fit) return nullptr;
    SubChunk* out = fit;
    if (fit->size - need >= kMinChunk) {
        fit->size = static_cast<uint32_t>(fit->size - need);
        out = sub_next(fit);
        out->size = static_cast<uint32_t>(need);
    }
    out->used = 1;
    s_sub_allocs++;
    return out + 1;
}

void sub_free_locked(void* p) {
    SubChunk* c = static_cast<SubChunk*>(p) - 1;
    c->used = 0;
    if (--s_sub_allocs == 0) s_sub_active = false;
    else sub_coalesce();
}

// Caller holds s_sub_mutex. The chunk keeps `bytes` and its start; the rest
// becomes a free chunk when it is at least a minimal one.
bool sub_shrink_locked(void* p, size_t bytes) {
    SubChunk* c = static_cast<SubChunk*>(p) - 1;
    const size_t need = align8(bytes) + sizeof(SubChunk);
    if (c->size < need + kMinChunk) return false;
    SubChunk* tail = reinterpret_cast<SubChunk*>(reinterpret_cast<uint8_t*>(c) + need);
    tail->size = static_cast<uint32_t>(c->size - need);
    tail->used = 0;
    c->size = static_cast<uint32_t>(need);
    sub_coalesce();
    return true;
}
} // namespace

extern "C" {

void* kube32_pool_sub_alloc(size_t bytes) {
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    void* p = sub_alloc_locked(bytes);
    if (p) std::memset(p, 0, bytes);
    return p;
}

int kube32_pool_sub_free(void* ptr) {
    if (!ptr || !sub_in_range(ptr)) return 0;
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    if (!s_sub_active || static_cast<uint8_t*>(ptr) <= s_sub_base) return 0;
    sub_free_locked(ptr);
    return 1;
}

int kube32_rt_shrink(void* ptr, size_t bytes) {
    if (!ptr || !sub_in_range(ptr)) return 0;
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    if (!s_sub_active || static_cast<uint8_t*>(ptr) <= s_sub_base) return 0;
    return sub_shrink_locked(ptr, bytes) ? 1 : 0;
}

size_t kube32_pool_whole_capacity(void) {
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    return s_sub_active ? static_cast<size_t>(s_sub_base - static_cast<uint8_t*>(s_block)) : s_block_size;
}

void kube32_pool_reserve(size_t bytes) {
    if (s_block || bytes == 0) return;
#if defined(ESP_PLATFORM)
    s_block = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    s_block = std::malloc(bytes);
#endif
    s_block_size = s_block ? bytes : 0;
}

void kube32_pool_reserve_external(size_t bytes) {
    if (s_block || bytes == 0) return;
#if defined(ESP_PLATFORM)
    s_block = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    s_block = std::malloc(bytes);
#endif
    s_block_size = s_block ? bytes : 0;
    s_external = (s_block != nullptr);
}

size_t kube32_pool_available(int* out_external) {
    if (out_external) *out_external = s_external ? 1 : 0;
    if (!s_block) return 0;
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    if (!s_sub_active) return s_in_use.load() ? s_block_size - align8(s_whole_bytes) : s_block_size;
    size_t free_bytes = 0;
    for (SubChunk* c = sub_first(); reinterpret_cast<uint8_t*>(c) < sub_end(); c = sub_next(c)) {
        if (!c->used) free_bytes += c->size - sizeof(SubChunk);
    }
    return free_bytes;
}

size_t kube32_pool_threshold(void) { return k_threshold; }

void kube32_pool_reserve_stack(size_t bytes) {
    if (s_stack || bytes == 0) return;
#if defined(ESP_PLATFORM)
    s_stack = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    s_stack = std::malloc(bytes);
#endif
    s_stack_size = s_stack ? bytes : 0;
}

void* kube32_pool_stack_alloc(size_t bytes) {
    if (!s_stack || bytes > s_stack_size) return nullptr;
    bool expected = false;
    if (!s_stack_in_use.compare_exchange_strong(expected, true)) return nullptr;
    return s_stack;
}

int kube32_pool_stack_free(void* ptr) {
    if (!ptr || ptr != s_stack) return 0;
    s_stack_in_use.store(false);
    return 1;
}

size_t kube32_pool_stack_size(void) { return s_stack_size; }
size_t kube32_pool_stack_available(void) { return (s_stack && !s_stack_in_use.load()) ? s_stack_size : 0; }
size_t kube32_pool_block_size(void) { return s_block_size; }

void* kube32_pool_alloc(size_t bytes) {
    // The block is sized for the node's largest pod: a request under half of
    // it is served by the general allocator so a small pod started first does
    // not take the block from the pod it was reserved for.
    if (!s_block || bytes < k_threshold || bytes < s_block_size / 2 || bytes > s_block_size) return nullptr;
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    if (s_in_use.load()) return nullptr;
    if (s_sub_active) {
        // The small heap lives at the top: the memory fits below it when the
        // bottom chunk is free and large enough
        SubChunk* first = sub_first();
        const size_t take = align8(bytes);
        if (s_sub_base != s_block || first->used || first->size < take + kMinChunk) return nullptr;
        const size_t rest = first->size - take;
        s_sub_base = static_cast<uint8_t*>(s_block) + take;
        SubChunk* moved = sub_first();
        moved->size = static_cast<uint32_t>(rest);
        moved->used = 0;
    }
    s_in_use.store(true);
    s_whole_bytes = bytes;
    std::memset(s_block, 0, bytes);
    return s_block;
}

// Where pod allocations (runtime, compiled code, linear memory) go:
//   with PSRAM: external RAM first (a small reserve kept for stacks), then
//     internal RAM
//   without PSRAM, when the pod pool is internal: the pool's block first,
//     then the general heap, which stays for the kubelet, Wi-Fi and lwIP.
//     A pod whose linear memory wants the block whole asks for the general
//     heap first while it loads (kube32_pool_prefer_general).
// Bytes pods hold in the general heap and in PSRAM are counted. The general
// heap keeps a reserve for the system that pods are never given: on the
// board 16 KiB of internal RAM stay free for the kubelet, Wi-Fi and lwIP; the
// host simulator models a board's heap with a budget instead
// (kube32_pool_set_heap_budget).
static std::atomic<int> s_prefer_general{0};
static std::atomic<size_t> s_general_bytes{0};
static std::atomic<size_t> s_psram_bytes{0};
static size_t s_heap_budget = 0;                    // 0: not modelled
static const size_t k_system_reserve_default = 16 * 1024;
static const size_t k_psram_stack_reserve = 16 * 1024;   // PSRAM kept for thread stacks made later

static size_t general_size(void* p) {
#if defined(ESP_PLATFORM)
    return heap_caps_get_allocated_size(p);
#elif defined(__APPLE__)
    return malloc_size(p);
#else
    return malloc_usable_size(p);
#endif
}

// Internal RAM pods leave to the system. A board without PSRAM keeps more:
// its general heap also absorbs the network's bursts (lwIP send buffers and
// Wi-Fi frames, measured near 25 KB while a pod streams a large response)
static size_t system_reserve() {
#if defined(ESP_PLATFORM)
    if (s_block && !s_external) return 24 * 1024;
#endif
    return k_system_reserve_default;
}

// Whether `bytes` more of the general heap leave the system reserve free
static bool general_has_room(size_t bytes) {
    if (s_heap_budget) return s_general_bytes.load() + bytes + system_reserve() <= s_heap_budget;
#if defined(ESP_PLATFORM)
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= bytes + system_reserve();
#else
    return true;
#endif
}

static void* general_calloc(size_t bytes) {
    if (!general_has_room(bytes)) return nullptr;
#if defined(ESP_PLATFORM)
    void* p = heap_caps_calloc(bytes, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    void* p = std::calloc(bytes, 1);
#endif
    if (p) s_general_bytes.fetch_add(general_size(p));
    return p;
}

static bool pool_first() { return s_block && !s_external && !s_prefer_general.load(); }

void kube32_pool_prefer_general(int on) { s_prefer_general.fetch_add(on ? 1 : -1); }
void kube32_pool_set_heap_budget(size_t bytes) { s_heap_budget = bytes; }
size_t kube32_pool_heap_budget(void) { return s_heap_budget; }
size_t kube32_pool_general_bytes(void) { return s_general_bytes.load(); }
size_t kube32_pool_system_reserve(void) { return system_reserve(); }

size_t kube32_pool_pod_room(size_t memory_bytes, size_t* out_capacity) {
    size_t room = 0;
    if (s_heap_budget) {
        const size_t used = s_general_bytes.load() + system_reserve();
        room = s_heap_budget > used ? s_heap_budget - used : 0;
    } else {
#if defined(ESP_PLATFORM)
        // Without PSRAM a pod is admitted on the pool block alone: general
        // heap that looks free now is what the network needs in a burst
        const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!(s_block && !s_external)) room = internal > system_reserve() ? internal - system_reserve() : 0;
        const size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (psram > k_psram_stack_reserve) room += psram - k_psram_stack_reserve;
#else
        room = static_cast<size_t>(1) << 30;   // the host's heap: not modelled
#endif
    }
    // The block is not part of the heap figures above. An internal block is
    // shared by small pods; an external one serves one large linear memory.
    size_t pool_held = 0;
    if (s_block && (!s_external || memory_bytes >= k_threshold)) {
        const size_t pool_free = kube32_pool_available(nullptr);
        room += pool_free;
        pool_held = s_block_size - pool_free;
    }
    if (out_capacity) *out_capacity = room + pool_held + s_general_bytes.load() + s_psram_bytes.load();
    return room;
}

static bool in_psram(const void* p) {
#if defined(ESP_PLATFORM)
    return esp_ptr_external_ram(p);
#else
    (void)p;
    return false;
#endif
}

void* kube32_rt_calloc(size_t bytes) {
#if defined(ESP_PLATFORM)
    // Keep a small PSRAM reserve for stacks allocated later; everything else
    // the interpreter needs is better off external than in the 300 KB SRAM.
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > bytes + k_psram_stack_reserve) {
        void* p = heap_caps_calloc(bytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) {
            s_psram_bytes.fetch_add(general_size(p));
            return p;
        }
    }
#endif
    if (pool_first()) {
        void* shared = kube32_pool_sub_alloc(bytes);
        if (shared) return shared;
    }
    void* p = general_calloc(bytes);
    return p ? p : kube32_pool_sub_alloc(bytes);
}

void* kube32_rt_realloc(void* ptr, size_t bytes) {
    if (!ptr) return kube32_rt_calloc(bytes);
    if (sub_in_range(ptr) && s_sub_active && static_cast<uint8_t*>(ptr) > s_sub_base) {
        // Within the block's own heap: in place when the chunk is large
        // enough, else moved
        SubChunk* c = static_cast<SubChunk*>(ptr) - 1;
        const size_t have = c->size - sizeof(SubChunk);
        if (bytes <= have) return ptr;
        void* moved = kube32_rt_calloc(bytes);
        if (!moved) return nullptr;
        std::memcpy(moved, ptr, have);
        kube32_pool_sub_free(ptr);
        return moved;
    }
    const size_t old = general_size(ptr);
    if (in_psram(ptr)) {
        // Grown in place in PSRAM, or moved to wherever kube32_rt_calloc finds room
        void* p = nullptr;
#if defined(ESP_PLATFORM)
        if (bytes <= old || heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > (bytes - old) + k_psram_stack_reserve) {
            p = heap_caps_realloc(ptr, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
#endif
        if (p) {
            s_psram_bytes.fetch_sub(old);
            s_psram_bytes.fetch_add(general_size(p));
            return p;
        }
        void* moved = kube32_rt_calloc(bytes);
        if (!moved) return nullptr;
        std::memcpy(moved, ptr, old < bytes ? old : bytes);
        kube32_rt_free(ptr);
        return moved;
    }
    if (bytes > old && !general_has_room(bytes - old)) return nullptr;
#if defined(ESP_PLATFORM)
    void* p = heap_caps_realloc(ptr, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    void* p = std::realloc(ptr, bytes);
#endif
    if (p) {
        s_general_bytes.fetch_sub(old);
        s_general_bytes.fetch_add(general_size(p));
    }
    return p;
}

void kube32_rt_free(void* ptr) {
    if (!ptr || kube32_pool_sub_free(ptr)) return;
    (in_psram(ptr) ? s_psram_bytes : s_general_bytes).fetch_sub(general_size(ptr));
#if defined(ESP_PLATFORM)
    heap_caps_free(ptr);
#else
    std::free(ptr);
#endif
}

static uint8_t* s_interp_stack = nullptr;
static size_t s_interp_size = 0;

void kube32_interp_stack_set(void* stack, size_t bytes) {
    s_interp_stack = static_cast<uint8_t*>(stack);
    s_interp_size = stack ? bytes : 0;
}

int kube32_interp_stack_active(void) { return s_interp_stack ? 1 : 0; }

void* kube32_interp_stack_get(size_t* out_bytes) {
    if (out_bytes) *out_bytes = s_interp_size;
    return s_interp_stack;
}

void* kube32_interp_stack_base_for(const void* sp) {
    const uint8_t* p = static_cast<const uint8_t*>(sp);
    return (s_interp_stack && p >= s_interp_stack && p < s_interp_stack + s_interp_size) ? s_interp_stack : nullptr;
}

int kube32_pool_owns(const void* ptr) {
    return (s_block && ptr == s_block) ? 1 : 0;
}

int kube32_pool_free(void* ptr) {
    if (!kube32_pool_owns(ptr)) return 0;
    std::lock_guard<std::mutex> lock(s_sub_mutex);
    if (s_sub_active && s_sub_base != s_block) {
        // The space the memory held joins the small heap's bottom chunk
        SubChunk* first = sub_first();
        const size_t gained = static_cast<size_t>(s_sub_base - static_cast<uint8_t*>(s_block));
        SubChunk* bottom = static_cast<SubChunk*>(s_block);
        bottom->size = static_cast<uint32_t>(gained + (first->used ? 0 : first->size));
        bottom->used = 0;
        if (first->used) bottom->size = static_cast<uint32_t>(gained);
        s_sub_base = static_cast<uint8_t*>(s_block);
    }
    s_whole_bytes = 0;
    s_in_use.store(false);
    return 1;
}

}
