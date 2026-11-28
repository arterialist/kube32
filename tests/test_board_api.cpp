// The board API pods see: k32_display_info, k32_touch_read and k32_led_set
// behind the capability firewall, the mock touch panel, and what the node
// advertises.

#include "pod_test_util.hpp"
#include "hal/capability_firewall.hpp"
#include "hal/mock_hal.hpp"
#include "kubelet/node_status_reporter.hpp"

#include <cassert>
#include <iostream>

using namespace kube32;

static std::string info(const std::string& pod) { return k32test::call_pod(pod, "/info").body; }

static uint32_t s_led_index = 99;
static uint32_t s_led_rgb = 0;

int main() {
    const std::vector<uint8_t> wasm = k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/tests/data/boardinfo.wasm");
    assert(!wasm.empty() && "tests/data/boardinfo.wasm");
    runtime::WasmRuntime rt;
    runtime::AppPlatformHooks hooks;
    hooks.display_info = [](hal::DisplayInfo& d) -> int32_t {
        d = {480, 480, hal::KUBE32_DISPLAY_RGB565};
        return hal::HAL_OK;
    };
    hooks.touch_read = [](hal::TouchPoint* out, uint32_t max) -> int32_t { return hal::MockHal::instance().touch_read(out, max); };
    hooks.led_set = [](uint32_t index, uint32_t rgb) -> int32_t {
        s_led_index = index;
        s_led_rgb = rgb;
        return hal::HAL_OK;
    };
    runtime::set_app_platform_hooks(hooks);

    // No grant: the imports refuse
    k32test::deploy(rt, "default/plain", wasm, 65536);
    assert(info("default/plain") == "{\"info\":-1,\"w\":0,\"h\":0,\"f\":0,\"touches\":-1,\"led\":-1}");
    assert(s_led_index == 99);
    assert(rt.terminate("default/plain"));

    // With the grants: the display's size, and the panel's points when touched
    k32test::deploy(rt, "default/panel", wasm, 65536, "\"DISPLAY\",\"TOUCH\"");
    assert(info("default/panel") == "{\"info\":0,\"w\":480,\"h\":480,\"f\":1,\"touches\":0,\"led\":-1}");
    hal::MockHal::instance().set_mock_touch_points({{120, 240, 3, 90}});
    assert(info("default/panel") == "{\"info\":0,\"w\":480,\"h\":480,\"f\":1,\"touches\":1,\"x\":120,\"y\":240,\"id\":3,\"led\":-1}");
    hal::MockHal::instance().set_mock_touch_points({});
    assert(rt.terminate("default/panel"));

    // The LED grant: the board gets the index and the color
    k32test::deploy(rt, "default/led", wasm, 65536, "\"LED\"");
    assert(info("default/led") == "{\"info\":-1,\"w\":0,\"h\":0,\"f\":0,\"touches\":-1,\"led\":0}");
    assert(s_led_index == 0 && s_led_rgb == 0x102030);
    assert(rt.terminate("default/led"));

    // A node with no display and no LED: the imports report it
    runtime::set_app_platform_hooks(runtime::AppPlatformHooks{});
    k32test::deploy(rt, "default/nodev", wasm, 65536, "\"DISPLAY\",\"LED\"");
    assert(info("default/nodev") == "{\"info\":-19,\"w\":0,\"h\":0,\"f\":0,\"touches\":-1,\"led\":-19}");
    assert(rt.terminate("default/nodev"));

    // The resource grants the LED like the capability does
    auto& fw = hal::CapabilityFirewall::instance();
    std::string err;
    const std::string led_pod = R"({"spec":{"containers":[{"name":"c","resources":{"requests":{"hardware.kube32.io/led":"1"}}}]}})";
    assert(fw.register_pod("default/res", led_pod, err));
    assert(fw.check_permission("default/res", hal::PeripheralType::Led, 0, hal::AccessMode::Write));
    assert(!fw.check_permission("default/res", hal::PeripheralType::Speaker, 0, hal::AccessMode::Write));
    fw.unregister_pod("default/res");

    // A board with an LED advertises the grant; one without does not
    kubelet::NodeStatusReporter reporter("node", "10.0.0.2");
    reporter.set_max_pods(4);
    hal::BoardProfile profile;
    profile.onboard_devices = hal::KUBE32_CAP_LED;
    fw.set_board_profile(profile);
    assert(reporter.build_registration_json(reporter.collect_telemetry()).find("\"hardware.kube32.io/led\":\"4\"") != std::string::npos);
    fw.set_board_profile(hal::BoardProfile{});
    assert(reporter.build_registration_json(reporter.collect_telemetry()).find("hardware.kube32.io/led") == std::string::npos);

    std::cout << "board API ok" << std::endl;
    return 0;
}
