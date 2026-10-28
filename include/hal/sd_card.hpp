#pragma once

// microSD card of the display board (M5Stack Cardputer).
//
// Firmware: the card sits on its own SPI bus (SPI3_HOST: MOSI 14, MISO 39,
// SCK 40, CS 12), separate from the LCD on SPI2_HOST. It is mounted with
// ESP-IDF's sdspi driver and FATFS at /sd, 20 MHz, at most 4 open files. The
// card must already carry a FAT filesystem; nothing is ever formatted here.
//
// Host simulator: /sd maps to the directory named by the KUBE32_SD_ROOT
// environment variable. Without that variable there is no card, exactly like
// a board without one. The figures come from statvfs() on that directory.

#include <cstdint>
#include <string>

namespace kube32::hal {

struct SdInfo {
    bool mounted{false};
    uint64_t total_bytes{0};     // filesystem size
    uint64_t free_bytes{0};      // free space, measured at the last query
    std::string name;            // product name from the card's CID ("SD32G"), or "host directory"
    std::string type;            // "SDHC/SDXC", "SDSC", "MMC", or "directory (simulated)"
    uint32_t bus_khz{0};         // real SPI clock after mount, 0 on the host
    std::string error;           // why the last mount failed, empty when mounted
};

// Mounts the card. Returns false (and leaves sd_info().error set) when there
// is no card, the SPI bus cannot be set up, or the filesystem is not FAT.
bool sd_mount();
void sd_unmount();
bool sd_is_mounted();
SdInfo sd_info();

// Mount point ("/sd" on the board, KUBE32_SD_ROOT on the host). Empty when
// not mounted.
std::string sd_root_path();

} // namespace kube32::hal
