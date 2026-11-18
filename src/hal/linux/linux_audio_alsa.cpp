// A sound card through the kernel's ALSA devices, with no sound library:
// /dev/snd/controlC<card> for the mixer controls and pcmC<card>D<device>p/c
// for the streams, set up with HW_REFINE/HW_PARAMS/SW_PARAMS and moved with
// WRITEI/READI_FRAMES. A PCM is opened non-blocking so a device another
// process holds answers -EBUSY at once instead of waiting.

#include "hal/linux_audio.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <iostream>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <sound/asound.h>

namespace kube32::hal::linux_audio {

namespace {

constexpr uint32_t kPeriodsPlayback = 4;   // 40 ms of 10 ms periods
constexpr uint32_t kPeriodsCapture = 8;    // room for a late read

// glibc takes the request as an unsigned long, musl as an int
#if defined(__GLIBC__)
using IoctlRequest = unsigned long;
#else
using IoctlRequest = int;
#endif

int xioctl(int fd, unsigned long request, void* arg = nullptr) {
    return ::ioctl(fd, static_cast<IoctlRequest>(request), arg);
}

snd_mask* mask(snd_pcm_hw_params& p, int param) {
    return &p.masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

snd_interval* interval(snd_pcm_hw_params& p, int param) {
    return &p.intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

// Every configuration the driver may take
void any_params(snd_pcm_hw_params& p) {
    std::memset(&p, 0, sizeof p);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; ++n)
        std::memset(mask(p, n)->bits, 0xff, sizeof mask(p, n)->bits);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; ++n) {
        interval(p, n)->min = 0;
        interval(p, n)->max = UINT_MAX;
    }
    p.rmask = ~0u;
    p.info = ~0u;
}

bool mask_has(snd_pcm_hw_params& p, int param, unsigned bit) {
    return (mask(p, param)->bits[bit >> 5] & (1u << (bit & 31))) != 0;
}

void mask_only(snd_pcm_hw_params& p, int param, unsigned bit) {
    std::memset(mask(p, param)->bits, 0, sizeof mask(p, param)->bits);
    mask(p, param)->bits[bit >> 5] = 1u << (bit & 31);
}

void interval_range(snd_pcm_hw_params& p, int param, unsigned min, unsigned max) {
    snd_interval* i = interval(p, param);
    std::memset(i, 0, sizeof *i);
    i->min = min;
    i->max = max;
    i->integer = 1;
}

bool refine(int fd, snd_pcm_hw_params& p) {
    p.rmask = ~0u;
    return xioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &p) == 0;
}

// p narrowed to param in [min, max], if the driver takes that
bool narrow(int fd, snd_pcm_hw_params& p, int param, unsigned min, unsigned max) {
    snd_pcm_hw_params q = p;
    interval_range(q, param, min, max);
    if (!refine(fd, q)) return false;
    p = q;
    return true;
}

class AlsaPcm final : public PcmStream {
public:
    AlsaPcm(int fd, bool capture, PcmFormat fmt) : fd_(fd), capture_(capture), fmt_(fmt) {
        frame_bytes_ = sample_bytes(fmt.format) * fmt.channels;
    }
    ~AlsaPcm() override {
        xioctl(fd_, SNDRV_PCM_IOCTL_DROP);
        ::close(fd_);
    }

    const PcmFormat& format() const override { return fmt_; }

    int32_t write(const uint8_t* frames, uint32_t n, int timeout_ms) override {
        return transfer(const_cast<uint8_t*>(frames), n, timeout_ms);
    }
    int32_t read(uint8_t* frames, uint32_t n, int timeout_ms) override {
        return transfer(frames, n, timeout_ms);
    }
    void start() override {
        xioctl(fd_, SNDRV_PCM_IOCTL_START);   // EBADFD when it runs already
    }

private:
    int32_t transfer(uint8_t* frames, uint32_t n, int timeout_ms) {
        uint32_t done = 0;
        timespec t0{};
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (done < n) {
            snd_xferi x{};
            x.buf = frames + static_cast<size_t>(done) * frame_bytes_;
            x.frames = n - done;
            if (xioctl(fd_, capture_ ? SNDRV_PCM_IOCTL_READI_FRAMES : SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) == 0) {
                done += static_cast<uint32_t>(x.result);
                continue;
            }
            const int e = errno;
            if (e == EINTR) continue;
            if (e == EPIPE || e == ESTRPIPE) {   // an underrun or overrun, or a suspend
                if (!recover(e)) return -e;
                continue;
            }
            if (e != EAGAIN) return done ? static_cast<int32_t>(done) : -e;
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            const long spent = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
            if (spent >= timeout_ms) return done ? static_cast<int32_t>(done) : -ETIMEDOUT;
            pollfd pfd{fd_, static_cast<short>(capture_ ? POLLIN : POLLOUT), 0};
            poll(&pfd, 1, static_cast<int>(timeout_ms - spent));
        }
        return static_cast<int32_t>(done);
    }

    bool recover(int e) {
        if (e == ESTRPIPE) {
            for (int i = 0; i < 100 && xioctl(fd_, SNDRV_PCM_IOCTL_RESUME) < 0 && errno == EAGAIN; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (xioctl(fd_, SNDRV_PCM_IOCTL_PREPARE) < 0) return false;
        if (capture_) xioctl(fd_, SNDRV_PCM_IOCTL_START);
        return true;
    }

    int fd_;
    bool capture_;
    PcmFormat fmt_;
    uint32_t frame_bytes_ = 0;
};

// A stream on fd in the first format, channel count and rate the driver
// takes, with periods of about 10 ms
std::unique_ptr<PcmStream> setup(int fd, bool capture, uint32_t rate_pref, int& err) {
    snd_pcm_hw_params p;
    any_params(p);
    if (!refine(fd, p)) {
        err = -errno;
        return nullptr;
    }
    if (!mask_has(p, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED)) {
        err = -EINVAL;
        return nullptr;
    }
    mask_only(p, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);

    PcmFormat fmt;
    static const struct { unsigned alsa; SampleFormat ours; } kFormats[] = {
        {SNDRV_PCM_FORMAT_S16_LE, SampleFormat::S16_LE},
        {SNDRV_PCM_FORMAT_S32_LE, SampleFormat::S32_LE},
        {SNDRV_PCM_FORMAT_S24_LE, SampleFormat::S24_LE},
    };
    bool have_format = false;
    for (const auto& f : kFormats) {
        if (!mask_has(p, SNDRV_PCM_HW_PARAM_FORMAT, f.alsa)) continue;
        snd_pcm_hw_params q = p;
        mask_only(q, SNDRV_PCM_HW_PARAM_FORMAT, f.alsa);
        if (!refine(fd, q)) continue;
        p = q;
        fmt.format = f.ours;
        have_format = true;
        break;
    }
    if (!have_format) {
        err = -EINVAL;
        return nullptr;
    }
    mask_only(p, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);

    fmt.channels = 0;
    for (unsigned c : {1u, 2u}) {
        if (narrow(fd, p, SNDRV_PCM_HW_PARAM_CHANNELS, c, c)) {
            fmt.channels = c;
            break;
        }
    }
    if (!fmt.channels) {
        fmt.channels = interval(p, SNDRV_PCM_HW_PARAM_CHANNELS)->min;
        if (!fmt.channels || !narrow(fd, p, SNDRV_PCM_HW_PARAM_CHANNELS, fmt.channels, fmt.channels)) {
            err = -EINVAL;
            return nullptr;
        }
    }

    // One rate for both directions keeps a codec with shared clocks happy
    fmt.rate = 0;
    for (unsigned r : {rate_pref, 48000u, 44100u, 32000u, 16000u, 96000u, 8000u}) {
        if (r && narrow(fd, p, SNDRV_PCM_HW_PARAM_RATE, r, r)) {
            fmt.rate = r;
            break;
        }
    }
    if (!fmt.rate) {
        err = -EINVAL;
        return nullptr;
    }

    // The nearest period and count at or above what is asked for
    const unsigned want_period = fmt.rate / 100;
    if (narrow(fd, p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, want_period, UINT_MAX)) {
        const unsigned period = interval(p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
        narrow(fd, p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period, period);
    }
    const unsigned want_periods = capture ? kPeriodsCapture : kPeriodsPlayback;
    if (narrow(fd, p, SNDRV_PCM_HW_PARAM_PERIODS, want_periods, UINT_MAX)) {
        const unsigned periods = interval(p, SNDRV_PCM_HW_PARAM_PERIODS)->min;
        narrow(fd, p, SNDRV_PCM_HW_PARAM_PERIODS, periods, periods);
    }
    p.rmask = ~0u;
    if (xioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p) < 0) {
        err = -errno;
        return nullptr;
    }
    fmt.period_frames = interval(p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
    fmt.buffer_frames = interval(p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min;

    snd_pcm_sw_params sw{};
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = fmt.period_frames;
    sw.start_threshold = capture ? 1 : fmt.period_frames;
    sw.stop_threshold = fmt.buffer_frames;
    sw.proto = SNDRV_PCM_VERSION;
    if (xioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
        err = -errno;
        return nullptr;
    }
    if (!capture) {
        // The driver zeroes what has played: an underrun plays silence,
        // never the buffer's old contents (sw.boundary came back above)
        sw.silence_threshold = 0;
        sw.silence_size = sw.boundary;
        xioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw);
    }
    if (xioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        err = -errno;
        return nullptr;
    }
    if (capture && xioctl(fd, SNDRV_PCM_IOCTL_START) < 0) {
        err = -errno;
        return nullptr;
    }
    return std::make_unique<AlsaPcm>(fd, capture, fmt);
}

// The card's mixer: controls found by name, set from text, put back at the end
class Mixer {
public:
    explicit Mixer(int fd) : fd_(fd) {}
    ~Mixer() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) xioctl(fd_, SNDRV_CTL_IOCTL_ELEM_WRITE, &*it);
        ::close(fd_);
    }

    bool set(const std::string& name, const std::string& text, std::string& why) {
        snd_ctl_elem_info info{};
        if (!find(name, info)) {
            why = "no such control";
            return false;
        }
        if (!(info.access & SNDRV_CTL_ELEM_ACCESS_WRITE)) {
            why = "read-only";
            return false;
        }
        ControlInfo ci;
        ci.count = info.count;
        switch (info.type) {
        case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
            ci.type = ControlInfo::Type::Boolean;
            ci.max = 1;
            break;
        case SNDRV_CTL_ELEM_TYPE_INTEGER:
            ci.type = ControlInfo::Type::Integer;
            ci.min = info.value.integer.min;
            ci.max = info.value.integer.max;
            break;
        case SNDRV_CTL_ELEM_TYPE_INTEGER64:
            ci.type = ControlInfo::Type::Integer64;
            ci.min = info.value.integer64.min;
            ci.max = info.value.integer64.max;
            break;
        case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
            ci.type = ControlInfo::Type::Enumerated;
            for (unsigned i = 0; i < info.value.enumerated.items; ++i) {
                snd_ctl_elem_info item = info;
                item.value.enumerated.item = i;
                if (xioctl(fd_, SNDRV_CTL_IOCTL_ELEM_INFO, &item) < 0) {
                    why = std::strerror(errno);
                    return false;
                }
                ci.items.emplace_back(item.value.enumerated.name);
            }
            break;
        default:
            why = "a type this does not set";
            return false;
        }
        if (ci.count > 64) {
            why = "too many values";
            return false;
        }
        std::vector<int64_t> values;
        if (!parse_control_value(ci, text, values, why)) return false;

        snd_ctl_elem_value now{};
        now.id = info.id;
        if (xioctl(fd_, SNDRV_CTL_IOCTL_ELEM_READ, &now) < 0) {
            why = std::strerror(errno);
            return false;
        }
        snd_ctl_elem_value next = now;
        for (uint32_t i = 0; i < ci.count; ++i) {
            if (ci.type == ControlInfo::Type::Enumerated) next.value.enumerated.item[i] = static_cast<unsigned>(values[i]);
            else if (ci.type == ControlInfo::Type::Integer64) next.value.integer64.value[i] = values[i];
            else next.value.integer.value[i] = static_cast<long>(values[i]);
        }
        if (std::memcmp(&next.value, &now.value, sizeof now.value) == 0) return true;
        if (xioctl(fd_, SNDRV_CTL_IOCTL_ELEM_WRITE, &next) < 0) {
            why = std::strerror(errno);
            return false;
        }
        saved_.push_back(now);
        return true;
    }

private:
    bool find(const std::string& name, snd_ctl_elem_info& info) {
        snd_ctl_elem_list list{};
        if (xioctl(fd_, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) return false;
        std::vector<snd_ctl_elem_id> ids(list.count);
        list.space = list.count;
        list.pids = ids.data();
        if (xioctl(fd_, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) return false;
        for (uint32_t i = 0; i < list.used; ++i) {
            const snd_ctl_elem_id& id = ids[i];
            if (id.iface != SNDRV_CTL_ELEM_IFACE_MIXER && id.iface != SNDRV_CTL_ELEM_IFACE_CARD) continue;
            if (std::strncmp(reinterpret_cast<const char*>(id.name), name.c_str(), sizeof id.name) != 0) continue;
            info = snd_ctl_elem_info{};
            info.id.numid = id.numid;
            return xioctl(fd_, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0;
        }
        return false;
    }

    int fd_;
    std::vector<snd_ctl_elem_value> saved_;
};

class AlsaCard final : public AudioCard {
public:
    AlsaCard(int card, const LinuxAudioConfig& cfg, int ctl_fd) : card_(card), cfg_(cfg), mixer_(ctl_fd) {
        playback_ = cfg.playback_device >= 0 && access(path(false).c_str(), F_OK) == 0;
        capture_ = cfg.capture_device >= 0 && access(path(true).c_str(), F_OK) == 0;
    }

    void set_controls() {
        for (const auto& [name, value] : cfg_.controls) {
            std::string why;
            if (!mixer_.set(name, value, why))
                std::cout << "[AUDIO] card " << card_ << ": control '" << name << "' = " << value << ": " << why << std::endl;
        }
    }

    bool has_playback() const override { return playback_; }
    bool has_capture() const override { return capture_; }

    std::unique_ptr<PcmStream> open(bool capture, int& err) override {
        const int fd = ::open(path(capture).c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            err = -errno;
            return nullptr;
        }
        std::unique_ptr<PcmStream> s = setup(fd, capture, cfg_.rate, err);
        if (!s) {
            ::close(fd);
            std::cout << "[AUDIO] " << path(capture) << ": no usable configuration (" << std::strerror(-err) << ")" << std::endl;
        }
        return s;
    }

private:
    std::string path(bool capture) const {
        char p[48];
        std::snprintf(p, sizeof p, "/dev/snd/pcmC%dD%d%c", card_, capture ? cfg_.capture_device : cfg_.playback_device, capture ? 'c' : 'p');
        return p;
    }

    int card_;
    LinuxAudioConfig cfg_;
    Mixer mixer_;
    bool playback_ = false, capture_ = false;
};

int open_control(int card) {
    char p[32];
    std::snprintf(p, sizeof p, "/dev/snd/controlC%d", card);
    return ::open(p, O_RDWR | O_CLOEXEC);
}

}  // namespace

std::unique_ptr<AudioCard> open_alsa_card(const LinuxAudioConfig& cfg, std::string& why) {
    int card = cfg.card;
    int fd = -1;
    if (card >= 0) {
        fd = open_control(card);
        if (fd < 0) {
            why = "/dev/snd/controlC" + std::to_string(card) + ": " + std::strerror(errno);
            return nullptr;
        }
    } else {
        for (int c = 0; c < 32 && fd < 0; ++c) {
            const int f = open_control(c);
            if (f < 0) continue;
            snd_ctl_card_info info{};
            if (xioctl(f, SNDRV_CTL_IOCTL_CARD_INFO, &info) == 0 &&
                (cfg.card_name == reinterpret_cast<const char*>(info.id) || cfg.card_name == reinterpret_cast<const char*>(info.name))) {
                card = c;
                fd = f;
            } else {
                ::close(f);
            }
        }
        if (fd < 0) {
            why = "no sound card called '" + cfg.card_name + "'";
            return nullptr;
        }
    }
    auto c = std::make_unique<AlsaCard>(card, cfg, fd);
    c->set_controls();
    return c;
}

}  // namespace kube32::hal::linux_audio
