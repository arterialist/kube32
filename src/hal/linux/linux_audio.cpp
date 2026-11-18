// The Linux board's speaker and microphone with the Cardputer's semantics
// (src/hal/tft_keyboard_hardware.cpp), on any AudioCard: the ALSA card on a
// Linux host (linux_audio_alsa.cpp), a simulated one in the tests.

#include "hal/linux_audio.hpp"

#include "hal/hal_types.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace kube32::hal {

// The codec's registers return to their power-up values when it powers up,
// and a control written while it is off does not reach it: the levels here
// are those values, and the microphone's gain is applied in software.
LinuxAudioConfig cardputer_zero_audio() {
    LinuxAudioConfig c;
    c.card_name = "ES8389Audio";
    c.controls = {
        {"Speaker Switch", "on"},                    // the amplifier, fed from the left output
        // 0 dB: a sample's full scale is the DAC's, so the ceiling holds
        {"DACL Playback Volume", "191"},
        {"DACR Playback Volume", "191"},
        // The analog microphone on the left ADC (the machine driver picks
        // the PGA input from the headphone jack)
        {"Mic Jack Switch", "on"},
        {"ADC MUX", "AMIC"},
        {"ADCL PGA Volume", "8"},                    // +24 dB
        {"ADCL Capture Volume", "191"},              // 0 dB
        {"ALC Capture Switch", "ALC OFF"},           // a fixed gain, as on the Cardputer
        // No loopback: the microphone does not hear the DAC, the speaker
        // does not play the microphone
        {"ADC Mixer DACL ADCL Mixer", "off"},
        {"ADC Mixer DACR ADCR Mixer", "off"},
        {"IF ADCDACL Mixer ADCL DACL Mixer", "off"},
        {"IF ADCDACR Mixer ADCR DACR Mixer", "off"},
    };
    c.capture_channel = 0;
    // At +24 dB the codec's samples sit where the Cardputer's PDM decimator's
    // do (a quiet room at about 15 rms): the Cardputer's gain applies as is
    c.mic_unity_gain = 1;
    // The output sounds 140 to 200 ms after a stream starts
    c.speaker_lead_in_ms = 200;
    c.speaker_hold_ms = 1000;
    return c;
}

namespace linux_audio {

int32_t speaker_gain_q15(uint32_t volume_pct) {
    if (volume_pct > 100) volume_pct = 100;
    return static_cast<int32_t>((32768u * kSpeakerMaxPct * volume_pct) / 10000u);
}

std::vector<int16_t> square_tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct) {
    std::vector<int16_t> pcm;
    if (freq_hz == 0 || duration_ms == 0 || volume_pct == 0) return pcm;
    if (duration_ms > 2000) duration_ms = 2000;
    if (volume_pct > 100) volume_pct = 100;
    int period = static_cast<int>(kToneRate / freq_hz);
    if (period < 2) period = 2;
    const int total = static_cast<int>((kToneRate * duration_ms) / 1000);
    const int amplitude = static_cast<int>((32767u * kSpeakerMaxPct * volume_pct) / 10000u);
    int ramp = total / 8 > 64 ? 64 : total / 8;
    if (ramp < 1) ramp = 1;
    pcm.resize(static_cast<size_t>(total));
    for (int i = 0, phase = 0; i < total; ++i) {
        int a = amplitude;
        if (i < ramp) a = amplitude * i / ramp;
        else if (total - i < ramp) a = amplitude * (total - i) / ramp;
        pcm[static_cast<size_t>(i)] = static_cast<int16_t>(phase < period / 2 ? a : -a);
        phase = (phase + 1) % period;
    }
    return pcm;
}

namespace {
constexpr int kPhases = 256;
constexpr int kZeroCrossings = 8;   // on each side of the kernel's centre
constexpr double kPi = 3.14159265358979323846;
}  // namespace

void Resampler::reset(uint32_t in_rate, uint32_t out_rate) {
    in_rate_ = in_rate;
    out_rate_ = out_rate;
    half_ = 0;
    table_.clear();
    buf_.clear();
    pos_ = 0;
    frac_ = 0;
    if (in_rate == out_rate || in_rate == 0 || out_rate == 0) return;
    // The cutoff in cycles per input sample, under the lower rate's Nyquist
    // frequency: images of an upsampled sound and aliases of a downsampled
    // one stay out
    const double fc = 0.45 * std::min(in_rate, out_rate) / in_rate;
    half_ = static_cast<int>(std::ceil(kZeroCrossings / (2.0 * fc)));
    const size_t taps = 2 * static_cast<size_t>(half_);
    table_.assign((kPhases + 1) * taps, 0.0f);
    for (int q = 0; q <= kPhases; ++q) {
        float* row = &table_[static_cast<size_t>(q) * taps];
        double sum = 0;
        for (size_t m = 0; m < taps; ++m) {
            const double t = static_cast<double>(static_cast<int>(m) - half_ + 1) - static_cast<double>(q) / kPhases;
            const double x = 2.0 * fc * t;
            const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double w = t / half_;
            const double blackman = std::fabs(w) >= 1.0 ? 0.0 : 0.42 + 0.5 * std::cos(kPi * w) + 0.08 * std::cos(2 * kPi * w);
            row[m] = static_cast<float>(sinc * blackman);
            sum += row[m];
        }
        for (size_t m = 0; m < taps; ++m) row[m] = static_cast<float>(row[m] / sum);   // unity gain at DC in every phase
    }
    buf_.assign(static_cast<size_t>(half_ - 1), 0.0f);   // silence before the first sample
    pos_ = static_cast<size_t>(half_ - 1);
}

float Resampler::tap_sum(size_t at, const float* row) const {
    const float* x = &buf_[at];
    float y = 0;
    for (int m = 0; m < 2 * half_; ++m) y += x[m] * row[m];
    return y;
}

void Resampler::process(const float* in, size_t n, std::vector<float>& out) {
    if (half_ == 0) {
        out.insert(out.end(), in, in + n);
        return;
    }
    buf_.insert(buf_.end(), in, in + n);
    const size_t taps = 2 * static_cast<size_t>(half_);
    const size_t back = static_cast<size_t>(half_ - 1);
    while (pos_ + static_cast<size_t>(half_) < buf_.size()) {
        const uint64_t scaled = frac_ * kPhases;
        const size_t q = static_cast<size_t>(scaled / out_rate_);
        const float a = static_cast<float>(scaled % out_rate_) / static_cast<float>(out_rate_);
        const float* row = &table_[q * taps];
        float y = tap_sum(pos_ - back, row);
        if (a > 0) y += a * (tap_sum(pos_ - back, row + taps) - y);
        out.push_back(y);
        frac_ += in_rate_;
        pos_ += static_cast<size_t>(frac_ / out_rate_);
        frac_ %= out_rate_;
    }
    const size_t drop = std::min(pos_ - back, buf_.size());
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop));
    pos_ -= drop;
}

uint32_t sample_bytes(SampleFormat format) {
    return format == SampleFormat::S16_LE ? 2 : 4;
}

void mono_to_frames(const float* in, size_t n, int32_t limit, SampleFormat format, uint32_t channels, uint8_t* out) {
    const float lim = static_cast<float>(limit);
    const uint32_t bytes = sample_bytes(format);
    for (size_t i = 0; i < n; ++i) {
        const float x = in[i] > lim ? lim : in[i] < -lim ? -lim : in[i];
        int32_t v = static_cast<int32_t>(std::lrint(x));
        if (format == SampleFormat::S32_LE) v *= 65536;
        else if (format == SampleFormat::S24_LE) v *= 256;
        const uint32_t u = static_cast<uint32_t>(v);
        const uint8_t s[4] = {static_cast<uint8_t>(u), static_cast<uint8_t>(u >> 8), static_cast<uint8_t>(u >> 16), static_cast<uint8_t>(u >> 24)};
        for (uint32_t c = 0; c < channels; ++c, out += bytes) std::memcpy(out, s, bytes);
    }
}

void frames_to_mono(const uint8_t* in, size_t n, SampleFormat format, uint32_t channels, int channel, float* out) {
    const uint32_t bytes = sample_bytes(format);
    auto sample = [&](const uint8_t* p) -> float {
        if (format == SampleFormat::S16_LE) return static_cast<float>(static_cast<int16_t>(p[0] | (p[1] << 8)));
        const uint32_t u = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
        if (format == SampleFormat::S32_LE) return static_cast<float>(static_cast<int32_t>(u)) / 65536.0f;
        int32_t v = static_cast<int32_t>(u & 0xffffffu);
        if (v & 0x800000) v -= 0x1000000;
        return static_cast<float>(v) / 256.0f;
    };
    const bool one = channel >= 0 && static_cast<uint32_t>(channel) < channels;
    for (size_t i = 0; i < n; ++i, in += bytes * channels) {
        if (one) {
            out[i] = sample(in + bytes * static_cast<uint32_t>(channel));
            continue;
        }
        float sum = 0;
        for (uint32_t c = 0; c < channels; ++c) sum += sample(in + bytes * c);
        out[i] = channels ? sum / static_cast<float>(channels) : 0.0f;
    }
}

// The constants of the Cardputer's mic_condition
static constexpr int32_t kMicDcShift = 10;       // offset tracking: 1/1024 of each sample
static constexpr int32_t kMicSettleShift = 6;    // while settling: 1/64

void MicConditioner::process(const float* in, size_t n, uint32_t unity_gain, int16_t* out) {
    const int64_t gain = gain_, unity = unity_gain ? unity_gain : 1;
    int32_t dc = dc_q8_;
    for (size_t i = 0; i < n; ++i) {
        const float f = in[i] > 1e6f ? 1e6f : in[i] < -1e6f ? -1e6f : in[i];
        const int32_t x = static_cast<int32_t>(std::lrint(f));
        if (settle_) {
            if (settle_-- == kMicSettleSamples) dc = x * 256;
            dc += (x * 256 - dc) >> kMicSettleShift;
            out[i] = 0;
            continue;
        }
        dc += (x * 256 - dc) >> kMicDcShift;
        const int64_t y = static_cast<int64_t>(x - (dc >> 8)) * gain / unity;
        out[i] = static_cast<int16_t>(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
    dc_q8_ = dc;
}

void MicRing::push(const int16_t* samples, size_t n) {
    if (n >= kMicBufferSamples) {
        samples += n - kMicBufferSamples;
        n = kMicBufferSamples;
        head_ = size_ = 0;
    }
    for (size_t i = 0; i < n; ++i) {
        if (size_ == kMicBufferSamples) {   // the oldest goes
            head_ = (head_ + 1) % kMicBufferSamples;
            --size_;
        }
        buf_[(head_ + size_) % kMicBufferSamples] = samples[i];
        ++size_;
    }
}

size_t MicRing::pop(int16_t* out, size_t max) {
    const size_t n = std::min(max, size_);
    for (size_t i = 0; i < n; ++i) out[i] = buf_[(head_ + i) % kMicBufferSamples];
    head_ = (head_ + n) % kMicBufferSamples;
    size_ -= n;
    return n;
}

static std::string trimmed(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static std::string lowered(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool whole_number(const std::string& s, int64_t& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long long n = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end != s.c_str() + s.size()) return false;
    v = n;
    return true;
}

static bool one_value(const ControlInfo& info, const std::string& text, int64_t& v, std::string& why) {
    switch (info.type) {
    case ControlInfo::Type::Boolean: {
        const std::string t = lowered(text);
        if (t == "on" || t == "yes" || t == "true" || t == "1" || t == "unmute") v = 1;
        else if (t == "off" || t == "no" || t == "false" || t == "0" || t == "mute") v = 0;
        else {
            why = "'" + text + "' is not on or off";
            return false;
        }
        return true;
    }
    case ControlInfo::Type::Integer:
    case ControlInfo::Type::Integer64: {
        if (!text.empty() && text.back() == '%') {
            int64_t pct = 0;
            if (!whole_number(trimmed(text.substr(0, text.size() - 1)), pct) || pct < 0 || pct > 100) {
                why = "'" + text + "' is not a percentage";
                return false;
            }
            v = info.min + ((info.max - info.min) * pct + 50) / 100;
            return true;
        }
        if (!whole_number(text, v)) {
            why = "'" + text + "' is not a number";
            return false;
        }
        if (v < info.min || v > info.max) {
            why = text + " is outside " + std::to_string(info.min) + ".." + std::to_string(info.max);
            return false;
        }
        return true;
    }
    case ControlInfo::Type::Enumerated: {
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t i = 0; i < info.items.size(); ++i) {
                if (pass == 0 ? info.items[i] == text : lowered(info.items[i]) == lowered(text)) {
                    v = static_cast<int64_t>(i);
                    return true;
                }
            }
        }
        if (whole_number(text, v) && v >= 0 && v < static_cast<int64_t>(info.items.size())) return true;
        why = "'" + text + "' is not one of its items";
        return false;
    }
    }
    return false;
}

bool parse_control_value(const ControlInfo& info, const std::string& text, std::vector<int64_t>& values, std::string& why) {
    std::vector<std::string> parts;
    for (size_t at = 0;;) {
        const size_t comma = text.find(',', at);
        parts.push_back(trimmed(text.substr(at, comma == std::string::npos ? std::string::npos : comma - at)));
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    if (parts.size() != 1 && parts.size() != info.count) {
        why = std::to_string(parts.size()) + " values for " + std::to_string(info.count) + " channels";
        return false;
    }
    values.clear();
    for (uint32_t c = 0; c < info.count; ++c) {
        int64_t v = 0;
        if (!one_value(info, parts.size() == 1 ? parts[0] : parts[c], v, why)) return false;
        values.push_back(v);
    }
    return true;
}

#if !defined(__linux__)
std::unique_ptr<AudioCard> open_alsa_card(const LinuxAudioConfig&, std::string& why) {
    why = "ALSA sound cards need a Linux host";
    return nullptr;
}
#endif

}  // namespace linux_audio

using namespace linux_audio;

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr uint32_t kFifoMs = 56;                  // with the stream's 40 ms: a call returns with about 96 ms to play
constexpr auto kMicIdle = std::chrono::seconds(1);
constexpr uint32_t kChunk = 256;                  // samples a call converts at a time
constexpr int kIoTimeoutMs = 1000;

enum class Stream : uint8_t { Closed, Opening, Open, Closing };

Clock::duration frames_time(uint64_t frames, uint32_t rate) {
    return std::chrono::microseconds(rate ? frames * 1000000 / rate : 0);
}

int32_t open_error(int err) {
    return err == -EBUSY ? HAL_ERR_BUSY : HAL_ERR_IO;
}

void name_thread(const char* name) {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

}  // namespace

struct LinuxAudio::Impl {
    LinuxAudioConfig cfg;
    std::unique_ptr<AudioCard> card;
    bool speaker = false, microphone = false;
    std::atomic<bool> quit{false};

    // One call at a time, as the Cardputer's audio mutex has it; the
    // speaker and the microphone take turns unless full duplex
    std::timed_mutex speaker_call, mic_call_own;
    std::timed_mutex& mic_call() { return cfg.full_duplex ? mic_call_own : speaker_call; }

    // Speaker: its thread owns the stream, calls fill the FIFO with samples
    // at the stream's rate
    std::mutex smu;
    std::condition_variable s_wake;     // to the thread
    std::condition_variable s_change;   // to calls: room in the FIFO, the stream opened
    Stream s_state = Stream::Closed;
    int s_err = 0;
    PcmFormat s_fmt;
    uint64_t s_opens = 0;
    std::deque<float> fifo;
    size_t fifo_cap = 0;
    bool s_calling = false, s_failed = false;
    bool s_running = false;             // the stream has had frames since it opened
    Clock::time_point written_until{}, sound_until{};   // when the stream has played what it holds, and its last sound
    Resampler s_rs;                     // under speaker_call
    uint64_t s_rs_open = 0;
    std::thread s_thread;

    // Microphone: its thread owns the stream and keeps the ring
    std::mutex mmu;
    std::condition_variable m_wake, m_change;
    Stream m_state = Stream::Closed;
    int m_err = 0;
    bool m_failed = false;
    MicRing ring;
    MicConditioner cond;
    Clock::time_point last_read{};
    std::thread m_thread;

    explicit Impl(LinuxAudioConfig c) : cfg(std::move(c)) {}

    void speaker_loop();
    void mic_loop();
    int32_t speaker_play(const int16_t* samples, uint32_t count, uint32_t rate, int32_t gain_q15);
    bool speaker_playing();
    void mic_stop();
    int32_t mic_take(int16_t* out, uint32_t max, Clock::time_point deadline);
};

void LinuxAudio::Impl::speaker_loop() {
    name_thread("k32_speaker");
    const auto hold = milliseconds(cfg.speaker_hold_ms);
    std::unique_ptr<PcmStream> out;
    size_t lead_in = 0;   // frames of silence still to start the stream with
    std::vector<float> chunk;
    std::vector<uint8_t> frames;
    std::unique_lock<std::mutex> lk(smu);
    auto write = [&](size_t n, bool sound) -> bool {
        const bool last = sound && fifo.empty() && !s_calling;
        lk.unlock();
        frames.resize(n * s_fmt.channels * sample_bytes(s_fmt.format));
        mono_to_frames(chunk.data(), n, kSpeakerCeiling, s_fmt.format, s_fmt.channels, frames.data());
        const int32_t rc = out->write(frames.data(), static_cast<uint32_t>(n), kIoTimeoutMs);
        if (rc >= 0 && last) out->start();
        lk.lock();
        const auto now = Clock::now();
        if (rc < 0) {
            std::cout << "[AUDIO] speaker: write failed (" << std::strerror(-rc) << ")" << std::endl;
            s_failed = true;
            fifo.clear();
            lk.unlock();
            out.reset();
            lk.lock();
            s_state = Stream::Closed;
            written_until = sound_until = now;
            s_change.notify_all();
            return false;
        }
        // What the stream holds plays within its length from now
        written_until = std::min(std::max(written_until, now) + frames_time(n, s_fmt.rate),
                                 now + frames_time(s_fmt.buffer_frames, s_fmt.rate));
        if (sound) sound_until = written_until;
        s_running = true;
        return true;
    };
    while (!quit) {
        if (!out) {
            if (s_state != Stream::Opening) {
                s_wake.wait(lk);
                continue;
            }
            lk.unlock();
            int err = 0;
            std::unique_ptr<PcmStream> opened = card->open(false, err);
            lk.lock();
            if (opened) {
                out = std::move(opened);
                s_fmt = out->format();
                lead_in = static_cast<size_t>(s_fmt.rate) * cfg.speaker_lead_in_ms / 1000;
                fifo_cap = std::max<size_t>(s_fmt.rate * kFifoMs / 1000, s_fmt.period_frames) + lead_in;
                written_until = sound_until = Clock::now();
                s_running = false;
                ++s_opens;
                s_state = Stream::Open;
            } else {
                s_err = err < 0 ? err : -EIO;
                s_state = Stream::Closed;
            }
            s_change.notify_all();
            continue;
        }
        if (lead_in) {
            const size_t n = std::min<size_t>(lead_in, s_fmt.period_frames);
            chunk.assign(n, 0.0f);
            lead_in -= n;
            write(n, false);
            continue;
        }
        const auto now = Clock::now();
        const auto period = frames_time(s_fmt.period_frames, s_fmt.rate);
        const bool low = now + period >= written_until;   // about to run dry
        const bool idle = fifo.empty() && !s_calling;
        if (!fifo.empty() && (fifo.size() >= s_fmt.period_frames || !s_calling || low)) {
            const size_t n = std::min<size_t>(fifo.size(), s_fmt.period_frames);
            chunk.assign(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>(n));
            fifo.erase(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>(n));
            s_change.notify_all();
            write(n, true);
            continue;
        }
        if (idle && now >= sound_until + hold) {
            lk.unlock();
            out.reset();
            lk.lock();
            s_state = Stream::Closed;
            s_change.notify_all();
            continue;
        }
        if (fifo.empty() && low && s_running) {
            // Silence while the next samples may still come: the stream
            // never runs dry and never replays what it held
            chunk.assign(s_fmt.period_frames, 0.0f);
            write(s_fmt.period_frames, false);
            continue;
        }
        // Until the samples come, the stream runs low, or the tail ends; a
        // stream with no sound yet has nothing to keep fed
        if (!s_running && !idle) {
            s_wake.wait(lk);
            continue;
        }
        auto until = s_running ? written_until - period : sound_until + hold;
        if (idle) until = std::min(until, sound_until + hold);
        s_wake.wait_until(lk, until);
    }
    lk.unlock();
    out.reset();
}

int32_t LinuxAudio::Impl::speaker_play(const int16_t* samples, uint32_t count, uint32_t rate, int32_t gain_q15) {
    if (!cfg.full_duplex) mic_stop();   // the speaker comes first: playback stops a recording
    std::unique_lock<std::mutex> lk(smu);
    // Marked first: a stream opened for this call does not close or start
    // on silence before the samples come
    s_calling = true;
    s_failed = false;
    if (s_state == Stream::Closed) {
        s_state = Stream::Opening;
        s_wake.notify_all();
        s_change.wait(lk, [&] { return s_state != Stream::Opening; });
        if (s_state != Stream::Open) {
            s_calling = false;
            return open_error(s_err);
        }
    }
    if (s_rs_open != s_opens || s_rs.in_rate() != rate) {
        s_rs.reset(rate, s_fmt.rate);
        s_rs_open = s_opens;
    }
    lk.unlock();

    int16_t raw[kChunk];
    float scaled[kChunk];
    std::vector<float> res;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(samples);
    bool ok = true;
    for (uint32_t done = 0; ok && done < count;) {
        const uint32_t n = std::min(count - done, kChunk);
        // The pod's buffer may sit at any address: copy it whole first
        std::memcpy(raw, src + 2 * static_cast<size_t>(done), n * sizeof(int16_t));
        for (uint32_t i = 0; i < n; ++i) scaled[i] = static_cast<float>((raw[i] * gain_q15) >> 15);
        res.clear();
        s_rs.process(scaled, n, res);
        done += n;
        lk.lock();
        for (size_t at = 0; at < res.size();) {
            s_change.wait(lk, [&] { return fifo.size() < fifo_cap || s_failed || quit; });
            if (s_failed || quit) {
                ok = false;
                break;
            }
            const size_t k = std::min(res.size() - at, fifo_cap - fifo.size());
            fifo.insert(fifo.end(), res.begin() + static_cast<std::ptrdiff_t>(at), res.begin() + static_cast<std::ptrdiff_t>(at + k));
            at += k;
            s_wake.notify_all();
        }
        lk.unlock();
    }
    lk.lock();
    s_calling = false;
    ok = ok && !s_failed;
    s_wake.notify_all();
    return ok ? static_cast<int32_t>(count) : HAL_ERR_IO;
}

bool LinuxAudio::Impl::speaker_playing() {
    std::lock_guard<std::mutex> lk(smu);
    return s_state == Stream::Opening ||
           (s_state == Stream::Open && (!fifo.empty() || s_calling || Clock::now() < sound_until));
}

void LinuxAudio::Impl::mic_loop() {
    name_thread("k32_mic");
    std::unique_ptr<PcmStream> in;
    PcmFormat fmt;
    Resampler rs;
    std::vector<uint8_t> frames;
    std::vector<float> mono, res;
    std::vector<int16_t> pcm;
    std::unique_lock<std::mutex> lk(mmu);
    while (!quit) {
        if (!in) {
            if (m_state == Stream::Closing) {
                m_state = Stream::Closed;
                m_change.notify_all();
                continue;
            }
            if (m_state != Stream::Opening) {
                m_wake.wait(lk);
                continue;
            }
            lk.unlock();
            int err = 0;
            std::unique_ptr<PcmStream> opened = card->open(true, err);
            lk.lock();
            if (opened) {
                in = std::move(opened);
                fmt = in->format();
                rs.reset(fmt.rate, kMicRate);
                cond.start();
                ring.clear();
                m_failed = false;
                last_read = Clock::now();
                m_state = Stream::Open;
            } else {
                m_err = err < 0 ? err : -EIO;
                m_state = Stream::Closed;
            }
            m_change.notify_all();
            continue;
        }
        if (m_state == Stream::Closing || Clock::now() - last_read > kMicIdle) {
            lk.unlock();
            in.reset();
            lk.lock();
            m_state = Stream::Closed;
            ring.clear();
            m_change.notify_all();
            continue;
        }
        lk.unlock();
        frames.resize(static_cast<size_t>(fmt.period_frames) * fmt.channels * sample_bytes(fmt.format));
        const int32_t got = in->read(frames.data(), fmt.period_frames, 200);
        res.clear();
        if (got > 0) {
            mono.resize(static_cast<size_t>(got));
            frames_to_mono(frames.data(), static_cast<size_t>(got), fmt.format, fmt.channels, cfg.capture_channel, mono.data());
            rs.process(mono.data(), mono.size(), res);
        }
        if (got < 0) {
            std::cout << "[AUDIO] microphone: read failed (" << std::strerror(-got) << ")" << std::endl;
            in.reset();
        }
        lk.lock();
        if (got < 0) {
            m_state = Stream::Closed;
            m_failed = true;
            ring.clear();
            m_change.notify_all();
            continue;
        }
        if (m_state != Stream::Open || res.empty()) continue;
        pcm.resize(res.size());
        cond.process(res.data(), res.size(), cfg.mic_unity_gain, pcm.data());
        ring.push(pcm.data(), pcm.size());
        m_change.notify_all();
    }
    lk.unlock();
    in.reset();
}

void LinuxAudio::Impl::mic_stop() {
    std::lock_guard<std::mutex> lk(mmu);
    if (m_state == Stream::Open) {
        m_state = Stream::Closing;
        m_wake.notify_all();
    }
    ring.clear();
}

int32_t LinuxAudio::Impl::mic_take(int16_t* out, uint32_t max, Clock::time_point deadline) {
    std::unique_lock<std::mutex> lk(mmu);
    m_change.wait(lk, [&] { return m_state != Stream::Closing || quit; });
    if (m_state == Stream::Closed) {
        m_state = Stream::Opening;
        m_wake.notify_all();
        m_change.wait(lk, [&] { return m_state != Stream::Opening || quit; });
        if (m_state != Stream::Open) return open_error(m_err);
    }
    uint8_t* dst = reinterpret_cast<uint8_t*>(out);
    int16_t tmp[kMicBufferSamples];
    uint32_t got = 0;
    for (;;) {
        last_read = Clock::now();
        const size_t n = ring.pop(tmp, std::min<size_t>(max - got, kMicBufferSamples));
        // out may sit at any address
        std::memcpy(dst + 2 * static_cast<size_t>(got), tmp, n * sizeof(int16_t));
        got += static_cast<uint32_t>(n);
        if (got >= max || m_state != Stream::Open || quit || Clock::now() >= deadline) break;
        m_change.wait_until(lk, deadline);
    }
    last_read = Clock::now();
    if (got == 0 && m_failed) return HAL_ERR_IO;
    return static_cast<int32_t>(got);
}

LinuxAudio::LinuxAudio(LinuxAudioConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}

LinuxAudio::LinuxAudio(LinuxAudioConfig cfg, std::unique_ptr<AudioCard> card) : impl_(std::make_unique<Impl>(std::move(cfg))) {
    impl_->card = std::move(card);
}

LinuxAudio::~LinuxAudio() {
    Impl& a = *impl_;
    a.quit = true;
    {
        std::lock_guard<std::mutex> lk(a.smu);
        a.s_wake.notify_all();
        a.s_change.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(a.mmu);
        a.m_wake.notify_all();
        a.m_change.notify_all();
    }
    if (a.s_thread.joinable()) a.s_thread.join();
    if (a.m_thread.joinable()) a.m_thread.join();
}

bool LinuxAudio::open(std::string& why) {
    Impl& a = *impl_;
    if (a.speaker || a.microphone) return true;
    if (!a.card) a.card = open_alsa_card(a.cfg, why);
    if (!a.card) return false;
    a.speaker = a.cfg.playback_device >= 0 && a.card->has_playback();
    a.microphone = a.cfg.capture_device >= 0 && a.card->has_capture();
    if (!a.speaker && !a.microphone) {
        why = "the sound card has neither a playback nor a capture stream";
        return false;
    }
    if (a.speaker) a.s_thread = std::thread([&a] { a.speaker_loop(); });
    if (a.microphone) a.m_thread = std::thread([&a] { a.mic_loop(); });
    return true;
}

bool LinuxAudio::has_speaker() const { return impl_->speaker; }
bool LinuxAudio::has_microphone() const { return impl_->microphone; }

int32_t LinuxAudio::tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct) {
    if (freq_hz == 0 || duration_ms == 0 || volume_pct == 0) return HAL_OK;
    if (!impl_->speaker) return HAL_ERR_NODEV;
    std::unique_lock<std::timed_mutex> call(impl_->speaker_call, milliseconds(2000));
    if (!call.owns_lock()) return HAL_ERR_BUSY;
    const std::vector<int16_t> pcm = square_tone(freq_hz, duration_ms, volume_pct);
    const int32_t rc = impl_->speaker_play(pcm.data(), static_cast<uint32_t>(pcm.size()), kToneRate, 32768);
    return rc < 0 ? rc : HAL_OK;
}

int32_t LinuxAudio::play(const int16_t* samples, uint32_t count, uint32_t rate, uint32_t volume_pct) {
    if (rate < 8000 || rate > 48000) return HAL_ERR_INVAL;
    if (count > rate) count = rate;   // one second per call
    if (volume_pct > 100) volume_pct = 100;
    if (count == 0) return 0;
    if (!impl_->speaker) return HAL_ERR_NODEV;
    std::unique_lock<std::timed_mutex> call(impl_->speaker_call, milliseconds(2000));
    if (!call.owns_lock()) return HAL_ERR_BUSY;
    return impl_->speaker_play(samples, count, rate, speaker_gain_q15(volume_pct));
}

int32_t LinuxAudio::mic_read(int16_t* out, uint32_t max_samples, uint32_t timeout_ms) {
    if (max_samples > kMicRate) max_samples = kMicRate;
    if (timeout_ms > 1000) timeout_ms = 1000;
    const auto deadline = Clock::now() + milliseconds(timeout_ms);
    if (!impl_->microphone) return HAL_ERR_NODEV;
    std::unique_lock<std::timed_mutex> call(impl_->mic_call(), milliseconds(timeout_ms));
    if (!call.owns_lock()) return HAL_ERR_BUSY;
    if (max_samples == 0) {
        impl_->mic_stop();
        return 0;
    }
    if (!impl_->cfg.full_duplex) {
        // Playback keeps the device until its samples have played
        while (impl_->speaker_playing()) {
            if (Clock::now() >= deadline) return HAL_ERR_BUSY;
            call.unlock();
            std::this_thread::sleep_for(milliseconds(10));
            if (!call.try_lock_for(milliseconds(timeout_ms))) return HAL_ERR_BUSY;
        }
    }
    return impl_->mic_take(out, max_samples, deadline);
}

void LinuxAudio::mic_set_gain(uint32_t gain) {
    std::lock_guard<std::mutex> lk(impl_->mmu);
    impl_->cond.set_gain(gain);
}

}  // namespace kube32::hal
