// I2C device discovery: the firewall lends a free bus to the node and holds
// pod admission off while it is lent, scans turn the devices on the bus into
// node labels, a bus a pod holds keeps its last result, and the kubelet
// patches label changes onto its Node. Behind a PCA9548A every channel is
// scanned alone and catalog parts are identified (once, and not while a
// device grant holds them); a bus shared by device grants is scanned beside
// the pods; the kubelet patches the parts found as a node annotation.
#include "hal/capability_firewall.hpp"
#include "hal/i2c_discovery.hpp"
#include "hal/mock_hal.hpp"
#include "hal/pod_buses.hpp"
#include "models/device.hpp"
#include "kubelet/kubelet.hpp"
#include "kubelet/kube_api_client.hpp"
#include "runtime/wasm_runtime.hpp"
#include "osal/thread.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <iostream>
#include <string>
#include <thread>

using namespace kube32;
using namespace kube32::hal;

namespace {

std::string pod(const std::string& requests) {
    return R"({"metadata":{"name":"p","namespace":"default"},"spec":{"containers":[{"name":"c","resources":{"requests":{)" + requests + "}}}]}}";
}

BoardProfile cardputer_profile() {
    BoardProfile p;
    p.name = "cardputer";
    p.i2c_bus_mask = 0x1;
    p.uart_port_mask = 0x2;
    p.i2c_pins[0] = {2, 1};
    p.i2c_pins[1] = {};
    p.uart_pins[1] = {2, 1};
    return p;
}

class Rfid : public MockI2cDevice {
public:
    int32_t write(const uint8_t*, size_t len) override { return static_cast<int32_t>(len); }
    int32_t read(uint8_t* buf, size_t len) override { for (size_t i = 0; i < len; ++i) buf[i] = 0; return static_cast<int32_t>(len); }
};

void test_lending() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    fw.set_board_profile(cardputer_profile());
    BusPins pins;
    assert(!fw.lend_i2c_bus(1, pins));   // not offered on this board
    assert(fw.lend_i2c_bus(0, pins));
    assert(pins.a == 2 && pins.b == 1);
    assert(!fw.lend_i2c_bus(0, pins));   // already lent

    // An admission waits for the bus to come back, then gets it
    std::atomic<bool> done{false};
    bool ok = false;
    std::string err;
    std::thread admit([&] {
        ok = fw.register_pod("default/p", pod(R"("esp32.io/i2c-bus-0":"1")"), err);
        done = true;
    });
    osal::Thread::sleep_ms(60);
    assert(!done.load());
    fw.return_i2c_bus(0);
    admit.join();
    assert(ok);

    // Held by a pod: neither the bus nor its pins are lent
    assert(!fw.lend_i2c_bus(0, pins));
    fw.unregister_pod("default/p");
    assert(fw.register_pod("default/u", pod(R"("esp32.io/uart-1":"1")"), err));   // UART1 on the same Grove pins
    assert(!fw.lend_i2c_bus(0, pins));
    fw.unregister_pod("default/u");
    assert(fw.lend_i2c_bus(0, pins));
    fw.return_i2c_bus(0);
    fw.reset();
    std::cout << "[PASS] test_lending" << std::endl;
}

void test_labels() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    fw.set_board_profile(cardputer_profile());
    MockHal::instance().reset();   // a BMP280 answers at 0x76 on bus 0
    MockHal::instance().register_mock_i2c_device(0, 0x28, std::make_shared<Rfid>());
    I2cDiscovery d;
    assert(d.scan());
    auto labels = d.labels();
    assert(labels.size() == 2);
    assert(labels["i2c.kube32.io/0x28"] == "bus-0");
    assert(labels["i2c.kube32.io/0x76"] == "bus-0");
    assert(!d.scan());   // nothing changed

    // A pod holds the bus: the scan leaves it alone and keeps what it found
    std::string err;
    assert(fw.register_pod("default/p", pod(R"("esp32.io/i2c-bus-0":"1")"), err));
    MockHal::instance().reset();   // the RFID reader is unplugged
    assert(!d.scan());
    assert(d.labels().count("i2c.kube32.io/0x28"));
    fw.unregister_pod("default/p");
    assert(d.scan());
    assert(d.labels().size() == 1 && d.labels().count("i2c.kube32.io/0x76"));

    // A bus the board does not offer on default pins is not scanned
    BoardProfile bare;
    bare.i2c_pins[0] = {};
    fw.set_board_profile(bare);
    I2cDiscovery none;
    assert(!none.scan());
    assert(none.labels().empty());
    fw.reset();
    std::cout << "[PASS] test_labels" << std::endl;
}

class NodeClient : public kubelet::KubeApiClient {
public:
    std::string registration;
    std::vector<std::string> patches;
    bool conflict{false};
    std::string existing_node;
    bool register_node(std::string_view json, bool& c) override { registration = std::string(json); c = conflict; return true; }
    bool patch_node_status(const std::string&, std::string_view p) override { patches.emplace_back(p); return true; }
    bool list_pods(std::vector<models::KubePod>&) override { return true; }
    bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>&) override { return true; }
    bool patch_pod_status(const std::string&, const std::string&, const std::string&) override { return true; }
    int get(const std::string& path, std::string& body) override {
        if (path == "/api/v1/nodes/node-a" && !existing_node.empty()) { body = existing_node; return 200; }
        body = "{\"items\":[]}";
        return 200;
    }
};

void test_kubelet_patches_labels() {
    std::map<std::string, std::string> found{{"i2c.kube32.io/0x28", "bus-0"}};
    auto rt = runtime::create_wasm_runtime();
    kubelet::KubeletConfig cfg;
    cfg.node_name = "node-a";
    cfg.node_label_source = [&] { return found; };
    cfg.node_label_prefix = "i2c.kube32.io/";
    auto client = std::make_unique<NodeClient>();
    NodeClient* c = client.get();
    c->conflict = true;   // the Node outlived the kubelet, with a label from the last boot
    c->existing_node = R"({"metadata":{"name":"node-a","labels":{"i2c.kube32.io/0x28":"bus-0","i2c.kube32.io/0x50":"bus-0","team":"a"}}})";
    kubelet::Kubelet k(cfg, *rt, std::move(client));
    k.reconcile_node_registration();
    assert(c->registration.find("\"i2c.kube32.io/0x28\":\"bus-0\"") != std::string::npos);
    // After the registration patch the stale label from the last boot goes; others stay
    assert(std::find(c->patches.begin(), c->patches.end(), R"({"metadata":{"labels":{"i2c.kube32.io/0x50":null}}})") != c->patches.end());

    c->patches.clear();
    k.refresh_node_labels();
    assert(c->patches.empty());   // unchanged
    found = {{"i2c.kube32.io/0x76", "bus-0"}};
    k.refresh_node_labels();
    assert(c->patches.size() == 1);
    assert(c->patches[0] == R"({"metadata":{"labels":{"i2c.kube32.io/0x76":"bus-0","i2c.kube32.io/0x28":null}}})");
    std::cout << "[PASS] test_kubelet_patches_labels" << std::endl;
}


// The parts of the hardware test rig: a PaHub with a BME688 on channel 2, a
// Mini Scales unit on channel 3 and an RFID 2 reader on channel 4
struct Rig {
    std::shared_ptr<MockPca9548> hub = std::make_shared<MockPca9548>();
    std::shared_ptr<MockBme688> bme = std::make_shared<MockBme688>();
    std::shared_ptr<MockMiniScale> scale = std::make_shared<MockMiniScale>();
    std::shared_ptr<MockMfrc522> rfid = std::make_shared<MockMfrc522>();
    Rig() {
        MockHal::instance().reset();   // the BMP280 at 0x76 stays on the bus itself
        MockHal::instance().register_mock_i2c_device(0, 0x70, hub);
        hub->attach(2, 0x77, bme);
        hub->attach(3, 0x26, scale);
        hub->attach(4, 0x28, rfid);
    }
};

const models::DiscoveredPart* part_at(const std::vector<models::DiscoveredPart>& parts, const models::I2cLocation& where) {
    for (const auto& p : parts) if (p.where == where) return &p;
    return nullptr;
}

void test_multiplexer_scan() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    fw.set_board_profile(cardputer_profile());
    Rig rig;
    I2cDiscovery d;
    assert(d.scan());

    // Each channel scanned alone, the parts identified, the PaHub left deselected
    auto parts = d.parts();
    assert(parts.size() == 5);
    assert(part_at(parts, {0, 0x70, 0, 0})->chip == "PCA9548A" && part_at(parts, {0, 0x70, 0, 0})->driver.empty());
    assert(part_at(parts, {0, 0x76, 0, 0})->chip.empty());   // a BMP280: no catalog part
    assert(part_at(parts, {0, 0x77, 0x70, 2})->chip == "BME688" && part_at(parts, {0, 0x77, 0x70, 2})->driver == "bme688");
    assert(part_at(parts, {0, 0x26, 0x70, 3})->chip == "MiniScale" && part_at(parts, {0, 0x26, 0x70, 3})->driver == "miniscale");
    assert(part_at(parts, {0, 0x28, 0x70, 4})->chip == "WS1850S" && part_at(parts, {0, 0x28, 0x70, 4})->driver == "rfid");
    assert(rig.hub->mask() == 0x00);
    assert(d.devices(0) == (std::vector<uint8_t>{0x70, 0x76}));
    // Reads only: register pointers and the multiplexer's control byte
    assert(rig.bme->writes() == 0 && rig.scale->writes() == 0 && rig.rfid->writes() == 0);

    auto labels = d.labels();
    assert(labels["i2c.kube32.io/0x70"] == "bus-0" && labels["i2c.kube32.io/0x76"] == "bus-0");
    assert(labels["i2c.kube32.io/0x77"] == "bus-0.mux-70.2" && labels["i2c.kube32.io/0x26"] == "bus-0.mux-70.3");
    assert(labels["i2c.kube32.io/0x28"] == "bus-0.mux-70.4");
    assert(d.annotation().find(R"({"bus":0,"mux":"0x70","channel":2,"address":"0x77","chip":"BME688","driver":"bme688"})") != std::string::npos);
    assert(!d.scan());   // nothing changed

    // Identified once: the next scans read no register of a known part
    rig.bme->set_reg(0xD0, 0x00);   // a later read of the chip id would no longer match
    assert(!d.scan() && part_at(d.parts(), {0, 0x77, 0x70, 2})->chip == "BME688");
    rig.bme->set_reg(0xD0, 0x61);

    // Unplugged from its channel: gone from the report and the labels; a
    // BME680 (variant 0) plugged in its place is not a catalog part
    // (kept through the first scan that misses it, gone after the second)
    rig.hub->detach(2, 0x77);
    assert(!d.scan() && part_at(d.parts(), {0, 0x77, 0x70, 2}) && d.labels().count("i2c.kube32.io/0x77"));
    assert(d.scan() && !part_at(d.parts(), {0, 0x77, 0x70, 2}) && !d.labels().count("i2c.kube32.io/0x77"));
    auto bme680 = std::make_shared<MockBme688>();
    bme680->set_reg(0xF0, 0x00);
    rig.hub->attach(5, 0x77, bme680);
    assert(d.scan() && part_at(d.parts(), {0, 0x77, 0x70, 5}) && part_at(d.parts(), {0, 0x77, 0x70, 5})->chip.empty());

    // A channel some earlier user left open: its parts are not taken for the bus's own
    rig.hub->detach(5, 0x77);
    rig.hub->attach(2, 0x77, rig.bme);
    const uint8_t open = 0x08;
    rig.hub->write(&open, 1);
    I2cDiscovery fresh;
    fresh.scan();
    assert(fresh.devices(0) == (std::vector<uint8_t>{0x70, 0x76}) && part_at(fresh.parts(), {0, 0x26, 0x70, 3}));
    assert(!part_at(fresh.parts(), {0, 0x26, 0, 0}) && rig.hub->mask() == 0x00);
    fw.reset();
    std::cout << "[PASS] test_multiplexer_scan" << std::endl;
}

// A bus shared by device grants is scanned beside the pods, through the
// same bus lock; the granted part is not read, and the bus stays installed
// for the pods
void test_scan_of_a_shared_bus() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    fw.set_board_profile(cardputer_profile());
    Rig rig;
    std::string err;
    const std::string granted = R"({"metadata":{"name":"bme","namespace":"default","annotations":{"kube32.io/i2c-device":"bus=0,addr=0x77,mux=0x70:2"}},
        "spec":{"containers":[{"name":"c","resources":{"requests":{"memory":"8Ki"}}}]}})";
    assert(fw.register_pod("default/bme", granted, err));
    assert(i2c_bus_state(0).sharers == 1);
    const uint32_t installs = i2c_bus_state(0).installs;
    BusPins pins;
    assert(!fw.lend_i2c_bus(0, pins));   // without out_shared: not lent
    I2cDiscovery d;
    assert(d.scan());
    assert(i2c_bus_state(0).installed && i2c_bus_state(0).sharers == 1 && i2c_bus_state(0).installs == installs);
    auto parts = d.parts();
    assert(part_at(parts, {0, 0x77, 0x70, 2}) && part_at(parts, {0, 0x77, 0x70, 2})->chip.empty());   // held by the pod: not read
    assert(part_at(parts, {0, 0x26, 0x70, 3})->chip == "MiniScale");
    assert(rig.bme->writes() == 0);
    // The pod's transfers still find their channel after the scan
    uint8_t reg = 0xD0, v = 0;
    assert(i2c_device_write({0, 0x77, 0x70, 2}, &reg, 1) == 1 && i2c_device_read({0, 0x77, 0x70, 2}, &v, 1) == 1 && v == 0x61);

    // Drivers switching the PaHub between their channels all the time: the
    // multiplexer test is not fooled by a channel switched in between, and
    // every scan finds every part
    const std::string scale = R"({"metadata":{"name":"scale","namespace":"default","annotations":{"kube32.io/i2c-device":"bus=0,addr=0x26,mux=0x70:3"}},
        "spec":{"containers":[{"name":"c","resources":{"requests":{"memory":"8Ki"}}}]}})";
    assert(fw.register_pod("default/scale", scale, err));
    std::atomic<bool> stop{false};
    std::thread drivers([&] {
        uint8_t v = 0;
        while (!stop.load()) {
            uint8_t r = 0xD0;
            i2c_device_write({0, 0x77, 0x70, 2}, &r, 1);
            i2c_device_read({0, 0x77, 0x70, 2}, &v, 1);
            r = 0xFF;
            i2c_device_write({0, 0x26, 0x70, 3}, &r, 1);
            i2c_device_read({0, 0x26, 0x70, 3}, &v, 1);
        }
    });
    for (int i = 0; i < 30; ++i) {
        d.scan();
        assert(part_at(d.parts(), {0, 0x77, 0x70, 2}) && part_at(d.parts(), {0, 0x26, 0x70, 3}) && part_at(d.parts(), {0, 0x28, 0x70, 4}));
        assert(part_at(d.parts(), {0, 0x70, 0, 0})->chip == "PCA9548A");
    }
    stop = true;
    drivers.join();
    fw.unregister_pod("default/scale");

    // The last grant goes during a scan's lend: the bus is removed once the scan returns it
    bool shared = false;
    assert(fw.lend_i2c_bus(0, pins, &shared) && shared);
    fw.unregister_pod("default/bme");
    assert(!i2c_bus_state(0).installed);
    assert(!fw.register_pod("default/whole", R"({"metadata":{"name":"w"},"spec":{"containers":[{"name":"c","resources":{"requests":{"esp32.io/i2c-bus-0":"1"}}}]}})", err));
    fw.return_i2c_bus(0);
    assert(fw.get_allocated_gpio_mask() == 0);
    fw.reset();
    std::cout << "[PASS] test_scan_of_a_shared_bus" << std::endl;
}

void test_kubelet_patches_annotation() {
    std::string report = R"([{"bus":0,"mux":"0x70","channel":2,"address":"0x77","chip":"BME688","driver":"bme688"}])";
    auto rt = runtime::create_wasm_runtime();
    kubelet::KubeletConfig cfg;
    cfg.node_name = "node-a";
    cfg.node_label_source = [] { return std::map<std::string, std::string>{{"i2c.kube32.io/0x77", "bus-0.mux-70.2"}}; };
    cfg.node_annotation_source = [&] { return std::map<std::string, std::string>{{"kube32.io/i2c-devices", report}}; };
    cfg.node_label_prefix = "i2c.kube32.io/";
    auto client = std::make_unique<NodeClient>();
    NodeClient* c = client.get();
    kubelet::Kubelet k(cfg, *rt, std::move(client));
    k.reconcile_node_registration();
    // Sent after registering, as an escaped JSON string
    const std::string sent = R"({"metadata":{"annotations":{"kube32.io/i2c-devices":"[{\"bus\":0,\"mux\":\"0x70\",\"channel\":2,\"address\":\"0x77\",\"chip\":\"BME688\",\"driver\":\"bme688\"}]"}}})";
    assert(std::find(c->patches.begin(), c->patches.end(), sent) != c->patches.end());
    c->patches.clear();
    k.refresh_node_labels();
    assert(c->patches.empty());
    report = "[]";
    k.refresh_node_labels();
    assert(c->patches.size() == 1 && c->patches[0] == R"({"metadata":{"annotations":{"kube32.io/i2c-devices":"[]"}}})");
    std::cout << "[PASS] test_kubelet_patches_annotation" << std::endl;
}

} // namespace

int main() {
    test_lending();
    test_labels();
    test_kubelet_patches_labels();
    test_multiplexer_scan();
    test_scan_of_a_shared_bus();
    test_kubelet_patches_annotation();
    std::cout << "All I2C discovery tests passed" << std::endl;
    return 0;
}
