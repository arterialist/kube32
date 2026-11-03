// Pod workers (AppPodHost::start_workers): pods on two workers tick at the
// same time, kube32.io/core places a pod, the balance moves a busy pod off a
// busy worker, pods that call each other in-process share a worker, and
// events reach each pod in order. The pods also call native imports
// (k32_matvec_i8, k32_log, k32_http_call) from both workers at once.

#include "pod_test_util.hpp"
#include "ipc/channel.hpp"
#include "runtime/pod_memory_pool.h"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace kube32;
using runtime::AppPodHost;

static std::vector<uint8_t> corespin() {
    std::vector<uint8_t> b = k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/tests/data/corespin.wasm");
    assert(!b.empty() && "tests/data/corespin.wasm");
    return b;
}

static long json_num(const std::string& body, const std::string& key) {
    const size_t p = body.find("\"" + key + "\":");
    return p == std::string::npos ? -1 : std::strtol(body.c_str() + p + key.size() + 3, nullptr, 10);
}

// The [start, end] times of the pod's last ticks
static std::vector<std::pair<long, long>> spans(const std::string& body) {
    std::vector<std::pair<long, long>> out;
    size_t p = body.find("\"spans\":[");
    if (p == std::string::npos) return out;
    p += 9;
    while ((p = body.find('[', p)) != std::string::npos) {
        char* end = nullptr;
        const long a = std::strtol(body.c_str() + p + 1, &end, 10);
        const long b = std::strtol(end + 1, nullptr, 10);
        out.emplace_back(a, b);
        p = static_cast<size_t>(end - body.c_str());
    }
    return out;
}

static std::string stats(const std::string& pod) { return k32test::call_pod(pod, "/stats").body; }

static bool wait_for(const std::function<bool()>& ok, uint32_t ms) {
    const uint64_t until = osal::System::get_uptime_ms() + ms;
    while (osal::System::get_uptime_ms() < until) {
        if (ok()) return true;
        osal::Thread::sleep_ms(50);
    }
    return ok();
}

static runtime::DeploySpec spec(std::vector<std::pair<std::string, std::string>> env, int core = -1, std::vector<std::string> topics = {}) {
    runtime::DeploySpec s;
    s.env = std::move(env);
    s.env.push_back({"HOSTNAME", "corespin"});
    s.core = core;
    s.event_topics = std::move(topics);
    s.gas_budget_per_call = 0;   // ticks of SPIN_MS milliseconds
    return s;
}

// Two busy pods, one a core: their ticks overlap, and together they tick
// for longer than the time that passed
static void test_parallel(runtime::WasmRuntime& rt, const std::vector<uint8_t>& wasm) {
    std::cout << "=== two busy pods on two workers ===" << std::endl;
    k32test::deploy(rt, "default/par-a", wasm, 65536, "", spec({{"SPIN_MS", "100"}}, 0));
    k32test::deploy(rt, "default/par-b", wasm, 65536, "", spec({{"SPIN_MS", "100"}}, 1));
    assert(AppPodHost::instance().pod_core("default/par-a") == 0);
    assert(AppPodHost::instance().pod_core("default/par-b") == 1);
    assert(wait_for([] { return json_num(stats("default/par-a"), "ticks") >= 16 && json_num(stats("default/par-b"), "ticks") >= 16; }, 10000));
    const auto a = spans(stats("default/par-a")), b = spans(stats("default/par-b"));
    long busy = 0, first = a[0].first, last = a[0].second, overlaps = 0;
    for (const auto& s : a) {
        busy += s.second - s.first;
        first = std::min(first, s.first);
        last = std::max(last, s.second);
        for (const auto& t : b) overlaps += (s.first < t.second && t.first < s.second) ? 1 : 0;
    }
    for (const auto& s : b) {
        busy += s.second - s.first;
        first = std::min(first, s.first);
        last = std::max(last, s.second);
    }
    std::cout << "ticks overlapping: " << overlaps << ", ticking " << busy << " ms in " << (last - first) << " ms" << std::endl;
    assert(overlaps > 0);
    // One worker ticks at most as long as the time that passed; a loaded
    // host leaves gaps between ticks, so the margin stays below 2x
    assert(busy * 100 > (last - first) * 125);
    assert(rt.terminate("default/par-a"));
    assert(rt.terminate("default/par-b"));
}

// Two pods placed by load on one worker become busy: the balance moves one
// of them to the idle worker
static void test_balance(runtime::WasmRuntime& rt, const std::vector<uint8_t>& wasm) {
    std::cout << "=== the balance moves a busy pod ===" << std::endl;
    // Placed in turn once no pod has been busy for a window: w, x on one
    // worker, y, z on the other
    osal::Thread::sleep_ms(2 * AppPodHost::kBalanceWindowMs + 200);
    const char* pods[] = {"default/bal-w", "default/bal-y", "default/bal-x", "default/bal-z"};
    for (const char* p : pods) k32test::deploy(rt, p, wasm, 65536, "", spec({}, -1, {"bal." + std::string(p + 12)}));
    const int core_w = AppPodHost::instance().pod_core("default/bal-w");
    assert(core_w >= 0 && AppPodHost::instance().pod_core("default/bal-x") == core_w);
    assert(AppPodHost::instance().pod_core("default/bal-y") != core_w);
    // w and x get busy
    auto& ch = ipc::ChannelManager::instance();
    const std::string spin = "spin 60";
    assert(wait_for([&] {
        ch.publish("test/driver", "bal.w", reinterpret_cast<const uint8_t*>(spin.data()), spin.size());
        ch.publish("test/driver", "bal.x", reinterpret_cast<const uint8_t*>(spin.data()), spin.size());
        return json_num(stats("default/bal-w"), "ticks") > 0 && json_num(stats("default/bal-x"), "ticks") > 0;
    }, 3000));
    assert(wait_for([&] { return AppPodHost::instance().pod_core("default/bal-w") != AppPodHost::instance().pod_core("default/bal-x"); },
                    3 * AppPodHost::kBalanceWindowMs + 2000));
    std::cout << "w on core " << AppPodHost::instance().pod_core("default/bal-w") << ", x on core "
              << AppPodHost::instance().pod_core("default/bal-x") << std::endl;
    // Events still reach both, in order, from the workers they are on now
    for (int i = 1; i <= 60; ++i) {
        const std::string n = std::to_string(i);
        ch.publish("test/driver", "bal.w", reinterpret_cast<const uint8_t*>(n.data()), n.size());
        ch.publish("test/driver", "bal.x", reinterpret_cast<const uint8_t*>(n.data()), n.size());
        osal::Thread::sleep_ms(15);
    }
    assert(wait_for([] { return json_num(stats("default/bal-w"), "events") >= 40 && json_num(stats("default/bal-x"), "events") >= 40; }, 5000));
    osal::Thread::sleep_ms(300);
    const std::string sw = stats("default/bal-w"), sx = stats("default/bal-x");
    std::cout << "w: " << json_num(sw, "events") << " events, x: " << json_num(sx, "events") << " events" << std::endl;
    assert(json_num(sw, "disorder") == 0 && json_num(sx, "disorder") == 0);
    for (const char* p : pods) assert(rt.terminate(p));
}

// A pod that calls another in-process from its ticks: the callee joins the
// caller's worker and stays with it
static void test_calls(runtime::WasmRuntime& rt, const std::vector<uint8_t>& wasm) {
    std::cout << "=== in-process calls keep two pods on one worker ===" << std::endl;
    // No pods and no load: f and e are placed on different workers
    osal::Thread::sleep_ms(2 * AppPodHost::kBalanceWindowMs + 200);
    k32test::deploy(rt, "default/call-f", wasm, 65536);
    const int placed = AppPodHost::instance().pod_core("default/call-f");
    k32test::deploy(rt, "default/call-e", wasm, 65536, "\"SERVICE_CALL\"", spec({{"SPIN_MS", "20"}, {"CALL", "http://call-f/x"}}));
    assert(wait_for([] { return json_num(stats("default/call-e"), "calls_ok") >= 5; }, 5000));
    assert(AppPodHost::instance().pod_core("default/call-e") == AppPodHost::instance().pod_core("default/call-f"));
    assert(AppPodHost::instance().pod_core("default/call-f") != placed);   // the callee followed its caller
    assert(json_num(stats("default/call-f"), "calls_in") >= 5);
    // Through more than one balance window
    osal::Thread::sleep_ms(2 * AppPodHost::kBalanceWindowMs + 500);
    assert(AppPodHost::instance().pod_core("default/call-e") == AppPodHost::instance().pod_core("default/call-f"));
    assert(rt.terminate("default/call-e"));
    assert(rt.terminate("default/call-f"));
}

// The stack of a worker that runs the interpreter comes from the pod block,
// as on a board with PSRAM: allocatable memory leaves it out
static void test_system_stack() {
    std::cout << "=== a worker stack from the pod block ===" << std::endl;
    kube32_pool_reserve_external(1284 * 1024);
    size_t before = 0, after = 0;
    kube32_pool_node_room(&before, nullptr);
    const size_t free_before = kube32_pool_available(nullptr);
    void* stack = kube32_pool_system_alloc(68 * 1024);
    assert(stack);
    kube32_pool_node_room(&after, nullptr);
    assert(before - after == 68 * 1024 && kube32_pool_system_bytes() == 68 * 1024);
    assert(kube32_pool_available(nullptr) + 68 * 1024 <= free_before);
}

int main() {
    test_system_stack();
    const std::vector<uint8_t> wasm = corespin();
    // In-process Service calls: http://<pod name>/<path> reaches pod default/<pod name>
    runtime::AppPlatformHooks hooks;
    hooks.service_call = [](const runtime::ServiceCallRequest& req, runtime::BufferResponseSink& sink) -> int32_t {
        const std::string url(req.url);
        const size_t host = url.find("//") + 2, path = url.find('/', host);
        runtime::AppRequest r;
        r.method = req.method;
        r.path_and_query = std::string_view(req.url).substr(path);
        r.body = req.body;
        r.call_chain = req.call_chain;
        return AppPodHost::instance().call_nested("default/" + url.substr(host, path - host), r, sink, req.timeout_ms);
    };
    runtime::set_app_platform_hooks(hooks);
    auto& ch = ipc::ChannelManager::instance();
    assert(ch.init(0, 7471));
    assert(ch.start_background(60000, 262144, false));

    std::vector<runtime::PodWorkerOptions> workers(2);
    workers[0].core = 1;
    workers[1].core = 0;
    assert(AppPodHost::instance().start_workers(workers) == 2);
    assert(AppPodHost::instance().worker_count() == 2);

    runtime::WasmRuntime rt;
    test_parallel(rt, wasm);
    test_balance(rt, wasm);
    test_calls(rt, wasm);

    AppPodHost::instance().stop_workers();
    assert(AppPodHost::instance().worker_count() == 0);
    ch.shutdown();
    std::cout << "All pod worker tests passed" << std::endl;
    return 0;
}
