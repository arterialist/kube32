// A Linux board's speaker and microphone (hal/linux_audio.hpp): the volume
// ceiling, k32_tone's samples, the resampler and the frame formats, the
// microphone's conditioning and buffer, the mixer values, and the
// Cardputer's semantics on a sound card simulated at its own pace.

#include "hal/hal_types.hpp"
#include "hal/linux_audio.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace kube32::hal;
using namespace kube32::hal::linux_audio;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

static constexpr double kPi = 3.14159265358979323846;

static void test_gain_and_tone() {
    assert(speaker_gain_q15(0) == 0);
    assert(speaker_gain_q15(10) == 819);
    assert(speaker_gain_q15(100) == 8192);
    assert(speaker_gain_q15(250) == 8192);
    assert((32767 * speaker_gain_q15(100)) >> 15 == kSpeakerCeiling);

    assert(square_tone(0, 100, 50).empty());
    assert(square_tone(440, 0, 50).empty());
    assert(square_tone(440, 100, 0).empty());
    assert(square_tone(440, 5000, 50).size() == 32000);   // 2 s at most

    // 1 kHz: 16 samples a period, a 64-sample ramp at both ends
    const std::vector<int16_t> t = square_tone(1000, 100, 100);
    assert(t.size() == 1600);
    assert(t[0] == 0);
    assert(t[64] == kSpeakerCeiling && t[71] == kSpeakerCeiling && t[72] == -kSpeakerCeiling && t[80] == kSpeakerCeiling);
    assert(std::abs(t[1599]) < kSpeakerCeiling / 32);
    for (int16_t s : t) assert(std::abs(s) <= kSpeakerCeiling);
    const std::vector<int16_t> quiet = square_tone(1000, 100, 10);
    assert(*std::max_element(quiet.begin(), quiet.end()) == 819);
}

static std::vector<float> sine(double hz, uint32_t rate, size_t n, double amp) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<float>(amp * std::sin(2 * kPi * hz * static_cast<double>(i) / rate));
    return v;
}

// Output sample k is the input at time k * in / out
static double max_error(const std::vector<float>& out, double hz, uint32_t in_rate, uint32_t out_rate, double amp, size_t skip) {
    double worst = 0;
    for (size_t k = skip; k + skip < out.size(); ++k) {
        const double t = static_cast<double>(k) * in_rate / out_rate;
        worst = std::max(worst, std::fabs(out[k] - amp * std::sin(2 * kPi * hz * t / in_rate)));
    }
    return worst;
}

static double rms(const float* v, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += static_cast<double>(v[i]) * v[i];
    return n ? std::sqrt(s / static_cast<double>(n)) : 0;
}

static void test_resampler() {
    Resampler r;
    r.reset(48000, 48000);
    std::vector<float> in = sine(1000, 48000, 480, 1000), out;
    r.process(in.data(), in.size(), out);
    assert(out == in);

    // Up from 16 kHz: a voice band tone keeps its shape
    r.reset(16000, 48000);
    in = sine(1000, 16000, 1600, 10000);
    out.clear();
    r.process(in.data(), in.size(), out);
    assert(out.size() > 4700 && out.size() <= 4800);
    const double up = max_error(out, 1000, 16000, 48000, 10000, 100);
    std::cout << "16k -> 48k, 1 kHz: max error " << up << " of 10000" << std::endl;
    assert(up < 30);

    r.reset(44100, 48000);
    in = sine(5000, 44100, 4410, 10000);
    out.clear();
    r.process(in.data(), in.size(), out);
    const double odd = max_error(out, 5000, 44100, 48000, 10000, 100);
    std::cout << "44.1k -> 48k, 5 kHz: max error " << odd << " of 10000" << std::endl;
    assert(odd < 100);

    // Down to 16 kHz: the voice band passes, what would alias does not
    r.reset(48000, 16000);
    in = sine(1000, 48000, 4800, 10000);
    out.clear();
    r.process(in.data(), in.size(), out);
    assert(out.size() > 1500 && out.size() <= 1600);
    const double down = max_error(out, 1000, 48000, 16000, 10000, 60);
    std::cout << "48k -> 16k, 1 kHz: max error " << down << " of 10000" << std::endl;
    assert(down < 30);
    r.reset(48000, 16000);
    in = sine(12000, 48000, 4800, 10000);
    out.clear();
    r.process(in.data(), in.size(), out);
    const double alias = rms(out.data() + 60, out.size() - 120);
    std::cout << "48k -> 16k, 12 kHz: " << alias << " rms left of 7071" << std::endl;
    assert(alias < 70);

    // Any split of the input gives the same output
    r.reset(22050, 48000);
    in = sine(440, 22050, 5000, 8000);
    std::vector<float> whole;
    r.process(in.data(), in.size(), whole);
    r.reset(22050, 48000);
    std::vector<float> parts;
    for (size_t at = 0, step = 1; at < in.size(); at += step, step = step * 7 % 997 + 1)
        r.process(in.data() + at, std::min(step, in.size() - at), parts);
    assert(parts.size() == whole.size());
    for (size_t i = 0; i < whole.size(); ++i) assert(std::fabs(parts[i] - whole[i]) < 1e-3f);
}

static void test_frames() {
    const float in[4] = {100.4f, -100.6f, 9000.0f, -32768.0f};
    uint8_t f16[4 * 2 * 2];
    mono_to_frames(in, 4, kSpeakerCeiling, SampleFormat::S16_LE, 2, f16);
    int16_t s16[8];
    std::memcpy(s16, f16, sizeof s16);
    assert(s16[0] == 100 && s16[1] == 100 && s16[2] == -101 && s16[3] == -101);
    assert(s16[4] == kSpeakerCeiling && s16[6] == -kSpeakerCeiling);   // the ceiling holds

    uint8_t f32[4 * 4 * 2];
    mono_to_frames(in, 4, 32767, SampleFormat::S32_LE, 2, f32);
    int32_t s32[8];
    std::memcpy(s32, f32, sizeof s32);
    assert(s32[0] == 100 * 65536 && s32[1] == 100 * 65536 && s32[3] == -101 * 65536 && s32[6] == -32767 * 65536);

    uint8_t f24[4 * 4];
    mono_to_frames(in, 4, 32767, SampleFormat::S24_LE, 1, f24);
    int32_t s24[4];
    std::memcpy(s24, f24, sizeof s24);
    assert(s24[0] == 100 * 256 && s24[1] == -101 * 256);

    float back[4];
    for (SampleFormat f : {SampleFormat::S16_LE, SampleFormat::S24_LE, SampleFormat::S32_LE}) {
        uint8_t buf[4 * 4 * 2];
        mono_to_frames(in, 4, 32767, f, 2, buf);
        frames_to_mono(buf, 4, f, 2, -1, back);
        assert(back[0] == 100 && back[1] == -101 && back[2] == 9000 && back[3] == -32767);
    }

    // One channel, or the mean of both
    int32_t lr[4] = {1000 * 65536, 3000 * 65536, -500 * 65536, 500 * 65536};
    uint8_t raw[sizeof lr];
    std::memcpy(raw, lr, sizeof raw);
    frames_to_mono(raw, 2, SampleFormat::S32_LE, 2, 0, back);
    assert(back[0] == 1000 && back[1] == -500);
    frames_to_mono(raw, 2, SampleFormat::S32_LE, 2, 1, back);
    assert(back[0] == 3000 && back[1] == 500);
    frames_to_mono(raw, 2, SampleFormat::S32_LE, 2, -1, back);
    assert(back[0] == 2000 && back[1] == 0);
}

static void test_conditioner() {
    MicConditioner c;
    assert(c.gain() == kMicDefaultGain);
    c.set_gain(0);
    assert(c.gain() == 1);
    c.set_gain(1000);
    assert(c.gain() == 64);
    c.set_gain(16);

    // The first 100 ms are silence while the offset is found
    c.start();
    std::vector<float> in(3200);
    for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(-1650 + 300 * std::sin(2 * kPi * 500 * static_cast<double>(i) / 16000));
    std::vector<int16_t> out(in.size());
    c.process(in.data(), in.size(), 16, out.data());
    for (size_t i = 0; i < kMicSettleSamples; ++i) assert(out[i] == 0);
    double sum = 0, peak = 0;
    for (size_t i = kMicSettleSamples; i < out.size(); ++i) {
        sum += out[i];
        peak = std::max(peak, std::fabs(static_cast<double>(out[i])));
    }
    const double mean = sum / static_cast<double>(out.size() - kMicSettleSamples);
    std::cout << "conditioned: mean " << mean << ", peak " << peak << " (300 in, at -1650)" << std::endl;
    assert(std::fabs(mean) < 20 && peak > 280 && peak < 330);

    // Gain over unity scales, and saturates
    c.set_gain(32);
    c.process(in.data(), in.size(), 16, out.data());
    peak = 0;
    for (int16_t s : out) peak = std::max(peak, std::fabs(static_cast<double>(s)));
    assert(peak > 570 && peak < 660);
    for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(1000 * std::sin(2 * kPi * 500 * static_cast<double>(i) / 16000));
    c.set_gain(64);
    c.process(in.data(), in.size(), 1, out.data());
    assert(*std::max_element(out.begin(), out.end()) == 32767 && *std::min_element(out.begin(), out.end()) == -32768);
}

static void test_ring() {
    MicRing r;
    std::vector<int16_t> v(2000);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<int16_t>(i);
    r.push(v.data(), 300);
    r.push(v.data() + 300, 1700);   // the newest 1024 stay
    assert(r.size() == kMicBufferSamples);
    int16_t out[2048];
    assert(r.pop(out, 10) == 10 && out[0] == 976 && out[9] == 985);
    r.push(v.data(), 5);            // wraps
    assert(r.pop(out, 2048) == kMicBufferSamples - 10 + 5);
    assert(out[0] == 986 && out[kMicBufferSamples - 11] == 1999 && out[kMicBufferSamples - 10] == 0 && out[kMicBufferSamples - 6] == 4);
    assert(r.size() == 0 && r.pop(out, 10) == 0);
    r.push(v.data(), 1);
    r.clear();
    assert(r.size() == 0);
}

static void test_control_values() {
    std::vector<int64_t> v;
    std::string why;
    ControlInfo sw;
    sw.type = ControlInfo::Type::Boolean;
    sw.max = 1;
    assert(parse_control_value(sw, "on", v, why) && v == std::vector<int64_t>{1});
    assert(parse_control_value(sw, "Off", v, why) && v == std::vector<int64_t>{0});
    assert(parse_control_value(sw, "1", v, why) && v[0] == 1);
    assert(!parse_control_value(sw, "maybe", v, why) && !why.empty());
    sw.count = 2;
    assert(parse_control_value(sw, "on", v, why) && v == (std::vector<int64_t>{1, 1}));
    assert(parse_control_value(sw, "on, off", v, why) && v == (std::vector<int64_t>{1, 0}));
    assert(!parse_control_value(sw, "on,off,on", v, why));

    ControlInfo vol;
    vol.min = 0;
    vol.max = 255;
    assert(parse_control_value(vol, "191", v, why) && v[0] == 191);
    assert(parse_control_value(vol, "50%", v, why) && v[0] == 128);
    assert(parse_control_value(vol, "100%", v, why) && v[0] == 255);
    assert(!parse_control_value(vol, "256", v, why));
    assert(!parse_control_value(vol, "-1", v, why));
    assert(!parse_control_value(vol, "loud", v, why));
    assert(!parse_control_value(vol, "120%", v, why));

    ControlInfo mux;
    mux.type = ControlInfo::Type::Enumerated;
    mux.items = {"AMIC", "DMIC"};
    assert(parse_control_value(mux, "DMIC", v, why) && v[0] == 1);
    assert(parse_control_value(mux, "amic", v, why) && v[0] == 0);
    assert(parse_control_value(mux, "1", v, why) && v[0] == 1);
    assert(!parse_control_value(mux, "2", v, why));
    assert(!parse_control_value(mux, "PDM", v, why));
    mux.items = {"DifferentialL", "Line 1P", "Line 2P"};
    assert(parse_control_value(mux, "Line 1P", v, why) && v[0] == 1);
}

// A sound card in software that keeps time: playback takes frames at its
// rate through a buffer, capture delivers a tone on channel 0
struct SimCard : AudioCard {
    PcmFormat out_fmt{SampleFormat::S32_LE, 2, 48000, 480, 1920};
    PcmFormat in_fmt{SampleFormat::S32_LE, 2, 48000, 480, 3840};
    bool playback = true, capture = true;
    std::atomic<bool> busy{false};
    std::atomic<int> out_opens{0}, out_closes{0}, in_opens{0}, in_closes{0};
    std::mutex mu;
    std::vector<float> played;            // channel 0, int16 scale
    std::atomic<uint64_t> played_frames{0};
    double mic_amp = 2000, mic_dc = -1650, mic_hz = 1000;

    bool has_playback() const override { return playback; }
    bool has_capture() const override { return capture; }
    std::unique_ptr<PcmStream> open(bool in, int& err) override;
};

struct SimPlayback : PcmStream {
    SimCard& card;
    Clock::time_point t0;
    uint64_t written = 0;
    bool started = false;
    explicit SimPlayback(SimCard& c) : card(c) {}
    ~SimPlayback() override { ++card.out_closes; }
    const PcmFormat& format() const override { return card.out_fmt; }
    uint64_t played(Clock::time_point now) const {
        return started ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - t0).count()) * card.out_fmt.rate / 1000000 : 0;
    }
    int32_t write(const uint8_t* frames, uint32_t n, int timeout_ms) override {
        auto now = Clock::now();
        if (!started) {
            started = true;
            t0 = now;
        }
        if (played(now) > written) t0 = now - std::chrono::microseconds(written * 1000000 / card.out_fmt.rate);   // ran dry: resumes now
        const auto deadline = now + milliseconds(timeout_ms);
        while (written + n > played(Clock::now()) + card.out_fmt.buffer_frames) {
            if (Clock::now() >= deadline) return -ETIMEDOUT;
            std::this_thread::sleep_for(milliseconds(1));
        }
        std::vector<float> mono(n);
        frames_to_mono(frames, n, card.out_fmt.format, card.out_fmt.channels, 0, mono.data());
        {
            std::lock_guard<std::mutex> lk(card.mu);
            card.played.insert(card.played.end(), mono.begin(), mono.end());
        }
        written += n;
        card.played_frames = std::min(played(Clock::now()), written);
        return static_cast<int32_t>(n);
    }
    int32_t read(uint8_t*, uint32_t, int) override { return -EINVAL; }
};

struct SimCapture : PcmStream {
    SimCard& card;
    Clock::time_point t0 = Clock::now();
    uint64_t taken = 0;
    explicit SimCapture(SimCard& c) : card(c) {}
    ~SimCapture() override { ++card.in_closes; }
    const PcmFormat& format() const override { return card.in_fmt; }
    uint64_t made() const {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count()) * card.in_fmt.rate / 1000000;
    }
    int32_t write(const uint8_t*, uint32_t, int) override { return -EINVAL; }
    int32_t read(uint8_t* frames, uint32_t n, int timeout_ms) override {
        const auto deadline = Clock::now() + milliseconds(timeout_ms);
        while (made() < taken + n && Clock::now() < deadline) std::this_thread::sleep_for(milliseconds(1));
        const uint64_t ready = made();
        if (ready > taken + card.in_fmt.buffer_frames) taken = ready - card.in_fmt.buffer_frames;   // an overrun loses the oldest
        const uint32_t k = static_cast<uint32_t>(std::min<uint64_t>(n, ready - taken));
        for (uint32_t i = 0; i < k; ++i) {
            const double t = static_cast<double>(taken + i) / card.in_fmt.rate;
            const int32_t l = static_cast<int32_t>(std::lrint((card.mic_dc + card.mic_amp * std::sin(2 * kPi * card.mic_hz * t)) * 65536));
            const int32_t r = 12345 * 65536;   // another input on channel 1
            std::memcpy(frames + 8 * i, &l, 4);
            std::memcpy(frames + 8 * i + 4, &r, 4);
        }
        taken += k;
        return static_cast<int32_t>(k);
    }
};

std::unique_ptr<PcmStream> SimCard::open(bool in, int& err) {
    if (busy) {
        err = -EBUSY;
        return nullptr;
    }
    if (in) {
        ++in_opens;
        return std::make_unique<SimCapture>(*this);
    }
    ++out_opens;
    return std::make_unique<SimPlayback>(*this);
}

static LinuxAudioConfig sim_config(bool full_duplex = false) {
    LinuxAudioConfig c;
    c.capture_channel = 0;
    c.full_duplex = full_duplex;
    return c;
}

static void wait_for(const std::function<bool()>& done, int ms) {
    const auto deadline = Clock::now() + milliseconds(ms);
    while (!done() && Clock::now() < deadline) std::this_thread::sleep_for(milliseconds(5));
}

static long since_ms(Clock::time_point t) {
    return static_cast<long>(std::chrono::duration_cast<milliseconds>(Clock::now() - t).count());
}

// k32_audio_play blocks until about 96 ms are left to play, at most the
// ceiling reaches the card, and the stream closes once the sound is out
static void test_play() {
    auto owned = std::make_unique<SimCard>();
    SimCard& card = *owned;
    LinuxAudio audio(sim_config(), std::move(owned));
    std::string why;
    assert(audio.open(why) && audio.has_speaker() && audio.has_microphone());

    std::vector<int16_t> loud(8000);
    for (size_t i = 0; i < loud.size(); ++i) loud[i] = (i / 8) % 2 ? 32767 : -32768;
    // At an odd address, as a pod's buffer may be
    std::vector<uint8_t> odd(loud.size() * 2 + 1);
    std::memcpy(odd.data() + 1, loud.data(), loud.size() * 2);
    const auto t0 = Clock::now();
    assert(audio.play(reinterpret_cast<const int16_t*>(odd.data() + 1), 8000, 16000, 100) == 8000);
    const long took = since_ms(t0);
    const double left_ms = (500.0 * 48 - static_cast<double>(card.played_frames)) / 48;
    std::cout << "500 ms of sound: the call took " << took << " ms and left " << left_ms << " ms to play" << std::endl;
    assert(left_ms > 30 && left_ms < 200);

    wait_for([&] { return card.out_closes == 1; }, 1000);
    assert(card.out_opens == 1 && card.out_closes == 1);
    {
        std::lock_guard<std::mutex> lk(card.mu);
        float peak = 0;
        for (float s : card.played) peak = std::max(peak, std::fabs(s));
        std::cout << "played " << card.played.size() << " frames, peak " << peak << std::endl;
        assert(card.played.size() >= 24000 && peak <= kSpeakerCeiling && peak > kSpeakerCeiling - 300);
    }

    // A tone at 10% of the ceiling, and the arguments the Cardputer refuses
    assert(audio.tone(1000, 50, 10) == HAL_OK);
    assert(audio.tone(0, 50, 10) == HAL_OK);
    assert(audio.play(loud.data(), 100, 7999, 50) == HAL_ERR_INVAL);
    assert(audio.play(loud.data(), 100, 48001, 50) == HAL_ERR_INVAL);
    assert(audio.play(loud.data(), 0, 16000, 50) == 0);
    std::vector<int16_t> second(8100);
    assert(audio.play(second.data(), 8100, 8000, 50) == 8000);   // one second per call
    {
        std::lock_guard<std::mutex> lk(card.mu);
        card.played.clear();
    }
    wait_for([&] { return card.out_closes == card.out_opens; }, 1000);
    assert(audio.tone(2000, 100, 10) == HAL_OK);
    wait_for([&] { return card.out_closes == card.out_opens; }, 1000);
    std::lock_guard<std::mutex> lk(card.mu);
    float peak = 0;
    for (float s : card.played) peak = std::max(peak, std::fabs(s));
    // Samples of +-819; the 2 and 6 kHz they hold peak higher between them,
    // as on a DAC's output
    std::cout << "tone at 10%: peak " << peak << std::endl;
    assert(peak > 700 && peak < 1100);
}

// A card that sounds only a while after a start: the stream starts with
// silence, a call waits no longer for it, and a stream held open after a
// sound takes the next one at once
static void test_lead_in() {
    auto owned = std::make_unique<SimCard>();
    SimCard& card = *owned;
    LinuxAudioConfig cfg = sim_config();
    cfg.speaker_lead_in_ms = 150;
    cfg.speaker_hold_ms = 400;
    LinuxAudio audio(cfg, std::move(owned));
    std::string why;
    assert(audio.open(why));

    auto t0 = Clock::now();
    assert(audio.tone(1000, 200, 10) == HAL_OK);
    const long took = since_ms(t0);
    std::cout << "a 200 ms tone after a 150 ms lead-in: the call took " << took << " ms" << std::endl;
    assert(took < 100);   // the Cardputer's takes about 104 ms

    auto sound_span = [&](size_t& first, size_t& last) {
        std::lock_guard<std::mutex> lk(card.mu);
        first = last = 0;
        bool any = false;
        for (size_t i = 0; i < card.played.size(); ++i) {
            if (std::fabs(card.played[i]) < 50) continue;
            if (!any) first = i;
            any = true;
            last = i;
        }
        return any;
    };
    wait_for([&] { size_t f, l; return sound_span(f, l) && card.played.size() > l + 4800; }, 1500);
    size_t first = 0, last = 0;
    assert(sound_span(first, last));
    std::cout << "sound from frame " << first << " to " << last << std::endl;
    assert(first >= 7200 - 100 && first < 7200 + 500);
    assert(last - first > 9400 && last - first < 9700);

    // Held open: the next sound needs no lead-in
    std::this_thread::sleep_for(milliseconds(150));
    assert(card.out_opens == 1 && card.out_closes == 0);
    {
        std::lock_guard<std::mutex> lk(card.mu);
        card.played.clear();
    }
    assert(audio.tone(1000, 50, 10) == HAL_OK);
    wait_for([&] { size_t f, l; return sound_span(f, l) && card.played.size() > l + 480; }, 1000);
    assert(sound_span(first, last));
    assert(card.out_opens == 1 && first < 2400);
    wait_for([&] { return card.out_closes == 1; }, 1500);
    assert(card.out_closes == 1);
}

// Another process holding the device, and a board without one
static void test_busy_and_missing() {
    auto owned = std::make_unique<SimCard>();
    SimCard& card = *owned;
    card.busy = true;
    LinuxAudio audio(sim_config(), std::move(owned));
    std::string why;
    assert(audio.open(why));
    int16_t buf[160] = {};
    assert(audio.play(buf, 160, 16000, 10) == HAL_ERR_BUSY);
    assert(audio.tone(1000, 50, 10) == HAL_ERR_BUSY);
    assert(audio.mic_read(buf, 160, 100) == HAL_ERR_BUSY);
    card.busy = false;
    assert(audio.mic_read(buf, 160, 100) == 160);

    auto none = std::make_unique<SimCard>();
    none->playback = false;
    LinuxAudio mic_only(sim_config(), std::move(none));
    assert(mic_only.open(why) && !mic_only.has_speaker() && mic_only.has_microphone());
    assert(mic_only.play(buf, 160, 16000, 10) == HAL_ERR_NODEV);
    assert(mic_only.tone(1000, 50, 10) == HAL_ERR_NODEV);

    auto silent = std::make_unique<SimCard>();
    silent->playback = silent->capture = false;
    LinuxAudio nothing(sim_config(), std::move(silent));
    assert(!nothing.open(why) && !why.empty());
    assert(nothing.mic_read(buf, 160, 10) == HAL_ERR_NODEV);

    LinuxAudio unopened(sim_config(), std::make_unique<SimCard>());
    assert(unopened.play(buf, 160, 16000, 10) == HAL_ERR_NODEV);
}

static double rms16(const int16_t* v, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += static_cast<double>(v[i]) * v[i];
    return n ? std::sqrt(s / static_cast<double>(n)) : 0;
}

// 16 kHz mono with the offset removed, 100 ms of silence after a start,
// the last 64 ms kept between reads, a fresh start after max 0 or a second
// without reads
static void test_mic() {
    auto owned = std::make_unique<SimCard>();
    SimCard& card = *owned;
    LinuxAudio audio(sim_config(), std::move(owned));
    std::string why;
    assert(audio.open(why));
    std::vector<int16_t> buf(16000);

    const auto t0 = Clock::now();
    assert(audio.mic_read(buf.data(), 1600, 1000) == 1600);
    const long took = since_ms(t0);
    assert(took >= 80 && took < 600);
    for (int i = 0; i < 1600; ++i) assert(buf[i] == 0);

    assert(audio.mic_read(buf.data(), 1600, 1000) == 1600);
    double sum = 0;
    for (int i = 0; i < 1600; ++i) sum += buf[i];
    const double level = rms16(buf.data(), 1600);
    std::cout << "microphone: mean " << sum / 1600 << ", rms " << level << " (2000 peak in, at -1650)" << std::endl;
    assert(std::fabs(sum / 1600) < 100 && level > 1300 && level < 1550);

    // 1 kHz at 16 kHz: 16 samples a period
    int crossings = 0;
    for (int i = 1; i < 1600; ++i) crossings += (buf[i - 1] < 0) != (buf[i] < 0);
    assert(crossings >= 196 && crossings <= 204);

    audio.mic_set_gain(32);
    assert(audio.mic_read(buf.data(), 1600, 1000) == 1600);
    const double louder = rms16(buf.data(), 1600);
    assert(louder > 1.9 * level && louder < 2.1 * level);
    audio.mic_set_gain(16);

    std::this_thread::sleep_for(milliseconds(300));
    assert(audio.mic_read(buf.data(), 16000, 0) == static_cast<int32_t>(kMicBufferSamples));
    assert(audio.mic_read(buf.data(), 16000, 0) < 200);

    assert(audio.mic_read(buf.data(), 0, 0) == 0);   // stops at once
    wait_for([&] { return card.in_closes == 1; }, 500);
    assert(card.in_opens == 1 && card.in_closes == 1);
    assert(audio.mic_read(buf.data(), 800, 1000) == 800);
    assert(card.in_opens == 2);
    for (int i = 0; i < 800; ++i) assert(buf[i] == 0);   // settling again

    std::this_thread::sleep_for(milliseconds(1300));     // a second without reads stops it
    assert(card.in_closes == 2);
    assert(audio.mic_read(buf.data(), 160, 1000) == 160);
    assert(card.in_opens == 3);
    for (int i = 0; i < 160; ++i) assert(buf[i] == 0);
}

// The speaker comes first: a read waits for playback to end, and playback
// stops a recording; full duplex records while the speaker plays
static void test_turns() {
    std::vector<int16_t> sound(4800, 1000), buf(1600);
    {
        auto owned = std::make_unique<SimCard>();
        SimCard& card = *owned;
        LinuxAudio audio(sim_config(), std::move(owned));
        std::string why;
        assert(audio.open(why));
        assert(audio.mic_read(buf.data(), 160, 1000) == 160);
        assert(audio.play(sound.data(), 4800, 16000, 10) == 4800);
        wait_for([&] { return card.in_closes == 1; }, 500);
        assert(card.in_closes == 1);
        assert(audio.mic_read(buf.data(), 160, 20) == HAL_ERR_BUSY);
        const auto t0 = Clock::now();
        assert(audio.mic_read(buf.data(), 160, 1000) == 160);
        std::cout << "a read after playback waited " << since_ms(t0) << " ms" << std::endl;
        assert(card.in_opens == 2);
    }
    {
        auto owned = std::make_unique<SimCard>();
        SimCard& card = *owned;
        LinuxAudio audio(sim_config(true), std::move(owned));
        std::string why;
        assert(audio.open(why));
        assert(audio.mic_read(buf.data(), 160, 1000) == 160);
        assert(audio.play(sound.data(), 4800, 16000, 10) == 4800);
        assert(audio.mic_read(buf.data(), 160, 200) == 160);
        assert(card.in_opens == 1 && card.in_closes == 0);
    }
}

int main() {
    test_gain_and_tone();
    test_resampler();
    test_frames();
    test_conditioner();
    test_ring();
    test_control_values();
    test_play();
    test_lead_in();
    test_busy_and_missing();
    test_mic();
    test_turns();
    std::cout << "linux audio ok" << std::endl;
    return 0;
}
