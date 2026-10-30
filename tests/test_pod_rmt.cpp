// RMT (IR) channels for pods: admission (esp32.io/rmt-tx and rmt-rx claims,
// pins and settings from annotations or the board profile, reserved and
// contested pins, channel counts), channel install and release, the stub's
// send and receive queue, node capacity and scheduling, and the irremote
// example end to end on the simulator.
#include "hal/capability_firewall.hpp"
#include "hal/pod_rmt.hpp"
#include "hal/mock_hal.hpp"
#include "kubelet/kubelet.hpp"
#include "kubelet/kube_api_client.hpp"
#include "kubelet/node_status_reporter.hpp"
#include "runtime/app_pod.hpp"
#include "runtime/pod_log.hpp"
#include "runtime/image_registry.hpp"
#include "runtime/k32_app_abi.h"
#include "embedded/embedded_blobs.hpp"
#include "runtime/wasm_runtime.hpp"
#include "scheduler/scheduler.hpp"
#include "osal/thread.hpp"

#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

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

size_t log_count(const std::string& pod_key, const std::string& needle) {
    std::vector<runtime::PodLogRecord> lines;
    runtime::PodLogStore::runtime().tail(pod_key, 0, lines);
    size_t n = 0;
    for (const auto& l : lines) if (l.text.find(needle) != std::string::npos) n++;
    return n;
}

bool log_has(const std::string& pod_key, const std::string& needle) { return log_count(pod_key, needle) > 0; }

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
    p.rmt_tx_pin_mask = 1ULL << 44;   // the IR LED on the UART0 RX pad
    p.rmt_tx_default_pin = 44;
    return p;
}

// Little-endian bytes of `durations`, starting one byte into the buffer:
// the host must not assume the guest's array is aligned
std::vector<uint8_t> le_bytes(const std::vector<uint16_t>& durations) {
    std::vector<uint8_t> b(1 + 2 * durations.size());
    for (size_t i = 0; i < durations.size(); ++i) {
        b[1 + 2 * i] = static_cast<uint8_t>(durations[i]);
        b[2 + 2 * i] = static_cast<uint8_t>(durations[i] >> 8);
    }
    return b;
}

int32_t send(const std::string& pod_id, const std::vector<uint16_t>& d) {
    std::vector<uint8_t> b = le_bytes(d);
    return rmt_send(pod_id, b.data() + 1, static_cast<uint32_t>(d.size()));
}

// The oldest queued frame, through an unaligned buffer; `rc` gets the result
std::vector<uint16_t> receive(const std::string& pod_id, uint32_t cap, int32_t& rc, RmtDrops* drops = nullptr) {
    std::vector<uint8_t> b(1 + 2 * cap);
    rc = rmt_receive(pod_id, b.data() + 1, cap, drops);
    std::vector<uint16_t> out;
    const uint32_t n = rc >= 0 ? static_cast<uint32_t>(rc) : (rc == HAL_ERR_TOOBIG ? cap : 0);
    for (uint32_t i = 0; i < n; ++i) out.push_back(static_cast<uint16_t>(b[1 + 2 * i] | (b[2 + 2 * i] << 8)));
    return out;
}

// NEC frame for addr/cmd (8-bit address with its complement), as a remote sends it
std::vector<uint16_t> nec(uint32_t addr, uint32_t cmd) {
    const uint32_t bits = addr | ((addr ^ 0xff) << 8) | (cmd << 16) | ((cmd ^ 0xff) << 24);
    std::vector<uint16_t> d{9000, 4500};
    for (int i = 0; i < 32; ++i) {
        d.push_back(560);
        d.push_back((bits >> i) & 1 ? 1690 : 560);
    }
    d.push_back(560);
    return d;
}

void test_rmt_admission_and_release() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    std::string err;

    // Pin and settings from the annotation; defaults for the rest
    assert(fw.register_pod("default/a", pod(R"("esp32.io/rmt-tx":"1","esp32.io/rmt-rx":"1")",
                                            R"("kube32.io/rmt-tx":"pin=4, carrier=36000","kube32.io/rmt-rx":"pin=5,active=high,idle_us=20000")"),
                           err));
    RmtState tx = rmt_state("default/a", RmtDir::Tx);
    RmtState rx = rmt_state("default/a", RmtDir::Rx);
    assert(tx.installed && tx.owner == "default/a" && tx.config.pin == 4 && tx.config.carrier_hz == 36000 && tx.config.duty_pct == 33);
    assert(rx.installed && rx.config.pin == 5 && !rx.config.active_low && rx.config.idle_us == 20000);
    assert((fw.get_allocated_gpio_mask() & ((1ULL << 4) | (1ULL << 5))) == ((1ULL << 4) | (1ULL << 5)));
    assert(fw.check_permission("default/a", PeripheralType::RmtTx, 0, AccessMode::Write));
    assert(fw.check_permission("default/a", PeripheralType::RmtRx, 0, AccessMode::Read));
    assert(!fw.check_permission("default/a", PeripheralType::GPIO, 4, AccessMode::Write));   // a channel pin, not a GPIO
    assert(fw.pod_rmt("default/a").size() == 2);
    const uint32_t installs = tx.installs;

    // Another pod cannot have the pins, as a channel or as a GPIO
    assert(!fw.register_pod("default/b", pod(R"("esp32.io/rmt-rx":"1")", R"("kube32.io/rmt-rx":"pin=4")"), err));
    assert(err == "Hardware Collision: GPIO 4 (RMT RX) already claimed by default/a");
    assert(!fw.register_pod("default/b", pod(R"("esp32.io/gpio-5":"1")"), err));
    assert(!rmt_state("default/b", RmtDir::Rx).installed);

    // A pod without the claim is refused at call time
    assert(fw.register_pod("default/c", pod(R"("esp32.io/gpio-6":"1")"), err));
    assert(!fw.check_permission("default/c", PeripheralType::RmtTx, 0, AccessMode::Write));
    assert(send("default/c", {100}) == HAL_ERR_NODEV);
    fw.unregister_pod("default/c");

    // Deleting the pod removes the channels and frees the pins
    fw.unregister_pod("default/a");
    assert(!rmt_state("default/a", RmtDir::Tx).installed && !rmt_state("default/a", RmtDir::Rx).installed);
    assert(fw.get_allocated_gpio_mask() == 0);
    assert(send("default/a", {100}) == HAL_ERR_NODEV);
    // ... and they install again for the next pod; carrier=0 sends bare pulses
    assert(fw.register_pod("default/a", pod(R"("esp.io/rmt-tx":"1")", R"("kube32.io/rmt-tx":"pin=4,carrier=0")"), err));
    tx = rmt_state("default/a", RmtDir::Tx);
    assert(tx.installed && tx.config.carrier_hz == 0 && tx.installs == installs + 1);
    fw.unregister_pod("default/a");

    // Bad claims and annotations
    const std::string tx_req = R"("esp32.io/rmt-tx":"1")";
    const std::string rx_req = R"("esp32.io/rmt-rx":"1")";
    assert(!fw.register_pod("default/d", pod(tx_req), err));
    assert(err == "RMT TX has no default pin on the generic board: set annotation kube32.io/rmt-tx: \"pin=<n>\"");
    assert(!fw.register_pod("default/d", pod(rx_req), err));
    assert(err.find("RMT RX has no default pin on the generic board") == 0);
    assert(!fw.register_pod("default/d", pod(R"("esp32.io/rmt-tx":"2")", R"("kube32.io/rmt-tx":"pin=4")"), err));
    assert(err.find("a pod claims one channel") != std::string::npos);
    assert(!fw.register_pod("default/d", pod(tx_req, R"("kube32.io/rmt-tx":"carrier=38000")"), err));
    assert(err.find("Invalid kube32.io/rmt-tx \"carrier=38000\" (want \"pin=<n>[,carrier=<Hz>][,duty=<percent>]\")") == 0);
    assert(!fw.register_pod("default/d", pod(tx_req, R"("kube32.io/rmt-tx":"pin=4,carrier=500")"), err));
    assert(err.find("carrier must be 0 (off) or 10000..500000 Hz") != std::string::npos);
    assert(!fw.register_pod("default/d", pod(tx_req, R"("kube32.io/rmt-tx":"pin=4,duty=0")"), err));
    assert(err.find("duty must be 1..99 percent") != std::string::npos);
    assert(!fw.register_pod("default/d", pod(tx_req, R"("kube32.io/rmt-tx":"pin=4,active=low")"), err));
    assert(err.find("unknown setting \"active\"") != std::string::npos);   // an RX setting
    assert(!fw.register_pod("default/d", pod(rx_req, R"("kube32.io/rmt-rx":"pin=5,active=middle")"), err));
    assert(err.find("active must be low or high") != std::string::npos);
    assert(!fw.register_pod("default/d", pod(rx_req, R"("kube32.io/rmt-rx":"pin=5,idle_us=100")"), err));
    assert(err.find("idle_us must be 1000..32767") != std::string::npos);
    assert(!fw.register_pod("default/d", pod(tx_req, R"("kube32.io/rmt-tx":"pin=20")"), err));
    assert(err == "Security Violation: Pin GPIO 20 (RMT TX) is reserved or unsafe.");   // native USB
    assert(!fw.register_pod("default/d", pod(tx_req, R"("kube32.io/rmt-tx":"pin=44")"), err));
    assert(err.find("GPIO 44 (RMT TX) is reserved") != std::string::npos);   // the UART0 console of a devkit
    assert(!fw.register_pod("default/d", pod(R"("esp32.io/rmt-tx":"1","esp32.io/rmt-rx":"1")",
                                             R"("kube32.io/rmt-tx":"pin=4","kube32.io/rmt-rx":"pin=4")"),
                            err));
    assert(err.find("claimed twice") != std::string::npos);
    // A claim refused after a bus was claimed leaves neither installed
    assert(!fw.register_pod("default/d", pod(R"("esp32.io/i2c-bus-0":"1","esp32.io/rmt-tx":"1")", R"("kube32.io/rmt-tx":"pin=8")"), err));
    assert(err == "GPIO 8 (RMT TX) is claimed twice by the pod");
    assert(fw.get_allocated_gpio_mask() == 0 && fw.get_allocated_i2c_mask() == 0 && !i2c_bus_state(0).installed);
    assert(!rmt_state("default/d", RmtDir::Tx).installed);

    // Four channels of each direction
    for (int i = 0; i < 4; ++i) {
        const std::string id = "default/t" + std::to_string(i);
        assert(fw.register_pod(id, pod(tx_req, R"("kube32.io/rmt-tx":"pin=)" + std::to_string(10 + i) + "\""), err));
    }
    assert(fw.get_allocated_rmt_tx_count() == 4);
    assert(!fw.register_pod("default/t4", pod(tx_req, R"("kube32.io/rmt-tx":"pin=16")"), err));
    assert(err == "Insufficient allocatable RMT TX channels: requested 1, available 0");
    assert(fw.register_pod("default/r0", pod(rx_req, R"("kube32.io/rmt-rx":"pin=16")"), err));   // RX counts apart
    fw.unregister_pod("default/t0");
    assert(fw.register_pod("default/t4", pod(tx_req, R"("kube32.io/rmt-tx":"pin=17")"), err));
    fw.reset();
    assert(fw.get_allocated_rmt_tx_count() == 0 && !rmt_state("default/t4", RmtDir::Tx).installed);
    std::cout << "[PASS] test_rmt_admission_and_release" << std::endl;
}

void test_rmt_send_and_receive() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    MockHal::instance().reset();
    std::string err;
    assert(fw.register_pod("default/ir", pod(R"("esp32.io/rmt-tx":"1","esp32.io/rmt-rx":"1")",
                                             R"("kube32.io/rmt-tx":"pin=4","kube32.io/rmt-rx":"pin=5")"),
                           err));

    // Sending: one frame per call, recorded with the carrier
    const std::vector<uint16_t> frame = nec(0x04, 0x08);
    assert(send("default/ir", frame) == 67);
    auto sent = MockHal::instance().get_rmt_tx_frames(4);
    assert(sent.size() == 1 && sent[0] == frame && MockHal::instance().get_rmt_tx_carrier(4) == 38000);
    assert(rmt_state("default/ir", RmtDir::Tx).frames == 1);
    assert(send("default/ir", {9000, 0, 560}) == HAL_ERR_INVAL);                       // a zero duration
    assert(send("default/ir", std::vector<uint16_t>(9, 60000)) == HAL_ERR_INVAL);      // 540 ms: too long
    assert(send("default/ir", std::vector<uint16_t>(513, 100)) == HAL_ERR_INVAL);      // too many durations
    assert(send("default/ir", std::vector<uint16_t>(512, 900)) == 512);
    assert(MockHal::instance().get_rmt_tx_frames(4).size() == 2);

    // Receiving: nothing yet, then the frames in order
    int32_t rc = 0;
    RmtDrops drops;
    assert(receive("default/ir", 512, rc).empty() && rc == HAL_ERR_AGAIN);
    assert(!rmt_inject(6, frame.data(), frame.size()));   // no receiver on GPIO 6
    assert(rmt_inject(5, frame.data(), frame.size()));
    const std::vector<uint16_t> repeat{9000, 2250, 560};
    assert(rmt_inject(5, repeat.data(), repeat.size()));
    // A mark longer than one symbol half arrives split and is joined again
    const std::vector<uint16_t> long_mark{40000, 600, 600};
    assert(rmt_inject(5, long_mark.data(), long_mark.size()));
    assert(rmt_state("default/ir", RmtDir::Rx).frames == 3);
    assert(receive("default/ir", 512, rc) == frame && rc == 67);
    assert(receive("default/ir", 512, rc) == repeat && rc == 3);
    assert(receive("default/ir", 512, rc) == long_mark && rc == 3);
    assert(receive("default/ir", 512, rc).empty() && rc == HAL_ERR_AGAIN);

    // A frame larger than the buffer: the first part is copied, the frame is gone
    assert(rmt_inject(5, frame.data(), frame.size()));
    std::vector<uint16_t> head = receive("default/ir", 10, rc);
    assert(rc == HAL_ERR_TOOBIG && head.size() == 10 && std::equal(head.begin(), head.end(), frame.begin()));
    assert(receive("default/ir", 512, rc).empty() && rc == HAL_ERR_AGAIN);

    // The queue holds kRmtRxQueueFrames frames; the next ones are dropped and counted
    for (uint32_t i = 0; i < kRmtRxQueueFrames + 2; ++i) assert(rmt_inject(5, repeat.data(), repeat.size()));
    assert(rmt_state("default/ir", RmtDir::Rx).frames == kRmtRxQueueFrames);
    receive("default/ir", 512, rc, &drops);
    assert(rc == 3 && drops.queue_full == 2 && drops.too_long == 0);
    receive("default/ir", 512, rc, &drops);
    assert(rc == 3 && drops.queue_full == 0);   // reported once
    while (rc != HAL_ERR_AGAIN) receive("default/ir", 512, rc);
    // ... and kRmtRxQueueDurations durations: seven NEC frames fill the queue
    for (int i = 0; i < 8; ++i) assert(rmt_inject(5, frame.data(), frame.size()));
    assert(rmt_state("default/ir", RmtDir::Rx).frames == 7);
    receive("default/ir", 512, rc, &drops);
    assert(rc == 67 && drops.queue_full == 1);
    while (rc != HAL_ERR_AGAIN) receive("default/ir", 512, rc);
    // A frame longer than kRmtMaxDurations is dropped as too long
    const std::vector<uint16_t> huge(kRmtMaxDurations + 1, 500);
    assert(rmt_inject(5, huge.data(), huge.size()));
    const std::vector<uint16_t> longest(kRmtMaxDurations, 500);
    assert(rmt_inject(5, longest.data(), longest.size()));
    assert(receive("default/ir", 512, rc, &drops) == longest && rc == static_cast<int32_t>(kRmtMaxDurations));
    assert(drops.too_long == 1 && drops.queue_full == 0);

    // IR loopback: the node's receivers see its own LED
    MockHal::instance().set_rmt_loopback(true);
    assert(send("default/ir", repeat) == 3);
    assert(receive("default/ir", 512, rc) == repeat && rc == 3);
    MockHal::instance().set_rmt_loopback(false);

    // Release empties the queue; the pins are reset and free
    assert(rmt_inject(5, repeat.data(), repeat.size()));
    fw.unregister_pod("default/ir");
    assert(!rmt_inject(5, repeat.data(), repeat.size()));
    assert(receive("default/ir", 512, rc).empty() && rc == HAL_ERR_NODEV);
    assert(fw.register_pod("default/ir", pod(R"("esp32.io/rmt-rx":"1")", R"("kube32.io/rmt-rx":"pin=5")"), err));
    assert(receive("default/ir", 512, rc).empty() && rc == HAL_ERR_AGAIN);
    fw.reset();
    MockHal::instance().reset();
    std::cout << "[PASS] test_rmt_send_and_receive" << std::endl;
}

void test_cardputer_profile_capacity_and_scheduling() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    kubelet::NodeStatusReporter reporter("node", "10.0.0.2");
    kubelet::NodeTelemetry t = reporter.collect_telemetry();
    assert(t.total_rmt_tx_count == 4 && t.total_rmt_rx_count == 4);
    std::string reg = reporter.build_registration_json(t);
    assert(reg.find("\"esp32.io/rmt-tx\":\"4\"") != std::string::npos && reg.find("\"esp32.io/rmt-rx\":\"4\"") != std::string::npos);

    std::string err;
    fw.set_board_profile(cardputer_profile());
    // The IR LED: the default TX pin, offered to RMT TX only
    assert(fw.register_pod("default/ir", pod(R"("esp32.io/rmt-tx":"1")"), err));
    RmtState tx = rmt_state("default/ir", RmtDir::Tx);
    assert(tx.installed && tx.config.pin == 44 && tx.config.carrier_hz == 38000);
    t = reporter.collect_telemetry();
    assert(t.total_rmt_tx_count == 4 && t.allocatable_rmt_tx_count == 3);
    fw.unregister_pod("default/ir");
    assert(!fw.register_pod("default/g", pod(R"("esp32.io/gpio-44":"1")"), err));
    assert(!fw.register_pod("default/g", pod(R"("esp32.io/rmt-rx":"1")", R"("kube32.io/rmt-rx":"pin=44")"), err));
    assert(err.find("GPIO 44 (RMT RX) is reserved") != std::string::npos);
    assert(!fw.register_pod("default/g", pod(R"("esp32.io/rmt-rx":"1")"), err));
    assert(err.find("RMT RX has no default pin on the cardputer board") == 0);
    assert(!fw.register_pod("default/g", pod(R"("esp32.io/rmt-tx":"1")", R"("kube32.io/rmt-tx":"pin=8")"), err));
    assert(err == "Security Violation: Pin GPIO 8 (RMT TX) is used by the cardputer board.");

    // The LED and the Grove port are separate pins: an I2C pod on GPIO 2/1
    // and a TX-only pod on GPIO 44 run side by side; a receiver on the
    // Grove port does not fit beside the I2C pod
    assert(fw.register_pod("default/rfid", pod(R"("esp32.io/i2c-bus-0":"1")"), err));
    assert(fw.register_pod("default/ir", pod(R"("esp32.io/rmt-tx":"1")"), err));
    assert(i2c_bus_state(0).installed && i2c_bus_state(0).pin_a == 2 && i2c_bus_state(0).pin_b == 1);
    assert(rmt_state("default/ir", RmtDir::Tx).config.pin == 44);
    assert(!fw.register_pod("default/rx", pod(R"("esp32.io/rmt-rx":"1")", R"("kube32.io/rmt-rx":"pin=1")"), err));
    assert(err == "Hardware Collision: GPIO 1 (RMT RX) already claimed by default/rfid");
    fw.unregister_pod("default/ir");
    fw.unregister_pod("default/rfid");

    // A board with no pin for a receiver advertises none
    BoardProfile bare = cardputer_profile();
    bare.reserved_gpio_mask = KUBE32_CHIP_GPIO_MASK;
    fw.set_board_profile(bare);
    t = reporter.collect_telemetry();
    assert(t.total_rmt_tx_count == 1 && t.total_rmt_rx_count == 0);
    reg = reporter.build_registration_json(t);
    assert(reg.find("\"esp32.io/rmt-tx\":\"1\"") != std::string::npos && reg.find("rmt-rx") == std::string::npos);
    fw.reset();

    // The scheduler counts the claims against the node's allocatable channels
    const std::string req = R"({"spec":{"containers":[{"name":"c","resources":{"requests":{"esp32.io/rmt-tx":"1","esp32.io/rmt-rx":"1"}}}]}})";
    scheduler::ResourceSpec want = scheduler::extract_pod_resource_requests(req);
    assert(want.rmt_tx_channels == 1 && want.rmt_rx_channels == 1);
    scheduler::ResourceSpec alloc = scheduler::extract_node_allocatable(R"({"status":{"allocatable":{"esp32.io/rmt-tx":"1"}}})");
    assert(alloc.rmt_tx_channels == 1 && alloc.rmt_rx_channels == 0);
    scheduler::NodePeripheralPredicate pred;
    std::string reason;
    scheduler::ResourceSpec used;
    assert(!pred.evaluate("", "", want, alloc, used, reason) && reason == "Insufficient esp32.io/rmt-rx");
    alloc.rmt_rx_channels = 1;
    assert(pred.evaluate("", "", want, alloc, used, reason));
    used.rmt_tx_channels = 1;
    assert(!pred.evaluate("", "", want, alloc, used, reason) && reason == "Insufficient esp32.io/rmt-tx");
    std::cout << "[PASS] test_cardputer_profile_capacity_and_scheduling" << std::endl;
}

// The kubelet runs the irremote example on a Cardputer profile: frames
// injected at the receiver are decoded, logged and published; ir.send
// messages and POST /send go out on the LED; the pod without an RX claim
// only sends; deleting the pod releases the channels.
class IrClient : public kubelet::KubeApiClient {
public:
    std::vector<std::pair<models::KubePod, std::string>> pods;
    std::string last_patch;
    bool register_node(std::string_view, bool& c) override { c = false; return true; }
    bool patch_node_status(const std::string&, std::string_view) override { return true; }
    bool list_pods(std::vector<models::KubePod>& out) override { for (auto& p : pods) out.push_back(p.first); return true; }
    bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out) override { out = pods; return true; }
    bool patch_pod_status(const std::string&, const std::string&, const std::string& p) override { last_patch = p; return true; }
    int get(const std::string&, std::string& body) override { body = "{\"items\":[]}"; return 200; }
};

std::string ir_pod(const std::string& name, bool rx, const std::string& env) {
    return R"({"metadata":{"name":")" + name + R"(","namespace":"default","uid":")" + name + R"(-uid",
               "annotations":{"kube32.io/event-topics":"ir.send")" + (rx ? R"(,"kube32.io/rmt-rx":"pin=1")" : "") + R"(}},
               "spec":{"nodeName":"node-a","restartPolicy":"Always","containers":[{"name":"irremote","image":"wasm://embedded/irremote",
               "env":[)" + env + R"(],
               "resources":{"requests":{"memory":"12Ki","esp32.io/rmt-tx":"1")" + (rx ? R"(,"esp32.io/rmt-rx":"1")" : "") +
           R"(,"hardware.kube32.io/ipc":"1"},"limits":{"memory":"12Ki"}}}]}})";
}

class StringSink : public runtime::AppResponseSink {
public:
    int status{0};
    std::string body;
    void begin(int s, std::string_view) override { status = s; }
    void header(std::string_view, std::string_view) override {}
    bool write(const uint8_t* data, size_t len) override { body.append(reinterpret_cast<const char*>(data), len); return true; }
    void end() override {}
};

int request(const std::string& pod_id, const char* method, const char* path, const std::string& body, std::string& out) {
    runtime::AppRequest req;
    req.method = method;
    req.path_and_query = path;
    req.body = body;
    StringSink sink;
    int rc = runtime::AppPodHost::instance().handle(pod_id, req, sink);
    out = sink.body;
    return rc;
}

void test_irremote_pod() {
    size_t n = 0;
    const auto* imgs = embedded::images(n);
    runtime::ImageRegistry::instance().add_all(imgs, n);   // images/*.wasm, as the firmware has them
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    fw.set_board_profile(cardputer_profile());
    MockHal::instance().reset();
    runtime::PodLogStore::runtime().clear();
    static std::mutex s_events_mutex;
    static std::vector<std::pair<std::string, std::string>> s_events;
    runtime::AppPlatformHooks hooks = runtime::app_platform_hooks();
    hooks.event_emit = [](const std::string&, std::string_view topic, const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lock(s_events_mutex);
        s_events.emplace_back(std::string(topic), std::string(reinterpret_cast<const char*>(data), len));
        return static_cast<int32_t>(len);
    };
    runtime::set_app_platform_hooks(hooks);
    auto events_have = [](const std::string& topic, const std::string& data) {
        std::lock_guard<std::mutex> lock(s_events_mutex);
        for (const auto& e : s_events) if (e.first == topic && e.second == data) return true;
        return false;
    };

    auto rt = runtime::create_wasm_runtime();
    kubelet::KubeletConfig cfg;
    cfg.node_name = "node-a";
    cfg.node_ip = "10.0.0.2";
    auto client = std::make_unique<IrClient>();
    IrClient* c = client.get();
    models::KubePod p;
    p.name = "irremote";
    p.namespace_name = "default";
    p.node_name = "node-a";
    c->pods.emplace_back(p, ir_pod("irremote", true, ""));
    kubelet::Kubelet k(cfg, *rt, std::move(client));
    const std::string id = "default/irremote";

    k.sync_assigned_pods();
    RmtState tx = rmt_state(id, RmtDir::Tx);
    RmtState rx = rmt_state(id, RmtDir::Rx);
    assert(tx.installed && tx.config.pin == 44 && rx.installed && rx.config.pin == 1 && rx.config.active_low);
    const uint32_t tx_installs = tx.installs;
    assert(wait_for([&]() { return log_has(id, "irremote: receiving"); }, 3000));
    auto& host = runtime::AppPodHost::instance();
    uint32_t now = 1000;

    // A remote's NEC code, its repeat, and a frame of another protocol
    const std::vector<uint16_t> code = nec(0x04, 0x08);
    assert(rmt_inject(1, code.data(), code.size()));
    const std::vector<uint16_t> repeat{9000, 2250, 560};
    assert(rmt_inject(1, repeat.data(), repeat.size()));
    const std::vector<uint16_t> other{2400, 600, 1200, 600, 600};
    assert(rmt_inject(1, other.data(), other.size()));
    assert(host.tick(id, now += 1000) == 1);   // frames taken: tick again soon
    assert(log_has(id, "irremote: nec addr=0x04 cmd=0x08"));
    assert(log_has(id, "irremote: nec repeat"));
    assert(log_has(id, "irremote: raw n=5 2400,600,1200,600,600"));
    assert(events_have("ir.received", "nec 0x04 0x08") && events_have("ir.received", "nec repeat"));
    assert(host.tick(id, now += 1000) == 0);
    // Extended NEC: a 16-bit address
    std::vector<uint16_t> ext = nec(0x34, 0x56);
    for (int i = 0; i < 8; ++i) ext[3 + 2 * (8 + i)] = ((0x12 >> i) & 1) ? 1690 : 560;
    assert(rmt_inject(1, ext.data(), ext.size()));
    host.tick(id, now += 1000);
    assert(log_has(id, "irremote: nec addr=0x1234 cmd=0x56"));

    // ir.send goes out on the LED, NEC with the address complement
    const std::string msg = "nec 0x10 0x20";
    host.event(id, "ir.send", reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    auto sent = MockHal::instance().get_rmt_tx_frames(44);
    assert(sent.size() == 1 && sent[0] == nec(0x10, 0x20));
    assert(log_has(id, "irremote: sent nec addr=0x10 cmd=0x20"));
    const std::string rep = "nec repeat";
    host.event(id, "ir.send", reinterpret_cast<const uint8_t*>(rep.data()), rep.size());
    assert(MockHal::instance().get_rmt_tx_frames(44).back() == repeat);
    const std::string bad = "sony 1 2";
    host.event(id, "ir.send", reinterpret_cast<const uint8_t*>(bad.data()), bad.size());
    assert(log_has(id, "irremote: cannot send: want"));
    assert(MockHal::instance().get_rmt_tx_frames(44).size() == 2);

    // GET / lists the recent codes, newest first; POST /send transmits
    std::string body;
    assert(request(id, "GET", "/", "", body) == 200);
    assert(body.find("\"received\":4,\"sent\":2,\"receiving\":true") != std::string::npos);
    assert(body.find("\"code\":\"nec 0x1234 0x56\"") < body.find("\"code\":\"nec 0x04 0x08\""));
    assert(body.find("{\"t_ms\":") != std::string::npos && body.find("\"code\":\"raw\",\"n\":5") != std::string::npos);
    assert(request(id, "POST", "/send", "raw 9000,4500,560", body) == 200 && body == "{\"sent\":3}\n");
    assert(MockHal::instance().get_rmt_tx_frames(44).back() == std::vector<uint16_t>({9000, 4500, 560}));
    assert(request(id, "POST", "/ir/send", "raw 9000,0,560", body) == 400);
    assert(request(id, "POST", "/send", "nec 0x10", body) == 400);
    assert(request(id, "DELETE", "/", "", body) == 405);

    // With the IR loopback a frame sent is received, decoded and published
    MockHal::instance().set_rmt_loopback(true);
    const std::string loop = "nec 0x22 0x33";
    host.event(id, "ir.send", reinterpret_cast<const uint8_t*>(loop.data()), loop.size());
    host.tick(id, now += 1000);
    assert(log_has(id, "irremote: nec addr=0x22 cmd=0x33") && events_have("ir.received", "nec 0x22 0x33"));
    MockHal::instance().set_rmt_loopback(false);

    // A remote held down between two ticks: the queue keeps eight frames,
    // the pod's log says how many it missed
    for (int i = 0; i < 10; ++i) assert(rmt_inject(1, repeat.data(), repeat.size()));
    assert(host.tick(id, now += 1000) == 1);
    assert(log_has(id, "[kube32] RMT receiver dropped 2 frame(s) on a full queue"));
    assert(host.tick(id, now += 1000) == 0);

    // Delete: the channels go and the pins are free
    c->pods.clear();
    k.sync_assigned_pods();
    assert(!rmt_state(id, RmtDir::Tx).installed && !rmt_state(id, RmtDir::Rx).installed);
    assert(fw.get_allocated_gpio_mask() == 0);

    // Sending a code periodically (IR_SEND_EVERY_S), seen by the pod's own
    // receiver through the loopback
    runtime::PodLogStore::runtime().clear();
    MockHal::instance().reset();
    MockHal::instance().set_rmt_loopback(true);
    c->pods.emplace_back(p, ir_pod("irremote", true, R"({"name":"IR_SEND_EVERY_S","value":"2"},{"name":"IR_SEND_CODE","value":"nec 0x01 0x02"})"));
    k.sync_assigned_pods();
    assert(rmt_state(id, RmtDir::Tx).installs == tx_installs + 1);
    assert(wait_for([&]() { return log_has(id, "irremote: sending \"nec 0x01 0x02\" every 2 s"); }, 3000));
    now = 5000;
    host.tick(id, now);                    // sends
    host.tick(id, now += 500);             // receives its own frame
    host.tick(id, now += 1000);            // not yet due
    host.tick(id, now += 1000);            // sends again
    assert(log_count(id, "irremote: sent nec addr=0x01 cmd=0x02") == 2);
    assert(log_has(id, "irremote: nec addr=0x01 cmd=0x02"));
    MockHal::instance().set_rmt_loopback(false);
    c->pods.clear();
    k.sync_assigned_pods();

    // TX only (the LED on GPIO 44) beside an I2C pod on the Grove port: the
    // missing receiver is said once, sending works
    std::string err;
    assert(fw.register_pod("default/rfid", pod(R"("esp32.io/i2c-bus-0":"1")"), err));
    runtime::PodLogStore::runtime().clear();
    c->pods.emplace_back(p, ir_pod("irremote", false, ""));
    k.sync_assigned_pods();
    assert(rmt_state(id, RmtDir::Tx).installed && !rmt_state(id, RmtDir::Rx).installed);
    assert(wait_for([&]() { return log_has(id, "irremote: no esp32.io/rmt-rx claim: not receiving, sending only"); }, 3000));
    for (int i = 0; i < 5; ++i) assert(host.tick(id, now += 1000) == 0);
    assert(log_count(id, "irremote: no esp32.io/rmt-rx claim") == 1);
    assert(log_count(id, "receive failed") == 0);
    host.event(id, "ir.send", reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    assert(MockHal::instance().get_rmt_tx_frames(44).back() == nec(0x10, 0x20));
    assert(request(id, "GET", "/", "", body) == 200 && body.find("\"receiving\":false") != std::string::npos);
    c->pods.clear();
    k.sync_assigned_pods();
    fw.unregister_pod("default/rfid");
    assert(fw.get_allocated_gpio_mask() == 0);
    fw.reset();
    MockHal::instance().reset();
    std::cout << "[PASS] test_irremote_pod" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== RMT (IR) for pods ===" << std::endl;
    test_rmt_admission_and_release();
    test_rmt_send_and_receive();
    test_cardputer_profile_capacity_and_scheduling();
    test_irremote_pod();
    std::cout << "=== All pod RMT tests passed ===" << std::endl;
    return 0;
}
