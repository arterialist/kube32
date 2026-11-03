// What a Node offers pods in status.allocatable and status.capacity, counted
// as the kubelet admits pods (runtime/pod_memory_pool.h), what it says is
// free now in its annotations, and what the scheduler makes of it. The
// boards are modelled as kube32-sim models them:
//
//   psram     a control plane with PSRAM: the 1284 KiB pool block (--psram)
//             and the PSRAM besides it (--heap-budget). Allocatable memory is
//             the block and that PSRAM above the reserve, and esp32.io/psram
//             the same figure: a pod's memory is all PSRAM there.
//   internal  a worker without PSRAM: the 80 KiB pool block and the heap
//             budget, as the simulator admits pods there.
//
// In both, allocatable stays the same while pods load and go; the scheduler
// subtracts what bound pods request, and kube32.io/memory-free follows what
// pods take. The pool is reserved once per process: run as
// `test_node_memory psram`, `test_node_memory internal` or
// `test_node_memory reserved`.

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

uint64_t annotation(const models::JsonValue& node, const char* key) {
    const models::JsonValue* a = node.get("metadata")->get("annotations");
    assert(a && a->get(key));
    return models::parse_quantity(a->get_string(key, ""));
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
    // Pods get the block and the PSRAM above the reserve, not the internal
    // heap; a pod's memory is all PSRAM, so esp32.io/psram is the same figure
    const uint64_t memory = quantity(node, "allocatable", "memory");
    assert(memory == 320 * 1024 - kube32_pool_system_reserve() + block);
    assert(quantity(node, "allocatable", "esp32.io/psram") == memory);
    assert(quantity(node, "capacity", "memory") == 320 * 1024 + block && quantity(node, "capacity", "esp32.io/psram") == 320 * 1024 + block);
    // What is free now: all of it, nothing runs
    assert(annotation(node, "kube32.io/memory-free") == memory);
    assert(annotation(node, "kube32.io/psram-free") == 320 * 1024 + block);
    assert(annotation(node, "kube32.io/system-reserved") == kube32_pool_system_reserve());
    assert(kube32_pool_linear_max() == block);

    // The examples' control-plane pods are bound to it: their requests count
    auto sched = scheduler::create_scheduler(*store);
    const std::string cp = R"({"node-role.kubernetes.io/control-plane":""})";
    assert(schedule(*sched, *store, "echo", R"({"memory":"64Ki","hardware.kube32.io/ipc":"1"})", cp) == "devkit");
    assert(schedule(*sched, *store, "tessera-web", R"({"memory":"4Ki"})", cp) == "devkit");
    assert(schedule(*sched, *store, "tessera-api", R"({"memory":"16Ki","esp32.io/psram":"1280Ki","hardware.kube32.io/ipc":"1"})", cp) == "devkit");
    assert(schedule(*sched, *store, "webhook", R"({"memory":"8Ki"})", cp) == "devkit");
    const scheduler::ResourceSpec bound = scheduler::calculate_node_allocated_resources(*store, "devkit");
    assert(bound.memory_bytes == (64 + 4 + 16 + 8) * 1024 && bound.psram_bytes == 1280 * 1024);
    // ... and the oversized one is not; nor a second 1280Ki of PSRAM
    const std::string oversized = schedule(*sched, *store, "oversized-pod", R"({"memory":"4000Ki","esp32.io/psram":"4Mi"})", cp);
    assert(oversized == "Unschedulable: 0/1 nodes are available: 1 Insufficient memory.");
    assert(schedule(*sched, *store, "second-api", R"({"memory":"16Ki","esp32.io/psram":"1280Ki"})", cp) ==
           "Unschedulable: 0/1 nodes are available: 1 Insufficient esp32.io/psram.");

    // The kubelet gives the echo pod room: its estimate fits what it admits
    // against now, and what the node can ever give is allocatable
    const runtime::EmbeddedImage* echo = echo_image();
    const size_t need = runtime::estimate_pod_memory(echo->data, echo->len, 64 * 1024, 4 * 1024, true);
    size_t capacity = 0;
    const size_t room = kube32_pool_pod_room(64 * 1024, &capacity);
    assert(need <= room && room == memory && capacity == memory);

    // Allocatable stays while pods load and go; what is free now follows
    // what they take. A module's structures go to the block's end
    kubelet::NodeStatusReporter reporter("devkit", "10.0.0.2", true);
    assert(reporter.collect_telemetry().memory_allocatable_bytes == memory);
    void* structures = kube32_rt_calloc(40 * 1024);
    assert(structures && kube32_pool_available(nullptr) <= block - 40 * 1024);
    kubelet::NodeTelemetry during = reporter.collect_telemetry();
    assert(during.memory_allocatable_bytes == memory && during.psram_allocatable_bytes == memory);
    assert(during.memory_free_bytes == memory - (block - kube32_pool_available(nullptr)));
    assert(during.free_psram_bytes == during.memory_free_bytes + kube32_pool_system_reserve());
    assert(!kube32_pool_alloc(1280 * 1024 + 4096));
    kube32_rt_free(structures);
    assert(reporter.collect_telemetry().memory_free_bytes == memory);
    // A large linear memory takes the block whole; structures then come from
    // the PSRAM besides it
    void* linear = kube32_pool_alloc(1280 * 1024 + 4096);
    structures = kube32_rt_calloc(40 * 1024);
    assert(linear && structures);
    during = reporter.collect_telemetry();
    assert(during.memory_allocatable_bytes == memory && during.psram_allocatable_bytes == memory);
    // At least the 40 KiB taken; the host allocator may round the block up
    // (macOS rounds large blocks to its pages), so allow up to 64 KiB
    const size_t besides = memory - block;
    assert(during.memory_free_bytes <= besides - 40 * 1024 && during.memory_free_bytes + 64 * 1024 >= besides);
    kube32_pool_free(linear);
    kube32_rt_free(structures);
    assert(reporter.collect_telemetry().memory_free_bytes == memory);
    std::cout << "[PASS] test_psram_node (allocatable memory and psram " << memory / 1024 << " KiB)" << std::endl;
}

// A control plane keeps part of its PSRAM for itself (the store, the API
// server, the controllers): allocatable memory leaves it out, the Node says
// how much, and the scheduler places pods on what is left
void test_system_reserved() {
    std::cout << "[RUN] test_system_reserved" << std::endl;
    kube32_pool_reserve_external(20 * 64 * 1024 + 4096);
    kube32_pool_set_heap_budget(640 * 1024);
    kube32_pool_set_psram_reserve(192 * 1024);
    const size_t block = kube32_pool_block_size();
    assert(kube32_pool_reserved_for_system() == 192 * 1024);

    auto store = ram_store();
    const models::JsonValue node = parse(register_node(*store, "devkit", true, "generic"));
    const uint64_t capacity = quantity(node, "capacity", "memory");
    const uint64_t allocatable = quantity(node, "allocatable", "memory");
    assert(capacity == 640 * 1024 + block);
    assert(allocatable == capacity - 192 * 1024);
    assert(quantity(node, "allocatable", "esp32.io/psram") == allocatable);
    assert(quantity(node, "capacity", "esp32.io/psram") == capacity);
    assert(annotation(node, "kube32.io/system-reserved") == 192 * 1024);
    // Pods are admitted on what is left of it, as the scheduler counts
    size_t room_capacity = 0;
    assert(kube32_pool_pod_room(64 * 1024, &room_capacity) == allocatable && room_capacity == allocatable);

    auto sched = scheduler::create_scheduler(*store);
    const std::string cp = R"({"node-role.kubernetes.io/control-plane":""})";
    const std::string fits = std::to_string(allocatable / 1024) + "Ki";
    const std::string over = std::to_string(allocatable / 1024 + 1) + "Ki";
    assert(schedule(*sched, *store, "fills", "{\"memory\":\"" + fits + "\"}", cp) == "devkit");
    assert(schedule(*sched, *store, "over", "{\"memory\":\"" + over + "\"}", cp) ==
           "Unschedulable: 0/1 nodes are available: 1 Insufficient memory.");
    std::cout << "[PASS] test_system_reserved (capacity " << capacity / 1024 << " KiB, allocatable " << allocatable / 1024 << " KiB)" << std::endl;
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
    size_t capacity = 0;
    assert(memory == kube32_pool_pod_room(64 * 1024, &capacity) && capacity == memory);
    assert(!node.get("status")->get("allocatable")->get("esp32.io/psram"));
    assert(quantity(node, "capacity", "memory") == 80 * 1024 + 51 * 1024);
    assert(annotation(node, "kube32.io/memory-free") == memory && annotation(node, "kube32.io/heap-free") == 51 * 1024);
    assert(!node.get("metadata")->get("annotations")->get("kube32.io/psram-free"));

    auto sched = scheduler::create_scheduler(*store);
    const std::string cardputer = R"({"kube32.io/board":"cardputer"})";
    assert(schedule(*sched, *store, "tessera-db", R"({"memory":"8Ki","hardware.kube32.io/sd":"1"})", cardputer) == "cardputer");
    assert(schedule(*sched, *store, "tessera-hud", R"({"memory":"1Ki","hardware.kube32.io/tft-display":"1"})", cardputer) == "cardputer");
    assert(schedule(*sched, *store, "tessera-api", R"({"memory":"16Ki","esp32.io/psram":"1280Ki"})", "{}").find("Insufficient esp32.io/psram") !=
           std::string::npos);
    // 98Ki left of 107Ki: a 64 KiB page fits once, not twice
    assert(schedule(*sched, *store, "page-1", R"({"memory":"64Ki"})", cardputer) == "cardputer");
    assert(schedule(*sched, *store, "page-2", R"({"memory":"64Ki"})", cardputer) == "Unschedulable: 0/1 nodes are available: 1 Insufficient memory.");

    // Small pods share the block: what one takes comes off what is free now,
    // not off allocatable; the free heap is internal RAM only
    kubelet::NodeStatusReporter reporter("cardputer", "10.0.0.3");
    void* p = kube32_rt_calloc(20 * 1024);
    assert(p && kube32_pool_owns(p) == 0);
    const kubelet::NodeTelemetry during = reporter.collect_telemetry();
    assert(during.memory_allocatable_bytes == memory);
    assert(during.memory_free_bytes < memory - 20 * 1024 && during.memory_free_bytes + 21 * 1024 >= memory);
    assert(during.free_heap_bytes == 51 * 1024);
    kube32_rt_free(p);
    assert(reporter.collect_telemetry().memory_free_bytes == memory);
    std::cout << "[PASS] test_internal_pool_node (allocatable memory " << memory / 1024 << " KiB)" << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "psram") test_psram_node();
    else if (mode == "internal") test_internal_pool_node();
    else if (mode == "reserved") test_system_reserved();
    else {
        std::cerr << "usage: test_node_memory psram|internal|reserved" << std::endl;
        return 2;
    }
    std::cout << "Node memory tests passed (" << mode << ")" << std::endl;
    return 0;
}
