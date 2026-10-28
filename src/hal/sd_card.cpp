#include "hal/sd_card.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

namespace kube32::hal {

static std::mutex s_sd_mutex;
static SdInfo s_info;

#if defined(KUBE32_TARGET_ESP32)

} // namespace kube32::hal

#include "esp_vfs_fat.h"
#include "esp_err.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_defs.h"

namespace kube32::hal {

// Cardputer microSD slot: its own bus, nothing shared with the LCD (SPI2_HOST).
static constexpr int PIN_SD_MOSI = 14;
static constexpr int PIN_SD_MISO = 39;
static constexpr int PIN_SD_SCK  = 40;
static constexpr int PIN_SD_CS   = 12;
static constexpr spi_host_device_t SD_SPI_HOST = SPI3_HOST;
static constexpr const char* SD_MOUNT_POINT = "/sd";

static sdmmc_card_t* s_card = nullptr;
static bool s_bus_ready = false;

static void refresh_space_locked() {
    uint64_t total = 0, free_b = 0;
    if (s_card && esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_b) == ESP_OK) {
        s_info.total_bytes = total;
        s_info.free_bytes = free_b;
    }
}

bool sd_mount() {
    std::lock_guard<std::mutex> lock(s_sd_mutex);
    if (s_card) return true;
    s_info = SdInfo{};

    if (!s_bus_ready) {
        spi_bus_config_t bus = {};
        bus.mosi_io_num = PIN_SD_MOSI;
        bus.miso_io_num = PIN_SD_MISO;
        bus.sclk_io_num = PIN_SD_SCK;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = 4096;   // one FAT cluster per DMA transaction is plenty
        esp_err_t r = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
        if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
            s_info.error = std::string("spi_bus_initialize(SPI3): ") + esp_err_to_name(r);
            return false;
        }
        s_bus_ready = true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_SPI_HOST;
    slot.gpio_cs = static_cast<gpio_num_t>(PIN_SD_CS);

    esp_vfs_fat_mount_config_t mount = {};
    mount.format_if_mount_failed = false;   // the user formats the card; we never do
    mount.max_files = 4;
    mount.allocation_unit_size = 0;
    mount.disk_status_check_enable = false;

    esp_err_t r = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (r != ESP_OK) {
        s_card = nullptr;
        if (r == ESP_FAIL) s_info.error = "card found but no FAT filesystem on it (not formatting; format it FAT32 on a PC)";
        else if (r == ESP_ERR_TIMEOUT || r == ESP_ERR_NOT_FOUND || r == ESP_ERR_INVALID_RESPONSE) s_info.error = std::string("no card answered on SPI3 (") + esp_err_to_name(r) + ")";
        else s_info.error = std::string("mount failed: ") + esp_err_to_name(r);
        return false;
    }

    char name[sizeof(s_card->cid.name) + 1] = {0};
    std::memcpy(name, s_card->cid.name, sizeof(s_card->cid.name));
    for (int i = static_cast<int>(sizeof(s_card->cid.name)) - 1; i >= 0 && (name[i] == ' ' || name[i] == 0); --i) name[i] = 0;
    s_info.name = name;
    s_info.type = s_card->is_mmc ? "MMC" : ((s_card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC");
    s_info.bus_khz = static_cast<uint32_t>(s_card->real_freq_khz);
    s_info.mounted = true;
    refresh_space_locked();
    return true;
}

void sd_unmount() {
    std::lock_guard<std::mutex> lock(s_sd_mutex);
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = nullptr;
    s_info = SdInfo{};
}

bool sd_is_mounted() {
    std::lock_guard<std::mutex> lock(s_sd_mutex);
    return s_card != nullptr;
}

SdInfo sd_info() {
    std::lock_guard<std::mutex> lock(s_sd_mutex);
    refresh_space_locked();
    return s_info;
}

std::string sd_root_path() {
    std::lock_guard<std::mutex> lock(s_sd_mutex);
    return s_card ? std::string(SD_MOUNT_POINT) : std::string();
}

#else // host simulator

} // namespace kube32::hal

#include <cstdlib>
#include <sys/stat.h>
#include <sys/statvfs.h>

namespace kube32::hal {

static std::string s_root;

static void refresh_space_locked() {
    struct statvfs vfs;
    if (!s_root.empty() && statvfs(s_root.c_str(), &vfs) == 0) {
        s_info.total_bytes = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
        s_info.free_bytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    }
}

bool sd_mount() {
    std::lock_guard<std::mutex> lock(s_sd_mutex);
    if (!s_root.empty()) return true;
    s_info = SdInfo{};
    const char* root = std::getenv("KUBE32_SD_ROOT");
    if (!root || !*root) {
        s_info.error = "KUBE32_SD_ROOT is not set: this host node has no (simulated) card";
        return false;
    }
    struct stat st;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        s_info.error = std::string("KUBE32_SD_ROOT=") + root + " is not a directory";
        return false;
    }
    s_root = root;
    while (s_root.size() > 1 && s_root.back() == '/') s_root.pop_back();
    s_info.name = "host directory";
    s_info.type = "directory (simulated)";
    s_info.mounted = true;
    refresh_space_locked();
    return true;
}

void sd_unmount() {
    std::lock_guard<std::mutex> lock(s_sd_mutex);
    s_root.clear();
    s_info = SdInfo{};
}

bool sd_is_mounted() {
    std::lock_guard<std::mutex> lock(s_sd_mutex);
    return !s_root.empty();
}

SdInfo sd_info() {
    std::lock_guard<std::mutex> lock(s_sd_mutex);
    refresh_space_locked();
    return s_info;
}

std::string sd_root_path() {
    std::lock_guard<std::mutex> lock(s_sd_mutex);
    return s_root;
}

#endif

} // namespace kube32::hal
