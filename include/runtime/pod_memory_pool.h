#pragma once
/*
 * One block reserved at boot for pods: internal RAM on nodes without PSRAM
 * (before Wi-Fi fragments the heap, so a 64 KiB page can still be carved out
 * later), PSRAM on nodes with it. wasm3 asks here first for a large linear
 * memory. Every other pod allocation is served from the block first, as a
 * small heap of its own placed from the block's end (kube32_pool_sub_alloc);
 * the general heap (without PSRAM) or the PSRAM above its reserve (with it)
 * is used only when the block is full. The block is whole again once those
 * are freed.
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
/* Boards without PSRAM run every host-initiated call into a pod (k32_init,
 * k32_handle, k32_tick, k32_event) on one shared interpreter stack, so no
 * task needs a stack sized for the interpreter (tens of KB on Xtensa, which
 * has no tail calls). Set once at boot; calls are serialized. */
void  kube32_interp_stack_set(void* stack, size_t bytes);
int   kube32_interp_stack_active(void);
void* kube32_interp_stack_get(size_t* out_bytes);
/* Memory the node itself takes from the block's end at boot (the stack of
 * a pod worker that runs the interpreter): allocatable memory leaves it
 * out. NULL when the block has no room. */
void*  kube32_pool_system_alloc(size_t bytes);
size_t kube32_pool_system_bytes(void);
/* Stacks of threads that call pods on their own stack (pod workers with a
 * stack sized for the interpreter): a call made there runs where it is.
 * Up to four, added once at boot. */
void  kube32_interp_stack_add(void* stack, size_t bytes);
/* Base of the interpreter stack `sp` lies in (the shared one or an added
 * one), else NULL. */
void* kube32_interp_stack_base_for(const void* sp);
/* 1 when `sp` lies in the shared stack. */
int   kube32_interp_stack_is_shared(const void* sp);
int   kube32_pool_owns(const void* ptr);
size_t kube32_pool_block_size(void);
int   kube32_pool_free(void* ptr);                 /* 1 when the pointer was pooled */
size_t kube32_pool_threshold(void);
/* 1 when a linear memory of `memory_bytes` wants the block whole (at least
 * the threshold and half the block): its pod keeps its other allocations
 * out of the block while it loads (kube32_pool_prefer_general). */
int   kube32_pool_wants_block(size_t memory_bytes);
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
/* What the node keeps back for the system and never offers pods: the PSRAM
 * reserve on a board with PSRAM, the internal-RAM reserve otherwise. The
 * simulator takes the reserve it is given (kube32-sim --system-reserved), or
 * its default heap reserve. The node reports it as the
 * kube32.io/system-reserved annotation. */
size_t kube32_pool_reserved_for_system(void);
void  kube32_pool_set_heap_budget(size_t bytes);
size_t kube32_pool_heap_budget(void);
/* Bytes a pod could be given now: the pool block's free part, and PSRAM
 * above its reserve on a board with PSRAM, or the general heap above the
 * system reserve on a board with neither PSRAM nor a block. A board with
 * PSRAM never gives pods internal RAM. The simulator without a heap budget
 * gives pods the host's available memory. *out_capacity (optional) is what
 * pods can ever be given (kube32_pool_node_room's memory): a pod that needs
 * more never fits. `memory_bytes` is the pod's memory limit. */
size_t kube32_pool_pod_room(size_t memory_bytes, size_t* out_capacity);
/* What the Node offers pods (status.allocatable): what pods can be given
 * with no pod running. Pods starting and stopping do not change it; the
 * scheduler subtracts what the pods bound to the node request.
 *   *out_memory: memory for pods' modules, structures and linear memories.
 *     Without PSRAM the pool block. With PSRAM the block and the PSRAM
 *     above its reserve. The host models a board with a heap budget (the
 *     budget above the system reserve, and the block); without one, the
 *     host's available memory and the block.
 *   *out_psram: with PSRAM, the same figure (a pod's memory is all PSRAM
 *     there, and an esp32.io/psram request sizes its linear memory); the
 *     simulator's PSRAM stand-in without a budget is the block alone. 0
 *     without PSRAM.
 * What the system's own use leaves of PSRAM (and of the host's memory) is
 * measured once, the first time, with what pods hold then added back: the
 * reserve covers the system's growth after that. */
void kube32_pool_node_room(size_t* out_memory, size_t* out_psram);
/* The largest linear memory the node can ever place in one piece: the
 * block, or the PSRAM (or host memory) besides it when that is larger. */
size_t kube32_pool_linear_max(void);
/* Grows or shrinks, in place, the large memory at the block's start to
 * `bytes`; 1 when it fits there (it then counts as that size), 0 when it
 * must move. */
int   kube32_pool_resize(void* ptr, size_t bytes);
/* Per-pod accounts. Every allocation made through this pool (the
 * kube32_rt_* allocator, kube32_pool_alloc, kube32_pool_sub_alloc) is
 * charged to the calling thread's current account and stays charged to it
 * until it is freed, on whatever thread that happens; what a realloc returns
 * stays with the owner of what it replaced. Account 0 is nobody's. A pod
 * allocation in the block's small heap keeps its owner in the chunk header
 * it has anyway; one in the general heap or PSRAM carries an 8-byte header. */
int    kube32_pod_account_open(void);           /* a free account, 0 when none is left */
void   kube32_pod_account_close(int account);   /* reused once what it holds is freed */
size_t kube32_pod_account_bytes(int account);   /* bytes it holds, headers included */
int    kube32_pod_account_enter(int account);   /* the thread's current account; returns the one before */
void   kube32_pod_account_leave(int previous);
int    kube32_rt_owner(const void* ptr);        /* the account an allocation is charged to */
/* Charges an allocation to `account` from now on, and no longer to the one
 * it was charged to (a module's bytes, read before its pod's account opened) */
void   kube32_rt_set_owner(void* ptr, int account);
/* Pod allocations in the general heap or PSRAM (each with its header) */
size_t kube32_pool_general_allocations(void);
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
