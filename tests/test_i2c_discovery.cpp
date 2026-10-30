// I2C device discovery: the firewall lends a free bus to the node and holds
// pod admission off while it is lent, scans turn the devices on the bus into
// node labels, a bus a pod holds keeps its last result, and the kubelet
// patches label changes onto its Node.
#include "hal/capability_firewall.hpp"
#include "hal/i2c_discovery.hpp"
#include "hal/mock_hal.hpp"
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

} // namespace

int main() {
    test_lending();
    test_labels();
    test_kubelet_patches_labels();
    std::cout << "All I2C discovery tests passed" << std::endl;
    return 0;
}
