#include "osal/system.hpp"

#if defined(ESP_PLATFORM)

#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <soc/rtc.h>

namespace kube32::osal {

size_t System::get_free_heap_bytes() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t System::get_min_free_heap_bytes() {
    return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t System::get_free_psram_bytes() {
#if CONFIG_SPIRAM_SUPPORT || CONFIG_ESP32S3_SPIRAM_SUPPORT
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#else
    return 0;
#endif
}

size_t System::get_total_psram_bytes() {
#if CONFIG_SPIRAM_SUPPORT || CONFIG_ESP32S3_SPIRAM_SUPPORT
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
#else
    return 0;
#endif
}

uint32_t System::get_cpu_freq_mhz() {
    rtc_cpu_freq_config_t conf;
    rtc_clk_cpu_freq_get_config(&conf);
    return conf.freq_mhz;
}

uint64_t System::get_uptime_ms() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

void System::restart() {
    esp_restart();
}

void System::feed_watchdog() {
    esp_task_wdt_reset();
}

} // namespace kube32::osal

#else // Host POSIX implementation

#include <chrono>
#include <cstdlib>

namespace kube32::osal {

static const auto g_boot_time = std::chrono::steady_clock::now();

size_t System::get_free_heap_bytes() {
    // Simulates 240KB fast DRAM heap
    return 240 * 1024;
}

size_t System::get_min_free_heap_bytes() {
    return 180 * 1024;
}

size_t System::get_free_psram_bytes() {
    // Simulates 2048KB external SPI PSRAM
    return 2048 * 1024;
}

size_t System::get_total_psram_bytes() {
    return 2048 * 1024;
}

uint32_t System::get_cpu_freq_mhz() {
    return 240;
}

uint64_t System::get_uptime_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now() - g_boot_time).count();
}

void System::restart() {
    // Host simulation exits process cleanly on restart if requested
    std::exit(0);
}

void System::feed_watchdog() {
    // Host no-op
}

} // namespace kube32::osal

#endif
