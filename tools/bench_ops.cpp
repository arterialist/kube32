// bench_ops: the wasm3 operations each bench kernel executes, with the
// interpreter built as the boards build it (no tail calls, no preloaded next
// operation). Dividing a kernel's time on a board by its operations gives the
// interpreter's cycles per operation (docs/benchmarks.md).
//
//   make -C apps/bench ops && build/bench_ops apps/bench/build/ops.wasm
#include "wasm3.h"
#include "m3_env.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" u64 m3_opCount;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bench_ops <ops.wasm>\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    const std::vector<uint8_t> wasm((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (wasm.empty()) {
        std::fprintf(stderr, "bench_ops: cannot read %s\n", argv[1]);
        return 1;
    }
    // The sizes docs/benchmarks.md lists
    const char* const names[] = {"fib", "crc32", "sieve", "sort", "mandel", "json"};
    const uint32_t sizes[] = {15, 4, 4096, 1024, 32, 200};
    std::printf("%-7s %10s %10s %12s\n", "kernel", "n", "operations", "metered");
    uint64_t ops[2][6] = {};
    uint32_t sums[2][6] = {};
    for (int metered = 0; metered < 2; metered++) {
        IM3Environment env = m3_NewEnvironment();
        IM3Runtime rt = m3_NewRuntime(env, 64 * 1024, nullptr);
        // A gas limit set before the load emits the metering (as for a metered pod)
        if (metered) m3_SetResourceLimit(rt, c_m3Limit_GasUnits, INT64_MAX);
        IM3Module mod = nullptr;
        IM3Function fn = nullptr;
        M3Result r = m3_ParseModule(env, &mod, wasm.data(), static_cast<uint32_t>(wasm.size()));
        if (!r) r = m3_LoadModule(rt, mod);
        if (!r) r = m3_FindFunction(&fn, rt, "run");
        if (r) {
            std::fprintf(stderr, "bench_ops: %s\n", r);
            return 1;
        }
        for (int k = 0; k < 6; k++) {
            m3_CallV(fn, k, sizes[k]);   // compiles what the kernel reaches
            const uint64_t before = m3_opCount;
            r = m3_CallV(fn, k, sizes[k]);
            if (r) {
                std::fprintf(stderr, "bench_ops: %s: %s\n", names[k], r);
                return 1;
            }
            ops[metered][k] = m3_opCount - before;
            m3_GetResultsV(fn, &sums[metered][k]);
        }
        m3_FreeRuntime(rt);
        m3_FreeEnvironment(env);
    }
    for (int k = 0; k < 6; k++) {
        std::printf("%-7s %10u %10llu %12llu   sum %u\n", names[k], static_cast<unsigned>(sizes[k]), static_cast<unsigned long long>(ops[0][k]),
                    static_cast<unsigned long long>(ops[1][k]), static_cast<unsigned>(sums[0][k]));
    }
    return 0;
}
