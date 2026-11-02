// What a pod takes from the pod pool of a board without PSRAM: an operand
// stack sized from the module's call chains, no room for the compiler's
// scratch (lent from the shared interpreter stack while a module loads), the
// kubelet's estimate against what a load really takes, loads that run out of
// memory part way, and a module read from the API server, which lives in
// the block on the pod's account.

#include "pod_test_util.hpp"
#include "runtime/pod_memory_pool.h"
#include "runtime/container_lifecycle.hpp"
#include "kubelet/kube_api_client.hpp"
#include "kubelet/kubelet.hpp"
#include "osal/sha256.hpp"
#include "osal/socket.hpp"
#include "m3_env.h"
#include "m3_host.h"

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <thread>

// The general heap as Wi-Fi leaves it on a board without PSRAM: while set,
// no piece of it holds more than this many bytes (0: any size)
static std::atomic<size_t> g_heap_largest{0};
void* operator new(size_t n) {
    const size_t largest = g_heap_largest.load();
    if (largest && n > largest) throw std::bad_alloc();
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

// Bytes of the block's small heap in use, chunk headers included
static size_t used_chunk_bytes() {
    size_t used = 0;
    kube32_pool_chunks([](size_t, size_t size, int in_use, void* ctx) { if (in_use) *static_cast<size_t*>(ctx) += size; }, &used);
    return used;
}

// An allocation of `bytes` in the block's small heap: 8-byte aligned, with its header
static size_t pool_chunk(size_t bytes) { return ((bytes + 7) & ~static_cast<size_t>(7)) + 8; }

// Whether the block's small heap has an allocation of exactly `size` bytes, header included
static bool has_chunk(size_t size) {
    struct Find { size_t size; bool found; } f{size, false};
    kube32_pool_chunks([](size_t, size_t sz, int in_use, void* ctx) {
        Find* f = static_cast<Find*>(ctx);
        if (in_use && sz == f->size) f->found = true;
    }, &f);
    return f.found;
}

// A kubelet's API server: the pods given, and the module of an inline image
// read over HTTP from a server that keeps it under its digest
class ImageStoreClient : public kubelet::KubeApiClient {
public:
    explicit ImageStoreClient(uint16_t port) : http_("127.0.0.1", port) {}
    std::vector<std::pair<models::KubePod, std::string>> pods;
    std::vector<std::string> status_patches;
    bool register_node(std::string_view, bool& c) override { c = false; return true; }
    bool patch_node_status(const std::string&, std::string_view) override { return true; }
    bool list_pods(std::vector<models::KubePod>&) override { return true; }
    bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out) override { out = pods; return true; }
    bool patch_pod_status(const std::string&, const std::string&, const std::string& patch) override {
        status_patches.push_back(patch);
        return true;
    }
    bool get_image(const std::string& sha256, runtime::ModuleBytes& out, std::string& why) override { return http_.get_image(sha256, out, why); }
    void add(const std::string& name, const std::string& digest, size_t size, const std::string& memory) {
        const std::string m = "{\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\",\"annotations\":{\"kube32.io/wasm-sha256\":\"" +
                              digest + "\",\"kube32.io/wasm-size\":\"" + std::to_string(size) + "\"}},\"spec\":{\"nodeName\":\"n1\",\"containers\":[{"
                              "\"name\":\"c\",\"image\":\"wasm://inline/" + name + "\",\"resources\":{\"limits\":{\"memory\":\"" + memory + "\"}}}]}}";
        pods.emplace_back(models::parse_pod_manifest(m), m);
    }
    size_t patches_with(const std::string& what) const {
        size_t n = 0;
        for (const auto& p : status_patches) if (p.find(what) != std::string::npos) ++n;
        return n;
    }

private:
    kubelet::HttpKubeClient http_;
};

// A module the kubelet reads from the API server's image store on a board
// without PSRAM: it goes into the pool block in one allocation, the general
// heap has no piece large enough for it and gives pods nothing, and once the
// pod runs the bytes are on its account. The kubelet admits the pod with the
// module counted.
static void test_pulled_module_in_the_block(size_t block) {
    const std::vector<uint8_t> wasm = k32test::svcprobe();
    const std::string digest = osal::Sha256::hex(osal::Sha256::of(wasm.data(), wasm.size()));
    std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: application/wasm\r\nContent-Length: " + std::to_string(wasm.size()) +
                        "\r\nConnection: close\r\n\r\n";
    reply.append(reinterpret_cast<const char*>(wasm.data()), wasm.size());
    const uint16_t port = 18790;
    osal::Socket listener;
    assert(listener.create(osal::SocketType::Tcp));
    listener.set_reuse_addr(true);
    assert(listener.bind("127.0.0.1", port) && listener.listen(4));
    std::atomic<bool> serving{true};
    std::thread server([&]() {
        for (;;) {
            osal::Socket c = listener.accept();
            if (!serving.load()) break;
            char head[1024];
            c.recv(head, sizeof head);
            c.send_all(reply.data(), reply.size());
            c.close();
        }
    });
    std::vector<uint8_t> interp(28 * 1024);
    kube32_interp_stack_set(interp.data(), interp.size());
    kube32_pool_set_heap_budget(kube32_pool_system_reserve());   // pods get none of the general heap
    g_heap_largest.store(wasm.size() - 1);                         // which has no piece for the module
    bool refused = false;
    try {
        std::vector<uint8_t> copy(wasm.begin(), wasm.end());
    } catch (const std::bad_alloc&) {
        refused = true;
    }
    assert(refused);   // one contiguous copy on the general heap finds no room

    // The download: one allocation of the module's size in the block,
    // nobody's until a pod takes it
    {
        kubelet::HttpKubeClient client("127.0.0.1", port);
        runtime::ModuleBytes got;
        std::string why;
        assert(client.get_image(digest, got, why));
        assert(got.size() == wasm.size() && std::memcmp(got.data(), wasm.data(), wasm.size()) == 0);
        assert(kube32_pool_general_bytes() == 0 && used_chunk_bytes() == pool_chunk(wasm.size()));
        assert(kube32_rt_owner(got.data()) == 0);
        const int pod = kube32_pod_account_open();
        got.charge_to(pod);
        assert(kube32_rt_owner(got.data()) == pod && kube32_pod_account_bytes(pod) == pool_chunk(wasm.size()));
        got.clear();
        assert(kube32_pod_account_bytes(pod) == 0 && kube32_pool_available(nullptr) == block);
        kube32_pod_account_close(pod);
        std::cout << "[pull] " << wasm.size() << " bytes read into the block, the general heap untouched" << std::endl;
    }

    // The kubelet: the pod with the image in the store, admitted with its
    // module counted, the module pulled into the block and kept by the
    // container on the pod's account
    {
        auto store = std::make_unique<ImageStoreClient>(port);
        store->add("pulled", digest, wasm.size(), "16Ki");
        ImageStoreClient* api = store.get();
        kubelet::KubeletConfig cfg;
        cfg.node_name = "n1";
        auto rt = runtime::create_wasm_runtime();
        {
            kubelet::Kubelet k(cfg, *rt, std::move(store));
            k.sync_assigned_pods();
            kubelet::LocalPodRecord rec;
            assert(k.get_pod_record("default/pulled", rec));
            if (rec.execution_phase != kubelet::PodExecutionPhase::Running) std::cout << "[pull] " << rec.last_error_message << std::endl;
            assert(rec.execution_phase == kubelet::PodExecutionPhase::Running);
            const runtime::ContainerStatus st = rt->get_status("default/pulled");
            assert(st.bytecode_bytes == wasm.size());
            // Everything the pod holds is in the block, the module's chunk
            // among it, and all of it is on the pod's account
            assert(kube32_pool_general_bytes() == 0 && has_chunk(pool_chunk(wasm.size())));
            size_t outside = 0;   // linear memory the host maps outside the pool
#if d_m3GuardedMemory
            const size_t page = m3_HostPageSize();
            outside = page + (16 * 1024 + page - 1) / page * page;
#endif
            std::cout << "[pull] the pod holds " << st.held_bytes - outside << " bytes of the block, its module's " << pool_chunk(wasm.size())
                      << " among them; " << kube32_pool_available(nullptr) << " free" << std::endl;
            assert(st.held_bytes == used_chunk_bytes() + outside);
            // ... which the estimate with the module's bytes covers, as for
            // the modules above (this build's structure sizes)
            const size_t taken = used_chunk_bytes() + (outside ? 16 * 1024 : 0);
            const size_t estimate = runtime::estimate_pod_memory(wasm.data(), wasm.size(), 16 * 1024, 4096, true, runtime::EstimateSizes::ThisBuild,
                                                                 nullptr, true);
            std::cout << "[pull] estimate " << estimate << " bytes, the pod took " << taken << std::endl;
            assert(estimate >= taken && estimate <= taken + taken / 5);
            api->pods.clear();
            k.sync_assigned_pods();
            assert(!rt->is_deployed("default/pulled"));
        }
        assert(kube32_pool_available(nullptr) == block);
    }

    // The block in pieces smaller than the module, with room enough in all
    // for the pod: it is admitted, the pull finds no piece for the module,
    // and the pod waits for memory as a load short of memory does (not
    // ErrImagePull); it starts once the block has a piece for it
    {
        std::vector<void*> pieces;
        while (void* p = kube32_pool_sub_alloc(1024)) pieces.push_back(p);
        for (size_t i = 0; i < pieces.size(); i += 2) kube32_rt_free(pieces[i]);
        assert(kube32_pool_largest_free() < wasm.size());
        const size_t free_before = kube32_pool_available(nullptr);
        auto store = std::make_unique<ImageStoreClient>(port);
        store->add("pulled-late", digest, wasm.size(), "8Ki");
        ImageStoreClient* api = store.get();
        kubelet::KubeletConfig cfg;
        cfg.node_name = "n1";
        cfg.memory_retry_ms = 0;
        auto rt = runtime::create_wasm_runtime();
        {
            kubelet::Kubelet k(cfg, *rt, std::move(store));
            k.sync_assigned_pods();
            kubelet::LocalPodRecord rec;
            assert(k.get_pod_record("default/pulled-late", rec) && rec.execution_phase == kubelet::PodExecutionPhase::InsufficientMemory);
            std::cout << "[pull] in pieces of " << kube32_pool_largest_free() << " bytes: " << rec.last_error_message << std::endl;
            assert(runtime::module_error_is_out_of_memory(rec.last_error_message) && rec.load_failures == 1 && !rt->is_deployed("default/pulled-late"));
            assert(api->patches_with("\"reason\":\"InsufficientMemory\"") == 1 && api->patches_with("ErrImagePull") == 0);
            assert(kube32_pool_available(nullptr) == free_before && kube32_pool_general_bytes() == 0);   // the failed pull left nothing
            for (size_t i = 1; i < pieces.size(); i += 2) kube32_rt_free(pieces[i]);
            k.sync_assigned_pods();
            assert(k.get_pod_record("default/pulled-late", rec) && rec.execution_phase == kubelet::PodExecutionPhase::Running);
            api->pods.clear();
            k.sync_assigned_pods();
        }
        assert(kube32_pool_available(nullptr) == block);
    }
    g_heap_largest.store(0);
    kube32_pool_set_heap_budget(0);
    kube32_interp_stack_set(nullptr, 0);
    serving.store(false);
    {
        osal::Socket poke;   // the server's last accept
        assert(poke.create(osal::SocketType::Tcp) && poke.connect("127.0.0.1", port, 1000));
        poke.close();
    }
    server.join();
    listener.close();
    std::cout << "[pull] the pods ran with their module in the block and on their account" << std::endl;
}

// A pod's account holds what was allocated for it until that is freed, on
// whichever thread; what a realloc returns stays with the owner of what it
// replaced, whoever grows it
static void test_accounts_follow_allocations(size_t block) {
    const int a = kube32_pod_account_open();
    const int b = kube32_pod_account_open();
    assert(a > 0 && b > 0 && a != b);
    void* small = nullptr;
    void* big = nullptr;
    const size_t general_before = kube32_pool_general_bytes();
    {
        runtime::PodMemoryScope charged(a);
        small = kube32_rt_calloc(100);               // the block's small heap
        kube32_pool_prefer_general(1);
        big = kube32_rt_calloc(5000);                // the general heap, after an 8-byte header
        kube32_pool_prefer_general(0);
    }
    assert(small && big && !kube32_pool_owns(small));
    assert(kube32_rt_owner(small) == a && kube32_rt_owner(big) == a);
    assert(kube32_pool_general_allocations() == 1);
    const size_t general = kube32_pool_general_bytes() - general_before;
    assert(general >= 5000 + 8 && used_chunk_bytes() == 8 + 104);
    assert(kube32_pod_account_bytes(a) == 112 + general && kube32_pod_account_bytes(b) == 0);

    // Grown on a thread charging another pod: still a's, and exact
    std::thread([&] {
        runtime::PodMemoryScope other(b);
        big = kube32_rt_realloc(big, 9000);
        small = kube32_rt_realloc(small, 2000);    // moved within the block
    }).join();
    assert(big && small && kube32_rt_owner(big) == a && kube32_rt_owner(small) == a);
    assert(kube32_pod_account_bytes(b) == 0);
    assert(kube32_pod_account_bytes(a) == used_chunk_bytes() + (kube32_pool_general_bytes() - general_before));

    // Freed on a thread of no pod: nothing is left on either account
    std::thread([&] {
        kube32_rt_free(big);
        kube32_rt_free(small);
    }).join();
    assert(kube32_pod_account_bytes(a) == 0 && kube32_pool_general_allocations() == 0 && kube32_pool_available(nullptr) == block);
    kube32_pod_account_close(a);
    kube32_pod_account_close(b);
    std::cout << "[accounts] owners kept across threads and reallocs" << std::endl;
}

int main() {
    std::cout << "=== pod pool fit ===" << std::endl;
    kube32_pool_reserve(80 * 1024);
    const size_t block = kube32_pool_block_size();
    auto rt = runtime::create_wasm_runtime();
    std::string err;
    test_accounts_follow_allocations(block);

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
    // as on the board): close, and not below it. The modules are not
    // embedded images: the pod keeps a copy of their bytes, which counts
    {
        std::vector<uint8_t> interp(28 * 1024);
        kube32_interp_stack_set(interp.data(), interp.size());
        struct Image { const char* name; uint32_t memory; } images[] = {
            {"tessera-db", 8 * 1024}, {"tessera-hud", 1024}, {"rfid", 4 * 1024}, {"irremote", 12 * 1024},
            {"driver-bme688", 3 * 1024}, {"driver-rfid", 3 * 1024}, {"driver-button", 3 * 1024},
        };
        for (const Image& img : images) {
            const std::vector<uint8_t> wasm = k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/images/" + img.name + ".wasm");
            assert(!wasm.empty());
            runtime::ContainerConfig cfg;
            cfg.pod_id = std::string("default/") + img.name;
            cfg.memory_limit_bytes = img.memory;
            cfg.stack_size_bytes = 4096;
            cfg.stack_from_module = true;
            const size_t estimate = runtime::estimate_pod_memory(wasm.data(), wasm.size(), img.memory, 4096, true, runtime::EstimateSizes::ThisBuild,
                                                                 nullptr, true);
            size_t taken = 0;
            {
                runtime::ContainerLifecycleController pod(cfg);
                const size_t before = kube32_pool_available(nullptr);
                assert(pod.deploy(wasm.data(), wasm.size()));
                taken = before - kube32_pool_available(nullptr) + kube32_pool_general_bytes();
                // The pod's account is exactly what it holds of the pool and
                // the heap, the module's copy included; its memory figure
                // adds, on the host, the linear memory mapped outside the pool
                const size_t account = kube32_pod_account_bytes(pod.memory_account());
                assert(account == used_chunk_bytes() + kube32_pool_general_bytes());
                assert(has_chunk(pool_chunk(wasm.size())) && pod.held_bytes() >= account);
                std::cout << "[account] " << img.name << ": " << account << " bytes held, " << pod.held_bytes() << " in all" << std::endl;
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

    // An embedded image is used in place: the same module deployed from the
    // image registry and from a copy differ by the copy, one chunk of the
    // block on the pod's account
    {
        std::vector<uint8_t> interp(28 * 1024);
        kube32_interp_stack_set(interp.data(), interp.size());
        static std::vector<std::vector<uint8_t>> embedded;   // the registry keeps pointers into them
        for (const char* name : {"tessera-db", "rfid", "driver-bme688"}) {
            embedded.push_back(k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/images/" + name + ".wasm"));
            const std::vector<uint8_t>& image = embedded.back();
            const std::string key = std::string("fit-") + name;
            runtime::ImageRegistry::instance().add({strdup(key.c_str()), image.data(), image.size()});
            const runtime::EmbeddedImage* mapped = runtime::ImageRegistry::instance().resolve("wasm://embedded/" + key);
            assert(mapped && mapped->data == image.data());
            const std::vector<uint8_t> copy = image;
            runtime::ContainerConfig cfg;
            cfg.pod_id = std::string("default/mapped-") + name;
            cfg.memory_limit_bytes = 4 * 1024;
            cfg.stack_size_bytes = 4096;
            cfg.stack_from_module = true;
            size_t in_place = 0, copied = 0;
            {
                runtime::ContainerLifecycleController pod(cfg);
                assert(pod.deploy(mapped->data, mapped->len));
                in_place = kube32_pod_account_bytes(pod.memory_account());
                assert(pod.get_status().bytecode_bytes == 0);
            }
            {
                runtime::ContainerLifecycleController pod(cfg);
                assert(pod.deploy(copy.data(), copy.size()));
                copied = kube32_pod_account_bytes(pod.memory_account());
                assert(pod.get_status().bytecode_bytes == copy.size());
            }
            std::cout << "[module] " << name << ": " << in_place << " bytes on the account in place, " << copied << " with a copy" << std::endl;
            assert(copied == in_place + pool_chunk(copy.size()));
            const size_t a = runtime::estimate_pod_memory(copy.data(), copy.size(), 4096, 4096, true, runtime::EstimateSizes::ThisBuild);
            const size_t b = runtime::estimate_pod_memory(copy.data(), copy.size(), 4096, 4096, true, runtime::EstimateSizes::ThisBuild, nullptr, true);
            assert(b == a + pool_chunk(copy.size()));
            assert(kube32_pool_available(nullptr) == block);
        }
        kube32_interp_stack_set(nullptr, 0);
    }

    test_pulled_module_in_the_block(block);

    // What the owner of an allocation costs: in the block it is kept in the
    // chunk header every allocation has; in the general heap (a pod whose
    // linear memory takes the block whole keeps its structures there) each
    // allocation carries an 8-byte header
    {
        std::vector<uint8_t> interp(28 * 1024);
        kube32_interp_stack_set(interp.data(), interp.size());
        struct Image { const char* name; uint32_t memory; } images[] = {{"echo", 64 * 1024}, {"driver-bme688", 3 * 1024}, {"tessera-hud", 1024}};
        for (const Image& img : images) {
            const std::vector<uint8_t> wasm = k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/images/" + img.name + ".wasm");
            runtime::ContainerConfig cfg;
            cfg.pod_id = std::string("default/cost-") + img.name;
            cfg.memory_limit_bytes = img.memory;
            cfg.stack_size_bytes = 4096;
            cfg.stack_from_module = true;
            runtime::ContainerLifecycleController pod(cfg);
            assert(pod.deploy(wasm.data(), wasm.size()));
            size_t chunks = 0;
            kube32_pool_chunks([](size_t, size_t, int in_use, void* ctx) { if (in_use) ++*static_cast<size_t*>(ctx); }, &chunks);
            const size_t general = kube32_pool_general_allocations();
            std::cout << "[owner] " << img.name << ": " << chunks << " allocations in the block (owner in the chunk header), " << general
                      << " in the general heap (" << general * 8 << " bytes of owner headers)" << std::endl;
            assert(kube32_pod_account_bytes(pod.memory_account()) == used_chunk_bytes() + kube32_pool_general_bytes());
        }
        assert(kube32_pool_general_allocations() == 0 && kube32_pool_available(nullptr) == block);
        kube32_interp_stack_set(nullptr, 0);
    }

    // A load short of memory at any point fails and gives back all it took:
    // each image with the pool block used up and the general heap budget
    // raised 1 KiB at a time until it loads
    {
        std::vector<uint8_t> interp(28 * 1024);
        kube32_interp_stack_set(interp.data(), interp.size());
        for (const char* name : {"tessera-db", "tessera-hud", "tessera-web", "tessera-api", "rfid", "irremote", "echo", "i2cscan", "driver-miniscale"}) {
            const std::vector<uint8_t> wasm = k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/images/" + name + ".wasm");
            assert(!wasm.empty());
            const uint32_t memory = std::strcmp(name, "tessera-api") == 0 ? 1280 * 1024 : 16 * 1024;
            int failed = 0;
            bool loaded = false;
            for (size_t extra = 0; !loaded && extra < 1024 * 1024; extra += 1024) {
                kube32_pool_set_heap_budget(kube32_pool_system_reserve() + extra);
                std::vector<void*> held;
                while (void* p = kube32_pool_sub_alloc(256)) held.push_back(p);
                {
                    runtime::ContainerConfig cfg;
                    cfg.pod_id = std::string("default/short-") + name;
                    cfg.memory_limit_bytes = memory;
                    cfg.stack_size_bytes = 4096;
                    cfg.stack_from_module = true;
                    runtime::ContainerLifecycleController pod(cfg);
                    loaded = pod.deploy(wasm.data(), wasm.size());
                    if (!loaded) ++failed;
                }
                assert(kube32_pool_general_bytes() == 0);
                for (void* p : held) kube32_rt_free(p);
                assert(kube32_pool_available(nullptr) == block);
            }
            std::cout << "[short] " << name << ": " << failed << " loads short of memory failed cleanly" << std::endl;
            assert(loaded && failed > 0);
        }
        kube32_pool_set_heap_budget(0);
        kube32_interp_stack_set(nullptr, 0);
    }

    std::cout << "[PASS] pod pool fit" << std::endl;
    return 0;
}
