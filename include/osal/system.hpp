#pragma once

#include <cstdint>
#include <cstddef>

namespace kube32::osal {

class System {
public:
    // Memory telemetry
    static size_t get_free_heap_bytes();        // Internal fast SRAM free
    static size_t get_min_free_heap_bytes();    // Historical low watermark
    static size_t get_free_psram_bytes();       // External SPI PSRAM free
    static size_t get_total_psram_bytes();      // Total available PSRAM

    // Hardware telemetry
    static uint32_t get_cpu_freq_mhz();
    static uint64_t get_uptime_ms();

    // Power & Watchdog
    static void restart();
    static void feed_watchdog();
};

} // namespace kube32::osal
