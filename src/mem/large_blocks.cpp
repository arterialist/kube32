#include "mem/large_blocks.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "multi_heap.h"

namespace {
multi_heap_handle_t s_heap = nullptr;
uint8_t* s_base = nullptr;
size_t s_size = 0;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<uint32_t> s_overflows{0};

bool in_region(const void* p) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return s_base && b >= s_base && b < s_base + s_size;
}

void* allocate(size_t n) {
    if (n == 0) n = 1;
    if (s_heap && n > kube32::mem::kLargeBlockMin) {
        if (void* p = multi_heap_malloc(s_heap, n)) return p;
        s_overflows.fetch_add(1, std::memory_order_relaxed);
    }
    for (;;) {
        if (void* p = std::malloc(n)) return p;
        std::new_handler handler = std::get_new_handler();
        if (!handler) return nullptr;
        handler();
    }
}

void release(void* p) {
    if (!p) return;
    if (in_region(p)) {
        multi_heap_free(s_heap, p);
    } else {
        std::free(p);
    }
}
} // namespace

namespace kube32::mem {

bool large_blocks_reserve(size_t bytes) {
    if (s_heap || bytes == 0) return false;
    void* block = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!block) return false;
    multi_heap_handle_t heap = multi_heap_register(block, bytes);
    if (!heap) {
        heap_caps_free(block);
        return false;
    }
    multi_heap_set_lock(heap, &s_lock);
    s_base = static_cast<uint8_t*>(block);
    s_size = bytes;
    s_heap = heap;   // last: operator new uses the region from here on
    return true;
}

LargeBlockStats large_blocks_stats() {
    LargeBlockStats st;
    if (!s_heap) return st;
    multi_heap_info_t info;
    multi_heap_get_info(s_heap, &info);
    st.size = s_size;
    st.free = info.total_free_bytes;
    st.largest = info.largest_free_block;
    st.min_free = info.minimum_free_bytes;
    st.overflows = s_overflows.load(std::memory_order_relaxed);
    return st;
}

} // namespace kube32::mem

void* operator new(std::size_t n) {
    if (void* p = allocate(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (void* p = allocate(n)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return allocate(n);
    } catch (...) {
        return nullptr;   // a new handler that throws
    }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return allocate(n);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
