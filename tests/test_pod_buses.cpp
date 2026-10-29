// I2C and UART for pods: admission (bus claims, pins from annotations or the
// board profile, reserved and contested pins), driver install and release
// over the pod lifecycle, node capacity, and the i2cscan example end to end
// on the simulator's stub bus.
#include "hal/capability_firewall.hpp"
#include "hal/pod_buses.hpp"
#include "hal/mock_hal.hpp"
#include "kubelet/kubelet.hpp"
#include "kubelet/kube_api_client.hpp"
#include "kubelet/node_status_reporter.hpp"
#include "runtime/pod_log.hpp"
#include "runtime/image_registry.hpp"
#include "embedded/embedded_blobs.hpp"
#include "runtime/wasm_runtime.hpp"
#include "osal/thread.hpp"

#include <cassert>
#include <functional>
#include <iostream>
#include <string>

using namespace kube32;
using namespace kube32::hal;

namespace {

std::string pod(const std::string& requests, const std::string& annotations = "") {
    return R"({"metadata":{"name":"p","namespace":"default")" + (annotations.empty() ? std::string() : ",\"annotations\":{" + annotations + "}") +
           R"(},"spec":{"containers":[{"name":"c","resources":{"requests":{)" + requests + "}}}]}}";
}

bool wait_for(const std::function<bool()>& cond, uint32_t timeout_ms) {
    for (uint32_t waited = 0; waited < timeout_ms; waited += 20) {
        if (cond()) return true;
        osal::Thread::sleep_ms(20);
    }
    return cond();
}

bool log_has(const std::string& pod_key, const std::string& needle) {
    std::vector<runtime::PodLogRecord> lines;
    runtime::PodLogStore::runtime().tail(pod_key, 0, lines);
    for (const auto& l : lines) if (l.text.find(needle) != std::string::npos) return true;
    return false;
}

BoardProfile cardputer_profile() {
    BoardProfile p;
    p.name = "cardputer";
    // Key matrix, LCD, I2S and microSD pins, as the firmware reserves them
    for (uint32_t pin : {3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42}) p.reserved_gpio_mask |= (1ULL << pin);
    p.i2c_bus_mask = 0x1;
    p.uart_port_mask = 0x2;
    p.i2c_pins[0] = {2, 1};
    p.i2c_pins[1] = {};
    p.uart_pins[1] = {2, 1};
    return p;
}

void test_i2c_admission_and_release() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    std::string err;

    // Pins from the annotation, with the SCL clock
    assert(fw.register_pod("default/a", pod(R"("esp32.io/i2c-bus-0":"1")", R"("kube32.io/i2c-bus-0":"sda=4, scl=5, hz=400000")"), err));
    BusState st = i2c_bus_state(0);
    assert(st.installed && st.owner == "default/a" && st.pin_a == 4 && st.pin_b == 5 && st.speed == 400000);
    assert((fw.get_allocated_gpio_mask() & ((1ULL << 4) | (1ULL << 5))) == ((1ULL << 4) | (1ULL << 5)));
    assert(fw.check_permission("default/a", PeripheralType::I2C, 0, AccessMode::Write));
    assert(!fw.check_permission("default/a", PeripheralType::GPIO, 4, AccessMode::Write));   // a bus pin, not a GPIO
    const uint32_t installs = st.installs;

    // Another pod cannot have the bus, nor its pins as a bus or as GPIOs
    assert(!fw.register_pod("default/b", pod(R"("esp32.io/i2c-bus-0":"1")"), err));
    assert(err.find("Hardware Collision: I2C bus 0") != std::string::npos);
    assert(!fw.register_pod("default/b", pod(R"("esp32.io/i2c-bus-1":"1")", R"("kube32.io/i2c-bus-1":"sda=5,scl=6")"), err));
    assert(err == "Hardware Collision: GPIO 5 (I2C1 sda) already claimed by default/a");
    assert(i2c_bus_state(1).installed == false && fw.get_allocated_i2c_mask() == 0x1);   // rolled back
    assert(!fw.register_pod("default/b", pod(R"("esp32.io/gpio-4":"1")"), err));

    // Deleting the pod removes the driver and frees bus and pins
    fw.unregister_pod("default/a");
    assert(!i2c_bus_state(0).installed);
    assert(fw.get_allocated_gpio_mask() == 0 && fw.get_allocated_i2c_mask() == 0);
    // ... and it installs again for the next pod
    assert(fw.register_pod("default/a", pod(R"("esp32.io/i2c-bus-0":"1")"), err));
    st = i2c_bus_state(0);
    assert(st.installed && st.pin_a == 8 && st.pin_b == 9 && st.speed == 100000);   // the board's default pins
    assert(st.installs == installs + 1);
    fw.unregister_pod("default/a");

    // Bad pin specifications
    assert(!fw.register_pod("default/c", pod(R"("esp32.io/i2c-bus-0":"1")", R"("kube32.io/i2c-bus-0":"sda=4")"), err));
    assert(err.find("Invalid kube32.io/i2c-bus-0") != std::string::npos);
    assert(!fw.register_pod("default/c", pod(R"("esp32.io/i2c-bus-0":"1")", R"("kube32.io/i2c-bus-0":"sda=4,scl=4")"), err));
    assert(err.find("same pin") != std::string::npos);
    assert(!fw.register_pod("default/c", pod(R"("esp32.io/i2c-bus-0":"1")", R"("kube32.io/i2c-bus-0":"sda=4,scl=5,speed=1")"), err));
    assert(err.find("unknown setting") != std::string::npos);
    assert(!fw.register_pod("default/c", pod(R"("esp32.io/i2c-bus-0":"1")", R"("kube32.io/i2c-bus-0":"sda=20,scl=5")"), err));
    assert(err.find("GPIO 20 (I2C0 sda) is reserved") != std::string::npos);   // native USB
    // A bus with no default pins needs them in the manifest
    assert(!fw.register_pod("default/c", pod(R"("esp32.io/i2c-bus-1":"1")"), err));
    assert(err.find("I2C1 has no default pins on the generic board") != std::string::npos);
    // The pod's own GPIO claim on a bus pin
    assert(!fw.register_pod("default/c", pod(R"("esp32.io/gpio-8":"1","esp32.io/i2c-bus-0":"1")"), err));
    assert(err.find("claimed twice") != std::string::npos);
    assert(fw.get_allocated_gpio_mask() == 0 && !i2c_bus_state(0).installed);
    fw.reset();
    std::cout << "[PASS] test_i2c_admission_and_release" << std::endl;
}

void test_uart_admission() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    std::string err;
    assert(fw.register_pod("default/u1", pod(R"("esp32.io/uart":"1")"), err));   // by count: UART1 on its default pins
    BusState st = uart_port_state(1);
    assert(st.installed && st.pin_a == 17 && st.pin_b == 18 && st.speed == 115200);
    // UART2 has no default pins
    assert(!fw.register_pod("default/u2", pod(R"("esp32.io/uart":"1")"), err));
    assert(err.find("UART2 has no default pins") != std::string::npos);
    assert(fw.register_pod("default/u2", pod(R"("esp32.io/uart-2":"1")", R"("kube32.io/uart-2":"tx=10,rx=11,baud=9600")"), err));
    assert(uart_port_state(2).installed && uart_port_state(2).speed == 9600);
    // Transfers reach the stub port only while it is installed
    const uint8_t msg[] = {'h', 'i'};
    MockHal::instance().reset();
    assert(uart_port_write(2, msg, 2) == 2);
    assert(MockHal::instance().get_uart_tx_bytes(2).size() == 2);
    assert(!fw.register_pod("default/u3", pod(R"("esp32.io/uart":"1")"), err));
    assert(err.find("Insufficient allocatable UART ports") != std::string::npos);
    fw.unregister_pod("default/u2");
    assert(uart_port_write(2, msg, 2) == HAL_ERR_NODEV);
    assert(!fw.register_pod("default/u0", pod(R"("esp32.io/uart-0":"1")"), err));   // the console
    fw.reset();
    std::cout << "[PASS] test_uart_admission" << std::endl;
}

void test_board_profile_buses_and_capacity() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    kubelet::NodeStatusReporter reporter("node", "10.0.0.2");
    kubelet::NodeTelemetry t = reporter.collect_telemetry();
    assert(t.total_i2c_count == 2 && t.total_uart_count == 2);
    std::string reg = reporter.build_registration_json(t);
    assert(reg.find("\"esp32.io/i2c\":\"2\"") != std::string::npos && reg.find("\"esp32.io/uart\":\"2\"") != std::string::npos);

    fw.set_board_profile(cardputer_profile());
    t = reporter.collect_telemetry();
    assert(t.total_i2c_count == 1 && t.total_uart_count == 1);
    std::string err;
    // The Grove port (GPIO 2/1) by default; keyboard pins are refused
    assert(!fw.register_pod("default/k", pod(R"("esp32.io/i2c-bus-0":"1")", R"("kube32.io/i2c-bus-0":"sda=8,scl=9")"), err));
    assert(err == "Security Violation: Pin GPIO 8 (I2C0 sda) is used by the cardputer board.");
    assert(!fw.register_pod("default/k", pod(R"("esp32.io/i2c-bus-1":"1")"), err));
    assert(err.find("not offered by the cardputer board") != std::string::npos);
    assert(!fw.register_pod("default/k", pod(R"("esp32.io/uart-2":"1")"), err));
    assert(err.find("UART2 is not offered by the cardputer board") != std::string::npos);
    assert(fw.register_pod("default/k", pod(R"("esp32.io/i2c-bus-0":"1")"), err));
    assert(i2c_bus_state(0).pin_a == 2 && i2c_bus_state(0).pin_b == 1);
    t = reporter.collect_telemetry();
    assert(t.allocatable_i2c_count == 0);
    // I2C and UART share the Grove port: the second claim finds its pins taken
    assert(!fw.register_pod("default/k2", pod(R"("esp32.io/uart-1":"1")"), err));
    assert(err.find("Hardware Collision: GPIO 2 (UART1 tx) already claimed by default/k") != std::string::npos);
    fw.reset();
    std::cout << "[PASS] test_board_profile_buses_and_capacity" << std::endl;
}

// The kubelet runs the i2cscan example: the driver is installed while the
// pod runs, the scan finds the simulator's device, and deleting and
// re-creating the pod releases and installs the bus again.
class BusClient : public kubelet::KubeApiClient {
public:
    std::vector<std::pair<models::KubePod, std::string>> pods;
    std::string last_patch;
    bool register_node(const std::string&, bool& c) override { c = false; return true; }
    bool patch_node_status(const std::string&, std::string_view) override { return true; }
    bool list_pods(std::vector<models::KubePod>& out) override { for (auto& p : pods) out.push_back(p.first); return true; }
    bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out) override { out = pods; return true; }
    bool patch_pod_status(const std::string&, const std::string&, const std::string& p) override { last_patch = p; return true; }
    int get(const std::string&, std::string& body) override { body = "{\"items\":[]}"; return 200; }
};

std::string scan_pod(const std::string& name, bool claim, const std::string& every_ms) {
    return R"({"metadata":{"name":")" + name + R"(","namespace":"default","uid":")" + name + R"(-uid"},
               "spec":{"nodeName":"node-a","restartPolicy":"Never","containers":[{"name":"i2cscan","image":"wasm://embedded/i2cscan",
               "env":[{"name":"I2C_BUS","value":"0"},{"name":"SCAN_EVERY_MS","value":")" + every_ms + R"("}],
               "resources":{"requests":{"memory":"64Ki")" + (claim ? R"(,"esp32.io/i2c-bus-0":"1")" : "") + "}}}]}}";
}

void test_i2cscan_pod_lifecycle() {
    size_t n = 0;
    const auto* imgs = embedded::images(n);
    runtime::ImageRegistry::instance().add_all(imgs, n);   // images/*.wasm, as the firmware has them
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    MockHal::instance().reset();   // a BMP280 answers at 0x76 on bus 0
    runtime::PodLogStore::runtime().clear();
    auto rt = runtime::create_wasm_runtime();
    kubelet::KubeletConfig cfg;
    cfg.node_name = "node-a";
    cfg.node_ip = "10.0.0.2";
    auto client = std::make_unique<BusClient>();
    BusClient* c = client.get();
    models::KubePod p;
    p.name = "i2cscan";
    p.namespace_name = "default";
    p.node_name = "node-a";
    c->pods.emplace_back(p, scan_pod("i2cscan", true, "100"));
    kubelet::Kubelet k(cfg, *rt, std::move(client));

    k.sync_assigned_pods();
    BusState st = i2c_bus_state(0);
    assert(st.installed && st.owner == "default/i2cscan" && st.pin_a == 8 && st.pin_b == 9);
    const uint32_t installs = st.installs;
    assert(wait_for([]() { return log_has("default/i2cscan", "i2cscan: bus 0: scan complete, 1 device"); }, 3000));
    assert(log_has("default/i2cscan", "i2cscan: bus 0: device at 0x76"));

    // Delete: the container stops, the driver goes, the pins are free
    c->pods.clear();
    k.sync_assigned_pods();
    assert(!i2c_bus_state(0).installed);
    assert(fw.get_allocated_gpio_mask() == 0 && fw.get_allocated_i2c_mask() == 0);

    // Re-create: installed again
    runtime::PodLogStore::runtime().clear();
    c->pods.emplace_back(p, scan_pod("i2cscan", true, "100"));
    k.sync_assigned_pods();
    assert(i2c_bus_state(0).installed && i2c_bus_state(0).installs == installs + 1);
    assert(wait_for([]() { return log_has("default/i2cscan", "scan complete, 1 device"); }, 3000));
    c->pods.clear();
    k.sync_assigned_pods();
    assert(!i2c_bus_state(0).installed);

    // A one-shot scan completes the pod (exit 0) and releases the bus at once
    c->pods.emplace_back(p, scan_pod("i2cscan", true, "0"));
    k.sync_assigned_pods();
    assert(wait_for([&]() { k.monitor_and_report_pod_statuses(); return c->last_patch.find("\"phase\":\"Succeeded\"") != std::string::npos; }, 3000));
    assert(!i2c_bus_state(0).installed);
    c->pods.clear();
    k.sync_assigned_pods();

    // Without the claim every probe is refused: the scanner exits 1
    models::KubePod q = p;
    q.name = "noclaim";
    c->pods.emplace_back(q, scan_pod("noclaim", false, "0"));
    k.sync_assigned_pods();
    assert(wait_for([&]() { k.monitor_and_report_pod_statuses(); return c->last_patch.find("\"phase\":\"Failed\"") != std::string::npos; }, 3000));
    assert(c->last_patch.find("\"exitCode\":1") != std::string::npos);
    assert(log_has("default/noclaim", "error -1 probing 0x08 (bus not claimed by this pod)"));
    c->pods.clear();
    k.sync_assigned_pods();
    fw.reset();
    std::cout << "[PASS] test_i2cscan_pod_lifecycle" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== I2C and UART for pods ===" << std::endl;
    test_i2c_admission_and_release();
    test_uart_admission();
    test_board_profile_buses_and_capacity();
    test_i2cscan_pod_lifecycle();
    std::cout << "=== All pod bus tests passed ===" << std::endl;
    return 0;
}
