// Memory limits below one 64 KiB Wasm page: the page is backed by the limit
// only (wasm3 memoryLimit), accesses past it trap, memory.grow fails, and
// such pods come from the general heap rather than the pod pool's block.

#include "pod_test_util.hpp"
#include "runtime/pod_memory_pool.h"
#include "kubelet/kubelet.hpp"
#include "m3_core.h"
#include "m3_env.h"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace kube32;
using k32test::call_pod;

// (memory 1) and an exported function "size" returning memory.size: no data,
// so it instantiates under any limit
static const uint8_t kTinyModule[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x05, 0x03, 0x01, 0x00, 0x01,
    0x07, 0x08, 0x01, 0x04, 's', 'i', 'z', 'e', 0x00, 0x00,
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x3f, 0x00, 0x0b,
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

static int json_int(const std::string& body, const std::string& key) {
    size_t at = body.find("\"" + key + "\":");
    assert(at != std::string::npos);
    return std::atoi(body.c_str() + at + key.size() + 3);
}

static std::string base64(const std::vector<uint8_t>& in) {
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t n = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += a[n >> 18]; out += a[(n >> 12) & 63]; out += a[(n >> 6) & 63]; out += a[n & 63];
    }
    if (i < in.size()) {
        uint32_t n = in[i] << 16 | (i + 1 < in.size() ? in[i + 1] << 8 : 0);
        out += a[n >> 18]; out += a[(n >> 12) & 63];
        out += (i + 1 < in.size()) ? a[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

class ListClient : public kubelet::KubeApiClient {
public:
    std::vector<std::pair<models::KubePod, std::string>> pods;
    std::vector<std::string> status_patches, events;
    bool create_event(const std::string&, const std::string& ev) override { events.push_back(ev); return true; }
    bool register_node(const std::string&, bool& c) override { c = false; return true; }
    bool patch_node_status(const std::string&, std::string_view) override { return true; }
    bool list_pods(std::vector<models::KubePod>&) override { return true; }
    bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out) override { out = pods; return true; }
    bool patch_pod_status(const std::string&, const std::string& name, const std::string& patch) override {
        status_patches.push_back(name + " " + patch);
        return true;
    }
    size_t count(const std::vector<std::string>& in, const std::string& what) const {
        size_t n = 0;
        for (const auto& s : in) if (s.find(what) != std::string::npos) ++n;
        return n;
    }
    void remove(const std::string& name) {
        for (auto it = pods.begin(); it != pods.end(); ++it) if (it->first.name == name) { pods.erase(it); return; }
    }
    void add(const std::string& name, const std::string& resources, const std::string& image_b64, const std::string& stack = "") {
        std::string m = "{\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\",\"annotations\":{\"kube32.io/wasm-base64\":\"" + image_b64 +
                        "\"" + (stack.empty() ? std::string() : ",\"kube32.io/wasm-stack\":\"" + stack + "\"") + "}},\"spec\":{\"nodeName\":\"n1\",\"containers\":[{\"name\":\"c\",\"image\":\"wasm://inline/" + name + "\"" +
                        (resources.empty() ? std::string() : ",\"resources\":" + resources) + "}]}}";
        pods.emplace_back(models::parse_pod_manifest(m), m);
    }
};

int main() {
    std::cout << "=== sub-page memory limits ===" << std::endl;
    // The pod pool's block, as a board without PSRAM reserves it
    kube32_pool_reserve(67 * 1024);
    const size_t block = kube32_pool_block_size();
    assert(block == 67 * 1024 && kube32_pool_available(nullptr) == block);

    auto rt = runtime::create_wasm_runtime();
    const std::vector<uint8_t> probe = k32test::svcprobe();

    // Without PSRAM (the pool is internal) pods are served from the pool's
    // block first: an 8 KiB and a 1 KiB pod leave the general heap the
    // kubelet, Wi-Fi and lwIP live on untouched
    {
        const size_t general0 = kube32_pool_general_bytes();
        k32test::deploy(*rt, "default/eight", probe, 8 * 1024);
        k32test::deploy(*rt, "default/one", std::vector<uint8_t>(kTinyModule, kTinyModule + sizeof(kTinyModule)), 1024);
        const size_t pool_used = block - kube32_pool_available(nullptr);
        std::cout << "[pool first] 8Ki + 1Ki pods: " << pool_used << " bytes from the pool block, "
                  << kube32_pool_general_bytes() - general0 << " bytes from the general heap" << std::endl;
        assert(kube32_pool_general_bytes() == general0 && pool_used > 9 * 1024);
        assert(rt->get_status("default/one").memory_used_bytes == 1024);
        assert(rt->terminate("default/eight") && rt->terminate("default/one"));
        assert(kube32_pool_available(nullptr) == block && kube32_pool_general_bytes() == general0);
    }

    k32test::deploy(*rt, "default/small-a", probe, 16 * 1024);
    k32test::deploy(*rt, "default/small-b", probe, 12 * 1024);

    // Two small pods side by side, both in the pool block
    assert(kube32_pool_available(nullptr) < block - 28 * 1024);
    assert(rt->get_status("default/small-a").memory_used_bytes == 16 * 1024);
    assert(rt->get_status("default/small-b").memory_used_bytes == 12 * 1024);

    k32test::Collect c = call_pod("default/small-a", "/mem");
    std::cout << "[mem] " << c.body << std::endl;
    assert(json_int(c.body, "used") == 16384 && json_int(c.body, "limit") == 16384 && json_int(c.body, "pages") == 1);
    c = call_pod("default/small-b", "/hello");
    assert(c.status == 200 && c.body == "hello from small-b\n");

    // Inside the backing: fine; memory.grow past it: refused (-1)
    c = call_pod("default/small-a", "/touch?a=16383");
    assert(c.status == 200);
    c = call_pod("default/small-a", "/grow");
    std::cout << "[grow] " << c.body << std::endl;
    assert(json_int(c.body, "grow") == -1);
    c = call_pod("default/small-a", "/mem");
    assert(json_int(c.body, "pages") == 1 && json_int(c.body, "used") == 16384);
    // Past the backing: the access traps (the pod answers 500 and fails)
    c = call_pod("default/small-a", "/touch?a=16384");
    std::cout << "[touch past the limit] status " << c.status << std::endl;
    assert(c.status == 500);
    runtime::ContainerStatus st = rt->get_status("default/small-a");
    assert(st.phase == runtime::ContainerPhase::Failed && st.exit_code == 139);

    rt->terminate("default/small-a");
    rt->terminate("default/small-b");
#if !d_m3GuardedMemory
    // A one-page pod still takes the pool block (on the board; the host
    // backs linear memory with guard-paged reservations instead)
    k32test::deploy(*rt, "default/page", probe, 64 * 1024);
    assert(kube32_pool_available(nullptr) == 0);
    assert(rt->terminate("default/page"));
    assert(kube32_pool_available(nullptr) == block);
#endif

    // The block as a small heap while no pod holds it whole (allocations the
    // general heap cannot place): shared, then whole again once empty
    {
        assert(kube32_pool_available(nullptr) == block);
        void* a = kube32_pool_sub_alloc(1000);
        void* b = kube32_pool_sub_alloc(20000);
        void* c = kube32_pool_sub_alloc(3000);
        assert(a && b && c && a != b && b != c);
        assert(static_cast<uint8_t*>(a)[999] == 0 && static_cast<uint8_t*>(b)[19999] == 0);
        std::memset(b, 0xab, 20000);
        size_t shared_free = kube32_pool_available(nullptr);
        assert(shared_free > 0 && shared_free < block - 24000);
        assert(kube32_pool_alloc(65 * 1024) == nullptr);   // parts are taken: no pod gets it whole
        kube32_rt_free(a);
        kube32_rt_free(b);
        void* big = kube32_pool_sub_alloc(20900);          // a and b coalesced
        assert(big);
        void* grown = kube32_rt_realloc(c, 2000);           // fits in place
        assert(grown == c);
        // Cut in place: the rest of the chunk is free again (wasm3's last code page)
        const size_t before_shrink = kube32_pool_available(nullptr);
        assert(kube32_rt_shrink(big, 4000) == 1 && kube32_pool_available(nullptr) >= before_shrink + 16000);
        assert(static_cast<uint8_t*>(big)[3999] == 0);
        assert(kube32_rt_shrink(big, 3990) == 0);           // too little to give back
        int on_stack = 0;
        assert(kube32_rt_shrink(&on_stack, 1) == 0);        // not the pool's
        kube32_rt_free(big);
        kube32_rt_free(c);
        assert(kube32_pool_available(nullptr) == block);
        std::cout << "[pool] shared block: " << shared_free << " bytes free with 3 allocations, whole again after" << std::endl;

        // A large memory and small allocations together: the small heap
        // grows down from the top, the memory takes the bottom
        void* small = kube32_pool_sub_alloc(1500);
        assert(small);
        void* whole = kube32_pool_alloc(60 * 1024);
        assert(whole && kube32_pool_owns(whole));
        assert(static_cast<uint8_t*>(small) >= static_cast<uint8_t*>(whole) + 60 * 1024);
        assert(kube32_pool_whole_capacity() >= 60 * 1024 && kube32_pool_whole_capacity() < block);
        void* small2 = kube32_pool_sub_alloc(2000);         // still room above the memory
        assert(small2 && static_cast<uint8_t*>(small2) >= static_cast<uint8_t*>(whole) + 60 * 1024);
        assert(kube32_pool_sub_alloc(8 * 1024) == nullptr); // not past the memory
        assert(kube32_pool_free(whole) == 1);
        void* below = kube32_pool_sub_alloc(30 * 1024);     // the memory's space is the small heap's again
        assert(below);
        kube32_rt_free(below);
        kube32_rt_free(small);
        kube32_rt_free(small2);
        assert(kube32_pool_available(nullptr) == block);
        void* again = kube32_pool_alloc(65 * 1024);
        assert(again && kube32_pool_free(again) == 1);
    }

    // A module exhausting its operand stack traps with a message naming it
    {
        runtime::DeploySpec spec;
        spec.memory_limit_bytes = 1024;
        spec.stack_size_bytes = 1024;
        spec.stack_size_set = true;
        std::string err;
        assert(rt->deploy_workload_ex("default/rec", kRecursiveModule, sizeof(kRecursiveModule), spec, err));
        int32_t r = rt->execute_function("default/rec", "rec", {0});
        runtime::ContainerStatus st2 = rt->get_status("default/rec");
        std::cout << "[wasm stack] rc " << r << ": " << st2.exit_message << std::endl;
        assert(r == -139 && st2.exit_message.find("stack of 1024 bytes is exhausted") != std::string::npos);
        rt->terminate("default/rec");
    }

    // With a modelled board heap (the simulator's --psram 0) pods may hold at
    // most the budget less 16 KiB in the general heap: a pod that fits
    // neither there nor in the pool fails to load, cleanly
    {
        kube32_pool_set_heap_budget(20 * 1024);
        int placed = 0;
        std::string err;
        for (int i = 0; i < 8; ++i) {
            runtime::DeploySpec spec;
            spec.memory_limit_bytes = 16 * 1024;
            spec.stack_size_bytes = 4096;
            if (!rt->deploy_workload_ex("default/fill-" + std::to_string(i), probe.data(), probe.size(), spec, err)) break;
            ++placed;
        }
        std::cout << "[budget] " << placed << " 16Ki pods placed in the pool + 4 KiB of heap; then: " << err << std::endl;
        assert(placed >= 1 && placed < 8 && err.find("allocation failed") != std::string::npos);
        assert(kube32_pool_general_bytes() <= 4 * 1024);
        for (int i = 0; i < placed; ++i) rt->terminate("default/fill-" + std::to_string(i));
        kube32_pool_set_heap_budget(0);
        assert(kube32_pool_available(nullptr) == block);
    }

    // Once a module is compiled its open code page is cut to the lines it
    // holds and closed; the rest goes back to the pool block
    {
        IM3Environment env = m3_NewEnvironment();
        IM3Runtime runtime = m3_NewRuntime(env, 4096, nullptr);
        IM3Module module = nullptr;
        assert(m3_ParseModule(env, &module, probe.data(), static_cast<uint32_t>(probe.size())) == m3Err_none);
        assert(m3_LoadModule(runtime, module) == m3Err_none);
        assert(m3_CompileModule(module) == m3Err_none);
        assert(runtime->pagesOpen && runtime->pagesOpen->info.lineIndex < runtime->pagesOpen->info.numLines);
        const size_t before = kube32_pool_available(nullptr);
        const uint32_t trimmed = m3_TrimCodePages(runtime);
        std::cout << "[trim] probe module: " << trimmed << " bytes of code page given back" << std::endl;
        assert(trimmed > 0 && !runtime->pagesOpen && kube32_pool_available(nullptr) >= before + trimmed - 16);
        for (IM3CodePage pg = runtime->pagesFull; pg; pg = pg->info.next) assert(pg->info.lineIndex == pg->info.numLines || pg->info.numLines - pg->info.lineIndex < 9);
        m3_FreeRuntime(runtime);
        m3_FreeEnvironment(env);
        assert(kube32_pool_available(nullptr) == block);
    }

    // Memory admission on the modelled board heap: a pod larger than the
    // node could ever give it is rejected (Failed, OutOfmemory); one that
    // fits once memory is free waits Pending (InsufficientMemory) with the
    // reason in its status and one Event, and starts when there is room
    {
        kube32_pool_set_heap_budget(24 * 1024);
        auto client = std::make_unique<ListClient>();
        ListClient* c = client.get();
        const std::string img = base64(probe);
        c->add("first", "{\"limits\":{\"memory\":\"32Ki\"}}", img);
        c->add("huge", "{\"limits\":{\"memory\":\"128Ki\"}}", img);
        kubelet::KubeletConfig cfg;
        cfg.node_name = "n1";
        cfg.memory_retry_ms = 0;
        auto wrt = runtime::create_wasm_runtime();
        kubelet::Kubelet k(cfg, *wrt, std::move(client));
        k.sync_assigned_pods();
        kubelet::LocalPodRecord rec;
        assert(k.get_pod_record("default/first", rec) && rec.execution_phase == kubelet::PodExecutionPhase::Running);
        assert(k.get_pod_record("default/huge", rec) && rec.execution_phase == kubelet::PodExecutionPhase::Failed);
        std::cout << "[admission] huge: " << rec.last_error_message << std::endl;
        assert(c->count(c->status_patches, "huge {\"status\":{\"phase\":\"Failed\",\"reason\":\"OutOfmemory\"") == 1);
        assert(c->count(c->events, "\"reason\":\"OutOfmemory\"") == 1);
        assert(!wrt->is_deployed("default/huge"));

        c->add("second", "{\"limits\":{\"memory\":\"32Ki\"}}", img);
        k.sync_assigned_pods();
        k.sync_assigned_pods();
        assert(k.get_pod_record("default/second", rec) && rec.execution_phase == kubelet::PodExecutionPhase::InsufficientMemory);
        std::cout << "[admission] second: " << rec.last_error_message << std::endl;
        assert(!wrt->is_deployed("default/second"));
        assert(c->count(c->status_patches, "second {\"status\":{\"phase\":\"Pending\"") == 1);
        assert(c->count(c->status_patches, "\"waiting\":{\"reason\":\"InsufficientMemory\"") == 1);
        assert(c->count(c->events, "\"reason\":\"InsufficientMemory\"") == 1);   // once, not per retry

        // The first pod goes away: the second is admitted on a later pass
        c->remove("first");
        k.sync_assigned_pods();
        k.sync_assigned_pods();
        assert(k.get_pod_record("default/second", rec) && rec.execution_phase == kubelet::PodExecutionPhase::Running);
        assert(wrt->is_deployed("default/second"));
        assert(c->count(c->status_patches, "second {\"status\":{\"phase\":\"Running\"") == 1);
        c->pods.clear();
        k.sync_assigned_pods();
        kube32_pool_set_heap_budget(0);
        assert(kube32_pool_available(nullptr) == block);
    }

    // The kubelet's budget: limits.memory, else requests.memory, else one page
    {
        auto client = std::make_unique<ListClient>();
        const std::string img = base64(probe);
        client->add("req16", "{\"requests\":{\"memory\":\"16Ki\"}}", img);
        client->add("lim8", "{\"requests\":{\"memory\":\"16Ki\"},\"limits\":{\"memory\":\"8Ki\"}}", img);
        client->add("plain", "", img);
        client->add("stk", "{\"requests\":{\"memory\":\"16Ki\"}}", img, "1Ki");
        kubelet::KubeletConfig cfg;
        cfg.node_name = "n1";
        auto wrt = runtime::create_wasm_runtime();
        kubelet::Kubelet k(cfg, *wrt, std::move(client));
        k.sync_assigned_pods();
        kubelet::LocalPodRecord rec;
        assert(k.get_pod_record("default/req16", rec) && rec.memory_limit_bytes == 16 * 1024);
        assert(k.get_pod_record("default/lim8", rec) && rec.memory_limit_bytes == 8 * 1024);
        assert(k.get_pod_record("default/plain", rec) && rec.memory_limit_bytes == 64 * 1024);
        assert(wrt->get_status("default/req16").memory_used_bytes == 16 * 1024);
        assert(wrt->get_status("default/lim8").memory_used_bytes == 8 * 1024);
        assert(wrt->get_status("default/plain").memory_used_bytes == 64 * 1024);
        // kube32.io/wasm-stack sets the module's operand stack (default 4 KiB)
        assert(wrt->get_status("default/stk").stack_bytes == 1024);
        assert(wrt->get_status("default/req16").stack_bytes == 4096);
        std::cout << "[kubelet] budgets 16Ki, 8Ki, 64Ki applied" << std::endl;
    }

    std::cout << "[PASS] sub-page memory limits" << std::endl;
    return 0;
}
