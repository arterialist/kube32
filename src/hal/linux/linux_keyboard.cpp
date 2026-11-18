// A Linux board's keyboard: its evdev events as the key codes a Cardputer
// returns, queued for k32_key_read. The reader runs on Linux only.

#include "hal/linux_board.hpp"
#include "hal/hal_types.hpp"
#include "hal/key_codes.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#endif

namespace kube32::hal {

namespace {

// Key codes of linux/input-event-codes.h (a stable kernel interface), named
// here so that the table builds on every host
constexpr uint16_t kKeyLeftShift = 42;
constexpr uint16_t kKeyRightShift = 54;
constexpr uint16_t kKeyCapsLock = 58;

struct KeyChar {
    uint16_t code;
    char c;
    bool shifts;   // Shift changes it (keypad keys stay as they are)
};

// The US layout: the character of each key code without Shift
constexpr KeyChar kUsKeys[] = {
    {1, KEY_CODE_ESCAPE, false},
    {2, '1', true}, {3, '2', true}, {4, '3', true}, {5, '4', true}, {6, '5', true},
    {7, '6', true}, {8, '7', true}, {9, '8', true}, {10, '9', true}, {11, '0', true},
    {12, '-', true}, {13, '=', true}, {14, KEY_CODE_BACKSPACE, false}, {15, KEY_CODE_TAB, false},
    {16, 'q', true}, {17, 'w', true}, {18, 'e', true}, {19, 'r', true}, {20, 't', true},
    {21, 'y', true}, {22, 'u', true}, {23, 'i', true}, {24, 'o', true}, {25, 'p', true},
    {26, '[', true}, {27, ']', true}, {28, KEY_CODE_ENTER, false},
    {30, 'a', true}, {31, 's', true}, {32, 'd', true}, {33, 'f', true}, {34, 'g', true},
    {35, 'h', true}, {36, 'j', true}, {37, 'k', true}, {38, 'l', true},
    {39, ';', true}, {40, '\'', true}, {41, '`', true}, {43, '\\', true},
    {44, 'z', true}, {45, 'x', true}, {46, 'c', true}, {47, 'v', true}, {48, 'b', true},
    {49, 'n', true}, {50, 'm', true}, {51, ',', true}, {52, '.', true}, {53, '/', true},
    {55, '*', false}, {57, ' ', false},
    {71, '7', false}, {72, '8', false}, {73, '9', false}, {74, '-', false},
    {75, '4', false}, {76, '5', false}, {77, '6', false}, {78, '+', false},
    {79, '1', false}, {80, '2', false}, {81, '3', false}, {82, '0', false}, {83, '.', false},
    {96, KEY_CODE_ENTER, false}, {98, '/', false},
    {103, KEY_CODE_UP, false}, {105, KEY_CODE_LEFT, false}, {106, KEY_CODE_RIGHT, false}, {108, KEY_CODE_DOWN, false},
    // Fn+Backspace on a Cardputer is a backspace too
    {111, KEY_CODE_BACKSPACE, false},
};

// The Cardputer Zero's Sym layer: the key codes it sends and the symbols its
// console keymap (M5Stack's tca8418_keypad_m5stack_keymap.map) gives them
constexpr KeyChar kCardputerZeroSym[] = {
    {26, '!', false}, {27, '@', false}, {39, '#', false}, {40, '$', false}, {41, '%', false},
    {43, '^', false}, {51, '&', false}, {52, '*', false}, {53, '(', false}, {94, ')', false},
    {55, '~', false}, {69, '`', false}, {70, '_', false}, {71, '-', false}, {72, '+', false},
    {73, '=', false}, {74, '[', false}, {75, ']', false}, {76, '{', false}, {77, '}', false},
    {79, ';', false}, {80, ':', false}, {81, '\'', false}, {82, '"', false}, {83, '<', false},
    {85, '>', false}, {86, '\\', false}, {89, '|', false},
    {90, ',', false}, {91, '.', false}, {92, '/', false}, {93, '?', false},
};

template <size_t N>
const KeyChar* find_key(const KeyChar (&table)[N], uint16_t code) {
    for (const KeyChar& k : table) {
        if (k.code == code) return &k;
    }
    return nullptr;
}

}  // namespace

bool parse_linux_keymap(std::string_view name, LinuxKeymap& out) {
    if (name == "us") out = LinuxKeymap::Us;
    else if (name == "cardputer-zero") out = LinuxKeymap::CardputerZero;
    else return false;
    return true;
}

int KeyTranslator::key_event(uint16_t code, int32_t value) {
    if (code == kKeyLeftShift) { left_shift_ = value != 0; return -1; }
    if (code == kKeyRightShift) { right_shift_ = value != 0; return -1; }
    if (code == kKeyCapsLock) {
        if (value == 1) caps_lock_ = !caps_lock_;
        return -1;
    }
    // A Cardputer's key produces its code once per press
    if (value != 1) return -1;
    const KeyChar* k = keymap_ == LinuxKeymap::CardputerZero ? find_key(kCardputerZeroSym, code) : nullptr;
    if (!k) k = find_key(kUsKeys, code);
    if (!k) return -1;
    const bool shift = left_shift_ || right_shift_;
    char c = k->c;
    if (c >= 'a' && c <= 'z') {
        if (shift != caps_lock_) c = shifted_key(c);
    } else if (k->shifts && shift) {
        c = shifted_key(c);
    }
    return static_cast<unsigned char>(c);
}

void LinuxBoard::push_key(int code) {
    std::lock_guard<std::mutex> lock(key_mutex_);
    const uint8_t next = static_cast<uint8_t>((key_head_ + 1) % sizeof(keys_));
    if (next == key_tail_) return;
    keys_[key_head_] = static_cast<char>(code);
    key_head_ = next;
}

int32_t LinuxBoard::key_read() {
    if (!keyboard_present_) return HAL_ERR_NODEV;
    take_from_owner();
    std::lock_guard<std::mutex> lock(key_mutex_);
    if (key_tail_ == key_head_) return HAL_ERR_AGAIN;
    const char c = keys_[key_tail_];
    key_tail_ = static_cast<uint8_t>((key_tail_ + 1) % sizeof(keys_));
    return static_cast<unsigned char>(c);
}

void LinuxBoard::key_inject(char c) {
    push_key(static_cast<unsigned char>(c));
}

bool LinuxBoard::probe_keyboard() {
    const std::string& dev = config_.keyboard;
#if defined(__linux__)
    const int fd = ::open(dev.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        printf("[BOARD] keyboard %s: cannot open it (%s); the node offers no keyboard\n", dev.c_str(), std::strerror(errno));
        return false;
    }
    unsigned long types = 0;
    char name[64] = {};
    const bool keys = ::ioctl(fd, EVIOCGBIT(0, sizeof(types)), &types) >= 0 && (types & (1UL << EV_KEY)) != 0;
    ::ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
    ::close(fd);
    if (!keys) {
        printf("[BOARD] keyboard %s: not a keyboard; the node offers no keyboard\n", dev.c_str());
        return false;
    }
    printf("[BOARD] keyboard %s: %s\n", dev.c_str(), name[0] ? name : "evdev");
    return true;
#else
    printf("[BOARD] keyboard %s: evdev devices are Linux devices; the node offers no keyboard\n", dev.c_str());
    return false;
#endif
}

void LinuxBoard::grab_keyboard() {
#if defined(__linux__)
    const std::string& dev = config_.keyboard;
    const int fd = ::open(dev.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        printf("[BOARD] keyboard %s: cannot open it (%s); no keys reach the pods\n", dev.c_str(), std::strerror(errno));
        return;
    }
    // Key presses go to the pods only, not to a console or a launcher
    if (::ioctl(fd, EVIOCGRAB, 1) < 0) {
        printf("[BOARD] keyboard %s: another program holds it (%s); no keys reach the pods\n", dev.c_str(), std::strerror(errno));
        ::close(fd);
        return;
    }
    {
        // Keys pressed while the panel was someone else's are not the pods'
        std::lock_guard<std::mutex> lock(key_mutex_);
        key_tail_ = key_head_;
    }
    keyboard_fd_ = fd;
    stop_reader_.store(false);
    reader_ = std::thread([this] { read_keys(); });
#endif
}

void LinuxBoard::release_keyboard() {
    stop_reader_.store(true);
    if (reader_.joinable()) reader_.join();
    if (keyboard_fd_ >= 0) ::close(keyboard_fd_);
    keyboard_fd_ = -1;
}

void LinuxBoard::read_keys() {
#if defined(__linux__)
    KeyTranslator keys(config_.keymap);
    pollfd p{keyboard_fd_, POLLIN, 0};
    input_event events[16];
    while (!stop_reader_.load()) {
        // A timeout lets the board stop the reader
        const int ready = ::poll(&p, 1, 200);
        if (ready <= 0) continue;
        const ssize_t n = ::read(keyboard_fd_, events, sizeof(events));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            printf("[BOARD] keyboard %s: %s; no more keys reach the pods\n", config_.keyboard.c_str(), std::strerror(errno));
            return;
        }
        for (size_t i = 0; i < static_cast<size_t>(n) / sizeof(input_event); ++i) {
            if (events[i].type != EV_KEY) continue;
            const int code = keys.key_event(events[i].code, events[i].value);
            if (code >= 0) push_key(code);
        }
    }
#endif
}

}  // namespace kube32::hal
