#pragma once

#include <cstddef>
#include <cstdint>

namespace kube32::mem {

enum class MemoryTier {
    InternalSRAM,  // Tier 1: Fast DRAM, DMA-capable (~240 KB)
    ExternalPSRAM, // Tier 2: 2MB External PSRAM
    ArenaScratch   // Tier 3: Request-scoped bump arena (16 KB)
};

struct MemoryStats {
    size_t sram_total;
    size_t sram_free;
    size_t sram_min_free;
    size_t psram_total;
    size_t psram_free;
    size_t psram_min_free;
    size_t arena_high_water;
};

class MemoryManager {
public:
    static void init();

    // Core allocation primitives
    static void* allocate(size_t size, MemoryTier tier, size_t alignment = 4);
    static void deallocate(void* ptr, MemoryTier tier);

    // Telemetry and diagnostics
    static MemoryStats get_stats();
    static bool check_integrity();

    // Test & simulation control
    static void reset_simulation();
};

} // namespace kube32::mem
