// k32_log on a node out of memory: the line is lost and the pod keeps running.
// The tick that logs runs with every allocation of the test's thread failing;
// nothing may unwind through the interpreter's C frames.

#include "pod_test_util.hpp"
#include "runtime/pod_log.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <new>

static thread_local bool g_fail_new = false;
void* operator new(size_t n) {
    if (g_fail_new) throw std::bad_alloc();
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

using namespace kube32;
using runtime::AppPodHost;

static size_t log_lines(const std::string& pod_id) {
    std::vector<runtime::PodLogRecord> out;
    runtime::PodLogStore::runtime().read_after(pod_id, 0, out);
    return out.size();
}

int main() {
    const std::vector<uint8_t> wasm = k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/tests/data/corespin.wasm");
    assert(!wasm.empty() && "tests/data/corespin.wasm");
    runtime::DeploySpec spec;
    spec.env = {{"HOSTNAME", "corespin"}, {"SPIN_MS", "1"}};
    spec.gas_budget_per_call = 0;
    runtime::WasmRuntime rt;
    const std::string id = "default/logger";
    k32test::deploy(rt, id, wasm, 65536, "", spec);

    auto& host = AppPodHost::instance();
    uint32_t now = 1000;
    // corespin logs on every sixteenth tick
    for (int i = 0; i < 15; ++i) assert(host.tick(id, now += 1000) == 1);
    // Without its ring the next line has to allocate one
    runtime::PodLogStore::runtime().remove(id);

    g_fail_new = true;
    const int32_t rc = host.tick(id, now += 1000);
    g_fail_new = false;
    assert(rc == 1);
    assert(log_lines(id) == 0);

    for (int i = 0; i < 16; ++i) assert(host.tick(id, now += 1000) == 1);
    assert(log_lines(id) == 1);

    assert(rt.terminate(id));
    std::cout << "k32_log survives a failed allocation" << std::endl;
    return 0;
}
