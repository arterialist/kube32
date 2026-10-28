#pragma once
/*
 * One reserved block of internal RAM for the Wasm linear memory of a pod on
 * nodes without PSRAM. Reserved at boot before Wi-Fi fragments the heap, so a
 * 64 KiB page can still be carved out later. wasm3's allocator asks here first
 * for large requests; everything else goes to calloc/free.
 */
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void  kube32_pool_reserve(size_t bytes);           /* call once at boot; 0 disables (internal RAM) */
void  kube32_pool_reserve_external(size_t bytes);  /* same, from external PSRAM */
void* kube32_pool_alloc(size_t bytes);             /* NULL when unavailable */
/* Boards without PSRAM also reserve one pod thread stack at boot, when a
 * contiguous block is still available; later the heap is too fragmented. */
void   kube32_pool_reserve_stack(size_t bytes);
void*  kube32_pool_stack_alloc(size_t bytes);      /* NULL when taken or too small */
int    kube32_pool_stack_free(void* ptr);
size_t kube32_pool_stack_size(void);
size_t kube32_pool_stack_available(void);          /* bytes free for scheduling purposes */
int   kube32_pool_owns(const void* ptr);
size_t kube32_pool_block_size(void);
int   kube32_pool_free(void* ptr);                 /* 1 when the pointer was pooled */
size_t kube32_pool_threshold(void);
/* Bytes the pool could still hand to a pod (0 when in use), and whether it is external RAM. */
size_t kube32_pool_available(int* out_external);
/* General allocator for the interpreter: external RAM first when present,
 * internal otherwise. Keeps code pages and runtime structures out of the
 * 300 KB internal heap that Wi-Fi and lwIP need. */
void* kube32_rt_calloc(size_t bytes);
void* kube32_rt_realloc(void* ptr, size_t bytes);
void  kube32_rt_free(void* ptr);
#ifdef __cplusplus
}
#endif
