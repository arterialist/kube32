// What a pod takes from the pod pool of a board without PSRAM: an operand
// stack sized from the module's call chains, no room for the compiler's
// scratch (lent from the shared interpreter stack while a module loads), and
// the kubelet's estimate against what a load really takes.

#include "pod_test_util.hpp"
#include "runtime/pod_memory_pool.h"
#include "runtime/container_lifecycle.hpp"
#include "m3_env.h"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace kube32;

// Six functions, each with 16 i64 locals, calling the next: "top" returns
// 42 + 5. Its stack is the chain of all six frames.
static const uint8_t kChainModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f, 0x03,
    0x07, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x07, 0x01,
    0x03, 0x74, 0x6f, 0x70, 0x00, 0x00, 0x0a, 0x3a, 0x06, 0x09, 0x01, 0x10, 0x7e, 0x10, 0x01, 0x41,
    0x01, 0x6a, 0x0b, 0x09, 0x01, 0x10, 0x7e, 0x10, 0x02, 0x41, 0x01, 0x6a, 0x0b, 0x09, 0x01, 0x10,
    0x7e, 0x10, 0x03, 0x41, 0x01, 0x6a, 0x0b, 0x09, 0x01, 0x10, 0x7e, 0x10, 0x04, 0x41, 0x01, 0x6a,
    0x0b, 0x09, 0x01, 0x10, 0x7e, 0x10, 0x05, 0x41, 0x01, 0x6a, 0x0b, 0x06, 0x01, 0x10, 0x7e, 0x41,
    0x2a, 0x0b,
};

// "top" calls, through the table, a function of another type with 64 i64
// locals: (i32) -> i32 adding 35 to its argument 7
static const uint8_t kIndirectModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x02, 0x60, 0x00, 0x01, 0x7f, 0x60,
    0x01, 0x7f, 0x01, 0x7f, 0x03, 0x03, 0x02, 0x00, 0x01, 0x04, 0x04, 0x01, 0x70, 0x00, 0x01, 0x05,
    0x03, 0x01, 0x00, 0x01, 0x07, 0x07, 0x01, 0x03, 0x74, 0x6f, 0x70, 0x00, 0x00, 0x09, 0x07, 0x01,
    0x00, 0x41, 0x00, 0x0b, 0x01, 0x01, 0x0a, 0x15, 0x02, 0x09, 0x00, 0x41, 0x07, 0x41, 0x00, 0x11,
    0x01, 0x00, 0x0b, 0x09, 0x01, 0x40, 0x7e, 0x20, 0x00, 0x41, 0x23, 0x6a, 0x0b,
};

// "rec": (func (param i32) (result i32) local.get 0 i32.const 1 i32.add call 0)
static const uint8_t kRecursiveModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x05, 0x03, 0x01, 0x00, 0x01,
    0x07, 0x07, 0x01, 0x03, 'r', 'e', 'c', 0x00, 0x00,
    0x0a, 0x0b, 0x01, 0x09, 0x00, 0x20, 0x00, 0x41, 0x01, 0x6a, 0x10, 0x00, 0x0b,
};

// (memory 1) and an exported function "size" returning memory.size
static const uint8_t kTinyModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x05, 0x03, 0x01, 0x00, 0x01,
    0x07, 0x08, 0x01, 0x04, 's', 'i', 'z', 'e', 0x00, 0x00,
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x3f, 0x00, 0x0b,
};

// The kubelet's spec for a pod that does not set kube32.io/wasm-stack
static runtime::DeploySpec unset_stack(uint32_t memory) {
    runtime::DeploySpec spec;
    spec.memory_limit_bytes = memory;
    spec.stack_size_bytes = 16 * 1024;
    spec.stack_size_set = false;
    return spec;
}

int main() {
    std::cout << "=== pod pool fit ===" << std::endl;
    kube32_pool_reserve(80 * 1024);
    const size_t block = kube32_pool_block_size();
    auto rt = runtime::create_wasm_runtime();
    std::string err;

    // The operand stack is the heaviest call chain's frames, the default
    // (4 KiB) at most; a chain that recurses has no bound and keeps it
    {
        assert(rt->deploy_workload_ex("default/chain", kChainModule, sizeof(kChainModule), unset_stack(1024), err));
        const uint64_t chain = rt->get_status("default/chain").stack_bytes;
        std::cout << "[stack] call chain of six frames: " << chain << " bytes" << std::endl;
        assert(chain >= 6 * 16 * 2 * 4 && chain < 4096);
        assert(rt->execute_function("default/chain", "top") == 47);

        // The same chain on a stack the pod sets smaller: the frames do not fit
        runtime::DeploySpec small = unset_stack(1024);
        small.stack_size_bytes = 512;
        small.stack_size_set = true;
        assert(rt->deploy_workload_ex("default/chain-512", kChainModule, sizeof(kChainModule), small, err));
        assert(rt->get_status("default/chain-512").stack_bytes == 512);
        assert(rt->execute_function("default/chain-512", "top") == -139);

        // An indirect call may reach any function of its type
        assert(rt->deploy_workload_ex("default/indirect", kIndirectModule, sizeof(kIndirectModule), unset_stack(1024), err));
        const uint64_t indirect = rt->get_status("default/indirect").stack_bytes;
        std::cout << "[stack] indirect call into a frame of 64 i64 locals: " << indirect << " bytes" << std::endl;
        assert(indirect >= 64 * 2 * 4 && indirect < 4096);
        assert(rt->execute_function("default/indirect", "top") == 42);

        assert(rt->deploy_workload_ex("default/rec", kRecursiveModule, sizeof(kRecursiveModule), unset_stack(1024), err));
        assert(rt->get_status("default/rec").stack_bytes == 4096);

        for (const char* id : {"default/chain", "default/chain-512", "default/indirect", "default/rec"}) assert(rt->terminate(id));
        assert(kube32_pool_available(nullptr) == block);
    }

    // The compiler's scratch (about 6 KB on the board) is lent from the
    // shared interpreter stack: a module loads into a pool cut into pieces
    // too small for it, which it could not without the loan
    {
        std::vector<uint8_t> interp(28 * 1024);
        kube32_pool_set_heap_budget(kube32_pool_system_reserve());   // pods get no general heap
        std::vector<void*> pieces;
        while (void* p = kube32_pool_sub_alloc(5000)) pieces.push_back(p);
        for (size_t i = 0; i < pieces.size(); i += 2) kube32_rt_free(pieces[i]);
        assert(m3_CompileScratchSize() > kube32_pool_largest_free());
        std::cout << "[scratch] pool in pieces of " << kube32_pool_largest_free() << " bytes, " << kube32_pool_available(nullptr)
                  << " free; the scratch is " << m3_CompileScratchSize() << " bytes" << std::endl;

        assert(!runtime::compile_scratch_lent());
        assert(!rt->deploy_workload_ex("default/unlent", kTinyModule, sizeof(kTinyModule), unset_stack(1024), err));
        std::cout << "[scratch] without the loan: " << err << std::endl;
        assert(err.find("allocation failed") != std::string::npos);

        kube32_interp_stack_set(interp.data(), interp.size());
        assert(runtime::compile_scratch_lent());
        assert(rt->deploy_workload_ex("default/lent", kTinyModule, sizeof(kTinyModule), unset_stack(1024), err));
        assert(rt->execute_function("default/lent", "size") == 1);
        assert(rt->terminate("default/lent"));

        for (size_t i = 1; i < pieces.size(); i += 2) kube32_rt_free(pieces[i]);
        assert(kube32_pool_available(nullptr) == block);

        // In pieces too small for a 4 KiB code page, the pages are smaller
        // and the module still loads in the block
        pieces.clear();
        while (void* p = kube32_pool_sub_alloc(2500)) pieces.push_back(p);
        for (size_t i = 0; i < pieces.size(); i += 2) kube32_rt_free(pieces[i]);
        assert(kube32_pool_largest_free() < d_m3CodePageAlignSize);
        assert(rt->deploy_workload_ex("default/small-pages", kChainModule, sizeof(kChainModule), unset_stack(1024), err));
        assert(rt->execute_function("default/small-pages", "top") == 47);
        assert(kube32_pool_general_bytes() == 0);
        std::cout << "[pages] module loaded into pieces of " << kube32_pool_largest_free() << " bytes" << std::endl;
        assert(rt->terminate("default/small-pages"));
        for (size_t i = 1; i < pieces.size(); i += 2) kube32_rt_free(pieces[i]);

        kube32_interp_stack_set(nullptr, 0);
        kube32_pool_set_heap_budget(0);
        assert(kube32_pool_available(nullptr) == block);
    }

    // The estimate the kubelet admits a pod on against what loading the pod
    // takes from the pool (with the interpreter stack lending the scratch,
    // as on the board): close, and not below it
    {
        std::vector<uint8_t> interp(28 * 1024);
        kube32_interp_stack_set(interp.data(), interp.size());
        struct Image { const char* name; uint32_t memory; } images[] = {
            {"tessera-db", 8 * 1024}, {"tessera-hud", 1024}, {"rfid", 4 * 1024}, {"irremote", 12 * 1024},
        };
        for (const Image& img : images) {
            const std::vector<uint8_t> wasm = k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/images/" + img.name + ".wasm");
            assert(!wasm.empty());
            runtime::ContainerConfig cfg;
            cfg.pod_id = std::string("default/") + img.name;
            cfg.memory_limit_bytes = img.memory;
            cfg.stack_size_bytes = 4096;
            cfg.stack_from_module = true;
            const size_t estimate = runtime::estimate_pod_memory(wasm.data(), wasm.size(), img.memory, 4096, true, runtime::EstimateSizes::ThisBuild);
            size_t taken = 0;
            {
                runtime::ContainerLifecycleController pod(cfg);
                const size_t before = kube32_pool_available(nullptr);
                assert(pod.deploy(wasm.data(), wasm.size()));
                taken = before - kube32_pool_available(nullptr) + kube32_pool_general_bytes();
#if d_m3GuardedMemory
                taken += img.memory;   // the host backs linear memory outside the pool
#endif
            }
            std::cout << "[estimate] " << img.name << ": estimate " << estimate << " bytes, the load took " << taken << std::endl;
            assert(estimate >= taken && estimate <= taken + taken / 5);
            assert(kube32_pool_available(nullptr) == block);
        }
        kube32_interp_stack_set(nullptr, 0);
    }

    std::cout << "[PASS] pod pool fit" << std::endl;
    return 0;
}
