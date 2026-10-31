// What the calls of the device drivers (apps/drivers) and the room-monitor
// example stand on the native stack of a board whose compiler has no tail
// calls, the ESP32-S3's Xtensa. There wasm3 keeps one frame per operation
// until the operation's function returns or its loop iterates, so a call
// takes native stack for every operation it runs in one function
// activation, and the interpreter traps ("stack overflow") at a call or a
// loop once the pod has used the stack the node lends it.
//
// The modules run here on an interpreter built that way (M3_HAS_TAIL_CALL 0)
// with the board's wasm3 configuration and the operation frames counted
// (d_m3CountOpFrames), against the simulated parts: k32_init, the ticks of a
// part found, measured, failing and found again, k32_handle for GET /, GET
// /describe and the actions. Each call's deepest frame count must fit the
// Cardputer's interpreter stack at 64 bytes a frame, the largest an operation
// takes there (most take 48), above the margin kept for host functions.

#include "wasm3.h"
#include "m3_env.h"
#include "device_sims.hpp"
#include "mfrc522_sim.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C" {
extern uint32_t m3_opFrames, m3_opFramesPeak;
}

namespace {

// The Cardputer lends pod calls a 28 KiB interpreter stack; wasm3 keeps
// d_m3NativeStackMargin (6 KiB) of it for host functions, and the frames
// below the first operation (the shared-stack trampoline, m3_CallV) take
// some hundred bytes more
constexpr uint32_t kInterpreterStack = 28672;
constexpr uint32_t kHostMargin = 6144;
constexpr uint32_t kBelowGuest = 1024;
constexpr uint32_t kFrameBytes = 64;
constexpr uint32_t kMaxFrames = (kInterpreterStack - kHostMargin - kBelowGuest) / kFrameBytes;

std::vector<uint8_t> read_image(const std::string& name) {
    std::ifstream f(std::string(KUBE32_SOURCE_DIR) + "/images/" + name + ".wasm", std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.empty()) std::printf("missing images/%s.wasm\n", name.c_str());
    assert(!b.empty());
    return b;
}

// ---------------------------------------------------------------------------
// The node, as far as a pod sees it
// ---------------------------------------------------------------------------
struct Node {
    std::map<std::string, std::string> env;
    std::map<uint32_t, std::shared_ptr<kube32::hal::MockI2cDevice>> i2c;   // bus << 8 | address
    uint8_t gpio[64] = {};
    int32_t chip_milli_c = 40000, chip_status = 0;
    uint32_t now = 100000;
    int32_t status = 0;
    std::string response, http_answer;
    int events = 0;
} g;

uint8_t* mem_at(void* mem, uint32_t off) { return static_cast<uint8_t*>(mem) + off; }

m3ApiRawFunction(env_get) {
    m3ApiReturnType(int32_t) m3ApiGetArg(uint32_t, key) m3ApiGetArg(uint32_t, key_len) m3ApiGetArg(uint32_t, out) m3ApiGetArg(uint32_t, cap);
    auto it = g.env.find(std::string(reinterpret_cast<const char*>(mem_at(_mem, key)), key_len));
    if (it == g.env.end()) m3ApiReturn(-1);
    std::memcpy(mem_at(_mem, out), it->second.data(), std::min<size_t>(cap, it->second.size()));
    m3ApiReturn(static_cast<int32_t>(it->second.size()));
}
m3ApiRawFunction(log_line) { m3ApiGetArg(uint32_t, p) m3ApiGetArg(uint32_t, n); (void)p; (void)n; m3ApiSuccess(); }
m3ApiRawFunction(unix_ms) { m3ApiReturnType(int64_t) m3ApiReturn(1793462616670LL); }
m3ApiRawFunction(time_ms) { m3ApiReturnType(int64_t) m3ApiReturn(static_cast<int64_t>(g.now)); }
m3ApiRawFunction(event_emit) {
    m3ApiReturnType(int32_t) m3ApiGetArg(uint32_t, t) m3ApiGetArg(uint32_t, tl) m3ApiGetArg(uint32_t, d) m3ApiGetArg(uint32_t, n);
    (void)t; (void)tl; (void)d;
    g.events++;
    m3ApiReturn(static_cast<int32_t>(n));
}
m3ApiRawFunction(resp_begin) { m3ApiGetArg(int32_t, s) m3ApiGetArg(uint32_t, c) m3ApiGetArg(uint32_t, n); (void)c; (void)n; g.status = s; m3ApiSuccess(); }
m3ApiRawFunction(resp_header) { m3ApiGetArg(uint32_t, a) m3ApiGetArg(uint32_t, b) m3ApiGetArg(uint32_t, c) m3ApiGetArg(uint32_t, d); (void)a; (void)b; (void)c; (void)d; m3ApiSuccess(); }
m3ApiRawFunction(resp_write) {
    m3ApiReturnType(int32_t) m3ApiGetArg(uint32_t, p) m3ApiGetArg(uint32_t, n);
    g.response.append(reinterpret_cast<const char*>(mem_at(_mem, p)), n);
    m3ApiReturn(static_cast<int32_t>(n));
}
m3ApiRawFunction(resp_end) { m3ApiSuccess(); }
m3ApiRawFunction(http_call) {
    m3ApiReturnType(int32_t)
    m3ApiGetArg(uint32_t, a) m3ApiGetArg(uint32_t, b) m3ApiGetArg(uint32_t, c) m3ApiGetArg(uint32_t, d) m3ApiGetArg(uint32_t, e) m3ApiGetArg(uint32_t, f)
    m3ApiGetArg(uint32_t, h) m3ApiGetArg(uint32_t, i) m3ApiGetArg(uint32_t, resp) m3ApiGetArg(uint32_t, cap) m3ApiGetArg(uint32_t, got) m3ApiGetArg(uint32_t, t);
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)h; (void)i; (void)t;
    const uint32_t n = std::min<uint32_t>(cap, static_cast<uint32_t>(g.http_answer.size()));
    std::memcpy(mem_at(_mem, resp), g.http_answer.data(), n);
    std::memcpy(mem_at(_mem, got), &n, 4);
    m3ApiReturn(200);
}
m3ApiRawFunction(http_status) { m3ApiReturnType(int32_t) m3ApiReturn(200); }
m3ApiRawFunction(i2c_write) {
    m3ApiReturnType(int32_t) m3ApiGetArg(uint32_t, bus) m3ApiGetArg(uint32_t, addr) m3ApiGetArg(uint32_t, p) m3ApiGetArg(uint32_t, n);
    auto it = g.i2c.find(bus << 8 | addr);
    m3ApiReturn(it == g.i2c.end() ? kube32::hal::HAL_ERR_NODEV : it->second->write(mem_at(_mem, p), n));
}
m3ApiRawFunction(i2c_read) {
    m3ApiReturnType(int32_t) m3ApiGetArg(uint32_t, bus) m3ApiGetArg(uint32_t, addr) m3ApiGetArg(uint32_t, p) m3ApiGetArg(uint32_t, n);
    auto it = g.i2c.find(bus << 8 | addr);
    m3ApiReturn(it == g.i2c.end() ? kube32::hal::HAL_ERR_NODEV : it->second->read(mem_at(_mem, p), n));
}
m3ApiRawFunction(delay_ms) { m3ApiGetArg(uint32_t, ms); g.now += ms; m3ApiSuccess(); }
m3ApiRawFunction(gpio_set_mode) { m3ApiReturnType(int32_t) m3ApiGetArg(uint32_t, pin) m3ApiGetArg(uint32_t, mode); (void)mode; m3ApiReturn(pin < 64 ? 0 : -1); }
m3ApiRawFunction(gpio_write) { m3ApiReturnType(int32_t) m3ApiGetArg(uint32_t, pin) m3ApiGetArg(uint32_t, v); if (pin >= 64) m3ApiReturn(-1); g.gpio[pin] = v ? 1 : 0; m3ApiReturn(0); }
m3ApiRawFunction(gpio_read) { m3ApiReturnType(int32_t) m3ApiGetArg(uint32_t, pin); m3ApiReturn(pin < 64 ? g.gpio[pin] : -1); }
m3ApiRawFunction(chip_temp) {
    m3ApiReturnType(int32_t) m3ApiGetArg(uint32_t, out);
    if (!g.chip_status) std::memcpy(mem_at(_mem, out), &g.chip_milli_c, 4);
    m3ApiReturn(g.chip_status);
}

// ---------------------------------------------------------------------------
// A pod: the module loaded as the node loads it, its calls measured
// ---------------------------------------------------------------------------
class Pod {
public:
    std::string name;
    uint32_t peak = 0;   // deepest operation frame count of any call

    Pod(const std::string& image, uint32_t memory_limit) : name(image), wasm_(read_image(image)) {
        env_ = m3_NewEnvironment();
        rt_ = m3_NewRuntime(env_, 16 * 1024, nullptr);
        assert(env_ && rt_);
        m3_SetResourceLimit(rt_, c_m3Limit_GasUnits, static_cast<uint64_t>(INT64_MAX));   // metered, as pods run
        m3_SetResourceLimit(rt_, c_m3Limit_MemoryBytes, memory_limit);
        rt_->memoryLimit = memory_limit;
        IM3Module& module = module_;
        assert(m3_ParseModule(env_, &module, wasm_.data(), static_cast<uint32_t>(wasm_.size())) == m3Err_none);
        assert(m3_LoadModule(rt_, module) == m3Err_none);
        struct { const char* name; const char* sig; M3RawCall fn; } imports[] = {
            {"k32_env_get", "i(iiii)", env_get}, {"k32_log", "v(ii)", log_line}, {"k32_unix_time_ms", "I()", unix_ms},
            {"k32_time_ms", "I()", time_ms}, {"k32_event_emit", "i(iiii)", event_emit}, {"k32_resp_begin", "v(iii)", resp_begin},
            {"k32_resp_header", "v(iiii)", resp_header}, {"k32_resp_write", "i(ii)", resp_write}, {"k32_resp_end", "v()", resp_end},
            {"k32_http_call", "i(iiiiiiiiiiii)", http_call}, {"k32_http_status", "i()", http_status},
            {"esp32_i2c_write", "i(iiii)", i2c_write}, {"esp32_i2c_read", "i(iiii)", i2c_read}, {"esp32_delay_ms", "v(i)", delay_ms},
            {"esp32_gpio_set_mode", "i(ii)", gpio_set_mode}, {"esp32_gpio_write", "i(ii)", gpio_write}, {"esp32_gpio_read", "i(i)", gpio_read},
            {"esp32_chip_temp_read", "i(i)", chip_temp},
        };
        for (const auto& imp : imports) m3_LinkRawFunction(module, "env", imp.name, imp.sig, imp.fn);
        assert(m3_CompileModule(module) == m3Err_none);
    }
    ~Pod() {
        m3_FreeRuntime(rt_);
        m3_FreeEnvironment(env_);
    }

    // A call with i32 arguments: its result; the deepest frame count is noted
    int32_t call(const char* fn_name, std::vector<int32_t> args, const char* what) {
        IM3Function fn = nullptr;
        assert(m3_FindFunction(&fn, rt_, fn_name) == m3Err_none);
        m3_opFrames = m3_opFramesPeak = 0;
        M3Result r = args.empty() ? m3_CallV(fn) : args.size() == 1 ? m3_CallV(fn, args[0]) : m3_CallV(fn, args[0], args[1]);
        if (r) std::printf("%s %s: %s\n", name.c_str(), what, r);
        assert(r == m3Err_none);
        int32_t rc = 0;
        if (m3_GetRetCount(fn)) m3_GetResultsV(fn, &rc);
        std::printf("[frames] %-18s %-34s %4u\n", name.c_str(), what, m3_opFramesPeak);
        if (m3_opFramesPeak > peak) peak = m3_opFramesPeak;
        return rc;
    }

    // Ticks until nothing is pending, 10 ms apart
    void settle(const char* what) {
        for (int i = 0; i < 200 && call("k32_tick", {static_cast<int32_t>(g.now)}, what) > 0; ++i) g.now += 10;
    }

    // k32_handle with a request block: the status
    int32_t request(const std::string& method, const std::string& path, const std::string& body = "", const std::string& headers = "") {
        const std::string block = method + " " + path + "\n" + headers + "\n" + body;
        const int32_t ptr = call("k32_alloc", {static_cast<int32_t>(block.size() + 1)}, "k32_alloc");
        assert(ptr > 0);
        size_t size = 0;
        uint8_t* mem = m3_GetMemory(module_, &size, 0);
        std::memcpy(mem + ptr, block.data(), block.size());
        mem[ptr + block.size()] = 0;
        g.response.clear();
        call("k32_handle", {ptr, static_cast<int32_t>(block.size())}, (method + " " + path).c_str());
        return g.status;
    }

private:
    std::vector<uint8_t> wasm_;
    IM3Environment env_ = nullptr;
    IM3Runtime rt_ = nullptr;
    IM3Module module_ = nullptr;
};

void check(const Pod& pod) {
    std::printf("[frames] %s: deepest call %u operation frames, %u bytes at %u a frame (at most %u frames)\n", pod.name.c_str(), pod.peak,
                pod.peak * kFrameBytes, kFrameBytes, kMaxFrames);
    assert(pod.peak <= kMaxFrames);
}

void tick(Pod& pod, const char* what) { pod.call("k32_tick", {static_cast<int32_t>(g.now)}, what); }

} // namespace

int main() {
    std::printf("=== device drivers on a native stack without tail calls ===\n");
    g.env = {{"DEVICE_NAME", "bme688-kube32-cardputer-a1b2c3-b0-m70c2-77"}, {"NODE_NAME", "kube32-cardputer-a1b2c3"},
             {"DEVICE_TOPIC", "device.bme688-kube32-cardputer-a1b2c3-b0-m70c2-77"}, {"DEVICE_INTERVAL_MS", "5000"}, {"I2C_BUS", "0"}};

    {
        auto part = std::make_shared<k32test::Bme68x>(1);
        g.i2c[0x77] = part;
        g.env["I2C_ADDR"] = "0x77";
        Pod pod("driver-bme688", 3072);
        pod.call("k32_init", {}, "k32_init");
        tick(pod, "first tick: identify, start");
        pod.settle("ticks to the reading");
        g.now += 5000;
        pod.settle("next measurement");
        pod.request("GET", "/");
        pod.request("GET", "/describe");
        pod.request("POST", "/", "{\"action\":\"tare\"}");
        part->absent = true;
        g.now += 5000;
        pod.settle("the part silent");
        g.now += 5000;
        pod.settle("still silent");
        pod.request("GET", "/");
        part->absent = false;
        g.now += 5000;
        pod.settle("found again");
        check(pod);
        g.i2c.clear();
    }
    {
        auto part = std::make_shared<k32test::Bme68x>(0);
        g.i2c[0x76] = part;
        g.env["I2C_ADDR"] = "0x76";
        Pod pod("driver-bme688", 3072);
        pod.name = "driver-bme688/680";
        pod.call("k32_init", {}, "k32_init");
        pod.settle("BME680: first reading");
        check(pod);
        g.i2c.clear();
    }
    {
        auto part = std::make_shared<k32test::MiniScale>();
        g.i2c[0x26] = part;
        g.env["I2C_ADDR"] = "0x26";
        Pod pod("driver-miniscale", 3072);
        pod.call("k32_init", {}, "k32_init");
        tick(pod, "first tick");
        pod.request("POST", "/", "{\"action\":\"tare\"}");
        pod.request("POST", "/", "{\"action\":\"led\",\"r\":255,\"g\":16,\"b\":0}");
        pod.request("POST", "/", "{\"action\":\"led\",\"r\":256}");
        pod.request("GET", "/");
        part->absent = true;
        tick(pod, "the part silent");
        part->absent = false;
        tick(pod, "found again");
        check(pod);
        g.i2c.clear();
    }
    {
        auto reader = std::make_shared<k32test::Mfrc522>();
        g.i2c[0x28] = reader;
        g.env["I2C_ADDR"] = "0x28";
        Pod pod("driver-rfid", 3072);
        pod.call("k32_init", {}, "k32_init");
        tick(pod, "first tick: reader set up");
        reader->uid = {0x04, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0x80};
        tick(pod, "a 7-byte UID");
        reader->uid = {0xde, 0xad, 0xbe, 0xef};
        tick(pod, "a 4-byte UID");
        reader->uid.clear();
        for (int i = 0; i < 3; ++i) tick(pod, "the field empties");
        pod.request("GET", "/");
        reader->absent = true;
        tick(pod, "the reader silent");
        reader->absent = false;
        tick(pod, "found again");
        check(pod);
        g.i2c.clear();
    }
    {
        Pod pod("driver-chip-temp", 3072);
        pod.call("k32_init", {}, "k32_init");
        tick(pod, "first tick");
        g.chip_status = -5;
        tick(pod, "the sensor failing");
        g.chip_status = 0;
        tick(pod, "back");
        pod.request("GET", "/");
        check(pod);
    }
    {
        g.env["PIN_PIN"] = "5";
        g.gpio[5] = 1;
        Pod pod("driver-button", 3072);
        pod.call("k32_init", {}, "k32_init");
        tick(pod, "first tick");
        g.gpio[5] = 0;
        g.now += 50;
        pod.settle("a press");
        g.now += 60000;
        tick(pod, "a report");
        pod.request("GET", "/");
        check(pod);
    }
    {
        g.env["PIN_PIN"] = "9";
        Pod pod("driver-switch", 3072);
        pod.call("k32_init", {}, "k32_init");
        tick(pod, "first tick");
        pod.request("POST", "/", "{\"action\":\"set\",\"on\":true}");
        pod.request("POST", "/", "{\"action\":\"toggle\"}");
        pod.request("POST", "/", "{\"action\":\"set\"}");
        pod.request("GET", "/");
        check(pod);
    }
    {
        g.env["ROOM"] = "kitchen";
        g.env["ENV_URL"] = "http://kitchen-env";
        g.env["SCALE_URL"] = "http://kitchen-scale";
        g.http_answer = "{\"device\":\"kitchen-env\",\"driver\":\"bme688\",\"node\":\"node-a\",\"ok\":true,\"at\":1793462616670,\"uptime_ms\":800150,"
                        "\"readings\":{\"temperature_c\":22.39,\"pressure_hpa\":1002.53,\"humidity_pct\":43.18,\"gas_ohms\":58700}}\n";
        Pod pod("room-monitor", 4096);
        pod.call("k32_init", {}, "k32_init");
        pod.request("GET", "/");
        pod.request("GET", "/kitchen/view");
        pod.request("POST", "/tare");
        pod.request("POST", "/tare", "", "Content-Type: application/x-www-form-urlencoded\n");
        check(pod);
    }
    std::printf("[PASS] device drivers fit the interpreter stack of a board without tail calls\n");
    return 0;
}
