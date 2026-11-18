#pragma once

// A Linux single-board computer's own devices for a kube32-sim node: the
// panel through its framebuffer, the keyboard through evdev, I2C buses
// through /dev/i2c-N and the SoC's temperature through a thermal zone. The
// pod API reaches them as it reaches a Cardputer's: the same font, the same
// key codes, the same I2C transfers. A preset names the devices of a known
// board (cardputer-zero); kube32-sim's flags describe any other.
//
// The devices are opened on Linux only. The drawing, the key translation and
// the reading of sysfs build on every host, so that tests run anywhere.

#include "hal/board.hpp"
#include "hal/pod_buses.hpp"

#include <atomic>
#include <bitset>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace kube32::hal {

// The characters a board's keys carry
enum class LinuxKeymap : uint8_t {
    Us,              // a US keyboard
    // The Cardputer Zero: a US layout, and its Sym layer, whose keys send key
    // codes that the board's console keymap turns into symbols
    // (Sym+Q, KEY_KPASTERISK, is '~')
    CardputerZero,
};

// "us" or "cardputer-zero"; false for another name
bool parse_linux_keymap(std::string_view name, LinuxKeymap& out);

struct LinuxBoardConfig {
    std::string name;                     // the kube32.io/board label
    std::string framebuffer;              // /dev/fbN; empty: no display
    std::string keyboard;                 // /dev/input/eventN; empty: no keyboard
    LinuxKeymap keymap{LinuxKeymap::Us};
    std::string backlight;                // /sys/class/backlight/<name>, turned on when off; empty: none
    std::map<uint8_t, std::string> i2c;   // pod bus (0, 1) -> /dev/i2c-N
    std::string thermal;                  // /sys/class/thermal/thermal_zoneN; empty: no chip temperature
    // A systemd user unit that has the panel and the keyboard while no pod
    // uses them (a launcher); empty: the node keeps them from its start
    std::string screen_owner;
    std::string sysfs{"/sys"};            // where the kernel lists its I2C devices
};

// The devices of a known board, by its name; false for a name without a preset
bool linux_board_preset(std::string_view name, LinuxBoardConfig& out);

// A framebuffer's pixels as the pods' RGB565 drawing calls see them: every
// call clips to the panel. 16 and 32 bits a pixel, each channel where the
// framebuffer's layout puts it.
struct PixelLayout {
    uint8_t bits{16};
    uint8_t red_offset{11}, red_length{5};
    uint8_t green_offset{5}, green_length{6};
    uint8_t blue_offset{0}, blue_length{5};
};

class FrameCanvas {
public:
    // Draws into `pixels`, rows `stride` bytes apart; false for a layout of
    // another pixel size
    bool attach(uint8_t* pixels, int width, int height, size_t stride, const PixelLayout& layout);
    int width() const { return width_; }
    int height() const { return height_; }

    // The font of display_font.hpp in 6x8 cells; characters outside 32..126
    // draw as spaces
    void text(int x, int y, const char* s, size_t len, uint16_t fg, uint16_t bg);
    void rect(int x, int y, int w, int h, uint16_t color);
    void bar(int x, int y, int w, int h, uint32_t pct, uint16_t fg, uint16_t bg);
    // w x h RGB565 pixels, row after row, any alignment
    void blit(int x, int y, int w, int h, const uint16_t* pixels);
    // An RGB565 color in the framebuffer's layout
    uint32_t native(uint16_t rgb565) const;

private:
    void put(int x, int y, uint32_t pixel);
    // Clips (x, y, w, h) to the panel; false when nothing is left
    bool clip(int& x, int& y, int& w, int& h) const;

    uint8_t* pixels_{nullptr};
    int width_{0};
    int height_{0};
    size_t stride_{0};
    PixelLayout layout_;
};

// Turns a keyboard's EV_KEY events into the codes k32_key_read returns
// (key_codes.hpp), as a Cardputer's keys produce them: a code for each press,
// none for releases, repeats or modifiers. Shift and Caps Lock change the
// letters, Shift the other printable keys; keys without a code (function and
// media keys) produce none.
class KeyTranslator {
public:
    explicit KeyTranslator(LinuxKeymap keymap) : keymap_(keymap) {}
    // The code of event (code, value: 0 release, 1 press, 2 repeat), or -1
    int key_event(uint16_t code, int32_t value);

private:
    LinuxKeymap keymap_;
    bool left_shift_{false};
    bool right_shift_{false};
    bool caps_lock_{false};
};

// The adapter number of an I2C device node: "/dev/i2c-1" is 1, a symlink is
// followed; -1 when the name is no i2c-<n>
int i2c_adapter_number(const std::string& device);
// Addresses on adapter `adapter` that a kernel driver holds: those with a
// <sysfs>/bus/i2c/devices/<adapter>-00<hh>/driver
std::bitset<128> kernel_i2c_addresses(const std::string& sysfs, int adapter);

// An I2C bus of the board (/dev/i2c-N): I2C_RDWR transfers, none to an
// address a kernel driver holds
class LinuxI2cAdapter final : public HostI2cAdapter {
public:
    LinuxI2cAdapter(std::string device, int fd, std::bitset<128> reserved);
    ~LinuxI2cAdapter() override;
    const std::string& device() const override { return device_; }
    int32_t write(uint8_t addr, const uint8_t* data, size_t len) override;
    int32_t read(uint8_t addr, uint8_t* buf, size_t len) override;
    const std::bitset<128>& reserved() const { return reserved_; }

private:
    std::string device_;
    int fd_;
    std::bitset<128> reserved_;
};

// The panel and the keyboard belong to the node, or, on a board with a
// screen owner (a launcher's systemd user unit), to the node only while pods
// use them: the first drawing call or key read of a pod stops the owner and
// takes them, and once no pod holds a display, keyboard or speaker grant they
// go back and the owner starts again. Stopping the owner also frees what else
// it holds: the Cardputer Zero's launcher keeps the speaker busy while it runs.
class LinuxBoard final : public Board {
public:
    explicit LinuxBoard(LinuxBoardConfig config);
    ~LinuxBoard() override;

    const char* name() const override { return config_.name.c_str(); }
    // Opens the configured devices. One line is logged for each device that
    // cannot be opened, and the board does not offer it. True when every
    // configured device opened. Without a screen owner the node takes the
    // panel and the keyboard now.
    bool init() override;
    // No GPIO, ADC, UART or RMT; the I2C buses that opened, with the addresses
    // their kernel drivers hold; the display and keyboard when they opened
    void describe(BoardProfile& profile) const override;

    DisplayInfo display_info() const override;
    int32_t display_text(int32_t x, int32_t y, const char* text, size_t len, uint16_t fg, uint16_t bg) override;
    int32_t display_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) override;
    int32_t display_bar(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pct, uint16_t fg, uint16_t bg) override;
    int32_t display_blit(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* pixels) override;

    int32_t key_read() override;
    void key_inject(char c) override;

    // The boards' ready screen: the node's address and the cluster's app URL,
    // on a panel the node keeps (no screen owner). The first drawing call
    // after it clears the panel to black, so that a pod drawing on less than
    // the whole panel shows nothing of it.
    void show_ready(const std::string& node_ip, const std::string& app_url);

    // Stops the screen owner and gives the node the panel and the keyboard;
    // returns at once when the node has them. The display and key calls take
    // them so, and a sound call takes them for the speaker the owner holds.
    void take_from_owner();
    // The node on its way out: the panel and the keyboard go back to the
    // screen owner for good, and the display and key calls of pods still
    // running answer HAL_ERR_NODEV
    void close();

    // The adapter of pod bus `bus`, nullptr when the board has none
    HostI2cAdapter* i2c_adapter(uint8_t bus) const;
    bool has_display() const { return display_present_; }
    bool has_keyboard() const { return keyboard_present_; }
    bool has_chip_temp() const { return !temp_path_.empty(); }
    // The thermal zone in milli-degrees Celsius: HAL_OK, HAL_ERR_NODEV without
    // one, HAL_ERR_IO when it cannot be read
    int32_t chip_temp_read(int32_t& out_milli_c) const;

private:
    // Where the visible panel is in the framebuffer device
    struct Panel {
        int width{0};
        int height{0};
        size_t stride{0};
        size_t offset{0};   // of the visible part, in bytes
        size_t bytes{0};    // of the whole mapping
        PixelLayout layout;
    };

    bool probe_display();
    bool probe_keyboard();
    bool open_i2c(uint8_t bus, const std::string& device);
    bool open_thermal();
    // The panel and the keyboard back to the screen owner, starting it again
    void give_back();
    // Gives them back once no pod holds a display, keyboard or speaker grant
    void watch_screen();
    bool map_display_locked();
    void unmap_display_locked();
    void grab_keyboard();
    void release_keyboard();
    void backlight_on();
    void read_keys();
    void push_key(int code);
    // Clears the ready screen before a pod's first drawing call; display_mutex_ held
    void leave_ready_locked();

    LinuxBoardConfig config_;

    std::mutex take_mutex_;
    std::atomic<bool> taken_{false};
    std::atomic<bool> closed_{false};   // set under take_mutex_
    std::thread watcher_;
    std::mutex watch_mutex_;
    std::condition_variable watch_wake_;
    bool watch_stop_{false};

    mutable std::mutex display_mutex_;
    bool display_present_{false};
    int framebuffer_fd_{-1};
    Panel panel_;
    uint8_t* framebuffer_{nullptr};   // mapped while the node has the panel
    FrameCanvas canvas_;
    bool showing_ready_{false};

    bool keyboard_present_{false};
    int keyboard_fd_{-1};
    std::atomic<bool> stop_reader_{false};
    std::thread reader_;
    std::mutex key_mutex_;
    // Key presses queue up as on the Cardputer (16 deep); a press arriving
    // when the queue is full is dropped
    char keys_[16]{};
    uint8_t key_head_{0};
    uint8_t key_tail_{0};

    std::unique_ptr<LinuxI2cAdapter> i2c_[2];
    std::string temp_path_;
};

}  // namespace kube32::hal
