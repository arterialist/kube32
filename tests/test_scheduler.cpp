#include "scheduler/scheduler.hpp"
#include "embedded/embedded_blobs.hpp"
#include "runtime/image_registry.hpp"
#include "runtime/wasm_runtime.hpp"
#include "storage/store.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#include <cassert>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using namespace kube32::scheduler;
using namespace kube32::storage;

void test_quantity_parser() {
    std::cout << "[RUN] test_quantity_parser" << std::endl;

    assert(parse_quantity("16Ki") == 16 * 1024);
    assert(parse_quantity("240Ki") == 240 * 1024);
    assert(parse_quantity("3840Ki") == 3840 * 1024);
    assert(parse_quantity("4Mi") == 4 * 1024 * 1024);
    assert(parse_quantity("1Gi") == 1024 * 1024 * 1024);
    assert(parse_quantity("14") == 14);
    assert(parse_quantity("2") == 2);
    assert(parse_quantity("0") == 0);
    assert(parse_quantity("") == 0);

    // Kubernetes suffixes: m is milli (memory "64m" rounds up to 1 byte, as
    // Quantity.Value() does), k/M/G are decimal, exponents are allowed, and
    // lower-case binary suffixes are not quantities.
    assert(parse_quantity("64m") == 1);
    assert(parse_quantity("1500m") == 2);
    assert(parse_quantity("2000m") == 2);
    assert(parse_quantity("1k") == 1000);
    assert(parse_quantity("1M") == 1000000);
    assert(parse_quantity("2G") == 2000000000ULL);
    assert(parse_quantity("1e3") == 1000);
    assert(parse_quantity("1.5Gi") == 1610612736ULL);
    assert(parse_quantity("1.1k") == 1100);
    assert(parse_quantity("64K") == 0);
    assert(parse_quantity("240ki") == 0);
    assert(parse_quantity("-1Ki") == 0);

    std::cout << "[PASS] test_quantity_parser" << std::endl;
}

void test_predicates() {
    std::cout << "[RUN] test_predicates" << std::endl;

    std::string ready_node = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-01\",\"labels\":{\"role\":\"worker\"}},"
                             "\"spec\":{\"unschedulable\":false},"
                             "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                             "\"allocatable\":{\"memory\":\"240Ki\",\"esp.io/gpio\":\"14\",\"esp.io/i2c\":\"2\"}}}";

    std::string not_ready_node = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-02\"},"
                                 "\"spec\":{\"unschedulable\":false},"
                                 "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"False\"}]}}";

    std::string unsched_node = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-03\"},"
                               "\"spec\":{\"unschedulable\":true},"
                               "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}]}}";

    // 1. NodeSchedulablePredicate
    {
        auto pred = std::make_shared<NodeSchedulablePredicate>();
        std::string reason;
        ResourceSpec req, alloc, used;
        assert(pred->evaluate("", ready_node, req, alloc, used, reason));
        assert(!pred->evaluate("", not_ready_node, req, alloc, used, reason));
        assert(!pred->evaluate("", unsched_node, req, alloc, used, reason));
    }

    // 2. NodeMemoryPredicate
    {
        auto pred = std::make_shared<NodeMemoryPredicate>();
        std::string reason;
        ResourceSpec alloc, used;
        alloc.memory_bytes = 240 * 1024;
        used.memory_bytes = 100 * 1024;

        ResourceSpec req_fit;
        req_fit.memory_bytes = 64 * 1024;
        assert(pred->evaluate("", ready_node, req_fit, alloc, used, reason));

        ResourceSpec req_overflow;
        req_overflow.memory_bytes = 200 * 1024; // 100 + 200 > 240
        assert(!pred->evaluate("", ready_node, req_overflow, alloc, used, reason));
    }

    // 3. NodePeripheralPredicate
    {
        auto pred = std::make_shared<NodePeripheralPredicate>();
        std::string reason;
        ResourceSpec alloc, used;
        alloc.gpio_pins = 14;
        used.gpio_pins = 10;
        alloc.i2c_buses = 2;
        used.i2c_buses = 0;

        ResourceSpec req_fit;
        req_fit.gpio_pins = 4;
        assert(pred->evaluate("", ready_node, req_fit, alloc, used, reason));

        ResourceSpec req_excess_gpio;
        req_excess_gpio.gpio_pins = 5; // 10 + 5 > 14
        assert(!pred->evaluate("", ready_node, req_excess_gpio, alloc, used, reason));
    }

    // 4. NodeSelectorPredicate
    {
        auto pred = std::make_shared<NodeSelectorPredicate>();
        std::string reason;
        ResourceSpec req, alloc, used;

        std::string pod_match = "{\"spec\":{\"nodeSelector\":{\"role\":\"worker\"}}}";
        std::string pod_mismatch = "{\"spec\":{\"nodeSelector\":{\"role\":\"gpu\"}}}";

        assert(pred->evaluate(pod_match, ready_node, req, alloc, used, reason));
        assert(!pred->evaluate(pod_mismatch, ready_node, req, alloc, used, reason));
    }

    std::cout << "[PASS] test_predicates" << std::endl;
}

void test_scheduler_atomic_binding() {
    std::cout << "[RUN] test_scheduler_atomic_binding" << std::endl;

    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);

    // Register 2 nodes
    std::string node1 = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-01\"},"
                        "\"spec\":{\"unschedulable\":false},"
                        "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                        "\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"192.168.4.1\"}],"
                        "\"allocatable\":{\"memory\":\"240Ki\",\"esp.io/gpio\":\"14\"}}}";

    std::string node2 = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-02\"},"
                        "\"spec\":{\"unschedulable\":false},"
                        "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                        "\"addresses\":[{\"type\":\"InternalIP\",\"address\":\"192.168.4.2\"}],"
                        "\"allocatable\":{\"memory\":\"240Ki\",\"esp.io/gpio\":\"14\"}}}";

    store->put("/registry/nodes/esp32-node-01", node1, true);
    store->put("/registry/nodes/esp32-node-02", node2, true);

    auto sched = create_scheduler(*store);

    // Schedule small pod
    std::string pod1 = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"small-pod\",\"namespace\":\"default\"},"
                       "\"spec\":{\"containers\":[{\"name\":\"c1\",\"resources\":{\"requests\":{\"memory\":\"32Ki\",\"esp.io/gpio\":\"2\"}}}]}}";
    ScheduleResult res1 = sched->schedule_pod_manifest(pod1);
    assert(res1.scheduled);
    assert(res1.target_node == "esp32-node-01"); // deterministic tie-break
    // Binding only assigns the node; the kubelet reports Running once it runs
    assert(pod1.find("\"phase\":\"Pending\"") != std::string::npos);
    assert(pod1.find("\"type\":\"PodScheduled\",\"status\":\"True\"") != std::string::npos);
    assert(pod1.find("\"nodeName\":\"esp32-node-01\"") != std::string::npos);

    // Commit small pod to store
    store->put("/registry/pods/default/small-pod", pod1, true);

    // Schedule second pod: should pick node2 under LeastAllocated
    std::string pod2 = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"second-pod\",\"namespace\":\"default\"},"
                       "\"spec\":{\"containers\":[{\"name\":\"c2\",\"resources\":{\"requests\":{\"memory\":\"32Ki\",\"esp.io/gpio\":\"2\"}}}]}}";
    ScheduleResult res2 = sched->schedule_pod_manifest(pod2);
    assert(res2.scheduled);
    assert(res2.target_node == "esp32-node-02"); // node2 has 100% free memory!
    assert(pod2.find("\"phase\":\"Pending\"") != std::string::npos);
    assert(pod2.find("\"type\":\"PodScheduled\",\"status\":\"True\"") != std::string::npos);

    // Schedule excessive pod that exceeds all nodes
    std::string pod_giant = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"giant-pod\",\"namespace\":\"default\"},"
                            "\"spec\":{\"containers\":[{\"name\":\"cg\",\"resources\":{\"requests\":{\"memory\":\"500Ki\"}}}]}}";
    ScheduleResult res3 = sched->schedule_pod_manifest(pod_giant);
    assert(!res3.scheduled);
    assert(pod_giant.find("\"phase\":\"Pending\"") != std::string::npos);
    assert(pod_giant.find("\"reason\":\"Unschedulable\"") != std::string::npos);

    std::cout << "[PASS] test_scheduler_atomic_binding" << std::endl;
}

void test_deleted_node_reconciliation() {
    std::cout << "[RUN] test_deleted_node_reconciliation" << std::endl;

    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);

    // Register 2 nodes
    std::string node1 = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-01\"},"
                        "\"spec\":{\"unschedulable\":false},"
                        "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                        "\"allocatable\":{\"memory\":\"240Ki\",\"esp.io/gpio\":\"14\"}}}";

    std::string node2 = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"esp32-node-02\"},"
                        "\"spec\":{\"unschedulable\":false},"
                        "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                        "\"allocatable\":{\"memory\":\"240Ki\",\"esp.io/gpio\":\"14\"}}}";

    store->put("/registry/nodes/esp32-node-01", node1, true);
    store->put("/registry/nodes/esp32-node-02", node2, true);

    auto sched = create_scheduler(*store);

    // Register a running pod on esp32-node-01
    std::string pod = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"migrating-pod\",\"namespace\":\"default\"},"
                      "\"spec\":{\"nodeName\":\"esp32-node-01\",\"containers\":[{\"name\":\"c1\",\"resources\":{\"requests\":{\"memory\":\"32Ki\"}}}]},"
                      "\"status\":{\"phase\":\"Running\"}}";
    store->put("/registry/pods/default/migrating-pod", pod, true);

    // Delete esp32-node-01 from registry
    store->remove("/registry/nodes/esp32-node-01");

    // Pass 1: Reconciliation detects assigned_node is absent from ready_nodes, evicts to Pending
    sched->reconcile_cluster();

    std::string updated_pod;
    store->get("/registry/pods/default/migrating-pod", updated_pod);
    assert(updated_pod.find("\"phase\":\"Pending\"") != std::string::npos);
    assert(updated_pod.find("\"nodeName\":\"\"") != std::string::npos);

    // Pass 2: Reconciliation re-evaluates Pending pod and schedules onto surviving esp32-node-02
    sched->reconcile_cluster();

    store->get("/registry/pods/default/migrating-pod", updated_pod);
    assert(updated_pod.find("\"type\":\"PodScheduled\",\"status\":\"True\"") != std::string::npos);
    assert(updated_pod.find("\"nodeName\":\"esp32-node-02\"") != std::string::npos);

    std::cout << "[PASS] test_deleted_node_reconciliation" << std::endl;
}

// Allocatable memory is what the node's pods can be given with no pod
// running, a fixed figure: every bound pod that has not terminated counts
// against it with its requests and its Pod Overhead, whether its kubelet has
// started it or not, and whenever the node last reported.
void test_allocatable_accounting_and_failure_message() {
    std::cout << "[RUN] test_allocatable_accounting_and_failure_message" << std::endl;
    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);

    const std::string running_pod =
        "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"hud\",\"namespace\":\"default\"},"
        "\"spec\":{\"nodeName\":\"node-a\",\"overhead\":{\"memory\":\"20Ki\"},"
        "\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"memory\":\"64Ki\"}}}]},"
        "\"status\":{\"phase\":\"Running\",\"containerStatuses\":[{\"name\":\"c\",\"state\":{\"running\":{}}}]}}";
    store->put("/registry/pods/default/hud", running_pod, true);
    // The node reports after the pod started: allocatable does not move
    const std::string node_a = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"node-a\",\"labels\":{\"board\":\"cardputer\"}},"
                               "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                               "\"allocatable\":{\"memory\":\"200Ki\"}}}";
    store->put("/registry/nodes/node-a", node_a, true);

    ResourceSpec used = calculate_node_allocated_resources(*store, "node-a");
    assert(used.memory_bytes == (64 + 20) * 1024);   // the running pod and its overhead

    auto sched = create_scheduler(*store);
    std::string fits = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"fits\",\"namespace\":\"default\"},"
                       "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"memory\":\"90Ki\"}}}]}}";
    ScheduleResult r = sched->schedule_pod_manifest(fits);
    assert(r.scheduled && r.target_node == "node-a");
    store->put("/registry/pods/default/fits", fits, true);   // bound, not started

    used = calculate_node_allocated_resources(*store, "node-a");
    assert(used.memory_bytes == (64 + 20 + 90) * 1024);
    // The node reporting again changes nothing
    store->put("/registry/nodes/node-a", node_a, true);
    store->put("/registry/pods/default/hud", running_pod, true);
    store->put("/registry/nodes/node-a", node_a, true);
    assert(calculate_node_allocated_resources(*store, "node-a").memory_bytes == (64 + 20 + 90) * 1024);
    // 26Ki left: a pod with 20Ki of requests and 8Ki of overhead does not fit
    std::string over = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"over\",\"namespace\":\"default\"},"
                       "\"spec\":{\"overhead\":{\"memory\":\"8Ki\"},\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"memory\":\"20Ki\"}}}]}}";
    assert(!sched->schedule_pod_manifest(over).scheduled);
    // A pod that terminated holds nothing: then it fits
    std::string done = running_pod;
    done.replace(done.find("\"phase\":\"Running\""), 17, "\"phase\":\"Succeeded\"");
    store->put("/registry/pods/default/hud", done, true);
    assert(calculate_node_allocated_resources(*store, "node-a").memory_bytes == 90 * 1024);
    r = sched->schedule_pod_manifest(over);
    assert(r.scheduled && r.target_node == "node-a");
    store->remove("/registry/pods/default/over");
    store->put("/registry/pods/default/hud", running_pod, true);

    // Nothing fits: the message counts each reason, kube-scheduler style
    const std::string node_b = "{\"kind\":\"Node\",\"metadata\":{\"name\":\"node-b\",\"labels\":{\"board\":\"devkit\"}},"
                               "\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                               "\"allocatable\":{\"memory\":\"4Mi\"}}}";
    store->put("/registry/nodes/node-b", node_b, true);
    std::string big = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"big\",\"namespace\":\"default\"},"
                      "\"spec\":{\"nodeSelector\":{\"board\":\"cardputer\"},"
                      "\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"memory\":\"64Ki\"}}}]}}";
    r = sched->schedule_pod_manifest(big);
    assert(!r.scheduled);
    const std::string expected = "0/2 nodes are available: 1 Insufficient memory, 1 node(s) didn't match Pod's node affinity/selector.";
    assert(r.failure_reason == expected);
    assert(big.find(expected) != std::string::npos);   // PodScheduled condition message
    std::vector<std::string> events;
    store->list("/registry/events/", events);
    bool found = false;
    for (const auto& e : events) {
        if (e.find("FailedScheduling") != std::string::npos && e.find(expected) != std::string::npos) found = true;
    }
    assert(found);

    // A node without the resource does not offer it
    std::string psram_pod = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"p\",\"namespace\":\"default\"},"
                            "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"memory\":\"1Ki\",\"esp32.io/psram\":\"64Ki\"}}}]}}";
    r = sched->schedule_pod_manifest(psram_pod);
    assert(!r.scheduled);
    assert(r.failure_reason == "0/2 nodes are available: 2 Insufficient esp32.io/psram.");
    std::cout << "[PASS] test_allocatable_accounting_and_failure_message" << std::endl;
}

// The loop tries a pod that does not fit again after a change that can make
// it fit, not while nothing changes: Node heartbeats that move nothing the
// filters read, and its own unchanged verdict, are no change. Attempts after
// changes wait for the pod's back-off. A cordon lifted binds the pod once its back-off ends.
static uint64_t failed_attempts(Store& store, const std::string& pod) {
    std::vector<std::string> events;
    store.list("/registry/events/", events);
    uint64_t n = 0;
    for (const auto& e : events) {
        kube32::models::JsonValue doc;
        std::string err;
        if (!kube32::models::parse_json(e, doc, err) || doc.get_string("reason", "") != "FailedScheduling") continue;
        if (doc.get("involvedObject")->get_string("name", "") == pod) n += static_cast<uint64_t>(doc.get_int("count", 1));
    }
    return n;
}

static bool eventually(const std::function<bool()>& cond, uint32_t ms = 2000) {
    const uint64_t end = kube32::osal::System::get_uptime_ms() + ms;
    while (kube32::osal::System::get_uptime_ms() < end) {
        if (cond()) return true;
        kube32::osal::Thread::sleep_ms(5);
    }
    return cond();
}

void test_retries_on_changes() {
    std::cout << "[RUN] test_retries_on_changes" << std::endl;
    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);
    auto node = [](const std::string& spec, const std::string& message) {
        return "{\"kind\":\"Node\",\"metadata\":{\"name\":\"node-a\",\"labels\":{\"kubernetes.io/hostname\":\"node-a\"}},\"spec\":" + spec +
               ",\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\",\"message\":\"" + message +
               "\"},{\"type\":\"MemoryPressure\",\"status\":\"False\"}],\"allocatable\":{\"memory\":\"240Ki\"}}}";
    };
    store->put("/registry/nodes/node-a", node("{\"unschedulable\":true}", "up"), true);
    SchedulerConfig cfg;
    cfg.reconcile_interval_ms = 50;
    cfg.resync_interval_ms = 60000;
    cfg.backoff_initial_ms = 100;
    cfg.backoff_max_ms = 400;
    auto sched = create_scheduler(*store, cfg);
    assert(sched->start());
    const std::string key = "/registry/pods/default/waiting";
    store->put(key, "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"waiting\",\"namespace\":\"default\"},"
                    "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"memory\":\"32Ki\"}}}]},"
                    "\"status\":{\"phase\":\"Pending\"}}", true);
    assert(eventually([&] {
        std::string json;
        store->get(key, json);
        return json.find("\"reason\":\"Unschedulable\"") != std::string::npos;
    }));

    // Nothing changes: no attempt and no write, heartbeats included
    kube32::osal::Thread::sleep_ms(300);
    const uint64_t attempts = failed_attempts(*store, "waiting");
    std::string before;
    uint64_t version = 0, after_version = 0;
    store->get_extended(key, before, &version);
    for (int i = 0; i < 10; ++i) {
        store->put("/registry/nodes/node-a", node("{\"unschedulable\":true}", "heartbeat " + std::to_string(i)), true);
        kube32::osal::Thread::sleep_ms(50);
    }
    kube32::osal::Thread::sleep_ms(500);
    assert(failed_attempts(*store, "waiting") == attempts);
    store->get_extended(key, before, &after_version);
    assert(after_version == version);

    // Changes keep coming: attempts wait for the back-off (100, 200, 400 ms).
    // The pods have completed: they take nothing from the node
    const uint64_t changes_from = kube32::osal::System::get_uptime_ms();
    for (int i = 0; i < 20; ++i) {
        store->put("/registry/pods/default/other-" + std::to_string(i),
                   "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"o\"},\"spec\":{\"nodeName\":\"node-a\"},\"status\":{\"phase\":\"Succeeded\"}}", true);
        kube32::osal::Thread::sleep_ms(50);
    }
    const uint64_t during = failed_attempts(*store, "waiting") - attempts;
    const uint64_t took = kube32::osal::System::get_uptime_ms() - changes_from;
    // The back-off doubles from 100 ms, so n attempts take at least 100 * (2^(n-1) - 1) ms.
    // A slow host stretches the 1 s of changes and allows more of them.
    uint64_t most = 1;
    while (100 * ((uint64_t{1} << most) - 1) <= took) ++most;
    // One more attempt can land between the last change and the count above
    most += 1;
    std::cout << "  " << during << " attempts in " << took << " ms of changes (at most " << most << ")" << std::endl;
    assert(during >= 1 && during <= most);

    // The cordon lifted: bound once the back-off ends
    const uint64_t t0 = kube32::osal::System::get_uptime_ms();
    store->put("/registry/nodes/node-a", node("{}", "up"), true);
    assert(eventually([&] {
        std::string json;
        store->get(key, json);
        return json.find("\"nodeName\":\"node-a\"") != std::string::npos;
    }));
    std::cout << "  bound " << kube32::osal::System::get_uptime_ms() - t0 << " ms after the uncordon" << std::endl;
    sched->stop();
    std::cout << "[PASS] test_retries_on_changes" << std::endl;
}

// Pod Overhead admission (runtime::set_pod_overhead): spec.overhead.memory
// is the kubelet's estimate of the pod beyond its linear memory, rounded up
// to whole KiB, with the linear memory taken as the kubelet takes it
void test_pod_overhead() {
    std::cout << "[RUN] test_pod_overhead" << std::endl;
    size_t n = 0;
    const kube32::runtime::EmbeddedImage* imgs = kube32::embedded::images(n);
    kube32::runtime::ImageRegistry::instance().add_all(imgs, n);
    const kube32::runtime::EmbeddedImage* echo = kube32::runtime::ImageRegistry::instance().resolve("wasm://embedded/echo");
    const kube32::runtime::EmbeddedImage* api = kube32::runtime::ImageRegistry::instance().resolve("wasm://embedded/tessera-api");
    assert(echo && api);
    auto parse = [](const std::string& json) {
        kube32::models::JsonValue doc;
        std::string err;
        bool ok = kube32::models::parse_json(json, doc, err);
        assert(ok);
        return doc;
    };
    auto kib_up = [](size_t bytes) { return (bytes + 1023) / 1024 * 1024; };
    auto overhead_of = [](const kube32::models::JsonValue& pod) {
        const kube32::models::JsonValue* o = pod.get("spec")->get("overhead");
        return o ? parse_quantity(o->get_string("memory", "")) : 0;
    };

    // No memory request or limit: one 64 KiB page, the operand stack sized
    // from the module (4 KiB at most)
    kube32::models::JsonValue pod = parse(R"({"metadata":{"name":"e"},"spec":{"containers":[{"name":"c","image":"wasm://embedded/echo"}]}})");
    size_t linear = 0;
    size_t need = kube32::runtime::estimate_pod_memory(echo->data, echo->len, 64 * 1024, 4 * 1024, true, kube32::runtime::EstimateSizes::BoardHeld, &linear);
    assert(linear == 64 * 1024);
    size_t got = kube32::runtime::set_pod_overhead(pod);
    assert(got == kib_up(need - linear) && overhead_of(pod) == got);
    // ... which the scheduler counts with the page the kubelet gives the pod
    assert(extract_pod_resource_requests(pod.to_string()).memory_bytes == 64 * 1024 + got);

    // A limit under a page: the limit is the linear memory; the request is
    // what the scheduler counts, the overhead on top
    pod = parse(R"({"metadata":{"name":"e"},"spec":{"containers":[{"name":"c","image":"wasm://embedded/echo",)"
                R"("resources":{"requests":{"memory":"3Ki"},"limits":{"memory":"3Ki"}}}]}})");
    need = kube32::runtime::estimate_pod_memory(echo->data, echo->len, 3 * 1024, 4 * 1024, true, kube32::runtime::EstimateSizes::BoardHeld, &linear);
    assert(linear == 3 * 1024);
    got = kube32::runtime::set_pod_overhead(pod);
    assert(got == kib_up(need - linear) && overhead_of(pod) == got);
    assert(extract_pod_resource_requests(pod.to_string()).memory_bytes == 3 * 1024 + got);

    // An esp32.io/psram request sizes the linear memory (the module's
    // initial pages count); the overhead is the rest, kube32.io/wasm-stack
    // sets the stack
    pod = parse(R"({"metadata":{"name":"a","annotations":{"kube32.io/wasm-stack":"8Ki"}},"spec":{"containers":[{"name":"c",)"
                R"("image":"wasm://embedded/tessera-api","resources":{"requests":{"memory":"16Ki","esp32.io/psram":"1280Ki"}}}]}})");
    need = kube32::runtime::estimate_pod_memory(api->data, api->len, 1280 * 1024, 8 * 1024, false, kube32::runtime::EstimateSizes::BoardHeld, &linear);
    got = kube32::runtime::set_pod_overhead(pod);
    assert(got == kib_up(need - linear) && got > 0);
    ResourceSpec req = extract_pod_resource_requests(pod.to_string());
    assert(req.memory_bytes == 16 * 1024 + got && req.psram_bytes == 1280 * 1024);

    // An image in the control plane's image store: estimated from its size.
    // The kubelet reads the module into the pod's memory, where it stays for
    // the module's lifetime (an embedded image is mapped from flash), so the
    // overhead counts its bytes: 20,000 in one 8-byte aligned chunk of the
    // pool block with its 8-byte header. The kubelet's estimate (board
    // sizes, with the compiler's scratch) counts the same bytes.
    pod = parse(R"({"metadata":{"name":"s","annotations":{"kube32.io/wasm-size":"20000","kube32.io/wasm-sha256":"ab"}},)"
                R"("spec":{"containers":[{"name":"c","image":"registry.local/s:1"}]}})");
    need = kube32::runtime::estimate_pod_memory(nullptr, 20000, 64 * 1024, 4 * 1024, true, kube32::runtime::EstimateSizes::BoardHeld, &linear, true);
    assert(need == kube32::runtime::estimate_pod_memory(nullptr, 20000, 64 * 1024, 4 * 1024, true, kube32::runtime::EstimateSizes::BoardHeld) + 20008);
    assert(kube32::runtime::estimate_pod_memory(nullptr, 20000, 64 * 1024, 4 * 1024, true, kube32::runtime::EstimateSizes::Board, nullptr, true) ==
           kube32::runtime::estimate_pod_memory(nullptr, 20000, 64 * 1024, 4 * 1024, true, kube32::runtime::EstimateSizes::Board) + 20008);
    assert(kube32::runtime::set_pod_overhead(pod) == kib_up(need - linear));

    // An image this node cannot see (a worker's card): no overhead, and a
    // stale one is taken away. A card image named like an embedded one is
    // the card's: the kubelet reads it from there
    pod = parse(R"({"metadata":{"name":"x"},"spec":{"overhead":{"memory":"99Ki"},"containers":[{"name":"c","image":"sd://x:1"}]}})");
    assert(kube32::runtime::set_pod_overhead(pod) == 0 && !pod.get("spec")->get("overhead"));
    assert(extract_pod_resource_requests(pod.to_string()).memory_bytes == 64 * 1024);
    pod = parse(R"({"metadata":{"name":"e"},"spec":{"containers":[{"name":"c","image":"wasm://sd/echo"}]}})");
    assert(kube32::runtime::set_pod_overhead(pod) == 0);
    std::cout << "[PASS] test_pod_overhead" << std::endl;
}

// The scheduler's memory per node: requests and overhead of every bound pod
// that has not terminated, against the node's fixed allocatable figure, for
// memory and for esp32.io/psram alike
void test_bound_pods_and_overhead_fill_a_node() {
    std::cout << "[RUN] test_bound_pods_and_overhead_fill_a_node" << std::endl;
    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);
    store->put("/registry/nodes/devkit",
               "{\"kind\":\"Node\",\"metadata\":{\"name\":\"devkit\"},\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
               "\"allocatable\":{\"memory\":\"1600Ki\",\"esp32.io/psram\":\"1600Ki\"}}}",
               true);
    auto pod = [](const std::string& name, const std::string& phase, const std::string& requests, const std::string& overhead) {
        return "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\"},\"spec\":{\"nodeName\":\"devkit\"," +
               (overhead.empty() ? std::string() : "\"overhead\":{\"memory\":\"" + overhead + "\"},") +
               "\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":" + requests + "}}]}" +
               (phase.empty() ? std::string() : ",\"status\":{\"phase\":\"" + phase + "\"}") + "}";
    };
    store->put("/registry/pods/default/api", pod("api", "Running", R"({"memory":"16Ki","esp32.io/psram":"1280Ki"})", "40Ki"), true);
    store->put("/registry/pods/default/web", pod("web", "Pending", R"({"memory":"4Ki"})", "12Ki"), true);
    store->put("/registry/pods/default/new", pod("new", "", R"({})", "10Ki"), true);       // bound, no status yet
    store->put("/registry/pods/default/gone", pod("gone", "Failed", R"({"memory":"900Ki"})", "10Ki"), true);
    store->put("/registry/pods/default/lost", pod("lost", "Unknown", R"({"memory":"8Ki"})", ""), true);
    const ResourceSpec used = calculate_node_allocated_resources(*store, "devkit");
    assert(used.memory_bytes == (16 + 40 + 4 + 12 + 64 + 10 + 8) * 1024);
    assert(used.psram_bytes == 1280 * 1024);

    // 1446Ki of memory left and 320Ki of PSRAM
    auto sched = create_scheduler(*store);
    auto try_pod = [&](const std::string& requests, const std::string& overhead) {
        std::string json = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"t\",\"namespace\":\"default\"},\"spec\":{" +
                           (overhead.empty() ? std::string() : "\"overhead\":{\"memory\":\"" + overhead + "\"},") +
                           "\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":" + requests + "}}]}}";
        ScheduleResult r = sched->pick_node(json, "");
        return r.scheduled ? std::string("devkit") : r.failure_reason;
    };
    assert(try_pod(R"({"memory":"1406Ki"})", "40Ki") == "devkit");
    assert(try_pod(R"({"memory":"1407Ki"})", "40Ki") == "0/1 nodes are available: 1 Insufficient memory.");
    assert(try_pod(R"({"memory":"1Ki","esp32.io/psram":"320Ki"})", "") == "devkit");
    assert(try_pod(R"({"memory":"1Ki","esp32.io/psram":"321Ki"})", "") == "0/1 nodes are available: 1 Insufficient esp32.io/psram.");
    std::cout << "[PASS] test_bound_pods_and_overhead_fill_a_node" << std::endl;
}

// Allocatable pods: a node full of pods that have not finished takes no more
// ("Too many pods"); finished pods do not count, and a node that reports no
// pods count has no such limit
void test_pod_count_limit() {
    std::cout << "[RUN] test_pod_count_limit" << std::endl;
    StoreOptions opts;
    opts.enable_flash_persistence = false;
    auto store = create_radix_store(opts);
    store->put("/registry/nodes/n1", "{\"kind\":\"Node\",\"metadata\":{\"name\":\"n1\"},\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                                     "\"allocatable\":{\"memory\":\"4Mi\",\"pods\":\"2\"}}}", true);
    auto pod = [](const std::string& name, const std::string& phase) {
        return "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\"},"
               "\"spec\":{\"nodeName\":\"n1\",\"containers\":[{\"name\":\"c\"}]},\"status\":{\"phase\":\"" + phase + "\"}}";
    };
    store->put("/registry/pods/default/a", pod("a", "Running"), true);
    store->put("/registry/pods/default/done", pod("done", "Succeeded"), true);
    auto sched = create_scheduler(*store);
    std::string next = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"next\",\"namespace\":\"default\"},\"spec\":{\"containers\":[{\"name\":\"c\"}]}}";
    ScheduleResult r = sched->schedule_pod_manifest(next);
    assert(r.scheduled && r.target_node == "n1");
    store->put("/registry/pods/default/b", pod("b", "Pending"), true);
    std::string third = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"third\",\"namespace\":\"default\"},\"spec\":{\"containers\":[{\"name\":\"c\"}]}}";
    r = sched->schedule_pod_manifest(third);
    assert(!r.scheduled && r.failure_reason == "0/1 nodes are available: 1 Too many pods.");
    store->put("/registry/nodes/n1", "{\"kind\":\"Node\",\"metadata\":{\"name\":\"n1\"},\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                                     "\"allocatable\":{\"memory\":\"4Mi\"}}}", true);
    third = "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"third\",\"namespace\":\"default\"},\"spec\":{\"containers\":[{\"name\":\"c\"}]}}";
    r = sched->schedule_pod_manifest(third);
    assert(r.scheduled);
    std::cout << "[PASS] test_pod_count_limit" << std::endl;
}

int main() {
    std::cout << "=== Running Kube32 Autonomous Scheduler Unit Tests ===" << std::endl;
    test_pod_count_limit();
    test_quantity_parser();
    test_predicates();
    test_scheduler_atomic_binding();
    test_deleted_node_reconciliation();
    test_allocatable_accounting_and_failure_message();
    test_pod_overhead();
    test_bound_pods_and_overhead_fill_a_node();
    test_retries_on_changes();
    std::cout << "=== All Scheduler Tests Passed Successfully ===" << std::endl;
    return 0;
}
