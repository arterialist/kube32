#include "hal/linux_board.hpp"
#include "hal/capability_firewall.hpp"
#include "hal/chip_caps.hpp"
#include "hal/hal_types.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace kube32::hal {

namespace {

// systemctl --user with `args`, waited for; true when it succeeded
bool systemctl_user(std::vector<std::string> args) {
    args.insert(args.begin(), {"systemctl", "--user"});
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, "systemctl", nullptr, nullptr, argv.data(), environ) != 0) return false;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace

bool linux_board_preset(std::string_view name, LinuxBoardConfig& out) {
    if (name == "cardputer-zero") {
        // The M5Stack Cardputer Zero (Raspberry Pi CM0): its udev rules name
        // the ST7789 panel and the TCA8418 keyboard, and its launcher has the
        // screen while no pod uses it. Its own parts (keyboard, codec, RTC,
        // IO expander, fuel gauge, IMU) sit on i2c-1 under kernel drivers.
        // The IR receiver (gpio-ir overlay, BCM 13) and LED (gpio-ir-tx, BCM
        // 12) go by their device tree nodes: their lircN follows probe order.
        out = LinuxBoardConfig{};
        out.name = "cardputer-zero";
        out.framebuffer = "/dev/fb_lcd";
        out.keyboard = "/dev/input/cardputer-zero-internal";
        out.keymap = LinuxKeymap::CardputerZero;
        out.backlight = "/sys/class/backlight/backlight";
        out.i2c[0] = "/dev/i2c-1";
        out.thermal = "/sys/class/thermal/thermal_zone0";
        out.screen_owner = "APPLaunch.service";
        out.audio = cardputer_zero_audio();
        out.ir_rx = "/sys/devices/platform/ir-receiver@d";
        out.ir_tx = "/sys/devices/platform/gpio-ir-transmitter@c";
        return true;
    }
    return false;
}

LinuxBoard::LinuxBoard(LinuxBoardConfig config) : config_(std::move(config)) {}

LinuxBoard::~LinuxBoard() {
    close();
    if (framebuffer_fd_ >= 0) ::close(framebuffer_fd_);
}

void LinuxBoard::close() {
    {
        // A take after this one finds the board closed
        std::lock_guard<std::mutex> lock(take_mutex_);
        if (closed_.load()) return;
        closed_.store(true);
    }
    {
        std::lock_guard<std::mutex> lock(watch_mutex_);
        watch_stop_ = true;
    }
    watch_wake_.notify_all();
    if (watcher_.joinable()) watcher_.join();
    give_back();
    if (ir_rx_) ir_rx_->stop();
    std::unique_lock<std::shared_mutex> audio(audio_mutex_);
    audio_.reset();
}

bool LinuxBoard::init() {
    bool all = true;
    if (!config_.framebuffer.empty()) {
        display_present_ = probe_display();
        all = display_present_ && all;
    }
    if (!config_.keyboard.empty()) {
        keyboard_present_ = probe_keyboard();
        all = keyboard_present_ && all;
    }
    for (const auto& bus : config_.i2c) all = open_i2c(bus.first, bus.second) && all;
    if (!config_.thermal.empty()) all = open_thermal() && all;
    if (config_.audio) all = open_audio() && all;
    if (!config_.ir_rx.empty()) all = open_ir(RmtDir::Rx, config_.ir_rx) && all;
    if (!config_.ir_tx.empty()) all = open_ir(RmtDir::Tx, config_.ir_tx) && all;
    if (display_present_ || keyboard_present_) {
        if (config_.screen_owner.empty()) {
            take_from_owner();
        } else {
            printf("[BOARD] the panel and keyboard stay with %s until a pod uses them\n", config_.screen_owner.c_str());
            watcher_ = std::thread([this] { watch_screen(); });
        }
    }
    return all;
}

void LinuxBoard::take_from_owner() {
    if (taken_.load()) return;
    std::lock_guard<std::mutex> lock(take_mutex_);
    if (taken_.load() || closed_.load()) return;
    const std::string& owner = config_.screen_owner;
    if (!owner.empty() && !systemctl_user({"stop", owner})) {
        printf("[BOARD] cannot stop %s; it may draw over the pods\n", owner.c_str());
    }
    if (display_present_) {
        std::lock_guard<std::mutex> display(display_mutex_);
        map_display_locked();
    }
    if (keyboard_present_) grab_keyboard();
    taken_.store(true);
    if (owner.empty()) return;
    printf("[BOARD] the panel and keyboard are the pods' (%s stopped)\n", owner.c_str());
    fflush(stdout);
}

void LinuxBoard::give_back() {
    std::lock_guard<std::mutex> lock(take_mutex_);
    if (!taken_.load()) return;
    release_keyboard();
    {
        std::lock_guard<std::mutex> display(display_mutex_);
        unmap_display_locked();
    }
    taken_.store(false);
    const std::string& owner = config_.screen_owner;
    if (owner.empty()) return;
    // Not waited for: the node may be on its way out
    if (!systemctl_user({"--no-block", "start", owner})) printf("[BOARD] cannot start %s again\n", owner.c_str());
    else printf("[BOARD] the panel and keyboard are back with %s\n", owner.c_str());
    fflush(stdout);
}

void LinuxBoard::watch_screen() {
    // A pod playing sound keeps the owner stopped too: the owner holds the speaker
    const uint32_t grants = KUBE32_CAP_DISPLAY | KUBE32_CAP_KEYBOARD | KUBE32_CAP_SPEAKER;
    std::unique_lock<std::mutex> lock(watch_mutex_);
    while (!watch_stop_) {
        watch_wake_.wait_for(lock, std::chrono::seconds(1));
        if (watch_stop_ || !taken_.load()) continue;
        if (CapabilityFirewall::instance().capability_held(grants)) continue;
        lock.unlock();
        give_back();
        lock.lock();
    }
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

bool LinuxBoard::open_audio() {
    auto audio = std::make_unique<LinuxAudio>(*config_.audio);
    std::string why;
    if (!audio->open(why)) {
        printf("[BOARD] audio: %s; the node offers no speaker or microphone\n", why.c_str());
        return false;
    }
    speaker_present_ = audio->has_speaker();
    microphone_present_ = audio->has_microphone();
    audio_ = std::move(audio);
    printf("[BOARD] audio:%s%s\n", speaker_present_ ? " speaker" : "", microphone_present_ ? " microphone" : "");
    return true;
}

template <typename Sound>
int32_t LinuxBoard::speaker_call(Sound sound) {
    if (!speaker_present_) return HAL_ERR_NODEV;
    // The owner's audio server closes the speaker only once it has been idle
    // a while (5 s for the Cardputer Zero's PipeWire): a sound right after
    // the take waits for it, up to 6 s
    const bool fresh = !taken_.load() && !config_.screen_owner.empty();
    take_from_owner();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    for (;;) {
        int32_t rc = HAL_ERR_NODEV;
        {
            std::shared_lock<std::shared_mutex> lock(audio_mutex_);
            if (audio_) rc = sound(*audio_);
        }
        if (rc != HAL_ERR_BUSY || !fresh || std::chrono::steady_clock::now() >= until) return rc;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

int32_t LinuxBoard::tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct) {
    return speaker_call([&](LinuxAudio& a) { return a.tone(freq_hz, duration_ms, volume_pct); });
}

int32_t LinuxBoard::audio_play(const int16_t* samples, uint32_t count, uint32_t rate, uint32_t volume_pct) {
    return speaker_call([&](LinuxAudio& a) { return a.play(samples, count, rate, volume_pct); });
}

int32_t LinuxBoard::mic_read(int16_t* out, uint32_t max_samples, uint32_t timeout_ms) {
    if (!microphone_present_) return HAL_ERR_NODEV;
    std::shared_lock<std::shared_mutex> lock(audio_mutex_);
    return audio_ ? audio_->mic_read(out, max_samples, timeout_ms) : HAL_ERR_NODEV;
}

void LinuxBoard::mic_set_gain(uint32_t gain) {
    std::shared_lock<std::shared_mutex> lock(audio_mutex_);
    if (audio_) audio_->mic_set_gain(gain);
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
    // UART or RMT pins. The buses and IR devices that opened run as the
    // kernel set them up.
    profile.name = config_.name;
    profile.reserved_gpio_mask = KUBE32_CHIP_GPIO_MASK;
    profile.uart_port_mask = 0;
    for (BusPins& p : profile.uart_pins) p = {};
    profile.rmt_tx_pin_mask = 0;
    profile.rmt_tx_default_pin = -1;
    profile.rmt_rx_default_pin = -1;
    profile.rmt_fixed_mask = static_cast<uint8_t>((ir_tx_ ? rmt_dir_bit(RmtDir::Tx) : 0) | (ir_rx_ ? rmt_dir_bit(RmtDir::Rx) : 0));
    profile.i2c_bus_mask = 0;
    for (uint8_t bus = 0; bus < 2; ++bus) {
        profile.i2c_pins[bus] = {};
        profile.i2c_reserved[bus].reset();
        if (!i2c_[bus]) continue;
        profile.i2c_bus_mask |= static_cast<uint8_t>(1U << bus);
        profile.i2c_reserved[bus] = i2c_[bus]->reserved();
    }
    profile.i2c_fixed_mask = profile.i2c_bus_mask;
    profile.onboard_devices = (has_display() ? KUBE32_CAP_DISPLAY : 0) | (has_keyboard() ? KUBE32_CAP_KEYBOARD : 0) |
                              (has_speaker() ? KUBE32_CAP_SPEAKER : 0) | (has_microphone() ? KUBE32_CAP_MICROPHONE : 0);
    profile.chip_temp = has_chip_temp();
}

}  // namespace kube32::hal
