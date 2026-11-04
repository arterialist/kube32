// The wasm3 operations one iteration of the interpreter speed kernel
// (runtime/interp_kernel.hpp) executes, with the interpreter built as the
// boards build it: kube32.io/interp-mops divides them by the time it takes.
// Then the 64-bit check of kube32.io/interp-check with the same interpreter.
#include "runtime/interp_kernel.hpp"

#include "wasm3.h"
#include "m3_env.h"

#include <cassert>
#include <cstdio>

extern "C" u64 m3_opCount;

int main() {
    using namespace kube32::runtime;
    IM3Environment env = m3_NewEnvironment();
    IM3Runtime rt = m3_NewRuntime(env, 1024, nullptr);
    IM3Module mod = nullptr;
    IM3Function fn = nullptr;
    M3Result r = m3_ParseModule(env, &mod, kInterpKernel, sizeof(kInterpKernel));
    if (!r) r = m3_LoadModule(rt, mod);
    if (!r) r = m3_FindFunction(&fn, rt, "run");
    assert(!r);
    assert(!m3_CallV(fn, 10));   // compiles
    auto ops = [&](uint32_t n) {
        const u64 before = m3_opCount;
        assert(!m3_CallV(fn, n));
        return m3_opCount - before;
    };
    const u64 a = ops(1000), b = ops(3000);
    assert((b - a) % 2000 == 0);
    const u64 per_iteration = (b - a) / 2000;
    std::printf("interp kernel: %llu operations per iteration (%llu for 1000 iterations)\n", (unsigned long long)per_iteration,
                (unsigned long long)a);
    assert(per_iteration == kInterpKernelOpsPerIteration);
    m3_FreeRuntime(rt);

    // The 64-bit check a node runs at boot (kube32.io/interp-check), with the
    // boards' dispatch; its host call returns a fixed value
    rt = m3_NewRuntime(env, 1024, nullptr);
    IM3Function check = nullptr, lo = nullptr, hi = nullptr;
    r = m3_ParseModule(env, &mod, kInterpCheck, sizeof(kInterpCheck));
    if (!r) r = m3_LoadModule(rt, mod);
    if (!r) r = m3_LinkRawFunction(mod, "env", "k32_time_ms", "I()", [](IM3Runtime, IM3ImportContext, uint64_t* sp, void*) -> const void* {
        *reinterpret_cast<uint64_t*>(sp) = 0xfedcba9876543210ull;
        return m3Err_none;
    });
    if (!r) r = m3_FindFunction(&check, rt, "check");
    if (!r) r = m3_FindFunction(&lo, rt, "value_lo");
    if (!r) r = m3_FindFunction(&hi, rt, "value_hi");
    assert(!r);
    int32_t step = -1;
    uint32_t vlo = 0, vhi = 0;
    assert(!m3_CallV(check, 16) && !m3_GetResultsV(check, &step));
    if (step > 0) std::printf("interp check: %s failed\n", kInterpCheckSteps[step - 1]);
    assert(step == 0);
    assert(!m3_CallV(lo) && !m3_GetResultsV(lo, &vlo) && !m3_CallV(hi) && !m3_GetResultsV(hi, &vhi));
    assert(vhi == 0xfedcba98u && vlo == 0x76543210u);
    std::printf("interp check: every step read its value\n");
    m3_FreeRuntime(rt);
    m3_FreeEnvironment(env);
    std::printf("PASS\n");
    return 0;
}
