#pragma once

#include <string>

#include <cstdint>
#include <cstddef>

namespace kube32::osal {

class System {
public:
    // Memory telemetry. On the board "heap" is the internal SRAM heap; on the
    // host it is the machine's physical memory (total) and the memory the
    // kernel reports as available (free). A figure of 0 means "unknown".
    static size_t get_free_heap_bytes();
    static size_t get_min_free_heap_bytes();    // low watermark since boot; 0 on the host (not tracked)
    static size_t get_free_psram_bytes();       // external PSRAM free; 0 without PSRAM and on the host
    static size_t get_total_psram_bytes();      // external PSRAM size; 0 without PSRAM and on the host
    static size_t get_total_heap_bytes();
    static uint32_t get_cpu_cores();          // cores the scheduler runs tasks on; 0 when unknown
    static std::string get_os_image();        // e.g. "ESP-IDF v5.5.3" or the host OS
    static std::string get_kernel_version();  // e.g. "FreeRTOS V10.5.1" or uname -r

    // Hardware telemetry
    static uint32_t get_cpu_freq_mhz();       // 0 when the platform does not report it
    static uint64_t get_uptime_ms();
    // Wall clock in Unix milliseconds. 0 until the clock has been set (RTC/SNTP
    // on the device); every caller must handle 0 as "no wall clock".
    static uint64_t get_unix_time_ms();

    // Power & Watchdog
    static void restart();
    static void feed_watchdog();
};

} // namespace kube32::osal
