#include "hal/linux_board.hpp"
#include "hal/chip_caps.hpp"
#include "hal/hal_types.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace kube32::hal {

bool linux_board_preset(std::string_view name, LinuxBoardConfig& out) {
    if (name == "cardputer-zero") {
        // The M5Stack Cardputer Zero (Raspberry Pi CM0): its udev rules name
        // the ST7789 panel and the TCA8418 keyboard. Its own parts (keyboard,
        // codec, RTC, IO expander, fuel gauge, IMU) sit on i2c-1 under kernel
        // drivers.
        out = LinuxBoardConfig{};
        out.name = "cardputer-zero";
        out.framebuffer = "/dev/fb_lcd";
        out.keyboard = "/dev/input/cardputer-zero-internal";
        out.keymap = LinuxKeymap::CardputerZero;
        out.backlight = "/sys/class/backlight/backlight";
        out.i2c[0] = "/dev/i2c-1";
        out.thermal = "/sys/class/thermal/thermal_zone0";
        return true;
    }
    return false;
}

LinuxBoard::LinuxBoard(LinuxBoardConfig config) : config_(std::move(config)) {}

LinuxBoard::~LinuxBoard() {
    stop_.store(true);
    if (reader_.joinable()) reader_.join();
    if (keyboard_fd_ >= 0) ::close(keyboard_fd_);
#if defined(__linux__)
    if (framebuffer_) ::munmap(framebuffer_, framebuffer_bytes_);
#endif
    if (framebuffer_fd_ >= 0) ::close(framebuffer_fd_);
}

bool LinuxBoard::init() {
    bool all = true;
    if (!config_.framebuffer.empty()) all = open_display() && all;
    if (!config_.keyboard.empty()) all = open_keyboard() && all;
    for (const auto& bus : config_.i2c) all = open_i2c(bus.first, bus.second) && all;
    if (!config_.thermal.empty()) all = open_thermal() && all;
    return all;
}

bool LinuxBoard::open_i2c(uint8_t bus, const std::string& device) {
    if (bus >= 2) {
        printf("[BOARD] I2C%u %s: pods have I2C buses 0 and 1; not offered\n", bus, device.c_str());
        return false;
    }
    const int fd = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("[BOARD] I2C%u %s: cannot open it (%s); the node offers no I2C%u\n", bus, device.c_str(), std::strerror(errno), bus);
        return false;
    }
    const std::bitset<128> held = kernel_i2c_addresses(config_.sysfs, i2c_adapter_number(device));
    std::string list;
    for (unsigned a = 0; a < 128; ++a) {
        if (!held.test(a)) continue;
        char hex[8];
        snprintf(hex, sizeof(hex), " 0x%02x", a);
        list += hex;
    }
    i2c_[bus] = std::make_unique<LinuxI2cAdapter>(device, fd, held);
    printf("[BOARD] I2C%u %s%s%s\n", bus, device.c_str(), list.empty() ? "" : ": kernel drivers hold", list.c_str());
    return true;
}

bool LinuxBoard::open_thermal() {
    const std::string path = config_.thermal + "/temp";
    long milli = 0;
    std::ifstream in(path);
    if (!(in >> milli)) {
        printf("[BOARD] chip temperature %s: cannot read it; the node offers no chip temperature\n", path.c_str());
        return false;
    }
    temp_path_ = path;
    printf("[BOARD] chip temperature %s: %.1f C\n", path.c_str(), static_cast<double>(milli) / 1000.0);
    return true;
}

int32_t LinuxBoard::chip_temp_read(int32_t& out_milli_c) const {
    if (temp_path_.empty()) return HAL_ERR_NODEV;
    long milli = 0;
    std::ifstream in(temp_path_);
    if (!(in >> milli)) return HAL_ERR_IO;
    out_milli_c = static_cast<int32_t>(milli);
    return HAL_OK;
}

HostI2cAdapter* LinuxBoard::i2c_adapter(uint8_t bus) const {
    return bus < 2 ? i2c_[bus].get() : nullptr;
}

void LinuxBoard::describe(BoardProfile& profile) const {
    // The kernel owns the SoC's pins: pods claim none of them, as GPIO, ADC,
    // UART or RMT. The buses that opened run as the kernel set them up.
    profile.name = config_.name;
    profile.reserved_gpio_mask = KUBE32_CHIP_GPIO_MASK;
    profile.uart_port_mask = 0;
    for (BusPins& p : profile.uart_pins) p = {};
    profile.rmt_tx_pin_mask = 0;
    profile.rmt_tx_default_pin = -1;
    profile.rmt_rx_default_pin = -1;
    profile.i2c_bus_mask = 0;
    for (uint8_t bus = 0; bus < 2; ++bus) {
        profile.i2c_pins[bus] = {};
        profile.i2c_reserved[bus].reset();
        if (!i2c_[bus]) continue;
        profile.i2c_bus_mask |= static_cast<uint8_t>(1U << bus);
        profile.i2c_reserved[bus] = i2c_[bus]->reserved();
    }
    profile.i2c_fixed_mask = profile.i2c_bus_mask;
    profile.onboard_devices = (has_display() ? KUBE32_CAP_DISPLAY : 0) | (has_keyboard() ? KUBE32_CAP_KEYBOARD : 0);
    profile.chip_temp = has_chip_temp();
}

}  // namespace kube32::hal
