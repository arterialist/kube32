#include "osal/system.hpp"

#if defined(ESP_PLATFORM)

#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <soc/rtc.h>
#include <sys/time.h>

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

size_t System::get_total_heap_bytes() {
    return heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

std::string System::get_os_image() {
    return std::string("ESP-IDF ") + esp_get_idf_version();
}

std::string System::get_kernel_version() {
    return std::string("FreeRTOS ") + tskKERNEL_VERSION_NUMBER;
}

uint64_t System::get_unix_time_ms() {
    struct timeval tv;
    if (gettimeofday(&tv, nullptr) != 0) return 0;
    uint64_t ms = static_cast<uint64_t>(tv.tv_sec) * 1000ULL + static_cast<uint64_t>(tv.tv_usec) / 1000ULL;
    // Before 2020-01-01 the clock has never been set (boots at 1970)
    return ms < 1577836800000ULL ? 0 : ms;
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
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
}

} // namespace kube32::osal

#else // Host POSIX implementation

#include <sys/utsname.h>

#include <chrono>
#include <cstdlib>
#include <sys/time.h>

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
    // Simulates 3840KB usable external SPI PSRAM (4MB total capacity)
    return 3840 * 1024;
}

size_t System::get_total_psram_bytes() {
    return 4096 * 1024;
}

size_t System::get_total_heap_bytes() {
    return 0; // the host process heap has no fixed size
}

uint64_t System::get_unix_time_ms() {
    struct timeval tv;
    if (gettimeofday(&tv, nullptr) != 0) return 0;
    uint64_t ms = static_cast<uint64_t>(tv.tv_sec) * 1000ULL + static_cast<uint64_t>(tv.tv_usec) / 1000ULL;
    return ms < 1577836800000ULL ? 0 : ms;
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


std::string System::get_os_image() {
    struct utsname u;
    if (uname(&u) == 0) return std::string(u.sysname) + " " + u.release + " (kube32 host simulator)";
    return "host simulator";
}

std::string System::get_kernel_version() {
    struct utsname u;
    if (uname(&u) == 0) return std::string(u.release);
    return "unknown";
}
} // namespace kube32::osal

#endif
