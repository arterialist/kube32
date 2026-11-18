// The clusterview example (apps/clusterview) against an API server: the
// nodes and their hardware from Table lists and Node objects, a node that
// is not Ready and one that is deleted, the selected pods and their nodes,
// a pod that moves and one that is replaced on another node, a reading from
// the miniscale driver behind a Service, channel messages with their age,
// lists read a few rows a page, an API server that stops answering, and the
// layout on a 320x170 and a 240x135 screen. The pod draws into memory, and
// the checks read the text back from the pixels.
//
// With CLUSTERVIEW_SHOTS=<dir> the screens are also written there as raw
// RGB565 frames (<name>.rgb565, width x height pixels, row after row).
#include "pod_test_util.hpp"
#include "device_sims.hpp"
#include "api/http_server.hpp"
#include "app/service_client.hpp"
#include "hal/display_font.hpp"
#include "hal/linux_board.hpp"
#include "hal/mock_hal.hpp"
#include "runtime/pod_log.hpp"
#include "runtime/pod_memory_pool.h"
#include "scheduler/scheduler.hpp"
#include "storage/store.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace kube32;

namespace {

constexpr uint16_t kPort = 18761;
const std::string kApi = "http://127.0.0.1:" + std::to_string(kPort);
const std::string kPod = "default/clusterview";

// ---- the screen ---------------------------------------------------------
struct Screen {
    int w = 320, h = 170;
    std::vector<uint16_t> px;
    hal::FrameCanvas canvas;
    void resize(int nw, int nh) {
        w = nw;
        h = nh;
        px.assign(static_cast<size_t>(w) * h, 0);
        canvas.attach(reinterpret_cast<uint8_t*>(px.data()), w, h, static_cast<size_t>(w) * 2, hal::PixelLayout{});
    }
    uint16_t at(int x, int y) const { return px[static_cast<size_t>(y) * w + x]; }
};
Screen screen;

struct Hit {
    bool found = false;
    int x = 0, y = 0;
    uint16_t fg = 0, bg = 0;
};

// The glyph of `c` at (x, y) drawn `scale` times the font's size: the cell's
// sixth column is its background, the other pixels lit or not as the font says
bool glyph_at(int x, int y, char c, int scale, uint16_t& fg, uint16_t& bg) {
    if (x + 6 * scale > screen.w || y + 8 * scale > screen.h) return false;
    bg = screen.at(x + 5 * scale, y);
    const uint8_t* g = hal::kFont5x7[c - 32];
    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 8; ++row) {
            const uint16_t p = screen.at(x + col * scale, y + row * scale);
            const bool lit = row < 7 && (g[col] >> row & 1);
            if (lit != (p != bg)) return false;
            if (lit) fg = p;
        }
    }
    return true;
}

// Where `text` is on the screen at `scale`, its colours from its first glyph
Hit find(const std::string& text, int scale = 1) {
    Hit h;
    for (int y = 0; y + 8 * scale <= screen.h; ++y) {
        for (int x = 0; x + 6 * scale * static_cast<int>(text.size()) <= screen.w; ++x) {
            uint16_t fg = 0, bg = 0, f2, b2;
            if (!glyph_at(x, y, text[0], scale, fg, bg)) continue;
            size_t i = 1;
            while (i < text.size() && glyph_at(x + static_cast<int>(i) * 6 * scale, y, text[i], scale, f2, b2) && b2 == bg) i++;
            if (i == text.size()) return Hit{true, x, y, fg, bg};
        }
    }
    return h;
}

void shot(const std::string& name) {
    const char* dir = std::getenv("CLUSTERVIEW_SHOTS");
    if (!dir) return;
    const std::string path = std::string(dir) + "/" + name + "-" + std::to_string(screen.w) + "x" + std::to_string(screen.h) + ".rgb565";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(screen.px.data(), 2, screen.px.size(), f);
    std::fclose(f);
    std::cout << "  wrote " << path << std::endl;
}

void install_screen_hooks() {
    runtime::AppPlatformHooks hooks = runtime::app_platform_hooks();
    hooks.display_info = [](hal::DisplayInfo& d) -> int32_t {
        d = {screen.w, screen.h, hal::KUBE32_DISPLAY_RGB565};
        return hal::HAL_OK;
    };
    hooks.display_text = [](int32_t x, int32_t y, const char* t, size_t n, uint32_t fg, uint32_t bg) -> int32_t {
        screen.canvas.text(x, y, t, n, static_cast<uint16_t>(fg), static_cast<uint16_t>(bg));
        return 0;
    };
    hooks.display_rect = [](int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) -> int32_t {
        screen.canvas.rect(x, y, w, h, static_cast<uint16_t>(c));
        return 0;
    };
    hooks.display_blit = [](int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* p) -> int32_t {
        screen.canvas.blit(x, y, w, h, p);
        return 0;
    };
    runtime::set_app_platform_hooks(hooks);
}

// ---- the cluster ---------------------------------------------------------
std::string node_json(const std::string& name, bool ready, const std::string& capacity) {
    return R"({"kind":"Node","apiVersion":"v1","metadata":{"name":")" + name + R"(","labels":{"kubernetes.io/hostname":")" + name +
           R"("},"creationTimestamp":"2026-11-18T13:54:00Z"},"status":{"conditions":[{"type":"Ready","status":")" + (ready ? "True" : "Unknown") +
           R"(","reason":"KubeletReady","lastHeartbeatTime":"2026-11-18T13:59:42Z","lastTransitionTime":"2026-11-18T13:54:02Z"}],)"
           R"("addresses":[{"type":"InternalIP","address":"127.0.0.1"}],"nodeInfo":{"kubeletVersion":"v1.33.0-kube32","osImage":"kube32 test",)"
           R"("kernelVersion":"test","containerRuntimeVersion":"wasm3://0.9.2"},"capacity":{"memory":"322Ki","cpu":"2","pods":"64",)" + capacity +
           R"(},"allocatable":{"memory":"80Ki","cpu":"2","pods":"64",)" + capacity + "}}}";
}

const std::string kZeroCaps =
    R"("esp32.io/i2c":"1","hardware.kube32.io/ipc":"64","hardware.kube32.io/keyboard":"64","hardware.kube32.io/microphone":"64",)"
    R"("hardware.kube32.io/speaker":"64","hardware.kube32.io/tft-display":"64")";
const std::string kAtomCaps =
    R"("esp32.io/adc":"6","esp32.io/gpio":"9","esp32.io/i2c":"1","esp32.io/rmt-rx":"4","esp32.io/rmt-tx":"4","hardware.kube32.io/ipc":"64")";

std::string pod_json(const std::string& name, const std::string& node, const std::string& labels, const std::string& extra_meta = "") {
    return R"({"apiVersion":"v1","kind":"Pod","metadata":{"name":")" + name + R"(","namespace":"default","labels":{)" + labels + "}" + extra_meta +
           R"(,"creationTimestamp":"2026-11-18T13:54:00Z"},"spec":{)" + (node.empty() ? std::string() : "\"nodeName\":\"" + node + "\",") +
           R"("containers":[{"name":"c","image":"wasm://x","ports":[{"containerPort":7000}]}]},"status":{"phase":")" +
           (node.empty() ? "Pending" : "Running") + R"(","podIP":"127.0.0.1"}})";
}

std::string service_json(const std::string& name, const std::string& selector) {
    return R"({"apiVersion":"v1","kind":"Service","metadata":{"name":")" + name + R"(","namespace":"default"},"spec":{"selector":{)" + selector +
           R"(},"ports":[{"name":"http","port":80,"targetPort":7000}]}})";
}

// The miniscale driver on a simulated scale on bus 0
void deploy_scale_driver(runtime::WasmRuntime& rt) {
    const std::string id = "default/kitchen-scale-driver";
    const std::string manifest = R"({"metadata":{"name":"kitchen-scale-driver"},"spec":{"containers":[{"name":"driver","image":"wasm://x",)"
                                 R"("resources":{"requests":{"memory":"3Ki","esp32.io/i2c-bus-0":"1","hardware.kube32.io/ipc":"1"}}}]}})";
    std::string err;
    hal::CapabilityFirewall::instance().unregister_pod(id);
    bool ok = hal::CapabilityFirewall::instance().register_pod(id, manifest, err);
    assert(ok);
    const std::vector<uint8_t> wasm = k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/images/driver-miniscale.wasm");
    assert(!wasm.empty() && "images/driver-miniscale.wasm (make -C apps/drivers)");
    runtime::DeploySpec spec;
    spec.memory_limit_bytes = 3072;
    spec.stack_size_bytes = 4096;
    spec.env = {{"DEVICE_NAME", "kitchen-scale"}, {"I2C_ADDR", "0x26"}};
    ok = rt.deploy_workload_ex(id, wasm.data(), wasm.size(), spec, err);
    if (!ok) std::cout << "deploy " << id << ": " << err << std::endl;
    assert(ok && rt.start(id));
}

void deploy_view(runtime::WasmRuntime& rt) {
    const std::vector<uint8_t> wasm = k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/images/sd/clusterview.wasm");
    assert(!wasm.empty() && "images/sd/clusterview.wasm (make -C apps/clusterview)");
    runtime::DeploySpec spec;
    spec.env = {{"API_URL", kApi},
                {"TITLE", "kube32"},
                {"POD_SELECTOR", "app in (web,ir)"},
                {"READING_1", "scale http://kitchen-scale/ weight_g g"},
                {"TOPICS", "ir.received"},
                {"REFRESH_MS", "2000"},
                {"TIMEOUT_MS", "1000"}};
    k32test::deploy(rt, kPod, wasm, 16384, "\"DISPLAY\",\"SERVICE_CALL\"", spec);
}

uint32_t now_ms = 1000000;

// One refresh: the tick that starts it, then a tick 10 ms apart while the
// pod has calls left, as a node ticks it
void refresh() {
    now_ms += 2000;
    int calls = 0;
    int32_t rc;
    while ((rc = runtime::AppPodHost::instance().tick(kPod, now_ms)) > 0) {
        now_ms += 10;
        assert(++calls < 40);
    }
    assert(rc == 0);
}

// A tick between refreshes, `ms` later
void idle(uint32_t ms) {
    now_ms += ms;
    const int32_t rc = runtime::AppPodHost::instance().tick(kPod, now_ms);
    if (rc != 0) std::cout << "idle tick: " << rc << std::endl;
    assert(rc == 0);
}

bool logged(const std::string& needle) {
    std::vector<runtime::PodLogRecord> lines;
    runtime::PodLogStore::runtime().tail(kPod, 0, lines);
    for (const auto& l : lines) if (l.text.find(needle) != std::string::npos) return true;
    return false;
}

void expect(const std::string& text, int scale = 1) {
    if (!find(text, scale).found) {
        std::cout << "not on the screen: \"" << text << "\" at scale " << scale << std::endl;
        assert(false);
    }
}

void expect_not(const std::string& text, int scale = 1) {
    if (find(text, scale).found) {
        std::cout << "on the screen: \"" << text << "\" at scale " << scale << std::endl;
        assert(false);
    }
}

} // namespace

int main() {
    std::cout << "=== clusterview ===" << std::endl;
    kube32_pool_reserve(80 * 1024);   // a board without PSRAM
    hal::MockHal::instance().reset();
    hal::CapabilityFirewall::instance().reset();
    runtime::PodLogStore::runtime().clear();

    const std::string dir = "/tmp/kube32_test_clusterview";
    assert(std::system(("rm -rf " + dir + " && mkdir -p " + dir).c_str()) == 0);
    auto store = storage::create_radix_store(storage::StoreOptions{dir, false});
    store->put("/registry/nodes/node-a", node_json("node-a", true, kZeroCaps), true);
    store->put("/registry/nodes/node-b", node_json("node-b", true, kAtomCaps), true);
    store->put("/registry/pods/default/web-6f7c9-abcde", pod_json("web-6f7c9-abcde", "node-a", R"("app":"web")"), true);
    store->put("/registry/pods/default/ir", pod_json("ir", "node-b", R"("app":"ir")"), true);
    store->put("/registry/pods/default/other", pod_json("other", "node-a", R"("app":"other")"), true);
    store->put("/registry/pods/default/kitchen-scale-driver",
               pod_json("kitchen-scale-driver", "node-a", R"("kube32.io/device":"kitchen-scale","kube32.io/driver":"miniscale")"), true);
    store->put("/registry/services/default/kitchen-scale", service_json("kitchen-scale", R"("kube32.io/device":"kitchen-scale")"), true);
    auto sched = scheduler::create_scheduler(*store);
    api::HttpServerOptions o;
    o.host = "127.0.0.1";
    o.port = kPort;
    o.workers = 2;
    o.worker_stack_bytes = 262144;
    auto server = std::make_unique<api::HttpServer>(o, *store, sched.get());
    assert(server->start());
    app::ServiceClient client(app::make_store_service_resolver(*store));
    app::install_service_client(&client);
    install_screen_hooks();

    auto scale = std::make_shared<k32test::MiniScale>();
    hal::MockHal::instance().register_mock_i2c_device(0, 0x26, scale);
    auto rt = runtime::create_wasm_runtime();
    deploy_scale_driver(*rt);
    runtime::AppPodHost::instance().tick("default/kitchen-scale-driver", now_ms);

    // ---- 320x170: names large, tags, pods, the scale, waiting for messages
    screen.resize(320, 170);
    deploy_view(*rt);
    assert(logged("clusterview: 320x170, API " + kApi + ", pods app in (web,ir) in default, 1 reading(s)"));
    expect("reading the API server");
    refresh();
    expect("kube32");
    expect("2/2 ready");
    const Hit a = find("node-a", 2), b = find("node-b", 2);
    assert(a.found && b.found && b.y > a.y && a.bg == b.bg);
    const uint16_t bg = a.bg;
    expect("disp keys spk mic i2c");
    expect("i2c ir gpio9");
    expect("web-6f7c9-abcde");
    expect("on node-a");
    expect("on node-b");
    expect_not("other");
    expect("scale");
    expect("246.90", 3);
    expect(" g");
    expect("waiting for ir.received");
    shot("start");

    // ---- a message, and its age a minute later
    const std::string code = "nec 0x04 0x08";
    assert(runtime::AppPodHost::instance().event(kPod, "ir.received", reinterpret_cast<const uint8_t*>(code.data()), code.size()) == 0);
    expect("nec 0x04 0x08");
    idle(0);
    expect("ir.received");
    expect("now");
    {
        // JSON reads without its punctuation
        const std::string j = R"({"device":"kitchen-scale","weight_g":12.5})";
        runtime::AppPodHost::instance().event(kPod, "device.kitchen-scale", reinterpret_cast<const uint8_t*>(j.data()), j.size());
        expect("device kitchen-scale");
    }
    now_ms += 59000;
    refresh();   // 61 s after the message
    expect("1m");

    // ---- a node that is not Ready: its row in the accent colour
    store->put("/registry/nodes/node-b", node_json("node-b", false, kAtomCaps), true);
    refresh();
    expect("1 not ready");
    const Hit nb = find("node-b", 2);
    assert(nb.found && nb.bg != bg);
    const uint16_t accent = nb.bg;
    const Hit nr = find("not ready i2c ir");
    assert(nr.found && nr.bg == accent);
    assert(find("node-a", 2).bg == bg);
    shot("not-ready");

    // ---- a pod that moves keeps its name: highlighted with the node it left
    store->put("/registry/pods/default/web-6f7c9-abcde", pod_json("web-6f7c9-abcde", "node-b", R"("app":"web")"), true);
    refresh();
    Hit mv = find("node-a > node-b");
    assert(mv.found && mv.bg == accent);
    shot("moved");
    for (int i = 0; i < 4; ++i) refresh();   // 8 s later
    expect_not("node-a > node-b");
    assert(find("on node-b").found);

    // ---- a replacement of the same owner on another node, bound after a refresh
    store->remove("/registry/pods/default/web-6f7c9-abcde");
    store->put("/registry/pods/default/web-6f7c9-fghij", pod_json("web-6f7c9-fghij", "", R"("app":"web")"), true);
    refresh();
    expect("web-6f7c9-fghij");
    expect("Pending");
    store->put("/registry/pods/default/web-6f7c9-fghij", pod_json("web-6f7c9-fghij", "node-a", R"("app":"web")"), true);
    refresh();
    mv = find("node-b > node-a");
    assert(mv.found && mv.bg == accent);

    // ---- a deleted node leaves the screen; its row is cleared
    store->remove("/registry/nodes/node-b");
    refresh();
    expect_not("node-b", 2);
    expect("1/1 ready");

    // ---- more nodes than a page holds: lists read 4 rows a page
    for (int i = 0; i < 6; ++i) {
        const std::string n = "worker-" + std::to_string(i);
        store->put("/registry/nodes/" + n, node_json(n, true, kAtomCaps), true);
    }
    refresh();
    expect("7/7 ready");
    for (int i = 0; i < 6; ++i) expect("worker-" + std::to_string(i));   // small names: seven rows
    shot("seven-nodes");

    // ---- the scale's Service without a ready pod
    store->remove("/registry/pods/default/kitchen-scale-driver");
    refresh();
    expect("scale no service");
    expect("--", 3);
    store->put("/registry/pods/default/kitchen-scale-driver",
               pod_json("kitchen-scale-driver", "node-a", R"("kube32.io/device":"kitchen-scale","kube32.io/driver":"miniscale")"), true);

    // ---- the API server stops answering: said in the header and the log
    server->stop();
    refresh();
    expect("api refused");
    assert(logged("clusterview: API " + kApi + ": refused"));
    shot("api-down");
    assert(server->start());
    refresh();
    expect("7/7 ready");
    assert(logged("answers again"));
    assert(rt->terminate(kPod));

    // ---- 240x135, the Cardputer's panel: the reading smaller
    for (int i = 0; i < 6; ++i) store->remove("/registry/nodes/worker-" + std::to_string(i));
    store->put("/registry/nodes/node-b", node_json("node-b", true, kAtomCaps), true);
    screen.resize(240, 135);
    deploy_view(*rt);
    refresh();
    shot("cardputer");
    expect("2/2 ready");
    expect("node-a", 2);
    expect("node-b", 2);
    expect("disp keys spk mic i2c");
    expect("web-6f7c9-fghij");
    expect("246.90", 2);
    assert(rt->terminate(kPod));

    rt->terminate("default/kitchen-scale-driver");
    app::install_service_client(nullptr);
    server->stop();
    hal::CapabilityFirewall::instance().reset();
    assert(std::system(("rm -rf " + dir).c_str()) == 0);
    std::cout << "[PASS] clusterview" << std::endl;
    return 0;
}
