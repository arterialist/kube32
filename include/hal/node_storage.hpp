#pragma once

// The node's storage root: one filesystem per board.
//
//   microSD card    when the board profile has a card slot and a FAT card
//                   answers (hal/sd_card.hpp), mounted at /sd
//   internal flash  otherwise: the "storage" data partition, FAT with wear
//                   levelling, mounted at /data (formatted on first use)
//
// Only one of the two is mounted, so a board pays for one FAT volume. On the
// host simulator the card is the KUBE32_SD_ROOT directory and the internal
// flash a directory given by the simulator (its --data-dir).
//
// Layout under the root:
//   kube32/store/journal.img   object store journal (control plane)
//   kube32/volumes/<claim>/    pod volumes (persistentVolumeClaim, kube32.io/sd)
//   kube32/logs/               container logs
//   kube32/images/             wasm://sd/<name> images (card only)
//
// Internal flash access disables the instruction cache on the ESP32-S3, which
// a task whose stack is in PSRAM must not do. Every file operation on the root
// goes through node_storage_call(), which moves it onto a stack in internal
// RAM when it has to.

#include "mem/function_ref.hpp"

#include <cstdint>
#include <string>

namespace kube32::hal {

enum class StorageMedium { None, SdCard, InternalFlash };

struct NodeStorageInfo {
    StorageMedium medium{StorageMedium::None};
    std::string root;              // mount point, empty without storage
    uint64_t total_bytes{0};
    uint64_t free_bytes{0};        // measured at the query
    int64_t internal_ram_bytes{0}; // internal RAM the mount took (board only)
    int64_t psram_bytes{0};        // PSRAM the mount took (board only)
    std::string error;             // why nothing is mounted
};

// Mounts the root: the card when `has_card_slot` and a card is present,
// otherwise internal flash (`flash_directory` on the host). Idempotent.
bool node_storage_mount(bool has_card_slot, const std::string& flash_directory = std::string());

NodeStorageInfo node_storage_info();
const char* storage_medium_name(StorageMedium medium);

// "" when nothing is mounted.
std::string node_storage_root();
// <root>/kube32/<relative>, "" when nothing is mounted.
std::string node_storage_path(const std::string& relative);

// Runs `fn` where file operations on the root are safe (see above) and
// returns when it has finished. Calls are serialized only when they must
// switch stacks. `fn` must not throw. It is invoked in place, so a call
// takes nothing from the heap.
void node_storage_call(mem::FunctionRef<void()> fn);

// mkdir -p, inside node_storage_call.
bool node_storage_make_dirs(const std::string& path);

} // namespace kube32::hal
