// Linux boards: the pods' drawing calls on framebuffer memory (16 and 32 bits
// a pixel, clipped to the panel), evdev key codes as a Cardputer's key codes
// (US layout, the Cardputer Zero's Sym layer), the addresses kernel drivers
// hold in a sysfs tree, and what such a board offers pods and the Node: no
// GPIO, ADC, UART or RMT, its I2C buses without pins and without the kernel's
// addresses, and only the devices that opened.
#include "hal/capability_firewall.hpp"
#include "hal/display_font.hpp"
#include "hal/i2c_discovery.hpp"
#include "hal/key_codes.hpp"
#include "hal/linux_board.hpp"
#include "hal/pod_buses.hpp"
#include "kubelet/node_status_reporter.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace kube32;
using namespace kube32::hal;

namespace {

std::string pod(const std::string& requests, const std::string& annotations = "") {
    return R"({"metadata":{"name":"p","namespace":"default","annotations":{)" + annotations +
           R"(}},"spec":{"containers":[{"name":"c","resources":{"requests":{)" + requests + "}}}]}}";
}

std::string temp_dir() {
    char tmpl[] = "/tmp/kube32_linux_board_XXXXXX";
    const char* dir = mkdtemp(tmpl);
    assert(dir);
    return dir;
}

void make_dirs(const std::string& path) {
    for (size_t at = 1; (at = path.find('/', at)) != std::string::npos; ++at) mkdir(path.substr(0, at).c_str(), 0755);
    mkdir(path.c_str(), 0755);
}

void write_file(const std::string& path, const std::string& text) {
    std::ofstream(path) << text;
}

uint16_t px16(const std::vector<uint8_t>& fb, size_t stride, int x, int y) {
    uint16_t v;
    std::memcpy(&v, fb.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 2, 2);
    return v;
}

uint32_t px32(const std::vector<uint8_t>& fb, size_t stride, int x, int y) {
    uint32_t v;
    std::memcpy(&v, fb.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4, 4);
    return v;
}

void test_rgb565_clipping() {
    // 20 x 10 with 8 bytes of padding a row, which nothing draws on
    const size_t stride = 48;
    std::vector<uint8_t> fb(stride * 10, 0xEE);
    FrameCanvas c;
    assert(c.attach(fb.data(), 20, 10, stride, PixelLayout{}));
    assert(c.width() == 20 && c.height() == 10);
    assert(c.native(0xF800) == 0xF800 && c.native(0x1234) == 0x1234);

    c.rect(-5, -5, 100, 100, 0x0000);
    for (size_t y = 0; y < 10; ++y) {
        for (size_t b = 40; b < stride; ++b) assert(fb[y * stride + b] == 0xEE);
        for (int x = 0; x < 20; ++x) assert(px16(fb, stride, x, static_cast<int>(y)) == 0);
    }
    c.rect(18, 8, 5, 5, 0xFFFF);   // the part on the panel only
    assert(px16(fb, stride, 19, 9) == 0xFFFF && px16(fb, stride, 17, 9) == 0 && px16(fb, stride, 18, 7) == 0);
    c.rect(30, 0, 5, 5, 0xFFFF);   // off the panel: nothing
    c.rect(0, 0, 0, 5, 0xFFFF);
    c.rect(2147483600, 2147483600, 2147483647, 2147483647, 0xFFFF);
    assert(px16(fb, stride, 0, 0) == 0);

    // A 4 x 3 source at (-1, 8): its columns 1..3 and row 0..1 land, read unaligned
    std::vector<uint8_t> src(1 + 4 * 3 * 2);
    for (int i = 0; i < 12; ++i) {
        const uint16_t v = static_cast<uint16_t>(0x100 + i);
        std::memcpy(src.data() + 1 + i * 2, &v, 2);
    }
    c.blit(-1, 8, 4, 3, reinterpret_cast<const uint16_t*>(src.data() + 1));
    assert(px16(fb, stride, 0, 8) == 0x101 && px16(fb, stride, 2, 8) == 0x103);
    assert(px16(fb, stride, 0, 9) == 0x105 && px16(fb, stride, 2, 9) == 0x107);
    assert(px16(fb, stride, 3, 8) == 0);

    c.bar(0, 0, 10, 2, 50, 0x07E0, 0x001F);
    assert(px16(fb, stride, 4, 1) == 0x07E0 && px16(fb, stride, 5, 1) == 0x001F && px16(fb, stride, 9, 0) == 0x001F);
    c.bar(0, 2, 10, 1, 250, 0x07E0, 0x001F);   // over 100 is full
    assert(px16(fb, stride, 9, 2) == 0x07E0);
    for (size_t y = 0; y < 10; ++y) {
        for (size_t b = 40; b < stride; ++b) assert(fb[y * stride + b] == 0xEE);
    }
}

void test_text() {
    const size_t stride = 40;
    std::vector<uint8_t> fb(stride * 10, 0);
    FrameCanvas c;
    assert(c.attach(fb.data(), 20, 10, stride, PixelLayout{}));
    // "AB\x01" from x = -3, y = 4: the right half of A, B, and the top six
    // rows of each, every pixel the font's; \x01 draws as a space
    c.text(-3, 4, "AB\x01", 3, 0xFFFF, 0x1111);
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 20; ++x) {
            const int col = x + 3, row = y - 4;
            uint16_t want = 0;
            if (row >= 0 && row < kFontCellHeight && col < 3 * kFontCellWidth) {
                const char ch = "AB\x01"[col / kFontCellWidth];
                want = font_pixel(static_cast<unsigned char>(ch), col % kFontCellWidth, row) ? 0xFFFF : 0x1111;
            }
            assert(px16(fb, stride, x, y) == want);
        }
    }
    for (int x = 9; x < 15; ++x) assert(px16(fb, stride, x, 5) == 0x1111);
    c.text(0, -8, "A", 1, 0xFFFF, 0x2222);   // above the panel
    c.text(20, 0, "A", 1, 0xFFFF, 0x2222);   // right of it
    assert(px16(fb, stride, 0, 0) == 0);
}

void test_xrgb8888() {
    const size_t stride = 8 * 4;
    std::vector<uint8_t> fb(stride * 4, 0);
    PixelLayout xrgb;
    xrgb.bits = 32;
    xrgb.red_offset = 16;
    xrgb.red_length = 8;
    xrgb.green_offset = 8;
    xrgb.green_length = 8;
    xrgb.blue_offset = 0;
    xrgb.blue_length = 8;
    FrameCanvas c;
    assert(c.attach(fb.data(), 8, 4, stride, xrgb));
    assert(c.native(0xF800) == 0xFF0000 && c.native(0x07E0) == 0x00FF00 && c.native(0x001F) == 0x0000FF);
    assert(c.native(0xFFFF) == 0xFFFFFF && c.native(0x0000) == 0);
    const uint16_t src[2] = {0xF800, 0x001F};
    c.blit(6, 3, 2, 1, src);
    assert(px32(fb, stride, 6, 3) == 0xFF0000 && px32(fb, stride, 7, 3) == 0x0000FF);
    c.rect(0, 0, 1, 1, 0x8410);   // mid grey: 5 and 6 bits widened
    assert(px32(fb, stride, 0, 0) == 0x848284);

    // 24 bits a pixel is not drawn
    PixelLayout rgb24 = xrgb;
    rgb24.bits = 24;
    FrameCanvas none;
    assert(!none.attach(fb.data(), 8, 4, stride, rgb24));
}

// Linux key codes the tests send
enum : uint16_t {
    ESC = 1, K1 = 2, K0 = 11, BACKSPACE = 14, TAB = 15, Q = 16, LEFTBRACE = 26, ENTER = 28, A = 30, SEMICOLON = 39,
    LEFTSHIFT = 42, RIGHTSHIFT = 54, KPASTERISK = 55, SPACE = 57, CAPSLOCK = 58, F1 = 59, KP7 = 71, NUMLOCK = 69,
    MUHENKAN = 94, UP = 103, LEFT = 105, RIGHT = 106, DOWN = 108, DELETE = 111, KEY_CD = 383, KEY_DVD = 389,
};

int press(KeyTranslator& k, uint16_t code) {
    const int c = k.key_event(code, 1);
    assert(k.key_event(code, 2) == -1);   // repeats produce nothing
    assert(k.key_event(code, 0) == -1);
    return c;
}

void test_us_keys() {
    KeyTranslator k(LinuxKeymap::Us);
    assert(press(k, A) == 'a' && press(k, K1) == '1' && press(k, K0) == '0' && press(k, SEMICOLON) == ';');
    assert(press(k, ENTER) == '\n' && press(k, BACKSPACE) == '\b' && press(k, TAB) == '\t' && press(k, ESC) == 27);
    assert(press(k, SPACE) == ' ' && press(k, DELETE) == '\b');
    assert(press(k, UP) == KEY_CODE_UP && press(k, DOWN) == 'D' && press(k, LEFT) == 'L' && press(k, RIGHT) == 'R');
    assert(press(k, F1) == -1 && press(k, LEFTSHIFT) == -1 && press(k, KEY_CD) == -1);
    assert(press(k, KP7) == '7' && press(k, KPASTERISK) == '*' && press(k, LEFTBRACE) == '[');

    assert(k.key_event(LEFTSHIFT, 1) == -1);
    assert(press(k, A) == 'A' && press(k, K1) == '!' && press(k, SEMICOLON) == ':' && press(k, LEFTBRACE) == '{');
    assert(press(k, KP7) == '7' && press(k, UP) == 'U' && press(k, ENTER) == '\n');
    assert(k.key_event(RIGHTSHIFT, 1) == -1 && k.key_event(LEFTSHIFT, 0) == -1);
    assert(press(k, A) == 'A');   // the other Shift still held
    assert(k.key_event(RIGHTSHIFT, 0) == -1 && press(k, A) == 'a');

    // Caps Lock: letters only, and Shift turns them back
    assert(press(k, CAPSLOCK) == -1);
    assert(press(k, A) == 'A' && press(k, K1) == '1');
    k.key_event(LEFTSHIFT, 1);
    assert(press(k, A) == 'a' && press(k, K1) == '!');
    k.key_event(LEFTSHIFT, 0);
    assert(press(k, CAPSLOCK) == -1 && press(k, A) == 'a');
}

void test_cardputer_zero_keys() {
    // The Zero's driver sends the Sym layer's own key codes; Shift is
    // KEY_LEFTSHIFT held while its layer is on; Sym and Fn report
    // KEY_CD and KEY_DVD, which carry no character
    KeyTranslator k(LinuxKeymap::CardputerZero);
    assert(press(k, A) == 'a' && press(k, K1) == '1' && press(k, ENTER) == '\n');
    assert(press(k, KEY_CD) == -1 && press(k, KEY_DVD) == -1);
    assert(press(k, LEFTBRACE) == '!' && press(k, SEMICOLON) == '#' && press(k, KPASTERISK) == '~');
    assert(press(k, KP7) == '-' && press(k, NUMLOCK) == '`' && press(k, MUHENKAN) == ')');
    assert(press(k, UP) == 'U' && press(k, DELETE) == '\b');
    k.key_event(LEFTSHIFT, 1);
    assert(press(k, A) == 'A' && press(k, LEFTBRACE) == '!');
    k.key_event(LEFTSHIFT, 0);
    LinuxKeymap m;
    assert(parse_linux_keymap("cardputer-zero", m) && m == LinuxKeymap::CardputerZero);
    assert(parse_linux_keymap("us", m) && m == LinuxKeymap::Us && !parse_linux_keymap("dvorak", m));
}

void test_kernel_addresses() {
    const std::string root = temp_dir();
    const std::string devices = root + "/sys/bus/i2c/devices";
    make_dirs(devices + "/1-0034/driver");
    make_dirs(devices + "/1-0055/driver");
    make_dirs(devices + "/1-0050");             // a client without a driver
    make_dirs(devices + "/10-0010/driver");     // another adapter
    make_dirs(devices + "/1-a050/driver");      // a 10-bit client
    make_dirs(devices + "/i2c-1");
    const std::bitset<128> held = kernel_i2c_addresses(root + "/sys", 1);
    assert(held.count() == 2 && held.test(0x34) && held.test(0x55));
    assert(kernel_i2c_addresses(root + "/sys", 10).count() == 1 && kernel_i2c_addresses(root + "/sys", 10).test(0x10));
    assert(kernel_i2c_addresses(root + "/sys", 2).none() && kernel_i2c_addresses(root + "/nowhere", 1).none());
    assert(kernel_i2c_addresses(root + "/sys", -1).none());

    assert(i2c_adapter_number("/dev/i2c-1") == 1 && i2c_adapter_number("/dev/i2c-10") == 10);
    assert(i2c_adapter_number("/dev/fb0") == -1 && i2c_adapter_number("/dev/i2c-") == -1 && i2c_adapter_number("/dev/i2c-1x") == -1);
    write_file(root + "/i2c-3", "");
    assert(symlink((root + "/i2c-3").c_str(), (root + "/grove").c_str()) == 0);
    assert(i2c_adapter_number(root + "/grove") == 3);
}

// A Linux board on files: an I2C "device" that opens, a thermal zone, a sysfs
// tree with two kernel clients on adapter 1, and a framebuffer and keyboard
// that are not there
LinuxBoardConfig test_board(const std::string& root) {
    make_dirs(root + "/dev");
    make_dirs(root + "/sys/bus/i2c/devices/1-0034/driver");
    make_dirs(root + "/sys/bus/i2c/devices/1-0068/driver");
    make_dirs(root + "/thermal");
    write_file(root + "/dev/i2c-1", "");
    write_file(root + "/thermal/temp", "50464\n");
    LinuxBoardConfig cfg;
    cfg.name = "test-sbc";
    cfg.framebuffer = root + "/dev/fb9";
    cfg.keyboard = root + "/dev/input/event9";
    cfg.i2c[0] = root + "/dev/i2c-1";
    cfg.thermal = root + "/thermal";
    cfg.sysfs = root + "/sys";
    return cfg;
}

// The resource list `list` ("capacity", "allocatable") of a Node object
std::string resource_list(const std::string& node, const char* list) {
    const size_t at = node.find(std::string("\"") + list + "\":{");
    assert(at != std::string::npos);
    return node.substr(at, node.find('}', at) - at);
}

void test_profile_and_capacity() {
    const std::string root = temp_dir();
    LinuxBoard board(test_board(root));
    assert(!board.init());   // the framebuffer and the keyboard did not open
    assert(!board.has_display() && !board.has_keyboard() && board.has_chip_temp());
    assert(board.display_info().width == 0 && board.display_rect(0, 0, 1, 1, 0) == HAL_ERR_NODEV);
    assert(board.key_read() == HAL_ERR_NODEV);
    int32_t milli = 0;
    assert(board.chip_temp_read(milli) == HAL_OK && milli == 50464);
    assert(board.i2c_adapter(0) && !board.i2c_adapter(1));

    BoardProfile profile;
    board.describe(profile);
    assert(profile.name == "test-sbc" && profile.i2c_bus_mask == 0x1 && profile.i2c_fixed_mask == 0x1);
    assert(profile.i2c_reserved[0].count() == 2 && profile.i2c_reserved[0].test(0x34) && profile.i2c_reserved[0].test(0x68));
    assert(profile.onboard_devices == 0 && profile.chip_temp);

    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    fw.set_board_profile(profile);
    assert(fw.claimable_gpio_mask() == 0 && fw.claimable_adc1_mask() == 0 && fw.claimable_uart_mask() == 0);
    assert(fw.claimable_rmt_tx_count() == 0 && fw.claimable_rmt_rx_count() == 0 && fw.claimable_i2c_mask() == 0x1);

    kubelet::NodeStatusReporter reporter("zero", "192.0.2.10");
    reporter.set_max_pods(4);
    const std::string node = reporter.build_registration_json(reporter.collect_telemetry());
    for (const char* list : {"capacity", "allocatable"}) {
        const std::string r = resource_list(node, list);
        assert(r.find("\"esp32.io/i2c\":\"1\"") != std::string::npos);
        assert(r.find("hardware.kube32.io/chip-temp") != std::string::npos);
        for (const char* absent : {"esp32.io/gpio", "esp32.io/adc", "esp32.io/uart", "esp32.io/rmt-tx", "esp32.io/rmt-rx",
                                   "hardware.kube32.io/tft-display", "hardware.kube32.io/keyboard", "hardware.kube32.io/speaker",
                                   "hardware.kube32.io/microphone", "hardware.kube32.io/touch"}) {
            assert(r.find(absent) == std::string::npos);
        }
    }

    // Nothing opens: no I2C and no chip temperature either
    LinuxBoardConfig bare = test_board(root);
    bare.i2c[0] = root + "/dev/i2c-7";
    bare.i2c[2] = root + "/dev/i2c-1";   // pods have buses 0 and 1
    bare.thermal = root + "/thermal-none";
    LinuxBoard nothing(bare);
    assert(!nothing.init());
    BoardProfile none;
    nothing.describe(none);
    assert(none.i2c_bus_mask == 0 && none.onboard_devices == 0 && !none.chip_temp);
    fw.reset();
    fw.set_board_profile(none);
    const std::string empty = reporter.build_registration_json(reporter.collect_telemetry());
    const std::string cap = resource_list(empty, "capacity");
    assert(cap.find("esp32.io/i2c") == std::string::npos && cap.find("hardware.kube32.io/chip-temp") == std::string::npos);
    assert(cap.find("hardware.kube32.io/ipc") != std::string::npos);
    fw.reset();
}

// An I2C bus that acknowledges `present` and records every address it sees
class FakeAdapter : public HostI2cAdapter {
public:
    const std::string& device() const override { return name_; }
    int32_t write(uint8_t addr, const uint8_t*, size_t len) override {
        seen.push_back(addr);
        if (std::find(present.begin(), present.end(), addr) == present.end()) return HAL_ERR_NODEV;
        return len == 0 ? HAL_OK : static_cast<int32_t>(len);
    }
    int32_t read(uint8_t addr, uint8_t* buf, size_t len) override {
        seen.push_back(addr);
        if (std::find(present.begin(), present.end(), addr) == present.end()) return HAL_ERR_NODEV;
        std::memset(buf, 0x5A, len);
        return static_cast<int32_t>(len);
    }
    std::vector<uint8_t> present;
    std::vector<uint8_t> seen;

private:
    std::string name_{"fake-i2c"};
};

void test_admission_and_transfers() {
    const std::string root = temp_dir();
    LinuxBoard board(test_board(root));
    board.init();
    BoardProfile profile;
    board.describe(profile);
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    fw.set_board_profile(profile);
    std::string err;

    // No pins to claim, of any kind
    for (const char* req : {R"("esp32.io/gpio-5":"1")", R"("esp32.io/gpio-input-0":"1")", R"("esp32.io/gpio":"1")",
                            R"("esp32.io/adc1-channel-0":"1")", R"("esp32.io/adc":"1")", R"("esp32.io/uart-1":"1")",
                            R"("esp32.io/uart":"1")", R"("esp32.io/rmt-tx":"1")", R"("esp32.io/rmt-rx":"1")",
                            R"("esp32.io/i2c-bus-1":"1")"}) {
        assert(!fw.register_pod("default/p", pod(req), err));
    }
    // Its bus pins and speed are the board's
    assert(!fw.register_pod("default/p", pod(R"("esp32.io/i2c-bus-0":"1")", R"("kube32.io/i2c-bus-0":"sda=2,scl=1")"), err));
    assert(err.find("own pins") != std::string::npos);
    // A device grant on an address the kernel holds
    assert(!fw.register_pod("default/p", pod("", R"("kube32.io/i2c-device":"bus=0,addr=0x34")"), err));
    assert(err.find("0x34") != std::string::npos);

    FakeAdapter fake;
    fake.present = {0x34, 0x40, 0x68};
    set_host_i2c_adapter(0, &fake);

    // Discovery: the bus's own devices, not the kernel's
    I2cDiscovery discovery;
    discovery.scan();
    const auto labels = discovery.labels();
    assert(labels.count("i2c.kube32.io/0x40") && labels.at("i2c.kube32.io/0x40") == "bus-0");
    assert(!labels.count("i2c.kube32.io/0x34") && !labels.count("i2c.kube32.io/0x68"));
    assert(std::find(fake.seen.begin(), fake.seen.end(), 0x34) == fake.seen.end());
    assert(std::find(fake.seen.begin(), fake.seen.end(), 0x68) == fake.seen.end());

    // The whole bus: transfers reach the adapter; the kernel's addresses
    // have no device for the pod
    assert(fw.register_pod("default/p", pod(R"("esp32.io/i2c-bus-0":"1")"), err));
    const auto buses = fw.pod_buses("default/p");
    assert(buses.size() == 1 && buses[0].pin_a == kBoardWiredPin && buses[0].pin_b == kBoardWiredPin);
    assert(fw.get_allocated_gpio_mask() == 0);
    models::I2cLocation loc;
    assert(fw.check_i2c("default/p", 0, 0x40, AccessMode::Write, loc) == I2cAccess::Bus);
    assert(fw.check_i2c("default/p", 0, 0x34, AccessMode::Write, loc) == I2cAccess::Absent);
    const uint8_t reg = 0x01;
    fake.seen.clear();
    assert(i2c_bus_write(0, 0x40, &reg, 1) == 1);
    uint8_t got[2] = {};
    assert(i2c_bus_read(0, 0x40, got, 2) == 2 && got[0] == 0x5A);
    assert(i2c_bus_write(0, 0x41, nullptr, 0) == HAL_ERR_NODEV);
    assert(fake.seen.size() == 3 && fake.seen[0] == 0x40);
    fw.unregister_pod("default/p");

    // A device grant: the bus is shared without pins
    assert(fw.register_pod("default/p", pod("", R"("kube32.io/i2c-device":"bus=0,addr=0x40")"), err));
    assert(fw.check_i2c("default/p", 0, 0x40, AccessMode::Read, loc) == I2cAccess::Device && loc.address == 0x40);
    assert(fw.check_i2c("default/p", 0, 0x34, AccessMode::Read, loc) == I2cAccess::Denied);   // outside its grant
    assert(i2c_device_read(loc, got, 1) == 1);
    fw.unregister_pod("default/p");

    set_host_i2c_adapter(0, nullptr);
    fw.reset();
}

void test_preset() {
    LinuxBoardConfig cfg;
    assert(!linux_board_preset("cardputer", cfg) && !linux_board_preset("generic", cfg));
    assert(linux_board_preset("cardputer-zero", cfg));
    assert(cfg.name == "cardputer-zero" && cfg.keymap == LinuxKeymap::CardputerZero);
    assert(cfg.i2c.size() == 1 && cfg.i2c.at(0) == "/dev/i2c-1" && !cfg.framebuffer.empty() && !cfg.keyboard.empty());
    assert(cfg.screen_owner == "APPLaunch.service");
}

// The watcher gives the screen back once no pod holds a display or keyboard
// grant: the firewall's answer it goes by
void test_capability_held() {
    auto& fw = CapabilityFirewall::instance();
    fw.reset();
    BoardProfile p;
    p.onboard_devices = KUBE32_CAP_DISPLAY | KUBE32_CAP_KEYBOARD;
    fw.set_board_profile(p);
    const uint32_t screen = KUBE32_CAP_DISPLAY | KUBE32_CAP_KEYBOARD;
    std::string err;
    assert(!fw.capability_held(screen));
    assert(fw.register_pod("default/k", pod(R"("hardware.kube32.io/keyboard":"1")"), err));
    assert(fw.capability_held(screen) && fw.capability_held(KUBE32_CAP_KEYBOARD) && !fw.capability_held(KUBE32_CAP_DISPLAY));
    fw.unregister_pod("default/k");
    assert(!fw.capability_held(screen));
    fw.reset();
}

}  // namespace

int main() {
    test_rgb565_clipping();
    test_text();
    test_xrgb8888();
    test_us_keys();
    test_cardputer_zero_keys();
    test_kernel_addresses();
    test_profile_and_capacity();
    test_admission_and_transfers();
    test_preset();
    test_capability_held();
    std::cout << "test_linux_board: all passed" << std::endl;
    return 0;
}
