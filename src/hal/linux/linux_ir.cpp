// A Linux board's infrared transmitter and receiver through the kernel's
// LIRC devices (Linux only): frames written in LIRC_MODE_PULSE, samples read
// in LIRC_MODE_MODE2 and cut into frames as the RMT receiver captures them.
// The framing and the device lookup in sysfs build on every host.

#include "hal/linux_board.hpp"
#include "hal/hal_types.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/lirc.h>
#include <sys/ioctl.h>

static_assert(kube32::hal::kLircSpace == LIRC_MODE2_SPACE && kube32::hal::kLircPulse == LIRC_MODE2_PULSE &&
                  kube32::hal::kLircTimeout == LIRC_MODE2_TIMEOUT && kube32::hal::kLircOverflow == LIRC_MODE2_OVERFLOW &&
                  kube32::hal::kLircKindMask == LIRC_MODE2_MASK && kube32::hal::kLircValueMask == LIRC_VALUE_MASK,
              "the mode2 values of linux/lirc.h");
#endif

namespace kube32::hal {

namespace {

// "lirc<n>"
bool is_lirc_name(const char* name) {
    if (std::strncmp(name, "lirc", 4) != 0 || name[4] == '\0') return false;
    for (const char* p = name + 4; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

// The lirc<n> entry of directory `dir`, empty for none
std::string lirc_entry(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return {};
    std::string found;
    while (const dirent* e = ::readdir(d)) {
        if (is_lirc_name(e->d_name)) {
            found = e->d_name;
            break;
        }
    }
    ::closedir(d);
    return found;
}

}  // namespace

bool LircFramer::feed(uint32_t sample) {
    if (ended_) {
        frame_.clear();
        ended_ = false;
    }
    const uint32_t kind = sample & kLircKindMask;
    const uint32_t us = sample & kLircValueMask;
    if (kind == kLircPulse) {
        if (us == 0) return false;
        if (frame_.empty()) {
            append(us);
        } else if (space_us_ > 0) {
            append(space_us_);
            append(us);
            space_us_ = 0;
        } else if (frame_.size() <= kRmtMaxDurations) {
            // Pulses in a row are one mark
            const uint32_t sum = frame_.back() + us;
            frame_.back() = static_cast<uint16_t>(sum > 0xffff ? 0xffff : sum);
        }
        return false;
    }
    if (frame_.empty()) return false;   // nothing before the first pulse counts
    if (kind == kLircSpace) {
        space_us_ += us;
        return space_us_ >= idle_us_ ? finish() : false;
    }
    if (kind == kLircTimeout) return finish();
    if (kind == kLircOverflow) {
        // The driver lost samples: the frame is incomplete
        frame_.clear();
        space_us_ = 0;
    }
    return false;
}

void LircFramer::append(uint32_t us) {
    if (frame_.size() > kRmtMaxDurations) return;   // too long already; the queue drops it
    frame_.push_back(static_cast<uint16_t>(us > 0xffff ? 0xffff : us));
}

bool LircFramer::finish() {
    space_us_ = 0;
    ended_ = true;
    return true;
}

size_t lirc_pulse_frame(const uint16_t* durations, size_t count, uint32_t* out, uint32_t& trailing_us) {
    const size_t n = count > 0 && count % 2 == 0 ? count - 1 : count;
    trailing_us = n < count ? durations[count - 1] : 0;
    for (size_t i = 0; i < n; ++i) out[i] = durations[i];
    return n;
}

std::string lirc_device_node(const std::string& device) {
    struct stat st;
    if (::stat(device.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return device;
    // An rc device lists its lircN itself; a kernel device below rc/rcN
    std::string name = lirc_entry(device);
    if (name.empty()) {
        if (DIR* d = ::opendir((device + "/rc").c_str())) {
            while (const dirent* e = ::readdir(d)) {
                if (std::strncmp(e->d_name, "rc", 2) != 0) continue;
                name = lirc_entry(device + "/rc/" + e->d_name);
                if (!name.empty()) break;
            }
            ::closedir(d);
        }
    }
    return name.empty() ? std::string() : "/dev/" + name;
}

LinuxIrTransmitter::LinuxIrTransmitter(std::string device, int fd, uint32_t features)
    : device_(std::move(device)), fd_(fd), features_(features) {}

LinuxIrTransmitter::~LinuxIrTransmitter() {
    if (fd_ >= 0) ::close(fd_);
}

bool LinuxIrTransmitter::configure(const RmtAssignment& a, std::string& err) {
    carrier_hz_ = a.carrier_hz;
    duty_pct_ = a.duty_pct;
    return apply(err);
}

bool LinuxIrTransmitter::apply(std::string& err) {
#if defined(__linux__)
    uint32_t v = carrier_hz_;
    if ((features_ & LIRC_CAN_SET_SEND_CARRIER) && ::ioctl(fd_, LIRC_SET_SEND_CARRIER, &v) < 0) {
        err = "cannot set the carrier of " + device_ + " to " + std::to_string(carrier_hz_) + " Hz: " + std::strerror(errno);
        return false;
    }
    v = duty_pct_;
    if ((features_ & LIRC_CAN_SET_SEND_DUTY_CYCLE) && ::ioctl(fd_, LIRC_SET_SEND_DUTY_CYCLE, &v) < 0) {
        err = "cannot set the duty cycle of " + device_ + " to " + std::to_string(duty_pct_) + "%: " + std::strerror(errno);
        return false;
    }
    return true;
#else
    (void)features_;
    err = "LIRC needs a Linux host";
    return false;
#endif
}

int32_t LinuxIrTransmitter::send(const uint16_t* durations, size_t count) {
    if (count == 0 || count > kRmtMaxDurations) return HAL_ERR_INVAL;
#if defined(__linux__)
    uint32_t frame[kRmtMaxDurations];
    uint32_t trailing_us = 0;
    const size_t n = lirc_pulse_frame(durations, count, frame, trailing_us);
    // Another program on the device may have set its own
    std::string err;
    if (!apply(err)) return HAL_ERR_IO;
    // The write returns once the driver has sent the frame
    ssize_t w;
    do {
        w = ::write(fd_, frame, n * sizeof(uint32_t));
    } while (w < 0 && errno == EINTR);
    if (w < 0) return errno == EINVAL ? HAL_ERR_INVAL : HAL_ERR_IO;
    if (static_cast<size_t>(w) != n * sizeof(uint32_t)) return HAL_ERR_IO;
    if (trailing_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(trailing_us));
    return static_cast<int32_t>(count);
#else
    (void)durations;
    return HAL_ERR_NODEV;
#endif
}

LinuxIrReceiver::LinuxIrReceiver(std::string device, int fd, uint32_t features)
    : device_(std::move(device)), fd_(fd), features_(features) {}

LinuxIrReceiver::~LinuxIrReceiver() {
    stop();
    if (fd_ >= 0) ::close(fd_);
}

bool LinuxIrReceiver::start(const RmtAssignment& a, Sink sink, std::string& err) {
#if defined(__linux__)
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable()) {
        err = device_ + " is in use";
        return false;
    }
    if (::pipe(wake_) != 0) {
        err = std::string("cannot start the receiver thread: ") + std::strerror(errno);
        return false;
    }
    for (int fd : wake_) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    // Frames end idle_us after their last pulse: the driver's timeout says so
    timeout_set_ = false;
    if (features_ & LIRC_CAN_SET_REC_TIMEOUT) {
        uint32_t want = a.idle_us, lo = 0, hi = 0;
        if (::ioctl(fd_, LIRC_GET_MIN_TIMEOUT, &lo) == 0 && want < lo) want = lo;
        if (::ioctl(fd_, LIRC_GET_MAX_TIMEOUT, &hi) == 0 && hi > 0 && want > hi) want = hi;
        uint32_t before = 0;
        const bool known = ::ioctl(fd_, LIRC_GET_REC_TIMEOUT, &before) == 0;
        if (::ioctl(fd_, LIRC_SET_REC_TIMEOUT, &want) == 0 && known && before != want) {
            saved_timeout_ = before;
            timeout_set_ = true;
        }
    }
    uint32_t on = 1;
    ::ioctl(fd_, LIRC_SET_REC_TIMEOUT_REPORTS, &on);
    // Samples from before the claim are no frame of the pod's
    uint32_t stale[64];
    while (::read(fd_, stale, sizeof(stale)) > 0) {}
    sink_ = std::move(sink);
    const uint32_t idle_us = a.idle_us;
    thread_ = std::thread([this, idle_us] { run(idle_us); });
    return true;
#else
    (void)a;
    (void)sink;
    (void)features_;
    (void)saved_timeout_;
    err = "LIRC needs a Linux host";
    return false;
#endif
}

void LinuxIrReceiver::run(uint32_t idle_us) {
    LircFramer framer(idle_us);
    uint32_t samples[128];
    for (;;) {
        pollfd fds[2] = {{fd_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents != 0) break;
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            printf("[BOARD] IR receiver %s: the device went away; no more frames\n", device_.c_str());
            break;
        }
        const ssize_t n = ::read(fd_, samples, sizeof(samples));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            printf("[BOARD] IR receiver %s: %s; no more frames\n", device_.c_str(), std::strerror(errno));
            break;
        }
        for (ssize_t i = 0; i < n / static_cast<ssize_t>(sizeof(uint32_t)); ++i) {
            if (framer.feed(samples[i])) sink_(framer.frame().data(), framer.frame().size());
        }
    }
}

void LinuxIrReceiver::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!thread_.joinable()) return;
    const char byte = 1;
    (void)!::write(wake_[1], &byte, 1);
    thread_.join();
    for (int& fd : wake_) {
        ::close(fd);
        fd = -1;
    }
    sink_ = nullptr;
#if defined(__linux__)
    if (timeout_set_) ::ioctl(fd_, LIRC_SET_REC_TIMEOUT, &saved_timeout_);
#endif
    timeout_set_ = false;
}

bool LinuxBoard::open_ir(RmtDir dir, const std::string& spec) {
    const bool tx = dir == RmtDir::Tx;
    const char* what = tx ? "IR transmitter" : "IR receiver";
    const char* channel = tx ? "RMT TX" : "RMT RX";
    const std::string device = lirc_device_node(spec);
    if (device.empty()) {
        printf("[BOARD] %s %s: the kernel lists no LIRC device for it; the node offers no %s\n", what, spec.c_str(), channel);
        return false;
    }
    const std::string name = device == spec ? device : device + " (" + spec + ")";
#if defined(__linux__)
    const int fd = ::open(device.c_str(), tx ? O_WRONLY | O_CLOEXEC : O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        printf("[BOARD] %s %s: cannot open it (%s); the node offers no %s\n", what, name.c_str(), std::strerror(errno), channel);
        return false;
    }
    uint32_t features = 0;
    uint32_t mode = tx ? LIRC_MODE_PULSE : LIRC_MODE_MODE2;
    const uint32_t needed = tx ? LIRC_CAN_SEND_PULSE : LIRC_CAN_REC_MODE2;
    if (::ioctl(fd, LIRC_GET_FEATURES, &features) < 0 || (features & needed) == 0 ||
        ::ioctl(fd, tx ? LIRC_SET_SEND_MODE : LIRC_SET_REC_MODE, &mode) < 0) {
        printf("[BOARD] %s %s: not a LIRC %s of raw pulses; the node offers no %s\n", what, name.c_str(), tx ? "transmitter" : "receiver",
               channel);
        ::close(fd);
        return false;
    }
    if (tx) {
        ir_tx_ = std::make_unique<LinuxIrTransmitter>(device, fd, features);
        printf("[BOARD] %s %s%s%s\n", what, name.c_str(), (features & LIRC_CAN_SET_SEND_CARRIER) ? "" : ", carrier set by the driver",
               (features & LIRC_CAN_SET_SEND_DUTY_CYCLE) ? "" : ", duty cycle set by the driver");
    } else {
        ir_rx_ = std::make_unique<LinuxIrReceiver>(device, fd, features);
        uint32_t timeout = 0;
        if ((features & LIRC_CAN_SET_REC_TIMEOUT) == 0 && ::ioctl(fd, LIRC_GET_REC_TIMEOUT, &timeout) == 0) {
            printf("[BOARD] %s %s: frames also end at the driver's timeout, %u us\n", what, name.c_str(), timeout);
        } else {
            printf("[BOARD] %s %s\n", what, name.c_str());
        }
    }
    fflush(stdout);
    return true;
#else
    printf("[BOARD] %s %s: LIRC needs a Linux host; the node offers no %s\n", what, name.c_str(), channel);
    return false;
#endif
}

}  // namespace kube32::hal
