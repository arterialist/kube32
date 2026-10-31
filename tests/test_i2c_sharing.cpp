// Shared I2C buses: device grants (kube32.io/i2c-device) of several pods on
// one installed bus, each limited to its address; PCA9548A channel
// selection before each transfer and only when it changes; interleaved
// register accesses of two pods behind different channels; the whole-bus
// claim and grants keeping each other out (and the kubelet keeping the
// second pod Pending); node capacity; recovery after a failed transfer; a
// Wasm pod refused at any other address.
#include "hal/capability_firewall.hpp"
#include "hal/mock_hal.hpp"
#include "hal/pod_buses.hpp"
#include "kubelet/kubelet.hpp"
#include "kubelet/kube_api_client.hpp"
#include "kubelet/node_status_reporter.hpp"
#include "runtime/image_registry.hpp"
#include "runtime/pod_log.hpp"
#include "runtime/wasm_runtime.hpp"
#include "embedded/embedded_blobs.hpp"
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

std::string grant(const std::string& value) { return pod(R"("memory":"8Ki")", R"("kube32.io/i2c-device":")" + value + "\""); }

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
    for (uint32_t pin : {3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42}) p.reserved_gpio_mask |= (1ULL << pin);
    p.i2c_bus_mask = 0x1;
    p.uart_port_mask = 0x2;
    p.i2c_pins[0] = {2, 1};
    p.i2c_pins[1] = {};
    p.uart_pins[1] = {2, 1};
    return p;
}

// The parts of the hardware test rig: a PaHub with a BME688 on channel 2, a
// Mini Scales unit on channel 3 and an RFID 2 reader on channel 4
struct Rig {
    std::shared_ptr<MockPca9548> hub = std::make_shared<MockPca9548>();
    std::shared_ptr<MockBme688> bme = std::make_shared<MockBme688>();
    std::shared_ptr<MockMiniScale> scale = std::make_shared<MockMiniScale>();
    std::shared_ptr<MockMfrc522> rfid = std::make_shared<MockMfrc522>();
    Rig() {
        MockHal::instance().reset();
        MockHal::instance().remove_mock_i2c_device(0, 0x76);
        MockHal::instance().register_mock_i2c_device(0, 0x70, hub);
        hub->attach(2, 0x77, bme);
        hub->attach(3, 0x26, scale);
        hub->attach(4, 0x28, rfid);
    }
};

const models::I2cLocation kBme{0, 0x77, 0x70, 2};
const models::I2cLocation kScale{0, 0x26, 0x70, 3};

void test_grants_share_one_bus() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    fw.set_board_profile(cardputer_profile());
    Rig rig;
    std::string err;
    bool busy = false;
    assert(fw.register_pod("default/bme", grant("bus=0,addr=0x77,mux=0x70:2"), err, &busy));
    assert(fw.register_pod("default/scale", grant("bus=0,addr=0x26,mux=0x70:3"), err, &busy));
    BusState st = i2c_bus_state(0);
    assert(st.installed && st.sharers == 2 && st.installs == 1 && st.pin_a == 2 && st.pin_b == 1);
    assert(fw.i2c_sharers(0) == 2);
    // The bus's pins are held for the bus: no pod gets them as GPIOs or a UART
    assert(!fw.register_pod("default/uart", pod(R"("esp32.io/uart-1":"1")"), err));
    assert(err.find("I2C0 (shared by device grants)") != std::string::npos);
    // Not free for a whole-bus claim: the node reports no I2C bus left
    assert((fw.get_allocated_i2c_mask() & 0x1) == 0x1);
    kubelet::NodeStatusReporter reporter("node-a", "10.0.0.2");
    assert(reporter.collect_telemetry().allocatable_i2c_count == 0);

    // Each pod reaches its own address only
    models::I2cLocation loc;
    assert(fw.check_i2c("default/bme", 0, 0x77, AccessMode::Read, loc) == I2cAccess::Device && loc == kBme);
    assert(fw.check_i2c("default/bme", 0, 0x26, AccessMode::Write, loc) == I2cAccess::Denied);   // the neighbour's
    assert(fw.check_i2c("default/bme", 0, 0x70, AccessMode::Write, loc) == I2cAccess::Denied);   // the multiplexer
    assert(fw.check_i2c("default/bme", 1, 0x77, AccessMode::Write, loc) == I2cAccess::Denied);   // another bus
    assert(fw.check_i2c("default/other", 0, 0x77, AccessMode::Write, loc) == I2cAccess::Denied); // no grant
    assert(fw.check_i2c("default/scale", 0, 0x26, AccessMode::Read, loc) == I2cAccess::Device && loc == kScale);

    // One driver per part
    assert(!fw.register_pod("default/second", grant("bus=0,addr=0x77,mux=0x70:2"), err, &busy) && busy);
    assert(err.find("is granted to default/bme") != std::string::npos);

    // The bus stays until the last grant goes; then its pins are free
    fw.unregister_pod("default/bme");
    assert(i2c_bus_state(0).installed && i2c_bus_state(0).sharers == 1);
    fw.unregister_pod("default/scale");
    assert(!i2c_bus_state(0).installed && fw.i2c_sharers(0) == 0);
    assert(fw.get_allocated_gpio_mask() == 0 && fw.get_allocated_i2c_mask() == 0);
    assert(fw.register_pod("default/uart", pod(R"("esp32.io/uart-1":"1")"), err));
    fw.reset();
    std::cout << "[PASS] test_grants_share_one_bus" << std::endl;
}

void test_bad_grants() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    fw.set_board_profile(cardputer_profile());
    std::string err;
    bool busy = false;
    assert(!fw.register_pod("default/a", grant("bus=0,addr=0x70,mux=0x70:2"), err, &busy) && !busy);
    assert(err.find("multiplexer's own address") != std::string::npos);
    assert(!fw.register_pod("default/a", grant("bus=0,addr=0x90"), err, &busy) && !busy);
    assert(!fw.register_pod("default/a", grant("bus=0,addr=0x77,mux=0x70:9"), err, &busy) && !busy);
    assert(!fw.register_pod("default/a", grant("bus=1,addr=0x77"), err, &busy) && !busy);
    assert(err.find("not offered by the cardputer board") != std::string::npos);
    // Both ways at once
    assert(!fw.register_pod("default/a", pod(R"("esp32.io/i2c-bus-0":"1")", R"("kube32.io/i2c-device":"bus=0,addr=0x77")"), err, &busy) && !busy);
    assert(err.find("claim one or the other") != std::string::npos);
    assert(fw.get_allocated_gpio_mask() == 0 && !i2c_bus_state(0).installed);
    fw.reset();
    std::cout << "[PASS] test_bad_grants" << std::endl;
}

void test_mux_channel_switching() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    fw.set_board_profile(cardputer_profile());
    Rig rig;
    auto direct = std::make_shared<MockMfrc522>(0x92);
    MockHal::instance().register_mock_i2c_device(0, 0x29, direct);
    std::string err;
    assert(fw.register_pod("default/bme", grant("bus=0,addr=0x77,mux=0x70:2"), err));
    assert(fw.register_pod("default/scale", grant("bus=0,addr=0x26,mux=0x70:3"), err));
    assert(fw.register_pod("default/reader", grant("bus=0,addr=0x29"), err));

    // The first transfer selects channel 2; the next ones on it write nothing more
    uint8_t reg = 0xD0, v = 0;
    assert(i2c_device_write(kBme, &reg, 1) == 1 && i2c_device_read(kBme, &v, 1) == 1 && v == 0x61);
    assert(rig.hub->mask() == 0x04 && rig.hub->mask_writes() == 1);
    reg = 0xF0;
    assert(i2c_device_write(kBme, &reg, 1) == 1 && i2c_device_read(kBme, &v, 1) == 1 && v == 0x01);
    assert(rig.hub->mask_writes() == 1);

    // Interleaved: the BME688 driver sets its register pointer, the scale's
    // transfer switches the channel, the BME688's read switches back and
    // still gets the register it pointed at
    reg = 0xD0;
    assert(i2c_device_write(kBme, &reg, 1) == 1);
    uint8_t sreg = 0xFF, addr = 0;
    assert(i2c_device_write(kScale, &sreg, 1) == 1 && i2c_device_read(kScale, &addr, 1) == 1 && addr == 0x26);
    assert(rig.hub->mask() == 0x08);
    assert(i2c_device_read(kBme, &v, 1) == 1 && v == 0x61);
    assert(rig.hub->mask() == 0x04 && rig.hub->mask_writes() == 3);

    // A part on the bus itself: the multiplexer is deselected first, so
    // nothing behind it answers along
    const models::I2cLocation reader{0, 0x29, 0, 0};
    reg = 0x37;
    assert(i2c_device_write(reader, &reg, 1) == 1 && i2c_device_read(reader, &v, 1) == 1 && v == 0x92);
    assert(rig.hub->mask() == 0x00 && rig.hub->mask_writes() == 4);
    assert(i2c_device_write(reader, &reg, 1) == 1 && rig.hub->mask_writes() == 4);

    // Wiring that went away: the transfer fails, the multiplexer's state is
    // unknown, and the next transfer writes the channel again
    rig.hub->detach(2, 0x77);
    assert(i2c_device_read(kBme, &v, 1) == HAL_ERR_NODEV);
    rig.hub->attach(2, 0x77, rig.bme);
    const uint32_t writes = rig.hub->mask_writes();
    assert(i2c_device_read(kBme, &v, 1) == 1 && rig.hub->mask_writes() == writes + 1 && rig.hub->mask() == 0x04);

    fw.unregister_pod("default/bme");
    fw.unregister_pod("default/scale");
    fw.unregister_pod("default/reader");
    fw.reset();
    std::cout << "[PASS] test_mux_channel_switching" << std::endl;
}

void test_whole_bus_and_grants_exclude_each_other() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    fw.set_board_profile(cardputer_profile());
    Rig rig;
    std::string err;
    bool busy = false;
    // Shared first: the whole-bus claim waits
    assert(fw.register_pod("default/bme", grant("bus=0,addr=0x77,mux=0x70:2"), err, &busy));
    assert(!fw.register_pod("default/rfid", pod(R"("esp32.io/i2c-bus-0":"1")"), err, &busy) && busy);
    assert(err.find("shared by 1 device grant(s)") != std::string::npos);
    assert(!fw.register_pod("default/any", pod(R"("esp32.io/i2c":"1")"), err, &busy));   // no free bus left to hand out
    fw.unregister_pod("default/bme");
    assert(fw.register_pod("default/rfid", pod(R"("esp32.io/i2c-bus-0":"1")"), err, &busy));
    // Claimed whole first: the grant waits, and the claimant reaches every address
    assert(!fw.register_pod("default/bme", grant("bus=0,addr=0x77,mux=0x70:2"), err, &busy) && busy);
    assert(err.find("claimed whole by default/rfid") != std::string::npos);
    models::I2cLocation loc;
    assert(fw.check_i2c("default/rfid", 0, 0x70, AccessMode::Write, loc) == I2cAccess::Bus);
    fw.unregister_pod("default/rfid");
    assert(fw.register_pod("default/bme", grant("bus=0,addr=0x77,mux=0x70:2"), err, &busy));
    fw.reset();
    std::cout << "[PASS] test_whole_bus_and_grants_exclude_each_other" << std::endl;
}

// The simulated BME688 as a driver reads it: calibration, a forced
// measurement, and the datasheet's integer compensation giving back the
// readings the part was set to
void test_simulated_bme688() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    fw.set_board_profile(cardputer_profile());
    Rig rig;
    rig.bme->set_readings(23.4, 1007.2, 41.0, 51234);
    std::string err;
    assert(fw.register_pod("default/bme", grant("bus=0,addr=0x77,mux=0x70:2"), err));
    auto reg = [](uint8_t r) {
        uint8_t v = 0;
        int32_t n = i2c_device_write(kBme, &r, 1);
        assert(n == 1 && i2c_device_read(kBme, &v, 1) == 1);
        return v;
    };
    auto u16 = [&](uint8_t lsb) { return static_cast<int32_t>(reg(lsb) | (reg(static_cast<uint8_t>(lsb + 1)) << 8)); };
    auto s16 = [&](uint8_t lsb) { return static_cast<int32_t>(static_cast<int16_t>(u16(lsb))); };
    auto s8 = [&](uint8_t r) { return static_cast<int32_t>(static_cast<int8_t>(reg(r))); };
    const int32_t t1 = u16(0xE9), t2 = s16(0x8A), t3 = s8(0x8C);
    const int32_t p1 = u16(0x8E), p2 = s16(0x90), p3 = s8(0x92), p4 = s16(0x94), p5 = s16(0x96), p6 = s8(0x99), p7 = s8(0x98);
    const int32_t p8 = s16(0x9C), p9 = s16(0x9E), p10 = reg(0xA0);

    // ctrl_hum, then ctrl_meas in forced mode, as address/value pairs
    const uint8_t start[] = {0x72, 0x01, 0x74, 0x55};
    assert(i2c_device_write(kBme, start, sizeof(start)) == 4);
    assert(rig.bme->measurements() == 1 && (reg(0x74) & 0x03) == 0 && (reg(0x1D) & 0x80));
    const int32_t t_adc = (reg(0x22) << 12) | (reg(0x23) << 4) | (reg(0x24) >> 4);
    const int32_t p_adc = (reg(0x1F) << 12) | (reg(0x20) << 4) | (reg(0x21) >> 4);

    int32_t var1 = (t_adc >> 3) - (t1 << 1);
    int32_t var2 = (var1 * t2) >> 11;
    int32_t var3 = (((var1 >> 1) * (var1 >> 1)) >> 12);
    var3 = (var3 * (t3 << 4)) >> 14;
    const int32_t t_fine = var2 + var3;
    const int32_t centi_c = ((t_fine * 5) + 128) >> 8;
    assert(centi_c >= 2335 && centi_c <= 2345);

    var1 = (t_fine >> 1) - 64000;
    var2 = ((((var1 >> 2) * (var1 >> 2)) >> 11) * p6) >> 2;
    var2 = var2 + ((var1 * p5) << 1);
    var2 = (var2 >> 2) + (p4 << 16);
    var1 = (((((var1 >> 2) * (var1 >> 2)) >> 13) * (p3 << 5)) >> 3) + ((p2 * var1) >> 1);
    var1 = var1 >> 18;
    var1 = ((32768 + var1) * p1) >> 15;
    int32_t pa = 1048576 - p_adc;
    pa = static_cast<int32_t>((pa - (var2 >> 12)) * static_cast<uint32_t>(3125));
    pa = pa >= (1 << 30) ? ((pa / var1) << 1) : ((pa << 1) / var1);
    var1 = (p9 * static_cast<int32_t>(((pa >> 3) * (pa >> 3)) >> 13)) >> 12;
    var2 = (static_cast<int32_t>(pa >> 2) * p8) >> 13;
    var3 = (static_cast<int32_t>(pa >> 8) * static_cast<int32_t>(pa >> 8) * static_cast<int32_t>(pa >> 8) * p10) >> 17;
    pa = pa + ((var1 + var2 + var3 + (p7 << 7)) >> 4);
    assert(pa >= 100700 && pa <= 100740);

    // Gas, BME688: valid, heater stable, 1e6 * (262144 >> range) / (4096 + 3 * (adc - 512))
    const uint8_t lsb = reg(0x2D);
    assert((lsb & 0x20) && (lsb & 0x10));
    const int32_t g_adc = (reg(0x2C) << 2) | (lsb >> 6), range = lsb & 0x0F;
    const double ohms = 1e6 * static_cast<double>(262144u >> range) / (4096 + 3 * (g_adc - 512));
    assert(ohms > 51234 * 0.99 && ohms < 51234 * 1.01);
    fw.reset();
    std::cout << "[PASS] test_simulated_bme688" << std::endl;
}

// Input pins: GPIO 0, the boot button, may be read and set up as an input,
// never driven; other pins as usual
void test_input_pins() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    std::string err;
    assert(fw.register_pod("default/button", pod(R"("esp32.io/gpio-input-0":"1")"), err));
    assert(fw.check_permission("default/button", PeripheralType::GPIO, 0, AccessMode::Read));
    assert(!fw.check_permission("default/button", PeripheralType::GPIO, 0, AccessMode::Config));
    assert(!fw.check_permission("default/button", PeripheralType::GPIO, 0, AccessMode::Write));
    assert(!fw.register_pod("default/other", pod(R"("esp32.io/gpio-input-0":"1")"), err));
    assert(err == "Hardware Collision: GPIO 0 already claimed by default/button");
    fw.unregister_pod("default/button");
    assert(!fw.register_pod("default/led", pod(R"("esp32.io/gpio-0":"1")"), err));   // never as an output
    assert(!fw.register_pod("default/usb", pod(R"("esp32.io/gpio-input-19":"1")"), err));   // native USB stays reserved
    assert(!fw.register_pod("default/bad", pod(R"("esp32.io/gpio-input-x":"1")"), err));
    assert(fw.register_pod("default/b4", pod(R"("esp32.io/gpio-input-4":"1")"), err));
    assert(fw.get_allocated_gpio_mask() == (1ULL << 4));
    fw.set_board_profile(cardputer_profile());
    fw.unregister_pod("default/b4");
    assert(!fw.register_pod("default/b4", pod(R"("esp32.io/gpio-input-4":"1")"), err));   // a key matrix pin there
    fw.reset();
    std::cout << "[PASS] test_input_pins" << std::endl;
}

class Client : public kubelet::KubeApiClient {
public:
    std::vector<std::pair<models::KubePod, std::string>> pods;
    std::vector<std::string> patches;
    bool register_node(std::string_view, bool& c) override { c = false; return true; }
    bool patch_node_status(const std::string&, std::string_view) override { return true; }
    bool list_pods(std::vector<models::KubePod>& out) override { for (auto& p : pods) out.push_back(p.first); return true; }
    bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out) override { out = pods; return true; }
    bool patch_pod_status(const std::string& ns, const std::string& name, const std::string& p) override {
        patches.push_back(ns + "/" + name + " " + p);
        return true;
    }
    int get(const std::string&, std::string& body) override { body = "{\"items\":[]}"; return 200; }
    bool patched(const std::string& pod, const std::string& needle) const {
        for (const auto& p : patches) if (p.rfind(pod + " ", 0) == 0 && p.find(needle) != std::string::npos) return true;
        return false;
    }
};

models::KubePod kube_pod(const std::string& name) {
    models::KubePod p;
    p.name = name;
    p.namespace_name = "default";
    p.node_name = "node-a";
    return p;
}

std::string scan_pod(const std::string& name, const std::string& requests, const std::string& annotations) {
    return R"({"metadata":{"name":")" + name + R"(","namespace":"default","uid":")" + name + R"(-uid","annotations":{)" + annotations +
           R"(}},"spec":{"nodeName":"node-a","restartPolicy":"Always","containers":[{"name":"i2cscan","image":"wasm://embedded/i2cscan",
           "env":[{"name":"I2C_BUS","value":"0"},{"name":"SCAN_EVERY_MS","value":"200"}],
           "resources":{"requests":{"memory":"64Ki")" + requests + "}}}]}}";
}

// Through the kubelet: a Wasm pod with a grant is refused every other
// address (i2cscan's first probe, 0x08), and a pod claiming the bus whole
// waits Pending (BusInUse) while the grant holds it, then starts
void test_kubelet_and_wasm() {
    size_t n = 0;
    const auto* imgs = embedded::images(n);
    runtime::ImageRegistry::instance().add_all(imgs, n);
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    fw.set_board_profile(cardputer_profile());
    Rig rig;
    runtime::PodLogStore::runtime().clear();
    auto rt = runtime::create_wasm_runtime();
    kubelet::KubeletConfig cfg;
    cfg.node_name = "node-a";
    cfg.memory_retry_ms = 100;
    auto client = std::make_unique<Client>();
    Client* c = client.get();
    c->pods.emplace_back(kube_pod("granted"), scan_pod("granted", "", R"("kube32.io/i2c-device":"bus=0,addr=0x77,mux=0x70:2")"));
    kubelet::Kubelet k(cfg, *rt, std::move(client));
    k.sync_assigned_pods();
    assert(i2c_bus_state(0).sharers == 1);
    assert(wait_for([] { return log_has("default/granted", "i2cscan: bus 0: error -1 probing 0x08"); }, 3000));

    c->pods.emplace_back(kube_pod("whole"), scan_pod("whole", R"(,"esp32.io/i2c-bus-0":"1")", ""));
    k.sync_assigned_pods();
    kubelet::LocalPodRecord rec;
    assert(k.get_pod_record("default/whole", rec) && rec.execution_phase == kubelet::PodExecutionPhase::BusInUse);
    assert(c->patched("default/whole", "\"phase\":\"Pending\"") && c->patched("default/whole", "\"reason\":\"BusInUse\""));
    assert(c->patched("default/whole", "shared by 1 device grant(s)"));

    // The grant's pod goes: the whole-bus pod starts at its next retry
    c->pods.erase(c->pods.begin());
    k.sync_assigned_pods();
    osal::Thread::sleep_ms(150);
    k.sync_assigned_pods();
    assert(k.get_pod_record("default/whole", rec) && rec.execution_phase == kubelet::PodExecutionPhase::Running);
    assert(i2c_bus_state(0).installed && i2c_bus_state(0).owner == "default/whole" && i2c_bus_state(0).sharers == 0);
    assert(wait_for([] { return log_has("default/whole", "i2cscan: bus 0: device at 0x70"); }, 3000));
    c->pods.clear();
    k.sync_assigned_pods();
    assert(!i2c_bus_state(0).installed);
    fw.reset();
    std::cout << "[PASS] test_kubelet_and_wasm" << std::endl;
}

} // namespace

int main() {
    test_grants_share_one_bus();
    test_bad_grants();
    test_mux_channel_switching();
    test_whole_bus_and_grants_exclude_each_other();
    test_simulated_bme688();
    test_input_pins();
    test_kubelet_and_wasm();
    std::cout << "All I2C sharing tests passed" << std::endl;
    return 0;
}
