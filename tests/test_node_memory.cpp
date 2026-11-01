// What a Node offers pods in status.allocatable and status.capacity, counted
// as the kubelet admits pods (runtime/pod_memory_pool.h), and what the
// scheduler makes of it. The boards are modelled as kube32-sim models them:
//
//   psram     a control plane with PSRAM: the 1284 KiB pool block (--psram)
//             and the PSRAM besides it (--heap-budget). Allocatable memory is
//             that PSRAM above the reserve: a 64Ki pod such as the echo
//             example and the Tessera pods of the control plane fit, and the
//             kubelet's admission gives the same pods room.
//   internal  a worker without PSRAM: the 80 KiB pool block and the heap
//             budget, as the simulator admits pods there.
//
// In both, the figures stay the same while no pod loads or goes and follow
// what pods take. The pool is reserved once per process: run as
// `test_node_memory psram` or `test_node_memory internal`.

#include "embedded/embedded_blobs.hpp"
#include "kubelet/node_status_reporter.hpp"
#include "models/kube_models.hpp"
#include "runtime/image_registry.hpp"
#include "runtime/pod_memory_pool.h"
#include "runtime/wasm_runtime.hpp"
#include "scheduler/scheduler.hpp"
#include "storage/store.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

using namespace kube32;

namespace {

std::unique_ptr<storage::Store> ram_store() {
    storage::StoreOptions o;
    o.enable_flash_persistence = false;
    return storage::create_radix_store(o);
}

models::JsonValue parse(const std::string& json) {
    models::JsonValue doc;
    std::string err;
    bool ok = models::parse_json(json, doc, err);
    assert(ok);
    return doc;
}

uint64_t quantity(const models::JsonValue& node, const char* list, const char* name) {
    const models::JsonValue* l = node.get("status")->get(list);
    return models::parse_quantity(l->get_string(name, ""));
}

// The Node a kubelet registers with these figures, Ready
std::string register_node(storage::Store& store, const std::string& name, bool control_plane, const std::string& board) {
    kubelet::NodeStatusReporter reporter(name, "10.0.0.2", control_plane);
    reporter.set_labels({{"kube32.io/board", board}});
    kubelet::NodeTelemetry t = reporter.collect_telemetry();
    t.ready = true;
    t.ready_reason = "KubeletReady";
    const std::string json = reporter.build_registration_json(t);
    bool ok = store.put("/registry/nodes/" + name, json, false).ok();
    assert(ok);
    return json;
}

// A pod of one container with these requests and limits, and node selector
std::string pod(const std::string& name, const std::string& resources, const std::string& selector) {
    return R"({"apiVersion":"v1","kind":"Pod","metadata":{"name":")" + name + R"(","namespace":"default"},"spec":{"nodeSelector":)" +
           selector + R"(,"containers":[{"name":"c","image":"wasm://embedded/echo","resources":{"requests":)" + resources +
           R"(,"limits":)" + resources + R"(}}]}})";
}

std::string schedule(scheduler::Scheduler& sched, storage::Store& store, const std::string& name, const std::string& resources,
                     const std::string& selector = "{}") {
    std::string json = pod(name, resources, selector);
    scheduler::ScheduleResult r = sched.schedule_pod_manifest(json);
    store.put("/registry/pods/default/" + name, json, false);
    return r.scheduled ? r.target_node : "Unschedulable: " + r.failure_reason;
}

const runtime::EmbeddedImage* echo_image() {
    size_t n = 0;
    const runtime::EmbeddedImage* imgs = embedded::images(n);
    runtime::ImageRegistry::instance().add_all(imgs, n);
    const runtime::EmbeddedImage* echo = runtime::ImageRegistry::instance().resolve("wasm://embedded/echo");
    assert(echo);
    return echo;
}

void test_psram_node() {
    std::cout << "[RUN] test_psram_node" << std::endl;
    kube32_pool_reserve_external(20 * 64 * 1024 + 4096);
    kube32_pool_set_heap_budget(320 * 1024);
    const size_t block = kube32_pool_block_size();
    assert(block == 20 * 64 * 1024 + 4096);

    auto store = ram_store();
    const models::JsonValue node = parse(register_node(*store, "devkit", true, "generic"));
    // PSRAM above the reserve, not the internal heap; esp32.io/psram adds the block
    const uint64_t memory = quantity(node, "allocatable", "memory");
    assert(memory == 320 * 1024 - kube32_pool_psram_reserve());
    assert(quantity(node, "allocatable", "esp32.io/psram") == memory + block);
    assert(quantity(node, "capacity", "memory") >= memory && quantity(node, "capacity", "esp32.io/psram") == block);

    // The examples' control-plane pods are bound to it
    auto sched = scheduler::create_scheduler(*store);
    const std::string cp = R"({"node-role.kubernetes.io/control-plane":""})";
    assert(schedule(*sched, *store, "echo", R"({"memory":"64Ki","hardware.kube32.io/ipc":"1"})", cp) == "devkit");
    assert(schedule(*sched, *store, "tessera-web", R"({"memory":"4Ki"})", cp) == "devkit");
    assert(schedule(*sched, *store, "tessera-api", R"({"memory":"16Ki","esp32.io/psram":"1280Ki","hardware.kube32.io/ipc":"1"})", cp) == "devkit");
    assert(schedule(*sched, *store, "webhook", R"({"memory":"8Ki"})", cp) == "devkit");
    // ... and the oversized one is not
    const std::string oversized = schedule(*sched, *store, "oversized-pod", R"({"memory":"4000Ki","esp32.io/psram":"4Mi"})", cp);
    assert(oversized == "Unschedulable: 0/1 nodes are available: 1 Insufficient memory.");

    // The kubelet gives the echo pod room: its estimate fits what it admits
    // against, and what the node advertises
    const runtime::EmbeddedImage* echo = echo_image();
    const size_t need = runtime::estimate_pod_memory(echo->data, echo->len, 64 * 1024, 4 * 1024, true);
    size_t capacity = 0;
    const size_t room = kube32_pool_pod_room(64 * 1024, &capacity);
    assert(need <= room && need <= memory && need <= capacity);
    // A 64 KiB memory is not one the block takes: the block is not counted
    // for it, only for the large memory of tessera-api
    assert(room == memory);
    assert(kube32_pool_pod_room(1280 * 1024, nullptr) == memory + block);

    // Stable while no pod loads or goes ...
    kubelet::NodeStatusReporter reporter("devkit", "10.0.0.2", true);
    const kubelet::NodeTelemetry before = reporter.collect_telemetry();
    assert(before.memory_allocatable_bytes == memory && reporter.collect_telemetry().memory_allocatable_bytes == memory);
    // ... and following what pods take: a module's structures, then a large
    // linear memory in the block
    void* structures = kube32_rt_calloc(40 * 1024);
    void* linear = kube32_pool_alloc(1280 * 1024 + 4096);
    assert(structures && linear);
    const kubelet::NodeTelemetry during = reporter.collect_telemetry();
    // At least the 40 KiB taken; the host allocator may round the block up
    // (macOS rounds large blocks to its pages), so allow up to 64 KiB
    assert(during.memory_allocatable_bytes <= memory - 40 * 1024 && during.memory_allocatable_bytes + 64 * 1024 >= memory);
    assert(during.free_psram_bytes == during.memory_allocatable_bytes + (block - (1280 * 1024 + 4096)));
    kube32_pool_free(linear);
    kube32_rt_free(structures);
    assert(reporter.collect_telemetry().memory_allocatable_bytes == memory);
    std::cout << "[PASS] test_psram_node (allocatable memory " << memory / 1024 << " KiB, psram " << (memory + block) / 1024 << " KiB)" << std::endl;
}

void test_internal_pool_node() {
    std::cout << "[RUN] test_internal_pool_node" << std::endl;
    kube32_pool_reserve(80 * 1024);
    kube32_pool_set_heap_budget(51 * 1024);
    auto store = ram_store();
    const models::JsonValue node = parse(register_node(*store, "cardputer", false, "cardputer"));
    // The pool block and the budget above the system reserve, as the
    // simulator admits pods; no PSRAM
    const uint64_t memory = quantity(node, "allocatable", "memory");
    assert(memory == 80 * 1024 + 51 * 1024 - kube32_pool_system_reserve());
    assert(memory == kube32_pool_pod_room(64 * 1024, nullptr));
    assert(!node.get("status")->get("allocatable")->get("esp32.io/psram"));
    assert(quantity(node, "capacity", "memory") == 80 * 1024 + 51 * 1024);

    auto sched = scheduler::create_scheduler(*store);
    const std::string cardputer = R"({"kube32.io/board":"cardputer"})";
    assert(schedule(*sched, *store, "tessera-db", R"({"memory":"8Ki","hardware.kube32.io/sd":"1"})", cardputer) == "cardputer");
    assert(schedule(*sched, *store, "tessera-hud", R"({"memory":"1Ki","hardware.kube32.io/tft-display":"1"})", cardputer) == "cardputer");
    assert(schedule(*sched, *store, "tessera-api", R"({"memory":"16Ki","esp32.io/psram":"1280Ki"})", "{}").find("Insufficient esp32.io/psram") !=
           std::string::npos);

    // Small pods share the block: what one takes comes off the figure
    kubelet::NodeStatusReporter reporter("cardputer", "10.0.0.3");
    void* p = kube32_rt_calloc(20 * 1024);
    assert(p && kube32_pool_owns(p) == 0);
    const size_t during = reporter.collect_telemetry().memory_allocatable_bytes;
    assert(during < memory - 20 * 1024 && during + 21 * 1024 >= memory);
    kube32_rt_free(p);
    assert(reporter.collect_telemetry().memory_allocatable_bytes == memory);
    std::cout << "[PASS] test_internal_pool_node (allocatable memory " << memory / 1024 << " KiB)" << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "psram") test_psram_node();
    else if (mode == "internal") test_internal_pool_node();
    else {
        std::cerr << "usage: test_node_memory psram|internal" << std::endl;
        return 2;
    }
    std::cout << "Node memory tests passed (" << mode << ")" << std::endl;
    return 0;
}
