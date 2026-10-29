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

uint32_t System::get_cpu_cores() {
    return portNUM_PROCESSORS;
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <sys/time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace kube32::osal {

static const auto g_boot_time = std::chrono::steady_clock::now();

size_t System::get_free_heap_bytes() {
#if defined(__APPLE__)
    // Free, inactive and speculative pages: what macOS can hand out without
    // swapping (the figure vm_stat prints).
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) != KERN_SUCCESS) return 0;
    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS) return 0;
    return static_cast<size_t>(vm.free_count + vm.inactive_count + vm.speculative_count) * static_cast<size_t>(page);
#else
    // MemAvailable from /proc/meminfo, in kB
    FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[128];
    size_t kb = 0;
    while (std::fgets(line, sizeof(line), f)) {
        unsigned long long v = 0;
        if (std::sscanf(line, "MemAvailable: %llu kB", &v) == 1) { kb = static_cast<size_t>(v); break; }
    }
    std::fclose(f);
    return kb * 1024;
#endif
}

size_t System::get_min_free_heap_bytes() {
    return 0;
}

size_t System::get_free_psram_bytes() {
    return 0;
}

size_t System::get_total_psram_bytes() {
    return 0;
}

size_t System::get_total_heap_bytes() {
#if defined(__APPLE__)
    uint64_t bytes = 0;
    size_t len = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &len, nullptr, 0) != 0) return 0;
    return static_cast<size_t>(bytes);
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long page = sysconf(_SC_PAGESIZE);
    return (pages > 0 && page > 0) ? static_cast<size_t>(pages) * static_cast<size_t>(page) : 0;
#endif
}

uint32_t System::get_cpu_cores() {
    return std::thread::hardware_concurrency();
}

uint64_t System::get_unix_time_ms() {
    struct timeval tv;
    if (gettimeofday(&tv, nullptr) != 0) return 0;
    uint64_t ms = static_cast<uint64_t>(tv.tv_sec) * 1000ULL + static_cast<uint64_t>(tv.tv_usec) / 1000ULL;
    return ms < 1577836800000ULL ? 0 : ms;
}

uint32_t System::get_cpu_freq_mhz() {
    return 0;
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
