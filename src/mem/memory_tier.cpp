#include "mem/memory_tier.hpp"

#if defined(ESP_PLATFORM)

#include <esp_heap_caps.h>

namespace kube32::mem {

void MemoryManager::init() {}

void* MemoryManager::allocate(size_t size, MemoryTier tier, size_t alignment) {
    if (size == 0) return nullptr;
    uint32_t caps = MALLOC_CAP_8BIT;
    if (tier == MemoryTier::InternalSRAM) {
        caps |= MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    } else if (tier == MemoryTier::ExternalPSRAM) {
        caps |= MALLOC_CAP_SPIRAM;
    }
    return heap_caps_aligned_alloc(alignment, size, caps);
}

void MemoryManager::deallocate(void* ptr, MemoryTier tier) {
    (void)tier;
    if (ptr) {
        heap_caps_free(ptr);
    }
}

MemoryStats MemoryManager::get_stats() {
    MemoryStats stats{};
    stats.sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    stats.sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    stats.sram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#if CONFIG_SPIRAM_SUPPORT || CONFIG_ESP32S3_SPIRAM_SUPPORT
    stats.psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    stats.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    stats.psram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
#else
    stats.psram_total = 0;
    stats.psram_free = 0;
    stats.psram_min_free = 0;
#endif
    stats.arena_high_water = 0;
    return stats;
}

bool MemoryManager::check_integrity() {
    return heap_caps_check_integrity_all(true);
}

void MemoryManager::reset_simulation() {}

} // namespace kube32::mem

#else // Host Simulation

#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <algorithm>

namespace kube32::mem {

static constexpr size_t SIM_SRAM_TOTAL = 240 * 1024;    // 240 KB
static constexpr size_t SIM_PSRAM_TOTAL = 2048 * 1024;  // 2 MB

struct SimState {
    std::mutex mtx;
    size_t sram_used{0};
    size_t sram_min_free{SIM_SRAM_TOTAL};
    size_t psram_used{0};
    size_t psram_min_free{SIM_PSRAM_TOTAL};
    std::unordered_map<void*, std::pair<size_t, MemoryTier>> allocs;
};

static SimState g_sim;

void MemoryManager::init() {
    std::lock_guard<std::mutex> lock(g_sim.mtx);
    g_sim.sram_used = 0;
    g_sim.sram_min_free = SIM_SRAM_TOTAL;
    g_sim.psram_used = 0;
    g_sim.psram_min_free = SIM_PSRAM_TOTAL;
    g_sim.allocs.clear();
}

void* MemoryManager::allocate(size_t size, MemoryTier tier, size_t alignment) {
    if (size == 0) return nullptr;
    std::lock_guard<std::mutex> lock(g_sim.mtx);

    // Enforce simulated memory constraints
    if (tier == MemoryTier::InternalSRAM) {
        if (g_sim.sram_used + size > SIM_SRAM_TOTAL) {
            return nullptr; // Out of simulated SRAM!
        }
    } else if (tier == MemoryTier::ExternalPSRAM) {
        if (g_sim.psram_used + size > SIM_PSRAM_TOTAL) {
            return nullptr; // Out of simulated PSRAM!
        }
    }

    size_t align = alignment;
    if (align < sizeof(void*)) {
        align = sizeof(void*);
    }
    // Alignment must be power of 2 for posix_memalign
    if ((align & (align - 1)) != 0) {
        size_t p = 1;
        while (p < align) p <<= 1;
        align = p;
    }

    void* ptr = nullptr;
    int rc = posix_memalign(&ptr, align, size);
    if (rc != 0 || !ptr) {
        return nullptr;
    }

    if (tier == MemoryTier::InternalSRAM) {
        g_sim.sram_used += size;
        size_t free_now = SIM_SRAM_TOTAL - g_sim.sram_used;
        if (free_now < g_sim.sram_min_free) {
            g_sim.sram_min_free = free_now;
        }
    } else if (tier == MemoryTier::ExternalPSRAM) {
        g_sim.psram_used += size;
        size_t free_now = SIM_PSRAM_TOTAL - g_sim.psram_used;
        if (free_now < g_sim.psram_min_free) {
            g_sim.psram_min_free = free_now;
        }
    }

    g_sim.allocs[ptr] = {size, tier};
    return ptr;
}

void MemoryManager::deallocate(void* ptr, MemoryTier tier) {
    if (!ptr) return;
    std::lock_guard<std::mutex> lock(g_sim.mtx);

    auto it = g_sim.allocs.find(ptr);
    if (it != g_sim.allocs.end()) {
        size_t sz = it->second.first;
        MemoryTier t = it->second.second;
        if (t == MemoryTier::InternalSRAM) {
            if (g_sim.sram_used >= sz) g_sim.sram_used -= sz;
            else g_sim.sram_used = 0;
        } else if (t == MemoryTier::ExternalPSRAM) {
            if (g_sim.psram_used >= sz) g_sim.psram_used -= sz;
            else g_sim.psram_used = 0;
        }
        g_sim.allocs.erase(it);
    }
    std::free(ptr);
}

MemoryStats MemoryManager::get_stats() {
    std::lock_guard<std::mutex> lock(g_sim.mtx);
    MemoryStats stats{};
    stats.sram_total = SIM_SRAM_TOTAL;
    stats.sram_free = (g_sim.sram_used < SIM_SRAM_TOTAL) ? (SIM_SRAM_TOTAL - g_sim.sram_used) : 0;
    stats.sram_min_free = g_sim.sram_min_free;
    stats.psram_total = SIM_PSRAM_TOTAL;
    stats.psram_free = (g_sim.psram_used < SIM_PSRAM_TOTAL) ? (SIM_PSRAM_TOTAL - g_sim.psram_used) : 0;
    stats.psram_min_free = g_sim.psram_min_free;
    stats.arena_high_water = 0;
    return stats;
}

bool MemoryManager::check_integrity() {
    std::lock_guard<std::mutex> lock(g_sim.mtx);
    return (g_sim.sram_used <= SIM_SRAM_TOTAL && g_sim.psram_used <= SIM_PSRAM_TOTAL);
}

void MemoryManager::reset_simulation() {
    std::lock_guard<std::mutex> lock(g_sim.mtx);
    for (auto& pair : g_sim.allocs) {
        std::free(pair.first);
    }
    g_sim.allocs.clear();
    g_sim.sram_used = 0;
    g_sim.sram_min_free = SIM_SRAM_TOTAL;
    g_sim.psram_used = 0;
    g_sim.psram_min_free = SIM_PSRAM_TOTAL;
}

} // namespace kube32::mem

#endif
