/* Benchmark kernels shared by the firmware (native, K32_CMD:BENCH) and the
 * bench pod (Wasm, apps/bench): the same source on both sides, so a kernel's
 * two timings differ only by how it runs. Freestanding C, no libc. Each
 * kernel returns a checksum that both sides must agree on. */
#ifndef KUBE32_BENCH_KERNELS_H
#define KUBE32_BENCH_KERNELS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Index of `name` ("fib", "crc32", "sieve", "sort", "mandel", "json") or -1 */
int bench_kernel_index(const char* name, uint32_t len);
const char* bench_kernel_name(int index);
/* Bytes of workspace a kernel run needs: the caller provides it, so the
 * firmware holds no benchmark buffers between runs */
#define BENCH_WORK_BYTES 4096u
/* Runs kernel `index` with size `n` (clamped to the kernel's range) in
 * `work` (BENCH_WORK_BYTES, 4-byte aligned) */
uint32_t bench_kernel_run(int index, uint32_t n, void* work);

#ifdef __cplusplus
}
#endif

#endif
