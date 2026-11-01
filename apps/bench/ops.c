/* The bench kernels with no imports, for tools/bench_ops.cpp: run(kernel, n)
 * returns the kernel's checksum. Built with the bench pod's flags, so the
 * kernels compile to the same Wasm. */
#include "../../include/bench/bench_kernels.h"

static uint32_t work[BENCH_WORK_BYTES / 4];

__attribute__((export_name("run"))) uint32_t run(int kernel, uint32_t n) { return bench_kernel_run(kernel, n, work); }
