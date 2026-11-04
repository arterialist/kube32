// kube32-podsize: what a module takes of a node, by the API server's own
// estimate (runtime/wasm_runtime.hpp estimate_pod_memory): the linear
// memory, and the overhead beyond it, with the module held in RAM and with
// it read in place from flash.
//
//   kube32-podsize <module.wasm> [--memory KiB] [--stack BYTES]
#include "runtime/wasm_runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: kube32-podsize <module.wasm> [--memory KiB] [--stack BYTES]\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> wasm((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (wasm.empty()) { std::fprintf(stderr, "%s: cannot read\n", argv[1]); return 1; }
    uint32_t memory_kib = 0, stack = 0;
    for (int i = 2; i < argc; i++) {
        if (!std::strcmp(argv[i], "--memory") && i + 1 < argc) memory_kib = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--stack") && i + 1 < argc) stack = static_cast<uint32_t>(std::atoi(argv[++i]));
    }
    using namespace kube32::runtime;
    for (int held = 1; held >= 0; held--) {
        size_t linear = 0;
        const size_t need = estimate_pod_memory(wasm.data(), wasm.size(), memory_kib * 1024, stack, stack == 0, EstimateSizes::BoardHeld, &linear, held != 0);
        const size_t overhead = need > linear ? need - linear : 0;
        std::printf("module %s in %s: linear memory %zu KiB, overhead %zu KiB (%zu B), admission %zu KiB\n", held ? "held in RAM" : "read from flash",
                    "pod memory", linear / 1024, (overhead + 1023) / 1024, overhead, (linear + overhead + 1023) / 1024);
    }
    return 0;
}
