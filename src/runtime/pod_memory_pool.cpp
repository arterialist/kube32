#include "runtime/pod_memory_pool.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

static void* s_block = nullptr;
static size_t s_block_size = 0;
static std::atomic<bool> s_in_use{false};
static bool s_external = false;
static const size_t k_threshold = 48 * 1024;

extern "C" {

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
    return (s_block && !s_in_use.load()) ? s_block_size : 0;
}

size_t kube32_pool_threshold(void) { return k_threshold; }
size_t kube32_pool_block_size(void) { return s_block_size; }

void* kube32_pool_alloc(size_t bytes) {
    if (!s_block || bytes < k_threshold || bytes > s_block_size) return nullptr;
    bool expected = false;
    if (!s_in_use.compare_exchange_strong(expected, true)) return nullptr;
    std::memset(s_block, 0, bytes);
    return s_block;
}

void* kube32_rt_calloc(size_t bytes) {
#if defined(ESP_PLATFORM)
    // Keep a small PSRAM reserve for stacks allocated later; everything else
    // the interpreter needs is better off external than in the 300 KB SRAM.
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > bytes + 16384) {
        void* p = heap_caps_calloc(bytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) return p;
    }
    return heap_caps_calloc(bytes, 1, MALLOC_CAP_8BIT);
#else
    return std::calloc(bytes, 1);
#endif
}

void* kube32_rt_realloc(void* ptr, size_t bytes) {
#if defined(ESP_PLATFORM)
    if (!ptr) return kube32_rt_calloc(bytes);
    return heap_caps_realloc(ptr, bytes, MALLOC_CAP_8BIT);
#else
    return std::realloc(ptr, bytes);
#endif
}

void kube32_rt_free(void* ptr) {
#if defined(ESP_PLATFORM)
    heap_caps_free(ptr);
#else
    std::free(ptr);
#endif
}

int kube32_pool_owns(const void* ptr) {
    return (s_block && ptr == s_block) ? 1 : 0;
}

int kube32_pool_free(void* ptr) {
    if (!kube32_pool_owns(ptr)) return 0;
    s_in_use.store(false);
    return 1;
}

}
