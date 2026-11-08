// The board API pods see: k32_display_info and k32_touch_read behind the
// capability firewall, the mock touch panel, and what the node advertises.

#include "pod_test_util.hpp"
#include "hal/mock_hal.hpp"

#include <cassert>
#include <iostream>

using namespace kube32;

static std::string info(const std::string& pod) { return k32test::call_pod(pod, "/info").body; }

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
    runtime::set_app_platform_hooks(hooks);

    // No grant: both imports refuse
    k32test::deploy(rt, "default/plain", wasm, 65536);
    assert(info("default/plain") == "{\"info\":-1,\"w\":0,\"h\":0,\"f\":0,\"touches\":-1}");
    assert(rt.terminate("default/plain"));

    // With the grants: the display's size, and the panel's points when touched
    k32test::deploy(rt, "default/panel", wasm, 65536, "\"DISPLAY\",\"TOUCH\"");
    assert(info("default/panel") == "{\"info\":0,\"w\":480,\"h\":480,\"f\":1,\"touches\":0}");
    hal::MockHal::instance().set_mock_touch_points({{120, 240, 3, 90}});
    assert(info("default/panel") == "{\"info\":0,\"w\":480,\"h\":480,\"f\":1,\"touches\":1,\"x\":120,\"y\":240,\"id\":3}");
    hal::MockHal::instance().set_mock_touch_points({});
    assert(rt.terminate("default/panel"));

    // A node with no display: the import reports it
    runtime::set_app_platform_hooks(runtime::AppPlatformHooks{});
    k32test::deploy(rt, "default/nodev", wasm, 65536, "\"DISPLAY\"");
    assert(info("default/nodev") == "{\"info\":-19,\"w\":0,\"h\":0,\"f\":0,\"touches\":-1}");
    assert(rt.terminate("default/nodev"));

    std::cout << "board API ok" << std::endl;
    return 0;
}
