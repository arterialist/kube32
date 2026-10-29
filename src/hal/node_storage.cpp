#include "hal/node_storage.hpp"
#include "hal/sd_card.hpp"

#include <cerrno>
#include <exception>
#include <mutex>
#include <utility>
#include <sys/stat.h>

#if defined(KUBE32_TARGET_ESP32)
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#else
#include <sys/statvfs.h>
#endif

namespace kube32::hal {

namespace {

std::mutex s_mutex;
NodeStorageInfo s_info;

#if defined(KUBE32_TARGET_ESP32)

constexpr const char* kFlashMount = "/data";
constexpr const char* kFlashPartition = "storage";
// Stack of the storage task: FATFS, wear levelling and the flash driver on
// one call path take about 3 KB; a journal batch written for an API request
// (records encoded, compaction moving older records) needs several more
constexpr uint32_t kStorageTaskStackBytes = 16384;

wl_handle_t s_wl = WL_INVALID_HANDLE;
// Flash is written with the cache off, so a call must not run on a PSRAM
// stack. Callers on one hand their call to this task (stack in internal RAM)
// and wait. A task rather than a stack switch: ESP-IDF's shared stack
// function keeps its state in globals, and pod calls already use it for the
// interpreter stack, sometimes with a storage call nested inside.
TaskHandle_t s_storage_task = nullptr;
SemaphoreHandle_t s_call_ready = nullptr;
SemaphoreHandle_t s_call_done = nullptr;
std::mutex s_call_mutex;
const std::function<void()>* s_call = nullptr;
std::exception_ptr s_call_error;

void storage_task(void*) {
    for (;;) {
        if (xSemaphoreTake(s_call_ready, portMAX_DELAY) != pdTRUE) continue;
        try {
            (*s_call)();
        } catch (...) {
            s_call_error = std::current_exception();
        }
        xSemaphoreGive(s_call_done);
    }
}

bool mount_internal_flash(std::string& error) {
    const bool psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    // Wear levelling keeps a 4 KiB sector buffer from plain malloc(). On a
    // board with PSRAM it goes there (the flash driver bounces PSRAM buffers
    // through internal RAM itself); FATFS prefers PSRAM already.
    if (psram) heap_caps_malloc_extmem_enable(16);
    esp_vfs_fat_mount_config_t cfg = {};
    cfg.format_if_mount_failed = true;
    cfg.max_files = 4;
    cfg.allocation_unit_size = 4096;
    cfg.disk_status_check_enable = false;
    cfg.use_one_fat = true;
    esp_err_t r = esp_vfs_fat_spiflash_mount_rw_wl(kFlashMount, kFlashPartition, &cfg, &s_wl);
    if (psram) heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    if (r != ESP_OK) {
        s_wl = WL_INVALID_HANDLE;
        error = std::string("internal flash partition \"") + kFlashPartition + "\": " + esp_err_to_name(r);
        return false;
    }
    if (psram) {
        // Tasks with PSRAM stacks (pods, the kubelet, API workers) reach the
        // volume through this internal-RAM stack
        s_call_ready = xSemaphoreCreateBinary();
        s_call_done = xSemaphoreCreateBinary();
        if (s_call_ready && s_call_done) {
            xTaskCreatePinnedToCore(storage_task, "k32_storage", kStorageTaskStackBytes, nullptr, 5, &s_storage_task, tskNO_AFFINITY);
        }
    }
    return true;
}

void refresh_space_locked() {
    uint64_t total = 0, free_b = 0;
    if (!s_info.root.empty() && esp_vfs_fat_info(s_info.root.c_str(), &total, &free_b) == ESP_OK) {
        s_info.total_bytes = total;
        s_info.free_bytes = free_b;
    }
}

#else

void refresh_space_locked() {
    struct statvfs vfs;
    if (!s_info.root.empty() && statvfs(s_info.root.c_str(), &vfs) == 0) {
        s_info.total_bytes = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
        s_info.free_bytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    }
}

#endif

} // namespace

// A card mounted (or removed) after boot becomes (or stops being) the root,
// unless the root is internal flash.
static void follow_card_locked() {
    if (s_info.medium == StorageMedium::InternalFlash) return;
    std::string card = sd_root_path();
    if (!card.empty()) {
        if (s_info.medium != StorageMedium::SdCard || s_info.root != card) {
            s_info.medium = StorageMedium::SdCard;
            s_info.root = card;
            s_info.error.clear();
        }
    } else if (s_info.medium == StorageMedium::SdCard) {
        s_info.medium = StorageMedium::None;
        s_info.root.clear();
        s_info.error = sd_info().error;
    }
}

const char* storage_medium_name(StorageMedium medium) {
    switch (medium) {
        case StorageMedium::SdCard: return "sd";
        case StorageMedium::InternalFlash: return "flash";
        default: return "none";
    }
}

bool node_storage_mount(bool has_card_slot, const std::string& flash_directory) {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        follow_card_locked();
        if (s_info.medium != StorageMedium::None) return true;
    }
#if defined(KUBE32_TARGET_ESP32)
    const size_t ram0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif
    NodeStorageInfo info;
    std::string card_error;
    if (has_card_slot) {
        if (sd_mount()) {
            info.medium = StorageMedium::SdCard;
            info.root = sd_root_path();
        } else {
            card_error = sd_info().error;
        }
    }
    if (info.medium == StorageMedium::None) {
#if defined(KUBE32_TARGET_ESP32)
        (void)flash_directory;
        std::string err;
        if (mount_internal_flash(err)) {
            info.medium = StorageMedium::InternalFlash;
            info.root = kFlashMount;
        } else {
            info.error = err;
        }
#else
        struct stat st;
        if (!flash_directory.empty() && (::mkdir(flash_directory.c_str(), 0755) == 0 || errno == EEXIST) &&
            stat(flash_directory.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            info.medium = StorageMedium::InternalFlash;
            info.root = flash_directory;
            while (info.root.size() > 1 && info.root.back() == '/') info.root.pop_back();
        } else {
            info.error = flash_directory.empty() ? "no storage directory given" : flash_directory + " is not a directory";
        }
#endif
        if (!card_error.empty() && info.medium == StorageMedium::None) info.error = card_error + "; " + info.error;
    }
#if defined(KUBE32_TARGET_ESP32)
    info.internal_ram_bytes = static_cast<int64_t>(ram0) - static_cast<int64_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    info.psram_bytes = static_cast<int64_t>(psram0) - static_cast<int64_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
    std::lock_guard<std::mutex> lock(s_mutex);
    s_info = info;
    refresh_space_locked();
    return s_info.medium != StorageMedium::None;
}

NodeStorageInfo node_storage_info() {
    std::lock_guard<std::mutex> lock(s_mutex);
    follow_card_locked();
    refresh_space_locked();
    return s_info;
}

std::string node_storage_root() {
    std::lock_guard<std::mutex> lock(s_mutex);
    follow_card_locked();
    return s_info.root;
}

std::string node_storage_path(const std::string& relative) {
    std::string root = node_storage_root();
    if (root.empty()) return std::string();
    return root + "/kube32" + (relative.empty() || relative[0] == '/' ? relative : "/" + relative);
}

void node_storage_call(const std::function<void()>& fn) {
#if defined(KUBE32_TARGET_ESP32)
    if (s_storage_task && xTaskGetCurrentTaskHandle() != s_storage_task &&
        esp_ptr_external_ram(reinterpret_cast<void*>(esp_cpu_get_sp()))) {
        std::lock_guard<std::mutex> lock(s_call_mutex);
        s_call = &fn;
        s_call_error = nullptr;
        xSemaphoreGive(s_call_ready);
        xSemaphoreTake(s_call_done, portMAX_DELAY);
        s_call = nullptr;
        if (s_call_error) std::rethrow_exception(std::exchange(s_call_error, nullptr));
        return;
    }
#endif
    fn();
}

bool node_storage_make_dirs(const std::string& path) {
    bool ok = true;
    node_storage_call([&]() {
        for (size_t pos = 1; pos <= path.size(); ++pos) {
            if (pos != path.size() && path[pos] != '/') continue;
            std::string dir = path.substr(0, pos);
            if (::mkdir(dir.c_str(), 0777) != 0 && errno != EEXIST) {
                struct stat st;
                if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) { ok = false; return; }
            }
        }
    });
    return ok;
}

} // namespace kube32::hal
