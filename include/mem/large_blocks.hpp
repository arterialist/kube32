#pragma once

#include <cstddef>
#include <cstdint>

// The firmware's region for large C++ allocations. A control plane's heap
// holds thousands of small, long-lived blocks (objects, stacks, buffers);
// once they are spread over PSRAM, a request or controller pass that needs a
// 30 KB block finds none although 150 KB are free. The region is one PSRAM
// block set aside at boot: operator new places blocks over kLargeBlockMin
// bytes there first and in the heap when it has no room. Only large blocks
// share it, and most of them live for one request or pass, so it does not
// split into small pieces.
namespace kube32::mem {

constexpr size_t kLargeBlockMin = 4096;

// Sets `bytes` of PSRAM aside for the region (once, before the heap is used)
bool large_blocks_reserve(size_t bytes);

struct LargeBlockStats {
    size_t size{0};          // 0: no region
    size_t free{0};
    size_t largest{0};       // largest free block
    size_t min_free{0};
    uint32_t overflows{0};   // large allocations the region had no room for (placed in the heap)
};
LargeBlockStats large_blocks_stats();

} // namespace kube32::mem
