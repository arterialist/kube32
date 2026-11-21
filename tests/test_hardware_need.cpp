// Hardware-following pods (docs/migration.md#hardware-following-pods): the
// device names and the needs annotation, the device grants that do not keep
// such a pod from moving, the firewall's grants on a node (the devices the
// spec lists and the node has), k32_need's results for a following and a
// plain pod (tests/data/boardinfo.c), the needs the kubelet records, and the
// scheduler's filter on the current need and kube32.io/prefer-nodes.

#include "pod_test_util.hpp"
#include "hal/capability_firewall.hpp"
#include "models/migration.hpp"
#include "runtime/k32_app_abi.h"
#include "runtime/pod_snapshot.hpp"
#include "scheduler/scheduler.hpp"
#include "storage/store.hpp"

#include <algorithm>
#include <iostream>

using namespace kube32;
using models::JsonValue;

#define CHECK(c)                                                                                \
    do {                                                                                        \
        if (!(c)) {                                                                             \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #c << std::endl;    \
            std::abort();                                                                       \
        }                                                                                       \
    } while (0)

namespace {

JsonValue parse(const std::string& json) {
    JsonValue d;
    std::string err;
    CHECK(models::parse_json(json, d, err));
    return d;
}

const std::string kFollow = R"("kube32.io/migration":"Auto","kube32.io/placement":"hardware")";

// A pod with `annotations` whose container requests `requests` and adds `caps`
std::string pod(const std::string& annotations, const std::string& requests, const std::string& caps = "") {
    return R"({"metadata":{"name":"job","namespace":"default","annotations":{)" + annotations +
           R"(}},"spec":{"containers":[{"name":"c","image":"wasm://x","resources":{"requests":{)" + requests + "}}" +
           (caps.empty() ? std::string() : R"(,"securityContext":{"capabilities":{"add":[)" + caps + "]}}") + "}]}}";
}

// A Ready node that offers `devices` (resource names after hardware.kube32.io/)
std::string node(const std::string& name, const std::vector<std::string>& devices, const std::string& board = "generic") {
    std::string alloc = R"("memory":"300Ki","pods":"8","hardware.kube32.io/chip-temp":"8")";
    for (const auto& d : devices) alloc += R"(,"hardware.kube32.io/)" + d + R"(":"8")";
    return R"({"kind":"Node","metadata":{"name":")" + name + R"(","labels":{"kube32.io/board":")" + board +
           R"("}},"status":{"conditions":[{"type":"Ready","status":"True"}],"allocatable":{)" + alloc + "}}}";
}

void test_names() {
    using hal::KUBE32_CAP_DISPLAY;
    CHECK(models::device_bit("hardware.kube32.io/tft-display") == KUBE32_CAP_DISPLAY);
    CHECK(models::device_bit("tft-display") == KUBE32_CAP_DISPLAY);
    CHECK(models::device_bit("DISPLAY") == KUBE32_CAP_DISPLAY && models::device_bit("TFT_DISPLAY") == KUBE32_CAP_DISPLAY);
    CHECK(models::device_bit("AUDIO") == hal::KUBE32_CAP_SPEAKER && models::device_bit("MIC") == hal::KUBE32_CAP_MICROPHONE);
    CHECK(models::device_bit("chip-temp") == hal::KUBE32_CAP_CHIP_TEMP);
    // Grants that name wiring of one board, storage or IPC are no devices here
    for (const char* other : {"hardware.kube32.io/sd", "SD", "hardware.kube32.io/ipc", "IPC", "esp32.io/gpio-4", "display", "", "led "}) {
        CHECK(models::device_bit(other) == 0);
    }
    uint32_t d = 0;
    CHECK(models::parse_devices(" keyboard , DISPLAY,,hardware.kube32.io/led ", d));
    CHECK(d == (KUBE32_CAP_DISPLAY | hal::KUBE32_CAP_KEYBOARD | hal::KUBE32_CAP_LED));
    CHECK(models::devices_text(d) == "hardware.kube32.io/tft-display,hardware.kube32.io/keyboard,hardware.kube32.io/led");
    CHECK(models::parse_devices("", d) && d == 0 && models::devices_text(0).empty());
    CHECK(!models::parse_devices("speaker,esp32.io/gpio-4", d));

    CHECK(models::follows_hardware(parse(pod(kFollow, ""))));
    CHECK(!models::follows_hardware(parse(pod(R"("kube32.io/placement":"hardware")", ""))));
    CHECK(!models::follows_hardware(parse(pod(R"("kube32.io/migration":"Auto","kube32.io/placement":"fastest")", ""))));
    CHECK(models::pod_needs(parse(pod(kFollow + R"(,"kube32.io/needs":"hardware.kube32.io/speaker")", ""))) == hal::KUBE32_CAP_SPEAKER);

    const JsonValue n = parse(node("s31", {"speaker", "microphone", "led"}, "s31-coreboard"));
    CHECK(models::node_devices(n) == (hal::KUBE32_CAP_SPEAKER | hal::KUBE32_CAP_MICROPHONE | hal::KUBE32_CAP_LED | hal::KUBE32_CAP_CHIP_TEMP));
    CHECK(models::node_devices(parse(R"({"status":{"allocatable":{"hardware.kube32.io/led":"0"}}})")) == 0);

    const JsonValue p = parse(pod(R"("kube32.io/prefer-nodes":"devkit, kube32.io/board=s31-coreboard")", ""));
    CHECK(models::preferred_rank(p, parse(node("devkit", {}))) == 0);
    CHECK(models::preferred_rank(p, n) == 1);
    CHECK(models::preferred_rank(p, parse(node("cardputer", {}, "cardputer"))) == -1);
    CHECK(models::preferred_rank(parse(pod("", "")), n) == -1);
    std::cout << "[PASS] Device names, the needs annotation, node devices and preferences" << std::endl;
}

void test_blockers() {
    const std::string devices = R"("hardware.kube32.io/tft-display":"1","hardware.kube32.io/keyboard":"1","hardware.kube32.io/speaker":"1","hardware.kube32.io/led":"1")";
    const std::string caps = R"("DISPLAY","KEYBOARD","SPEAKER","CHIP_TEMP","SERVICE_CALL")";
    // A following pod: its device grants go with it
    CHECK(runtime::static_migration_blockers(pod(kFollow, devices, caps)).empty());
    // The same pod without the placement: every grant keeps it in place
    const auto plain = runtime::static_migration_blockers(pod(R"("kube32.io/migration":"Auto")", devices, caps));
    CHECK(std::find(plain.begin(), plain.end(), "grant hardware.kube32.io/led") != plain.end());
    CHECK(std::find(plain.begin(), plain.end(), "grant capability DISPLAY") != plain.end());
    // Pins and volumes keep a following pod in place too
    const auto pins = runtime::static_migration_blockers(pod(kFollow, devices + R"(,"esp32.io/gpio-4":"1","hardware.kube32.io/sd":"1")", R"("SD")"));
    CHECK(pins.size() == 3);
    CHECK(pins[0] == "grant esp32.io/gpio-4" && pins[1] == "grant hardware.kube32.io/sd" && pins[2] == "grant capability SD");
    std::cout << "[PASS] Blockers: a following pod's device grants move, pins and volumes do not" << std::endl;
}

void set_devices(uint32_t devices) {
    hal::BoardProfile profile = hal::CapabilityFirewall::instance().board_profile();
    profile.onboard_devices = devices;
    hal::CapabilityFirewall::instance().set_board_profile(profile);
}

void test_grants() {
    hal::CapabilityFirewall& fw = hal::CapabilityFirewall::instance();
    set_devices(hal::KUBE32_CAP_LED);
    std::string err;
    const std::string requests = R"("hardware.kube32.io/tft-display":"1","hardware.kube32.io/led":"1")";
    CHECK(fw.register_pod("default/follow", pod(kFollow, requests), err));
    CHECK(fw.register_pod("default/plain", pod("", requests), err));
    // The following pod holds what it lists and the node has
    CHECK(fw.check_permission("default/follow", hal::PeripheralType::Led, 0, hal::AccessMode::Write));
    CHECK(!fw.check_permission("default/follow", hal::PeripheralType::Display, 0, hal::AccessMode::Write));
    uint32_t listed = 0, held = 0;
    bool follows = false;
    CHECK(fw.pod_devices("default/follow", listed, held, follows) && follows);
    CHECK(listed == (hal::KUBE32_CAP_DISPLAY | hal::KUBE32_CAP_LED) && held == hal::KUBE32_CAP_LED);
    // A plain pod holds what it lists
    CHECK(fw.check_permission("default/plain", hal::PeripheralType::Display, 0, hal::AccessMode::Write));
    CHECK(fw.pod_devices("default/plain", listed, held, follows) && !follows && held == listed);
    CHECK(fw.node_devices() == (hal::KUBE32_CAP_LED | hal::KUBE32_CAP_CHIP_TEMP));
    fw.unregister_pod("default/follow");
    fw.unregister_pod("default/plain");
    CHECK(!fw.pod_devices("default/follow", listed, held, follows));
    std::cout << "[PASS] Grants: a following pod holds the devices it lists that the node has" << std::endl;
}

std::string need(const std::string& pod_id, const std::string& list) { return k32test::call_pod(pod_id, "/need", "POST", list).body; }
std::string rc(int32_t v) { return "{\"need\":" + std::to_string(v) + "}"; }
// The need the kubelet would record next, "<pod> <devices>", empty for none
std::string unrecorded() {
    std::string pod;
    uint32_t devices = 0;
    return runtime::AppPodHost::instance().unrecorded_need(pod, devices) ? pod + " " + models::devices_text(devices) : std::string();
}

void deploy(runtime::WasmRuntime& rt, const std::string& pod_id, const std::string& manifest, uint32_t recorded = 0) {
    std::string err;
    hal::CapabilityFirewall::instance().unregister_pod(pod_id);
    CHECK(hal::CapabilityFirewall::instance().register_pod(pod_id, manifest, err));
    runtime::DeploySpec spec;
    spec.memory_limit_bytes = 65536;
    spec.stack_size_bytes = 4096;
    spec.recorded_need = recorded;
    const std::vector<uint8_t> wasm = k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/tests/data/boardinfo.wasm");
    CHECK(!wasm.empty());
    CHECK(rt.deploy_workload_ex(pod_id, wasm.data(), wasm.size(), spec, err));
    CHECK(rt.start(pod_id));
}

void test_need_call() {
    runtime::WasmRuntime rt;
    runtime::AppPlatformHooks hooks;
    hooks.led_set = [](uint32_t, uint32_t) -> int32_t { return hal::HAL_OK; };
    hooks.display_info = [](hal::DisplayInfo& d) -> int32_t {
        d = {240, 135, hal::KUBE32_DISPLAY_RGB565};
        return hal::HAL_OK;
    };
    runtime::set_app_platform_hooks(hooks);
    set_devices(hal::KUBE32_CAP_LED);
    runtime::AppPodHost& host = runtime::AppPodHost::instance();
    const std::string requests = R"("hardware.kube32.io/tft-display":"1","hardware.kube32.io/keyboard":"1","hardware.kube32.io/led":"1")";

    // A following pod on a node with an LED only
    deploy(rt, "default/job", pod(kFollow, requests));
    CHECK(unrecorded().empty());
    CHECK(need("default/job", "led") == rc(K32_OK));
    CHECK(need("default/job", "hardware.kube32.io/led, LED") == rc(K32_OK));
    CHECK(unrecorded() == "default/job hardware.kube32.io/led");
    CHECK(need("default/job", "") == rc(K32_OK));
    CHECK(unrecorded().empty());   // back to what the Pod shows
    CHECK(need("default/job", "DISPLAY,keyboard") == rc(K32_NEED_MOVING));
    CHECK(unrecorded() == "default/job hardware.kube32.io/tft-display,hardware.kube32.io/keyboard");
    host.need_recorded("default/job", hal::KUBE32_CAP_DISPLAY | hal::KUBE32_CAP_KEYBOARD);
    CHECK(unrecorded().empty());
    CHECK(need("default/job", "DISPLAY,keyboard") == rc(K32_NEED_MOVING));   // asked again: still not met, nothing new to record
    CHECK(unrecorded().empty());
    // Not listed, no device, too long
    CHECK(need("default/job", "speaker") == rc(K32_ERR_PERM));
    CHECK(need("default/job", "led,esp32.io/gpio-4") == rc(K32_ERR_INVAL));
    CHECK(need("default/job", std::string(K32_NEED_MAX_BYTES + 1, ' ')) == rc(K32_ERR_INVAL));
    // The firewall refuses what the node lacks, though the spec lists it
    CHECK(k32test::call_pod("default/job", "/info").body == "{\"info\":-1,\"w\":0,\"h\":0,\"f\":0,\"touches\":-1,\"led\":0}");
    CHECK(rt.terminate("default/job"));
    CHECK(unrecorded().empty());

    // Restored where its Pod records a need: the same need records nothing
    set_devices(hal::KUBE32_CAP_DISPLAY | hal::KUBE32_CAP_KEYBOARD);
    deploy(rt, "default/job", pod(kFollow + R"(,"kube32.io/needs":"hardware.kube32.io/tft-display,hardware.kube32.io/keyboard")", requests),
           hal::KUBE32_CAP_DISPLAY | hal::KUBE32_CAP_KEYBOARD);
    CHECK(need("default/job", "DISPLAY,KEYBOARD") == rc(K32_OK));
    CHECK(unrecorded().empty());
    CHECK(k32test::call_pod("default/job", "/info").body == "{\"info\":0,\"w\":240,\"h\":135,\"f\":1,\"touches\":-1,\"led\":-1}");
    CHECK(rt.terminate("default/job"));

    // A plain pod: the node has it or not, nothing is recorded
    set_devices(hal::KUBE32_CAP_LED);
    deploy(rt, "default/plain", pod("", requests));
    CHECK(need("default/plain", "led") == rc(K32_OK));
    CHECK(need("default/plain", "tft-display") == rc(K32_ERR_NODEV));
    CHECK(unrecorded().empty());
    CHECK(rt.terminate("default/plain"));
    set_devices(0);
    runtime::set_app_platform_hooks(runtime::AppPlatformHooks{});
    std::cout << "[PASS] k32_need: met, moving, not listed, no device, plain pods; the needs to record" << std::endl;
}

void test_scheduler() {
    storage::StoreOptions o;
    o.enable_flash_persistence = false;
    auto store = storage::create_radix_store(o);
    store->put("/registry/nodes/cardputer", node("cardputer", {"tft-display", "keyboard", "speaker", "microphone"}, "cardputer"), true);
    store->put("/registry/nodes/zero", node("zero", {"tft-display", "keyboard", "speaker", "microphone"}, "cardputer-zero"), true);
    store->put("/registry/nodes/s31", node("s31", {"speaker", "microphone", "led"}, "s31-coreboard"), true);
    store->put("/registry/nodes/devkit", node("devkit", {}), true);
    auto sched = scheduler::create_scheduler(*store);
    const std::string all = R"("memory":"16Ki","hardware.kube32.io/tft-display":"1","hardware.kube32.io/keyboard":"1","hardware.kube32.io/speaker":"1","hardware.kube32.io/led":"1")";
    const std::string prefer = R"(,"kube32.io/prefer-nodes":"kube32.io/board=generic,kube32.io/board=cardputer")";
    auto pick = [&](const std::string& annotations, const std::string& exclude = "") { return sched->pick_node(pod(annotations, all), exclude); };

    // No need: any node; the preference picks the devkit, then the next one
    CHECK(pick(kFollow + prefer).target_node == "devkit");
    CHECK(pick(kFollow + prefer, "devkit").target_node == "cardputer");
    // The step's need, not the union the spec lists
    CHECK(pick(kFollow + prefer + R"(,"kube32.io/needs":"hardware.kube32.io/tft-display,hardware.kube32.io/keyboard")").target_node == "cardputer");
    CHECK(pick(kFollow + R"(,"kube32.io/prefer-nodes":"zero","kube32.io/needs":"hardware.kube32.io/keyboard")").target_node == "zero");
    CHECK(pick(kFollow + prefer + R"(,"kube32.io/needs":"hardware.kube32.io/speaker,hardware.kube32.io/led")").target_node == "s31");
    // Nothing has it
    const scheduler::ScheduleResult none = pick(kFollow + R"(,"kube32.io/needs":"hardware.kube32.io/touch")");
    CHECK(!none.scheduled);
    CHECK(none.failure_reason == "0/4 nodes are available: 4 Insufficient hardware.kube32.io/touch.");
    // A pod that does not follow its hardware is not filtered on the annotation
    CHECK(sched->pick_node(pod(R"("kube32.io/needs":"hardware.kube32.io/touch")", all), "").scheduled);
    std::cout << "[PASS] Scheduler: the current need filters, kube32.io/prefer-nodes ranks" << std::endl;
}

} // namespace

int main() {
    test_names();
    test_blockers();
    test_grants();
    test_need_call();
    test_scheduler();
    std::cout << "All hardware need tests passed" << std::endl;
    return 0;
}
