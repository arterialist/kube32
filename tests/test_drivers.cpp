// The device drivers (apps/drivers) against simulated parts on the stub I2C
// bus and the stub GPIO pins: the driver HTTP API (GET /, GET /describe,
// POST / actions), the events on DEVICE_TOPIC, a part that is absent or
// goes silent and comes back (503, one log line, no flood), and what each
// driver takes from a board's pod pool.
//
//   bme688     calibration, forced mode, integer compensation against the
//              datasheet's floating-point formulas, heater set-up, BME680 gas
//   miniscale  weight and raw value, tare, LED, negative weights
//   rfid       tags coming into and leaving the field
//   chip-temp  the esp32_chip_temp_read import and its grant
//   button     debounce, presses, active level and pull, periodic reports
//   switch     initial state, set, toggle, active low
//
// and the room-monitor example (apps/room-monitor) reaching drivers through
// Services that select them by label. Each driver also runs loaded as the
// kubelet loads it with its operand stack painted, to show the stack the
// runtime sizes from the module holds what its calls use, and a pod's first
// tick is shown to wait for its k32_init. (tests/test_driver_frames.cpp
// measures the native stack the calls take on a board.)

#include "pod_test_util.hpp"
#include "mfrc522_sim.hpp"
#include "device_sims.hpp"
#include "app/service_client.hpp"
#include "hal/mock_hal.hpp"
#include "runtime/container_lifecycle.hpp"
#include "runtime/k32_app_abi.h"
#include "runtime/pod_memory_pool.h"
#include "osal/system.hpp"
#include "m3_env.h"
#include "runtime/pod_log.hpp"
#include "storage/store.hpp"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using namespace kube32;
using hal::MockHal;
using k32test::Bme68x;
using k32test::Calib;
using k32test::MiniScale;

namespace {

// ---------------------------------------------------------------------------
// Pods, events and the pod log
// ---------------------------------------------------------------------------
struct Event { std::string topic, body; };
std::mutex g_events_mutex;
std::vector<Event> g_events;

void record_events() {
    runtime::AppPlatformHooks h = runtime::app_platform_hooks();
    h.event_emit = [](const std::string&, std::string_view topic, const uint8_t* d, size_t n) -> int32_t {
        std::lock_guard<std::mutex> lock(g_events_mutex);
        g_events.push_back({std::string(topic), std::string(reinterpret_cast<const char*>(d), n)});
        return static_cast<int32_t>(n);
    };
    runtime::set_app_platform_hooks(h);
}

std::vector<Event> take_events(const std::string& topic) {
    std::lock_guard<std::mutex> lock(g_events_mutex);
    std::vector<Event> mine, rest;
    for (auto& e : g_events) (e.topic == topic ? mine : rest).push_back(e);
    g_events = rest;
    return mine;
}

std::vector<uint8_t> image(const std::string& name) {
    std::vector<uint8_t> w = k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/images/" + name + ".wasm");
    if (w.empty()) std::cout << "missing images/" << name << ".wasm (make -C apps/drivers)" << std::endl;
    assert(!w.empty());
    return w;
}

using Env = std::vector<std::pair<std::string, std::string>>;

// Pod default/<name> running images/driver-<driver>.wasm with the resource
// requests `requests` (JSON members) and the pod annotations `annotations`
void deploy(runtime::WasmRuntime& rt, const std::string& name, const std::string& driver, const std::string& requests, const Env& env,
            const std::string& annotations = "") {
    const std::string pod_id = "default/" + name;
    const std::string manifest = "{\"metadata\":{\"name\":\"" + name + "\",\"annotations\":{" + annotations + "}},"
                                 "\"spec\":{\"containers\":[{\"name\":\"driver\",\"image\":\"wasm://embedded/driver-" + driver + "\","
                                 "\"resources\":{\"requests\":{\"memory\":\"3Ki\"" + (requests.empty() ? "" : "," + requests) + "}}}]}}";
    std::string err;
    auto& fw = hal::CapabilityFirewall::instance();
    fw.unregister_pod(pod_id);
    bool ok = fw.register_pod(pod_id, manifest, err);
    if (!ok) std::cout << "admission of " << pod_id << ": " << err << std::endl;
    assert(ok);
    const std::vector<uint8_t> wasm = image("driver-" + driver);
    runtime::DeploySpec spec;
    spec.memory_limit_bytes = 3072;
    spec.stack_size_bytes = 4096;
    spec.env = env;
    ok = rt.deploy_workload_ex(pod_id, wasm.data(), wasm.size(), spec, err);
    if (!ok) std::cout << "deploy " << pod_id << ": " << err << std::endl;
    assert(ok);
    assert(rt.start(pod_id));
}

void remove(runtime::WasmRuntime& rt, const std::string& name) {
    assert(rt.terminate("default/" + name));
    hal::CapabilityFirewall::instance().unregister_pod("default/" + name);
}

int32_t tick(const std::string& name, uint32_t now) { return runtime::AppPodHost::instance().tick("default/" + name, now); }

// Ticks until the driver has nothing pending (k32_tick returns 0), 10 ms
// apart as the node ticks such a pod; returns the time of the last tick
uint32_t settle(const std::string& name, uint32_t now) {
    for (int i = 0; i < 1000 && tick(name, now) > 0; ++i) now += 10;
    return now;
}

k32test::Collect get(const std::string& name, const std::string& path = "/") { return k32test::call_pod("default/" + name, path); }
k32test::Collect post(const std::string& name, const std::string& body) { return k32test::call_pod("default/" + name, "/", "POST", body); }

int log_count(const std::string& name, const std::string& needle) {
    std::vector<runtime::PodLogRecord> lines;
    runtime::PodLogStore::runtime().tail("default/" + name, 0, lines);
    int n = 0;
    for (const auto& l : lines) if (l.text.find(needle) != std::string::npos) n++;
    return n;
}

bool has(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

// The number after "key": in a JSON text
double num(const std::string& body, const std::string& key) {
    const size_t at = body.find("\"" + key + "\":");
    if (at == std::string::npos) std::cout << "no " << key << " in " << body << std::endl;
    assert(at != std::string::npos);
    return std::strtod(body.c_str() + at + key.size() + 3, nullptr);
}

// Leaves "<key>":<value>, out of a flat JSON text
std::string without(std::string s, const std::string& key) {
    const size_t at = s.find("\"" + key + "\":");
    if (at == std::string::npos) return s;
    size_t end = at + key.size() + 3;
    if (s[end] == '"') end = s.find('"', end + 1) + 1;
    else while (end < s.size() && s[end] != ',' && s[end] != '}') end++;
    if (s[end] == ',') end++;
    else if (at > 0 && s[at - 1] == ',') return s.erase(at - 1, end - at + 1);
    return s.erase(at, end - at);
}

// The event published with a reading is the GET / body, or what fits an
// event of it: without uptime_ms, then node, then device and driver
void check_event_is_body(const std::string& name, const std::string& topic) {
    std::vector<Event> ev = take_events(topic);
    assert(!ev.empty());
    std::string body = get(name).body;
    while (!body.empty() && body.back() == '\n') body.pop_back();
    std::string want = body;
    for (const char* drop : {"uptime_ms", "node", "device"}) {
        if (want.size() <= K32_MAX_EVENT_BYTES) break;
        want = without(want, drop);
        if (std::strcmp(drop, "device") == 0) want = without(want, "driver");
    }
    if (ev.back().body != want) std::cout << "event " << ev.back().body << "\nwant  " << want << std::endl;
    assert(ev.back().body == want && want.size() <= K32_MAX_EVENT_BYTES);
}

// ---------------------------------------------------------------------------
// What the BME68x datasheet says the readings are (tests/device_sims.hpp has
// the simulated parts)
// ---------------------------------------------------------------------------
// The datasheet's floating-point compensation (BME68x Sensor API, FPU build)
struct Compensated { double t, p, h, gas_high, gas_low; };
Compensated compensate(const Calib& c, uint32_t adc_t, uint32_t adc_p, uint32_t adc_h, uint16_t adc_gas, uint8_t range) {
    Compensated r{};
    double v1 = ((adc_t / 16384.0) - (c.t1 / 1024.0)) * c.t2;
    double v2 = ((adc_t / 131072.0) - (c.t1 / 8192.0)) * ((adc_t / 131072.0) - (c.t1 / 8192.0)) * (c.t3 * 16.0);
    const double t_fine = v1 + v2;
    r.t = t_fine / 5120.0;
    v1 = t_fine / 2.0 - 64000.0;
    v2 = v1 * v1 * (c.p6 / 131072.0);
    v2 = v2 + v1 * c.p5 * 2.0;
    v2 = v2 / 4.0 + c.p4 * 65536.0;
    v1 = ((c.p3 * v1 * v1) / 16384.0 + c.p2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * c.p1;
    double p = 1048576.0 - adc_p;
    p = ((p - v2 / 4096.0) * 6250.0) / v1;
    v1 = c.p9 * p * p / 2147483648.0;
    v2 = p * (c.p8 / 32768.0);
    const double v3 = (p / 256.0) * (p / 256.0) * (p / 256.0) * (c.p10 / 131072.0);
    r.p = p + (v1 + v2 + v3 + c.p7 * 128.0) / 16.0;
    const double tc = r.t;
    v1 = adc_h - (c.h1 * 16.0 + (c.h3 / 2.0) * tc);
    v2 = v1 * (c.h2 / 262144.0 * (1.0 + c.h4 / 16384.0 * tc + c.h5 / 1048576.0 * tc * tc));
    r.h = v2 + (c.h6 / 16384.0 + c.h7 / 2097152.0 * tc) * v2 * v2;
    r.gas_high = 1000000.0 * (262144 >> range) / (4096 + (adc_gas - 512) * 3);
    static const double k1[16] = {0, 0, 0, 0, 0, -1, 0, -0.8, 0, 0, -0.2, -0.5, 0, -1, 0, 0};
    static const double k2[16] = {0, 0, 0, 0, 0.1, 0.7, 0, -0.8, -0.1, 0, 0, 0, 0, 0, 0, 0};
    const double g1 = (1340.0 + 5.0 * c.range_sw_err) * (1.0 + k1[range] / 100.0);
    const double g3 = 1.0 + k2[range] / 100.0;
    r.gas_low = 1.0 / (g3 * 0.000000125 * (1 << range) * ((adc_gas - 512.0) / g1 + 1.0));
    return r;
}

// The heater resistance code for a target temperature (datasheet, floating point)
int res_heat(const Calib& c, double target, double ambient) {
    const double v1 = c.gh1 / 16.0 + 49.0;
    const double v2 = (c.gh2 / 32768.0) * 0.0005 + 0.00235;
    const double v3 = c.gh3 / 1024.0;
    const double v4 = v1 * (1.0 + v2 * target);
    const double v5 = v4 + v3 * ambient;
    return static_cast<int>(3.4 * (v5 * (4.0 / (4.0 + c.res_heat_range)) * (1.0 / (1.0 + c.res_heat_val * 0.002)) - 25));
}

const std::string kIpc = "\"hardware.kube32.io/ipc\":\"1\"";

// ---------------------------------------------------------------------------
// bme688
// ---------------------------------------------------------------------------
void test_bme688(runtime::WasmRuntime& rt) {
    std::cout << "=== driver-bme688 ===" << std::endl;
    const Calib cal;
    auto part = std::make_shared<Bme68x>(1, cal);
    MockHal::instance().register_mock_i2c_device(0, 0x77, part);
    deploy(rt, "env-driver", "bme688", "\"esp32.io/i2c-bus-0\":\"1\"," + kIpc,
           {{"DEVICE_NAME", "kitchen-env"}, {"NODE_NAME", "node-a"}, {"DEVICE_TOPIC", "device.kitchen-env"}, {"DEVICE_INTERVAL_MS", "5000"},
            {"I2C_BUS", "0"}, {"I2C_ADDR", "0x77"}});
    // Nothing read yet
    k32test::Collect c = get("env-driver");
    assert(c.status == 503 && has(c.body, "\"ok\":false,\"error\":\"no reading yet\""));

    // One measurement: started on the first tick, the result ticks later,
    // without the interpreter waiting for it
    uint32_t now = 100000;
    assert(tick("env-driver", now) == 1);
    assert(part->forced == 1);
    assert(log_count("env-driver", "bme688: BME688 at 0x77 on bus 0, heater 300 C for 100 ms") == 1);
    assert(tick("env-driver", now + 10) == 1);           // the heater is still on
    now = settle("env-driver", now + 10);
    assert(now >= 100000 + 43 + 100);
    c = get("env-driver");
    std::cout << "[bme688] " << c.body;
    assert(c.status == 200);
    assert(has(c.body, "{\"device\":\"kitchen-env\",\"driver\":\"bme688\",\"node\":\"node-a\",\"ok\":true,\"at\":"));
    const Compensated want = compensate(cal, part->adc_t, part->adc_p, part->adc_h, part->adc_gas, part->gas_range);
    std::cout << "[bme688] floating point: " << want.t << " C, " << want.p / 100 << " hPa, " << want.h << " %RH, " << want.gas_high << " ohm" << std::endl;
    assert(std::fabs(num(c.body, "temperature_c") - want.t) < 0.015);
    assert(std::fabs(num(c.body, "pressure_hpa") - want.p / 100) < 0.05);
    assert(std::fabs(num(c.body, "humidity_pct") - want.h) < 0.05);
    assert(std::fabs(num(c.body, "gas_ohms") - want.gas_high) / want.gas_high < 0.001);
    assert(num(c.body, "uptime_ms") == now);
    // The heater step: 300 C from 25 C ambient for the first measurement, 100 ms, gas on (BME688: run_gas 0x20)
    assert(std::abs(part->regs[0x5A] - res_heat(cal, 300, 25)) <= 1);
    assert(part->regs[0x64] == 0x59);                     // 25 x 4 ms
    assert(part->regs[0x71] == 0x20 && part->regs[0x70] == 0x00 && part->regs[0x72] == 0x01);
    check_event_is_body("env-driver", "device.kitchen-env");

    // The next one: the heater target from the temperature just measured
    part->adc_t = 520000;
    now = settle("env-driver", now + 5000);
    c = get("env-driver");
    const Compensated warm = compensate(cal, part->adc_t, part->adc_p, part->adc_h, part->adc_gas, part->gas_range);
    assert(std::fabs(num(c.body, "temperature_c") - warm.t) < 0.015);
    assert(std::abs(part->regs[0x5A] - res_heat(cal, 300, std::floor(want.t))) <= 1);
    assert(take_events("device.kitchen-env").size() == 1);

    // The heater did not reach its temperature: no gas reading
    part->heat_stab = false;
    now = settle("env-driver", now + 5000);
    c = get("env-driver");
    assert(c.status == 200 && has(c.body, "\"gas_ohms\":null"));
    assert(take_events("device.kitchen-env").size() == 1);
    part->heat_stab = true;

    // GET /describe; no actions
    c = get("env-driver", "/describe");
    assert(c.status == 200 && has(c.body, "\"driver\":\"bme688\"") && has(c.body, "\"pressure_hpa\":\"hPa\"") && has(c.body, "\"actions\":[]"));
    c = post("env-driver", "{\"action\":\"tare\"}");
    assert(c.status == 400 && has(c.body, "bme688 takes no actions"));
    c = post("env-driver", "{}");
    assert(c.status == 400);
    c = get("env-driver", "/nope");
    assert(c.status == 404);

    // The part goes silent: 503, one log line and one event however many ticks fail
    part->absent = true;
    for (int i = 0; i < 20; ++i) now = settle("env-driver", now + 5000);
    c = get("env-driver");
    std::cout << "[bme688 absent] " << c.body;
    assert(c.status == 503 && has(c.body, "\"ok\":false,\"error\":\"no answer at 0x77 on bus 0 (error -19)\""));
    assert(log_count("env-driver", "bme688: no answer at 0x77 on bus 0 (error -19), retrying every 5000 ms") == 1);
    std::vector<Event> ev = take_events("device.kitchen-env");
    assert(ev.size() == 1 && has(ev[0].body, "\"ok\":false"));
    // Back: identified and calibrated again
    part->absent = false;
    now = settle("env-driver", now + 5000);
    assert(get("env-driver").status == 200);
    assert(log_count("env-driver", "bme688: BME688 at 0x77 on bus 0") == 1);
    assert(log_count("env-driver", "bme688: answering again at 0x77 on bus 0") == 1);
    take_events("device.kitchen-env");

    // Another chip at the address
    part->regs[0xD0] = 0x58;
    part->absent = true;
    now = settle("env-driver", now + 5000);
    part->absent = false;
    now = settle("env-driver", now + 5000);
    c = get("env-driver");
    assert(c.status == 503 && has(c.body, "another chip (its id is not 0x61) answers at 0x77 on bus 0"));
    part->regs[0xD0] = 0x61;
    remove(rt, "env-driver");

    // A BME680 (variant 0): gas registers 0x2A/0x2B and the other gas formula; gas off
    Calib cal680;
    cal680.range_sw_err = 2;
    auto bme680 = std::make_shared<Bme68x>(0, cal680);
    bme680->adc_gas = 700;
    bme680->gas_range = 7;
    bme680->busy_reads = 0;
    MockHal::instance().register_mock_i2c_device(0, 0x76, bme680);
    deploy(rt, "bme680", "bme688", "\"esp32.io/i2c-bus-0\":\"1\"," + kIpc, {{"I2C_ADDR", "76"}, {"CONFIG_HEATER_C", "320"}, {"CONFIG_HEATER_MS", "150"}});
    now = settle("bme680", now + 10);
    c = get("bme680");
    std::cout << "[bme680] " << c.body;
    const Compensated g680 = compensate(cal680, bme680->adc_t, bme680->adc_p, bme680->adc_h, bme680->adc_gas, bme680->gas_range);
    std::cout << "[bme680] floating point gas: " << g680.gas_low << " ohm" << std::endl;
    assert(has(c.body, "{\"device\":\"bme688\",\"driver\":\"bme688\",\"ok\":true"));   // no DEVICE_NAME, no NODE_NAME
    assert(std::fabs(num(c.body, "gas_ohms") - g680.gas_low) / g680.gas_low < 0.01);
    assert(bme680->regs[0x71] == 0x10 && bme680->regs[0x64] == (150 / 4 + 64));
    assert(log_count("bme680", "bme688: BME680 at 0x76 on bus 0, heater 320 C for 150 ms") == 1);
    assert(take_events("device.bme688").size() == 1);
    remove(rt, "bme680");

    deploy(rt, "bme680-nogas", "bme688", "\"esp32.io/i2c-bus-0\":\"1\"", {{"I2C_ADDR", "0x76"}, {"CONFIG_GAS", "off"}});
    const int forced = bme680->forced;
    now = settle("bme680-nogas", now + 10);
    c = get("bme680-nogas");
    assert(c.status == 200 && !has(c.body, "gas_ohms") && bme680->forced == forced + 1);
    assert(bme680->regs[0x70] == 0x08 && bme680->regs[0x71] == 0x00);
    // Without hardware.kube32.io/ipc: served over HTTP, one line about events
    assert(log_count("bme680-nogas", "bme688: cannot publish on device.bme688 (error -1, no hardware.kube32.io/ipc grant)") == 1);
    now = settle("bme680-nogas", now + 5000);
    assert(log_count("bme680-nogas", "cannot publish") == 1);
    remove(rt, "bme680-nogas");

    // No part at all: one line, 503
    deploy(rt, "nobody", "bme688", "\"esp32.io/i2c-bus-0\":\"1\"", {{"I2C_ADDR", "0x75"}});
    for (int i = 0; i < 10; ++i) now = settle("nobody", now + 5000);
    assert(get("nobody").status == 503 && log_count("nobody", "bme688: no answer at 0x75 on bus 0") == 1);
    remove(rt, "nobody");
    std::cout << "[PASS] driver-bme688" << std::endl;
}

// ---------------------------------------------------------------------------
// miniscale
// ---------------------------------------------------------------------------
void test_miniscale(runtime::WasmRuntime& rt) {
    std::cout << "=== driver-miniscale ===" << std::endl;
    auto part = std::make_shared<MiniScale>();
    MockHal::instance().register_mock_i2c_device(1, 0x26, part);
    deploy(rt, "scale-driver", "miniscale", "\"esp32.io/i2c-bus-1\":\"1\"," + kIpc,
           {{"DEVICE_NAME", "kitchen-scale"}, {"NODE_NAME", "node-a"}, {"I2C_BUS", "1"}, {"I2C_ADDR", "0x26"}}, "\"kube32.io/i2c-bus-1\":\"sda=10,scl=11\"");
    assert(log_count("scale-driver", "miniscale: Unit Mini Scales at 0x26 on bus 1, firmware 2") == 1);
    uint32_t now = 200000;
    assert(tick("scale-driver", now) == 0);
    k32test::Collect c = get("scale-driver");
    std::cout << "[miniscale] " << c.body;
    assert(c.status == 200 && has(c.body, "\"readings\":{\"weight_g\":246.90,\"raw\":812345}"));
    check_event_is_body("scale-driver", "device.kitchen-scale");

    // Tare: the next reading is relative to the new zero
    c = post("scale-driver", "{\"action\": \"tare\"}");
    assert(c.status == 200 && part->tares == 1 && has(c.body, "\"ok\":true"));
    tick("scale-driver", now += 1000);
    assert(has(get("scale-driver").body, "\"weight_g\":0.00,"));
    // Lighter than at the tare: a negative weight
    part->raw -= 617;
    tick("scale-driver", now += 1000);
    assert(has(get("scale-driver").body, "\"weight_g\":-12.34,"));

    // The LED
    c = post("scale-driver", "{\"action\":\"led\",\"r\":255,\"g\":16}");
    assert(c.status == 200 && part->led[0] == 255 && part->led[1] == 16 && part->led[2] == 0);
    c = post("scale-driver", "{\"action\":\"led\",\"r\":256}");
    assert(c.status == 400 && has(c.body, "led takes r, g and b from 0 to 255") && part->led[0] == 255);
    c = post("scale-driver", "{\"action\":\"weigh\"}");
    assert(c.status == 400);
    c = get("scale-driver", "/describe");
    assert(has(c.body, "{\"action\":\"tare\"},{\"action\":\"led\""));

    // Unplugged and back
    part->absent = true;
    for (int i = 0; i < 10; ++i) tick("scale-driver", now += 1000);
    c = get("scale-driver");
    assert(c.status == 503 && has(c.body, "the scale does not answer at 0x26 on bus 1 (error -19)"));
    c = post("scale-driver", "{\"action\":\"tare\"}");
    assert(c.status == 503);
    assert(log_count("scale-driver", "miniscale: the scale does not answer at 0x26 on bus 1 (error -19), retrying every 1000 ms") == 1);
    part->absent = false;
    tick("scale-driver", now += 1000);
    assert(get("scale-driver").status == 200 && log_count("scale-driver", "miniscale: answering again at 0x26 on bus 1") == 1);
    remove(rt, "scale-driver");

    // Absent from the start
    deploy(rt, "scale-gone", "miniscale", "\"esp32.io/i2c-bus-1\":\"1\"", {{"I2C_BUS", "1"}, {"I2C_ADDR", "27"}}, "\"kube32.io/i2c-bus-1\":\"sda=10,scl=11\"");
    assert(log_count("scale-gone", "miniscale: no Unit Mini Scales at 0x27 on bus 1 yet (error -19)") == 1);
    tick("scale-gone", now += 1000);
    assert(get("scale-gone").status == 503);
    remove(rt, "scale-gone");
    std::cout << "[PASS] driver-miniscale" << std::endl;
}

// ---------------------------------------------------------------------------
// rfid
// ---------------------------------------------------------------------------
void test_rfid(runtime::WasmRuntime& rt) {
    std::cout << "=== driver-rfid ===" << std::endl;
    auto reader = std::make_shared<k32test::Mfrc522>();
    MockHal::instance().register_mock_i2c_device(0, 0x28, reader);
    deploy(rt, "door-driver", "rfid", "\"esp32.io/i2c-bus-0\":\"1\"," + kIpc, {{"DEVICE_NAME", "door"}, {"I2C_ADDR", "0x28"}});
    uint32_t now = 300000;
    tick("door-driver", now);
    assert(log_count("door-driver", "rfid: reader at 0x28 on bus 0, version 0x15") == 1);
    k32test::Collect c = get("door-driver");
    assert(c.status == 200 && has(c.body, "\"readings\":{\"tag\":null,\"present\":false}"));
    assert(take_events("device.door").size() == 1);

    // A tag comes into the field: one event while it stays
    { std::lock_guard<std::mutex> lock(reader->m); reader->uid = {0x04, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0x80}; }
    for (int i = 0; i < 5; ++i) tick("door-driver", now += 200);
    c = get("door-driver");
    std::cout << "[rfid] " << c.body;
    assert(has(c.body, "\"readings\":{\"tag\":\"04a1b2c3d4e580\",\"present\":true}"));
    std::vector<Event> ev = take_events("device.door");
    assert(ev.size() == 1 && has(ev[0].body, "\"tag\":\"04a1b2c3d4e580\",\"present\":true"));
    assert(log_count("door-driver", "rfid: tag 04a1b2c3d4e580") == 1);

    // It leaves (three empty polls): one event, the last tag kept
    { std::lock_guard<std::mutex> lock(reader->m); reader->uid.clear(); }
    for (int i = 0; i < 5; ++i) tick("door-driver", now += 200);
    ev = take_events("device.door");
    assert(ev.size() == 1 && has(ev[0].body, "\"tag\":\"04a1b2c3d4e580\",\"present\":false"));

    // Another tag, then the same one again after it left
    { std::lock_guard<std::mutex> lock(reader->m); reader->uid = {0xde, 0xad, 0xbe, 0xef}; }
    tick("door-driver", now += 200);
    { std::lock_guard<std::mutex> lock(reader->m); reader->uid = {0x04, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0x80}; }
    tick("door-driver", now += 200);
    ev = take_events("device.door");
    assert(ev.size() == 2 && has(ev[0].body, "\"tag\":\"deadbeef\"") && has(ev[1].body, "\"tag\":\"04a1b2c3d4e580\""));

    // The reader stops answering and comes back
    { std::lock_guard<std::mutex> lock(reader->m); reader->absent = true; }
    for (int i = 0; i < 10; ++i) tick("door-driver", now += 200);
    c = get("door-driver");
    assert(c.status == 503 && has(c.body, "no reader answers at 0x28 on bus 0 (error -19)"));
    assert(log_count("door-driver", "rfid: the reader stopped answering at 0x28 on bus 0 (error -19), retrying every 200 ms") == 1);
    { std::lock_guard<std::mutex> lock(reader->m); reader->absent = false; }
    tick("door-driver", now += 200);
    assert(get("door-driver").status == 200 && log_count("door-driver", "rfid: reader at 0x28") == 1);
    assert(log_count("door-driver", "rfid: answering again at 0x28 on bus 0") == 1);
    remove(rt, "door-driver");
    std::cout << "[PASS] driver-rfid" << std::endl;
}

// ---------------------------------------------------------------------------
// chip-temp
// ---------------------------------------------------------------------------
void test_chip_temp(runtime::WasmRuntime& rt) {
    std::cout << "=== driver-chip-temp ===" << std::endl;
    deploy(rt, "chip", "chip-temp", "\"hardware.kube32.io/chip-temp\":\"1\"," + kIpc, {{"DEVICE_NAME", "board-temp"}, {"DEVICE_INTERVAL_MS", "10000"}});
    uint32_t now = 400000;
    tick("chip", now);
    k32test::Collect c = get("chip");
    std::cout << "[chip-temp] " << c.body;
    assert(c.status == 200 && has(c.body, "\"readings\":{\"temperature_c\":40.0}"));
    check_event_is_body("chip", "device.board-temp");
    MockHal::instance().set_chip_temp(-2349);
    tick("chip", now += 10000);
    assert(has(get("chip").body, "\"temperature_c\":-2.3}"));
    MockHal::instance().set_chip_temp(51260);
    tick("chip", now += 10000);
    assert(has(get("chip").body, "\"temperature_c\":51.3}"));
    MockHal::instance().set_chip_temp(0, hal::HAL_ERR_IO);
    for (int i = 0; i < 5; ++i) tick("chip", now += 10000);
    c = get("chip");
    assert(c.status == 503 && has(c.body, "the temperature sensor did not answer (error -5)"));
    assert(log_count("chip", "chip-temp: the temperature sensor did not answer (error -5), retrying every 10000 ms") == 1);
    MockHal::instance().set_chip_temp(MockHal::kChipTempMilliC);
    tick("chip", now += 10000);
    assert(get("chip").status == 200);
    remove(rt, "chip");

    // Without hardware.kube32.io/chip-temp the import is refused
    deploy(rt, "chip-nogrant", "chip-temp", kIpc, {});
    tick("chip-nogrant", now += 10000);
    c = get("chip-nogrant");
    assert(c.status == 503 && has(c.body, "not granted to this pod (error -1)"));
    const hal::PodSecurityRecord* rec = hal::CapabilityFirewall::instance().get_pod_security_record("default/chip-nogrant");
    assert(rec && rec->violation_count >= 1);
    remove(rt, "chip-nogrant");
    // The securityContext capability grants it as well
    std::string err;
    assert(hal::CapabilityFirewall::instance().register_pod("default/cap", R"({"spec":{"containers":[{"name":"c","securityContext":{"capabilities":{"add":["CHIP_TEMP"]}}}]}})", err));
    assert(hal::CapabilityFirewall::instance().check_permission("default/cap", hal::PeripheralType::ChipTemp, 0, hal::AccessMode::Read));
    hal::CapabilityFirewall::instance().unregister_pod("default/cap");
    std::cout << "[PASS] driver-chip-temp" << std::endl;
}

// ---------------------------------------------------------------------------
// button
// ---------------------------------------------------------------------------
void test_button(runtime::WasmRuntime& rt) {
    std::cout << "=== driver-button ===" << std::endl;
    auto& gpio = MockHal::instance();
    gpio.gpio_write(5, 1);                                  // released: the pull-up holds it high
    deploy(rt, "btn", "button", "\"esp32.io/gpio-5\":\"1\"," + kIpc,
           {{"DEVICE_NAME", "doorbell"}, {"PIN_PIN", "5"}, {"CONFIG_PULL", "up"}, {"CONFIG_ACTIVE", "low"}, {"DEVICE_INTERVAL_MS", "50"}});
    assert(log_count("btn", "button: on GPIO 5, active low, pull-up") == 1);
    uint32_t now = 500000;
    assert(tick("btn", now) == 0);
    assert(gpio.get_gpio_mode(5) == hal::PinMode::InputPullup);
    k32test::Collect c = get("btn");
    assert(c.status == 200 && has(c.body, "\"readings\":{\"pressed\":false,\"presses\":0}"));
    check_event_is_body("btn", "device.doorbell");

    // Pressed: trusted after 30 ms of the same level, ticked every 10 ms until then
    gpio.gpio_write(5, 0);
    assert(tick("btn", now += 50) == 1);
    assert(tick("btn", now += 10) == 1);
    assert(tick("btn", now += 10) == 1);
    assert(take_events("device.doorbell").empty());
    assert(tick("btn", now += 10) == 0);
    std::vector<Event> ev = take_events("device.doorbell");
    assert(ev.size() == 1 && has(ev[0].body, "\"pressed\":true,\"presses\":1"));
    // Held: no more events
    for (int i = 0; i < 20; ++i) tick("btn", now += 50);
    assert(take_events("device.doorbell").empty());

    // A bounce shorter than the debounce time is not a change
    gpio.gpio_write(5, 1);
    assert(tick("btn", now += 50) == 1);
    gpio.gpio_write(5, 0);
    assert(tick("btn", now += 10) == 0);
    for (int i = 0; i < 5; ++i) tick("btn", now += 10);
    assert(take_events("device.doorbell").empty() && has(get("btn").body, "\"pressed\":true"));

    // Released, then pressed again
    gpio.gpio_write(5, 1);
    now = settle("btn", now + 50);
    gpio.gpio_write(5, 0);
    now = settle("btn", now + 50);
    ev = take_events("device.doorbell");
    assert(ev.size() == 2 && has(ev[0].body, "\"pressed\":false,\"presses\":1") && has(ev[1].body, "\"pressed\":true,\"presses\":2"));

    // A report a minute after the last event, without a change
    for (int i = 0; i < 59; ++i) tick("btn", now += 1000);
    assert(take_events("device.doorbell").empty());
    tick("btn", now += 1000);
    ev = take_events("device.doorbell");
    assert(ev.size() == 1 && has(ev[0].body, "\"pressed\":true,\"presses\":2"));
    remove(rt, "btn");

    // Active high on a pull-down, no periodic reports
    gpio.gpio_write(6, 0);
    deploy(rt, "btn-high", "button", "\"esp32.io/gpio-6\":\"1\"," + kIpc,
           {{"PIN_PIN", "6"}, {"CONFIG_PULL", "down"}, {"CONFIG_ACTIVE", "high"}, {"CONFIG_REPORT_MS", "0"}, {"CONFIG_DEBOUNCE_MS", "0"}});
    tick("btn-high", now += 50);
    assert(gpio.get_gpio_mode(6) == hal::PinMode::InputPulldown && has(get("btn-high").body, "\"pressed\":false"));
    gpio.gpio_write(6, 1);
    now = settle("btn-high", now + 50);
    assert(has(get("btn-high").body, "\"pressed\":true,\"presses\":1"));
    take_events("device.button");
    for (int i = 0; i < 120; ++i) tick("btn-high", now += 1000);
    assert(take_events("device.button").empty());
    remove(rt, "btn-high");

    // A pin the pod was not granted; no pin at all
    deploy(rt, "btn-nogrant", "button", kIpc, {{"PIN_PIN", "7"}});
    for (int i = 0; i < 10; ++i) tick("btn-nogrant", now += 50);
    c = get("btn-nogrant");
    assert(c.status == 503 && has(c.body, "not granted to this pod on GPIO 7 (error -1)"));
    assert(log_count("btn-nogrant", "button: not granted to this pod on GPIO 7 (error -1)") == 1);
    remove(rt, "btn-nogrant");
    deploy(rt, "btn-nopin", "button", "", {});
    assert(log_count("btn-nopin", "button: PIN_PIN is not set") == 1);
    tick("btn-nopin", now += 50);
    assert(get("btn-nopin").status == 503 && has(get("btn-nopin").body, "PIN_PIN is not set"));
    remove(rt, "btn-nopin");
    std::cout << "[PASS] driver-button" << std::endl;
}

// ---------------------------------------------------------------------------
// switch
// ---------------------------------------------------------------------------
void test_switch(runtime::WasmRuntime& rt) {
    std::cout << "=== driver-switch ===" << std::endl;
    auto& gpio = MockHal::instance();
    deploy(rt, "relay", "switch", "\"esp32.io/gpio-9\":\"1\"," + kIpc,
           {{"DEVICE_NAME", "fan"}, {"PIN_PIN", "9"}, {"CONFIG_INITIAL", "on"}, {"CONFIG_ACTIVE", "low"}, {"DEVICE_INTERVAL_MS", "30000"}});
    assert(log_count("relay", "switch: on GPIO 9, active low, on") == 1);
    uint32_t now = 600000;
    tick("relay", now);
    assert(gpio.get_gpio_mode(9) == hal::PinMode::Output && gpio.gpio_read(9) == 0);   // on, active low
    k32test::Collect c = get("relay");
    assert(c.status == 200 && has(c.body, "\"readings\":{\"on\":true}"));
    check_event_is_body("relay", "device.fan");

    c = post("relay", "{\"action\":\"set\",\"on\":false}");
    std::cout << "[switch] " << c.body;
    assert(c.status == 200 && has(c.body, "\"on\":false") && gpio.gpio_read(9) == 1);
    std::vector<Event> ev = take_events("device.fan");
    assert(ev.size() == 1 && has(ev[0].body, "\"on\":false"));
    c = post("relay", "{\"action\":\"set\",\"on\":false}");   // no change, no event
    assert(c.status == 200 && take_events("device.fan").empty());
    c = post("relay", "{\"action\":\"toggle\"}");
    assert(c.status == 200 && has(c.body, "\"on\":true") && gpio.gpio_read(9) == 0);
    assert(take_events("device.fan").size() == 1);
    c = post("relay", "{\"action\":\"set\"}");
    assert(c.status == 400 && has(c.body, "\"error\":\"set takes on: true or false\""));
    c = post("relay", "{\"action\":\"set\",\"on\":\"yes\"}");
    assert(c.status == 400);
    c = post("relay", "not json");
    assert(c.status == 400);
    c = get("relay", "/describe");
    assert(has(c.body, "{\"action\":\"set\",\"on\":\"bool\"},{\"action\":\"toggle\"}"));

    // Each tick drives the pin again and reports the state
    gpio.gpio_write(9, 1);
    tick("relay", now += 30000);
    assert(gpio.gpio_read(9) == 0);
    ev = take_events("device.fan");
    assert(ev.size() == 1 && has(ev[0].body, "\"on\":true"));
    remove(rt, "relay");

    // Defaults: off, active high
    deploy(rt, "led", "switch", "\"esp32.io/gpio-10\":\"1\"", {{"PIN_PIN", "10"}});
    tick("led", now);
    assert(gpio.gpio_read(10) == 0 && has(get("led").body, "\"on\":false"));
    assert(post("led", "{\"action\":\"toggle\"}").status == 200 && gpio.gpio_read(10) == 1);
    remove(rt, "led");

    // A pin the pod was not granted: the action fails with 503
    deploy(rt, "relay-nogrant", "switch", "", {{"PIN_PIN", "11"}});
    tick("relay-nogrant", now);
    c = post("relay-nogrant", "{\"action\":\"toggle\"}");
    assert(c.status == 503 && has(c.body, "not granted to this pod on GPIO 11"));
    assert(has(get("relay-nogrant").body, "\"ok\":false"));
    remove(rt, "relay-nogrant");
    std::cout << "[PASS] driver-switch" << std::endl;
}

// ---------------------------------------------------------------------------
// Long names: the event leaves out what does not fit a frame
// ---------------------------------------------------------------------------
void test_event_size(runtime::WasmRuntime& rt) {
    std::cout << "=== events of long names ===" << std::endl;
    auto part = std::make_shared<Bme68x>(1);
    part->busy_reads = 0;
    MockHal::instance().register_mock_i2c_device(0, 0x77, part);
    const std::string device = "bme688-kube32-cardputer-a1b2c3-b0-m70c2-77";
    deploy(rt, "long", "bme688", "\"esp32.io/i2c-bus-0\":\"1\"," + kIpc,
           {{"DEVICE_NAME", device}, {"NODE_NAME", "kube32-cardputer-a1b2c3"}, {"DEVICE_TOPIC", "device." + device}});
    settle("long", 700000);
    const std::string body = get("long").body;
    std::vector<Event> ev = take_events("device." + device);
    std::cout << "[event] " << ev[0].body.size() << " bytes: " << ev[0].body << std::endl;
    assert(body.size() > K32_MAX_EVENT_BYTES && ev.size() == 1 && ev[0].body.size() <= K32_MAX_EVENT_BYTES);
    assert(has(ev[0].body, "\"readings\":{\"temperature_c\":") && has(ev[0].body, "\"gas_ohms\":") && !has(ev[0].body, "\"node\""));
    remove(rt, "long");
    std::cout << "[PASS] events of long names" << std::endl;
}

// ---------------------------------------------------------------------------
// The room-monitor example: drivers found by label through Services
// ---------------------------------------------------------------------------
std::string pod_json(const std::string& name, const std::string& labels) {
    return "{\"apiVersion\":\"v1\",\"kind\":\"Pod\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\",\"labels\":{" + labels + "}},"
           "\"spec\":{\"nodeName\":\"node-a\",\"containers\":[{\"name\":\"driver\",\"image\":\"wasm://x\",\"ports\":[{\"containerPort\":7000}]}]},"
           "\"status\":{\"phase\":\"Running\",\"podIP\":\"127.0.0.1\"}}";
}

std::string service_json(const std::string& name, const std::string& selector) {
    return "{\"apiVersion\":\"v1\",\"kind\":\"Service\",\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\"},"
           "\"spec\":{\"selector\":{" + selector + "},\"ports\":[{\"name\":\"http\",\"port\":80,\"targetPort\":7000}]}}";
}

void test_room_monitor(runtime::WasmRuntime& rt) {
    std::cout << "=== room-monitor ===" << std::endl;
    // Driver pods as the Device controller labels them, and the Services of
    // examples/devices/room-monitor.yaml selecting them by driver and location
    auto store = storage::create_radix_store(storage::StoreOptions{"/tmp/kube32_test_drivers_room", false});
    store->put("/registry/pods/default/kitchen-env-driver",
               pod_json("kitchen-env-driver", R"("location":"kitchen","kube32.io/device":"kitchen-env","kube32.io/driver":"bme688")"), true);
    store->put("/registry/pods/default/kitchen-scale-driver",
               pod_json("kitchen-scale-driver", R"("location":"kitchen","kube32.io/device":"kitchen-scale","kube32.io/driver":"miniscale")"), true);
    store->put("/registry/pods/default/hall-env-driver",
               pod_json("hall-env-driver", R"("location":"hall","kube32.io/device":"hall-env","kube32.io/driver":"bme688")"), true);
    store->put("/registry/services/default/kitchen-env", service_json("kitchen-env", R"("kube32.io/driver":"bme688","location":"kitchen")"), true);
    store->put("/registry/services/default/kitchen-scale", service_json("kitchen-scale", R"("kube32.io/driver":"miniscale","location":"kitchen")"), true);
    app::ServiceClient client(app::make_store_service_resolver(*store));
    app::install_service_client(&client);

    auto env = std::make_shared<Bme68x>(1);
    env->busy_reads = 0;
    auto hall = std::make_shared<Bme68x>(1);
    hall->busy_reads = 0;
    hall->adc_t = 530000;
    auto scale = std::make_shared<MiniScale>();
    MockHal::instance().register_mock_i2c_device(0, 0x77, env);
    MockHal::instance().register_mock_i2c_device(0, 0x76, hall);
    MockHal::instance().register_mock_i2c_device(1, 0x26, scale);
    // The kitchen's parts on buses 0 and 1, the hall's at another address on bus 0
    deploy(rt, "kitchen-env-driver", "bme688", "\"esp32.io/i2c-bus-0\":\"1\"," + kIpc, {{"DEVICE_NAME", "kitchen-env"}, {"I2C_ADDR", "0x77"}});
    deploy(rt, "kitchen-scale-driver", "miniscale", "\"esp32.io/i2c-bus-1\":\"1\"," + kIpc,
           {{"DEVICE_NAME", "kitchen-scale"}, {"I2C_BUS", "1"}, {"I2C_ADDR", "0x26"}}, "\"kube32.io/i2c-bus-1\":\"sda=10,scl=11\"");
    uint32_t now = 800000;
    settle("kitchen-env-driver", now);
    tick("kitchen-scale-driver", now);

    // The app: no hardware, only Services
    const std::vector<uint8_t> wasm = image("room-monitor");
    runtime::DeploySpec spec;
    spec.env = {{"ROOM", "kitchen"}, {"ENV_URL", "http://kitchen-env"}, {"SCALE_URL", "http://kitchen-scale/"}};
    k32test::deploy(rt, "default/room-monitor-0", wasm, 4096, "\"SERVICE_CALL\"", spec);
    assert(log_count("room-monitor-0", "room-monitor: kitchen, env http://kitchen-env, scale http://kitchen-scale") == 1);
    k32test::Collect c = k32test::call_pod("default/room-monitor-0", "/");
    std::cout << "[room-monitor] " << c.body;
    assert(c.status == 200 && c.ctype == "application/json");
    assert(c.body.rfind("{\"room\":\"kitchen\",\"env\":{\"device\":\"kitchen-env\",\"driver\":\"bme688\",\"ok\":true,", 0) == 0);
    assert(has(c.body, "\"temperature_c\":22.39,") && has(c.body, ",\"scale\":{\"device\":\"kitchen-scale\",\"driver\":\"miniscale\",\"ok\":true,"));
    assert(has(c.body, "\"weight_g\":246.90,") && c.body.substr(c.body.size() - 3) == "}}\n");

    // The page, also behind an Ingress prefix
    c = k32test::call_pod("default/room-monitor-0", "/kitchen/view");
    std::cout << "[room-monitor view] " << c.body << std::endl;
    assert(c.status == 200 && has(c.ctype, "text/html"));
    assert(has(c.body, "<h1>kitchen</h1>") && has(c.body, "<tr><td>Temperature</td><td>22.39 &deg;C</td></tr>"));
    assert(has(c.body, "<tr><td>Weight</td><td>246.90 g</td></tr>") && has(c.body, "<form method=post action=tare>"));

    // Tare through the app: the scale's answer, or back to the page from the form
    c = k32test::call_pod("default/room-monitor-0", "/tare", "POST");
    assert(c.status == 200 && scale->tares == 1 && has(c.body, "\"device\":\"kitchen-scale\""));
    {
        runtime::AppRequest req;
        req.method = "POST";
        req.path_and_query = "/kitchen/tare";
        req.content_type = "application/x-www-form-urlencoded";
        k32test::Collect form;
        runtime::AppPodHost::instance().handle("default/room-monitor-0", req, form);
        assert(form.status == 303 && has(form.headers, "Location: view") && scale->tares == 2);
    }
    tick("kitchen-scale-driver", now += 1000);
    assert(has(k32test::call_pod("default/room-monitor-0", "/").body, "\"weight_g\":0.00,"));

    // The scale unplugged: its driver's 503 answer, the rest as before
    scale->absent = true;
    tick("kitchen-scale-driver", now += 1000);
    c = k32test::call_pod("default/room-monitor-0", "/");
    assert(c.status == 200 && has(c.body, ",\"scale\":{\"device\":\"kitchen-scale\",\"driver\":\"miniscale\",\"ok\":false,\"error\":\"the scale does not answer"));
    assert(has(c.body, "\"temperature_c\":22.39,"));
    c = k32test::call_pod("default/room-monitor-0", "/view");
    assert(has(c.body, "<tr><td>scale</td><td>the scale does not answer at 0x26 on bus 1 (error -19)</td></tr>"));
    c = k32test::call_pod("default/room-monitor-0", "/tare", "POST");
    assert(c.status == 503);
    scale->absent = false;

    // The scale's pod gone: no pod behind its Service
    remove(rt, "kitchen-scale-driver");
    store->remove("/registry/pods/default/kitchen-scale-driver");
    c = k32test::call_pod("default/room-monitor-0", "/");
    std::cout << "[room-monitor no scale] " << c.body;
    assert(has(c.body, ",\"scale\":{\"ok\":false,\"error\":\"no such Service, or no ready pod behind it (-2)\"}"));

    // Another room by label: the hall's sensor, the app unchanged
    store->put("/registry/services/default/hall-env", service_json("hall-env", R"("kube32.io/driver":"bme688","location":"hall")"), true);
    remove(rt, "kitchen-env-driver");   // the bus claim is a pod's: bus 0 for the hall's driver
    deploy(rt, "hall-env-driver", "bme688", "\"esp32.io/i2c-bus-0\":\"1\"", {{"DEVICE_NAME", "hall-env"}, {"I2C_ADDR", "0x76"}});
    settle("hall-env-driver", now += 1000);
    runtime::DeploySpec hall_spec;
    hall_spec.env = {{"ROOM", "hall"}, {"ENV_URL", "http://hall-env"}};
    k32test::deploy(rt, "default/room-monitor-1", wasm, 4096, "\"SERVICE_CALL\"", hall_spec);
    c = k32test::call_pod("default/room-monitor-1", "/");
    std::cout << "[room-monitor hall] " << c.body;
    assert(c.body.rfind("{\"room\":\"hall\",\"env\":{\"device\":\"hall-env\",", 0) == 0 && !has(c.body, "\"scale\""));
    assert(std::fabs(num(c.body, "temperature_c") - compensate(Calib(), hall->adc_t, hall->adc_p, hall->adc_h, hall->adc_gas, hall->gas_range).t) < 0.015);
    c = k32test::call_pod("default/room-monitor-1", "/tare", "POST");
    assert(c.status == 404 && has(c.body, "SCALE_URL is not set"));
    c = k32test::call_pod("default/room-monitor-1", "/view");
    assert(!has(c.body, "<form"));

    // Without SERVICE_CALL the calls are refused
    k32test::deploy(rt, "default/room-monitor-2", wasm, 4096, "", hall_spec);
    c = k32test::call_pod("default/room-monitor-2", "/");
    assert(has(c.body, "\"env\":{\"ok\":false,\"error\":\"denied (NetworkPolicy, or no SERVICE_CALL capability) (-1)\"}"));

    for (const char* id : {"default/room-monitor-0", "default/room-monitor-1", "default/room-monitor-2"}) {
        assert(rt.terminate(id));
        hal::CapabilityFirewall::instance().unregister_pod(id);
    }
    remove(rt, "hall-env-driver");
    app::install_service_client(nullptr);
    std::cout << "[PASS] room-monitor" << std::endl;
}

// ---------------------------------------------------------------------------
// A driver loaded as the kubelet loads it (memory 3Ki, no kube32.io/wasm-stack:
// the operand stack is what the module's call chains need), its operand stack
// painted: what its calls use against what it was given
// ---------------------------------------------------------------------------
class StackProbe {
public:
    std::string pod_id;
    runtime::ContainerLifecycleController ctrl;

    StackProbe(const std::string& name, const std::string& driver, const std::string& requests, const Env& env)
        : pod_id("default/" + name), ctrl(config(pod_id)) {
        const std::string manifest = "{\"metadata\":{\"name\":\"" + name + "\"},\"spec\":{\"containers\":[{\"name\":\"driver\","
                                     "\"resources\":{\"requests\":{\"memory\":\"3Ki\"" + (requests.empty() ? "" : "," + requests) + "}}}]}}";
        std::string err;
        assert(hal::CapabilityFirewall::instance().register_pod(pod_id, manifest, err));
        runtime::AppPodHost::instance().set_env(pod_id, env);
        const std::vector<uint8_t> wasm = image("driver-" + driver);
        assert(ctrl.deploy(wasm.data(), wasm.size()));
        rt_ = static_cast<IM3Runtime>(ctrl.m3_runtime_handle());
        bytes_ = rt_->numStackSlots * sizeof(m3slot_t);
        assert(bytes_ == ctrl.get_status().stack_bytes);
        // the stack and the four slots wasm3 allocates past it
        std::memset(rt_->originStack, kPaint, bytes_ + 4 * sizeof(m3slot_t));
        assert(runtime::AppPodHost::instance().register_pod(pod_id, &ctrl));
        assert(runtime::AppPodHost::instance().init_pod(pod_id) == 0);
    }
    ~StackProbe() {
        runtime::AppPodHost::instance().unregister_pod(pod_id);
        hal::CapabilityFirewall::instance().unregister_pod(pod_id);
    }

    uint32_t given() const { return bytes_; }
    // The deepest byte any call has written
    uint32_t used() const {
        const uint8_t* stack = static_cast<const uint8_t*>(rt_->originStack);
        for (uint32_t i = bytes_ + 4 * sizeof(m3slot_t); i > 0; --i) {
            if (stack[i - 1] != kPaint) return i;
        }
        return 0;
    }
    int32_t tick(uint32_t now) { return runtime::AppPodHost::instance().tick(pod_id, now); }
    uint32_t settle(uint32_t now) {
        for (int i = 0; i < 1000 && tick(now) > 0; ++i) now += 10;
        return now;
    }

private:
    static constexpr uint8_t kPaint = 0xA5;
    IM3Runtime rt_ = nullptr;
    uint32_t bytes_ = 0;

    static runtime::ContainerConfig config(const std::string& pod_id) {
        runtime::ContainerConfig cfg;
        cfg.pod_id = pod_id;
        cfg.container_name = pod_id;
        cfg.memory_limit_bytes = 3072;
        cfg.stack_size_bytes = 4096;         // what the kubelet passes for a one-page pod without kube32.io/wasm-stack
        cfg.stack_from_module = true;
        return cfg;
    }
};

void report(const StackProbe& p) {
    std::cout << "[operand stack] " << p.pod_id << ": given " << p.given() << " bytes, used " << p.used() << std::endl;
    assert(p.used() <= p.given());
}

void test_operand_stacks() {
    std::cout << "=== operand stacks as the kubelet sizes them ===" << std::endl;
    uint32_t now = 900000;
    {
        auto part = std::make_shared<Bme68x>(1);
        MockHal::instance().register_mock_i2c_device(0, 0x77, part);
        StackProbe p("stack-bme688", "bme688", "\"esp32.io/i2c-bus-0\":\"1\"," + kIpc, {{"I2C_ADDR", "0x77"}});
        now = p.settle(now);                                           // first tick: identify and start, then the reading
        now = p.settle(now + 5000);
        k32test::call_pod(p.pod_id, "/");
        k32test::call_pod(p.pod_id, "/describe");
        k32test::call_pod(p.pod_id, "/", "POST", "{\"action\":\"tare\"}");
        part->absent = true;
        now = p.settle(now + 5000);
        part->absent = false;
        now = p.settle(now + 5000);
        report(p);
    }
    {
        auto part = std::make_shared<Bme68x>(0);
        MockHal::instance().register_mock_i2c_device(0, 0x76, part);
        StackProbe p("stack-bme680", "bme688", "\"esp32.io/i2c-bus-0\":\"1\"," + kIpc, {{"I2C_ADDR", "0x76"}});
        now = p.settle(now);
        report(p);
    }
    {
        auto part = std::make_shared<MiniScale>();
        MockHal::instance().register_mock_i2c_device(0, 0x26, part);
        StackProbe p("stack-miniscale", "miniscale", "\"esp32.io/i2c-bus-0\":\"1\"," + kIpc, {{"I2C_ADDR", "0x26"}});
        p.tick(now += 1000);
        k32test::call_pod(p.pod_id, "/", "POST", "{\"action\":\"tare\"}");
        k32test::call_pod(p.pod_id, "/", "POST", "{\"action\":\"led\",\"r\":255,\"g\":16,\"b\":1}");
        k32test::call_pod(p.pod_id, "/");
        part->absent = true;
        p.tick(now += 1000);
        part->absent = false;
        p.tick(now += 1000);
        report(p);
    }
    {
        auto reader = std::make_shared<k32test::Mfrc522>();
        MockHal::instance().register_mock_i2c_device(0, 0x28, reader);
        StackProbe p("stack-rfid", "rfid", "\"esp32.io/i2c-bus-0\":\"1\"," + kIpc, {{"I2C_ADDR", "0x28"}});
        p.tick(now += 200);
        { std::lock_guard<std::mutex> lock(reader->m); reader->uid = {0x04, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0x80}; }
        p.tick(now += 200);
        { std::lock_guard<std::mutex> lock(reader->m); reader->uid.clear(); }
        for (int i = 0; i < 4; ++i) p.tick(now += 200);
        k32test::call_pod(p.pod_id, "/");
        report(p);
    }
    {
        StackProbe p("stack-chip-temp", "chip-temp", "\"hardware.kube32.io/chip-temp\":\"1\"," + kIpc, {});
        p.tick(now += 10000);
        k32test::call_pod(p.pod_id, "/");
        report(p);
    }
    {
        MockHal::instance().gpio_write(12, 1);
        StackProbe p("stack-button", "button", "\"esp32.io/gpio-12\":\"1\"," + kIpc, {{"PIN_PIN", "12"}});
        p.tick(now += 50);
        MockHal::instance().gpio_write(12, 0);
        now = p.settle(now + 50);
        p.tick(now += 60000);
        k32test::call_pod(p.pod_id, "/");
        report(p);
    }
    {
        StackProbe p("stack-switch", "switch", "\"esp32.io/gpio-13\":\"1\"," + kIpc, {{"PIN_PIN", "13"}});
        p.tick(now += 30000);
        k32test::call_pod(p.pod_id, "/", "POST", "{\"action\":\"set\",\"on\":true}");
        k32test::call_pod(p.pod_id, "/", "POST", "{\"action\":\"toggle\"}");
        k32test::call_pod(p.pod_id, "/");
        report(p);
    }
    take_events("device.bme688");
    std::cout << "[PASS] operand stacks as the kubelet sizes them" << std::endl;
}

// ---------------------------------------------------------------------------
// The first tick waits for k32_init: the tick thread can find a pod between
// its registration and k32_init
// ---------------------------------------------------------------------------
void test_init_before_tick() {
    std::cout << "=== k32_init before the first tick ===" << std::endl;
    {
        // Registered as the runtime registers it, k32_init not run yet
        const std::string pod_id = "default/early";
        runtime::ContainerConfig cfg;
        cfg.pod_id = pod_id;
        cfg.memory_limit_bytes = 3072;
        cfg.stack_size_bytes = 4096;
        cfg.stack_from_module = true;
        std::string err;
        assert(hal::CapabilityFirewall::instance().register_pod(pod_id,
            R"({"spec":{"containers":[{"name":"c","resources":{"requests":{"hardware.kube32.io/chip-temp":"1","hardware.kube32.io/ipc":"1"}}}]}})", err));
        runtime::AppPodHost::instance().set_env(pod_id, {{"DEVICE_NAME", "early"}});
        runtime::ContainerLifecycleController ctrl(cfg);
        const std::vector<uint8_t> wasm = image("driver-chip-temp");
        assert(ctrl.deploy(wasm.data(), wasm.size()));
        auto& host = runtime::AppPodHost::instance();
        assert(host.register_pod(pod_id, &ctrl));
        take_events("device.early");
        host.tick_due(osal::System::get_uptime_ms() + 1000000);
        assert(take_events("device.early").empty() && log_count("early", "chip-temp:") == 0);   // not ticked
        assert(host.init_pod(pod_id) == 0);
        assert(log_count("early", "chip-temp: the chip's temperature every 10000 ms") == 1);
        host.tick_due(osal::System::get_uptime_ms() + 1000000);
        std::vector<Event> ev = take_events("device.early");
        assert(ev.size() == 1 && has(ev[0].body, "\"device\":\"early\""));
        host.unregister_pod(pod_id);
        hal::CapabilityFirewall::instance().unregister_pod(pod_id);
    }
    std::cout << "[PASS] k32_init before the first tick" << std::endl;
}

// ---------------------------------------------------------------------------
// What each driver takes from a board's pod pool
// ---------------------------------------------------------------------------
// The kubelet's estimate with the board's sizes, as a board without PSRAM
// admits a pod: on its 80 KiB pool block, the compiler's scratch lent by the
// interpreter stack (tests/test_pod_pool_fit.cpp checks the estimate against
// what loading a module takes)
void test_pool_estimates() {
    std::cout << "=== pod pool estimates (memory 3Ki) ===" << std::endl;
    std::vector<uint8_t> interp(28 * 1024);
    kube32_interp_stack_set(interp.data(), interp.size());
    size_t i2c = 0;
    for (const char* name : {"bme688", "miniscale", "rfid", "chip-temp", "button", "switch"}) {
        const std::vector<uint8_t> wasm = image(std::string("driver-") + name);
        const size_t board = runtime::estimate_pod_memory(wasm.data(), wasm.size(), 3072, 4096, true, runtime::EstimateSizes::Board);
        std::cout << "[estimate] driver-" << name << ": " << wasm.size() << " bytes of Wasm, " << board << " bytes on a board" << std::endl;
        assert(board <= 28 * 1024);
        if (std::strcmp(name, "bme688") == 0 || std::strcmp(name, "miniscale") == 0 || std::strcmp(name, "rfid") == 0) i2c += board;
    }
    kube32_interp_stack_set(nullptr, 0);
    // The ENV Pro, the Mini Scales and the RFID 2 on one Cardputer
    std::cout << "[estimate] bme688 + miniscale + rfid: " << i2c << " of " << kube32_pool_block_size() << " bytes" << std::endl;
    assert(i2c <= kube32_pool_block_size() - 4 * 1024);
}

} // namespace

int main() {
    std::cout << "=== device drivers ===" << std::endl;
    kube32_pool_reserve(80 * 1024);                     // a board without PSRAM
    MockHal::instance().reset();
    hal::CapabilityFirewall::instance().reset();
    runtime::PodLogStore::runtime().clear();
    record_events();
    auto rt = runtime::create_wasm_runtime();
    test_bme688(*rt);
    test_miniscale(*rt);
    test_rfid(*rt);
    test_chip_temp(*rt);
    test_button(*rt);
    test_switch(*rt);
    test_event_size(*rt);
    test_room_monitor(*rt);
    test_operand_stacks();
    test_init_before_tick();
    test_pool_estimates();
    hal::CapabilityFirewall::instance().reset();
    std::cout << "[PASS] device drivers" << std::endl;
    return 0;
}
