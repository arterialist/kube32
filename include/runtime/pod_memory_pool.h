#pragma once
/*
 * One reserved block of internal RAM for the Wasm linear memory of a pod on
 * nodes without PSRAM. Reserved at boot before Wi-Fi fragments the heap, so a
 * 64 KiB page can still be carved out later. wasm3 asks here first for a large
 * linear memory. On a board without PSRAM every other pod allocation is
 * served from the block first, as a small heap of its own placed from the
 * block's end (kube32_pool_sub_alloc), and the general heap is used only when
 * the block is full; the block is whole again once those are freed.
 */
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void  kube32_pool_reserve(size_t bytes);           /* call once at boot; 0 disables (internal RAM) */
void  kube32_pool_reserve_external(size_t bytes);  /* same, from external PSRAM */
void* kube32_pool_alloc(size_t bytes);             /* NULL when unavailable or under half the block */
/* Boards without PSRAM also reserve one pod thread stack at boot, when a
 * contiguous block is still available; later the heap is too fragmented. */
void   kube32_pool_reserve_stack(size_t bytes);
void*  kube32_pool_stack_alloc(size_t bytes);      /* NULL when taken or too small */
int    kube32_pool_stack_free(void* ptr);
size_t kube32_pool_stack_size(void);
size_t kube32_pool_stack_available(void);          /* bytes free for scheduling purposes */
/* Boards without PSRAM run every host-initiated call into a pod (k32_init,
 * k32_handle, k32_tick, k32_event) on one shared interpreter stack, so no
 * task needs a stack sized for the interpreter (tens of KB on Xtensa, which
 * has no tail calls). Set once at boot; calls are serialized. */
void  kube32_interp_stack_set(void* stack, size_t bytes);
int   kube32_interp_stack_active(void);
void* kube32_interp_stack_get(size_t* out_bytes);
/* Base of the shared stack when `sp` lies in it, else NULL. */
void* kube32_interp_stack_base_for(const void* sp);
int   kube32_pool_owns(const void* ptr);
size_t kube32_pool_block_size(void);
int   kube32_pool_free(void* ptr);                 /* 1 when the pointer was pooled */
size_t kube32_pool_threshold(void);
/* Bytes the pool could still hand to pods (0 when one holds it whole, the
 * free part while small pods share it), and whether it is external RAM. */
size_t kube32_pool_available(int* out_external);
// The largest free piece of the pool block, and the size of the last
// allocation a pod was refused (both for load errors)
size_t kube32_pool_largest_free(void);
// Calls `each` for every chunk of the block's small heap, in address order
// (offset from the block, size with header, in use); returns the count
size_t kube32_pool_chunks(void (*each)(size_t offset, size_t size, int used, void* ctx), void* ctx);
size_t kube32_pool_last_refused(void);
/* The block as a heap for small allocations (NULL when a pod holds it whole
 * or nothing fits); zero-filled. kube32_pool_sub_free returns 1 when the
 * pointer was one of these. */
void* kube32_pool_sub_alloc(size_t bytes);
int   kube32_pool_sub_free(void* ptr);
/* Bytes the large memory at the block's start may grow to in place. */
size_t kube32_pool_whole_capacity(void);
/* Nesting counter: while above zero, pod allocations try the general heap
 * first (a pod loading whose linear memory wants the block whole). */
void  kube32_pool_prefer_general(int on);
/* Bytes pods hold in the general heap. Pods never take its last 16 KiB, or
 * 24 KiB with an internal pool block (kube32_pool_system_reserve): that much
 * internal RAM stays free for the kubelet, Wi-Fi and lwIP; with a budget set
 * (the host modelling a board's heap) pods may hold at most budget - reserve. */
size_t kube32_pool_general_bytes(void);
size_t kube32_pool_system_reserve(void);
/* PSRAM pods are never given (default 16 KiB, for thread stacks made later).
 * A control plane keeps more for the API server, the object store, the
 * console relay and firmware staging. Set at boot. */
void  kube32_pool_set_psram_reserve(size_t bytes);
size_t kube32_pool_psram_reserve(void);
void  kube32_pool_set_heap_budget(size_t bytes);
size_t kube32_pool_heap_budget(void);
/* Bytes a pod whose linear memory is `memory_bytes` could be given now: the
 * pool block's free part, the general heap above the system reserve (on
 * the board, not with an internal pool block) and PSRAM above its
 * reserve. *out_capacity (optional) adds what pods
 * hold already: what the pod could be given with no other pod running. */
size_t kube32_pool_pod_room(size_t memory_bytes, size_t* out_capacity);
/* General allocator for the interpreter: external RAM first when present,
 * internal otherwise. Keeps code pages and runtime structures out of the
 * 300 KB internal heap that Wi-Fi and lwIP need. */
void* kube32_rt_calloc(size_t bytes);
void* kube32_rt_realloc(void* ptr, size_t bytes);
void  kube32_rt_free(void* ptr);
/* Gives back all but the first `bytes` of an allocation without moving it;
 * 1 when it did (allocations from the pool block's small heap), else 0. */
int   kube32_rt_shrink(void* ptr, size_t bytes);
/* What to allocate in place of `bytes` (at least `minimum`): the pool block's
 * largest piece when pods are served from the block first and it has no
 * piece of `bytes` left, so a code page stays in the block, smaller. */
size_t kube32_rt_fit(size_t bytes, size_t minimum);
#ifdef __cplusplus
}
#endif
