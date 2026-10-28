#pragma once

#include <string>

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

    // Total internal heap the allocator manages. 0 when the platform cannot
    // report it (host build), so callers must treat 0 as "unknown".
    static size_t get_total_heap_bytes();
    static std::string get_os_image();        // e.g. "ESP-IDF v5.5.3" or the host OS
    static std::string get_kernel_version();  // e.g. "FreeRTOS V10.5.1" or uname -r

    // Hardware telemetry
    static uint32_t get_cpu_freq_mhz();
    static uint64_t get_uptime_ms();
    // Wall clock in Unix milliseconds. 0 until the clock has been set (RTC/SNTP
    // on the device); every caller must handle 0 as "no wall clock".
    static uint64_t get_unix_time_ms();

    // Power & Watchdog
    static void restart();
    static void feed_watchdog();
};

} // namespace kube32::osal
