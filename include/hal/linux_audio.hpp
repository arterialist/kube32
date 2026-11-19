#pragma once

// The speaker and microphone of a Linux board, through the kernel's ALSA PCM
// and control devices (/dev/snd) with no sound library, so a static binary
// carries them. Pods get what the Cardputer gives them: the same volume
// ceiling, k32_audio_play returning with about 96 ms still to play, a 16 kHz
// mono microphone that keeps the last 64 ms between reads, and the speaker
// before the microphone. The card's own format (rate, channels, sample
// width) is converted in software.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kube32::hal {

struct LinuxAudioConfig {
    int card = -1;              // ALSA card number, -1: the card called card_name
    std::string card_name;      // the card's id or name, as /proc/asound/cards lists it
    int playback_device = 0;    // -1: no speaker
    int capture_device = 0;     // -1: no microphone
    // Mixer controls set at open (name -> value, as amixer cset takes them),
    // put back as they were when the LinuxAudio goes
    std::vector<std::pair<std::string, std::string>> controls;
    uint32_t rate = 0;          // hardware rate of both directions, 0: the first of 48000, 44100, 32000, 16000 the card takes
    int capture_channel = -1;   // the microphone's channel in a capture frame, -1: the mean of all of them
    uint32_t mic_unity_gain = 16;   // the mic_set_gain value that keeps the codec's level
    bool full_duplex = false;   // the microphone records while the speaker plays (false: the speaker comes first)
    // Silence a playback stream starts with, for a card that sounds only a
    // while after a start; the output queue grows by as much, so a call
    // never waits longer for it
    uint32_t speaker_lead_in_ms = 0;
    uint32_t speaker_hold_ms = 30;  // how long the stream stays open after the last sound
};

// The M5Stack Cardputer Zero: speaker and microphone on its ES8389 codec
LinuxAudioConfig cardputer_zero_audio();

namespace linux_audio {

constexpr uint32_t kSpeakerMaxPct = 25;          // K32_SPEAKER_MAX_PCT
constexpr int32_t kSpeakerCeiling = 32767 * static_cast<int32_t>(kSpeakerMaxPct) / 100;
constexpr uint32_t kToneRate = 16000;
constexpr uint32_t kMicRate = 16000;
constexpr uint32_t kMicBufferSamples = 1024;     // 64 ms
constexpr uint32_t kMicSettleSamples = 1600;     // 100 ms
constexpr uint32_t kMicDefaultGain = 16;

// The Q15 factor that scales a sample to volume_pct of the speaker's ceiling
int32_t speaker_gain_q15(uint32_t volume_pct);

// k32_tone's samples at kToneRate: a square wave at volume_pct of the
// ceiling, ramped at both ends (empty for a zero argument)
std::vector<int16_t> square_tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct);

// One channel from one rate to another through a windowed sinc. Output
// sample k is the input at time k * in_rate / out_rate; it comes out once
// the input a few samples past that time has gone in.
class Resampler {
public:
    void reset(uint32_t in_rate, uint32_t out_rate);
    uint32_t in_rate() const { return in_rate_; }
    uint32_t out_rate() const { return out_rate_; }
    // Appends what n more input samples complete
    void process(const float* in, size_t n, std::vector<float>& out);

private:
    float tap_sum(size_t at, const float* row) const;

    uint32_t in_rate_ = 0, out_rate_ = 0;
    int half_ = 0;                 // taps on each side of an output position
    std::vector<float> table_;     // kPhases + 1 rows of 2 * half_ taps
    std::vector<float> buf_;       // input from half_ - 1 samples before the position on
    size_t pos_ = 0;               // the position's whole input sample in buf_
    uint64_t frac_ = 0;            // its fraction, in 1/out_rate of an input sample
};

// Little endian PCM samples; S24_LE holds 24 bits in the low bytes of 32
enum class SampleFormat : uint8_t { S16_LE, S24_LE, S32_LE };
uint32_t sample_bytes(SampleFormat format);

// n mono samples on the int16 scale, clamped to +-limit, to interleaved
// frames with the sample in every channel
void mono_to_frames(const float* in, size_t n, int32_t limit, SampleFormat format, uint32_t channels, uint8_t* out);
// n interleaved frames to mono samples on the int16 scale: one channel, or
// the mean of all of them for channel -1
void frames_to_mono(const uint8_t* in, size_t n, SampleFormat format, uint32_t channels, int channel, float* out);

// The Cardputer's conditioning of microphone samples: the DC offset tracked
// and removed (a one-pole high-pass at 2.5 Hz), the gain applied with
// saturation, and silence for the first 100 ms after a start while the
// offset settles
class MicConditioner {
public:
    void start() { settle_ = kMicSettleSamples; }
    void set_gain(uint32_t gain) { gain_ = gain < 1 ? 1 : gain > 64 ? 64 : gain; }
    uint32_t gain() const { return gain_; }
    // gain / unity_gain is the factor on the codec's level
    void process(const float* in, size_t n, uint32_t unity_gain, int16_t* out);

private:
    uint32_t gain_ = kMicDefaultGain;
    int32_t dc_q8_ = 0;
    uint32_t settle_ = 0;
};

// What the microphone keeps between reads: the newest kMicBufferSamples
class MicRing {
public:
    void push(const int16_t* samples, size_t n);
    size_t pop(int16_t* out, size_t max);
    size_t size() const { return size_; }
    void clear() { head_ = size_ = 0; }

private:
    int16_t buf_[kMicBufferSamples] = {};
    size_t head_ = 0, size_ = 0;   // head_: the oldest
};

// A mixer control as the card describes it
struct ControlInfo {
    enum class Type : uint8_t { Boolean, Integer, Integer64, Enumerated };
    Type type = Type::Integer;
    uint32_t count = 1;            // values: one per channel
    int64_t min = 0, max = 0;
    std::vector<std::string> items;
};

// A control's value as amixer cset takes it: one for every channel or one
// per channel, comma separated; on/off for a switch, a number or a
// percentage of the range for a volume, an item's name or index for an
// enumeration
bool parse_control_value(const ControlInfo& info, const std::string& text, std::vector<int64_t>& values, std::string& why);

struct PcmFormat {
    SampleFormat format = SampleFormat::S16_LE;
    uint32_t channels = 0;
    uint32_t rate = 0;
    uint32_t period_frames = 0;
    uint32_t buffer_frames = 0;
};

// An open PCM stream
class PcmStream {
public:
    virtual ~PcmStream() = default;
    virtual const PcmFormat& format() const = 0;
    // Frames written, waiting up to timeout_ms for room, or -errno. An
    // underrun is recovered inside.
    virtual int32_t write(const uint8_t* frames, uint32_t n, int timeout_ms) = 0;
    // Up to n frames read, waiting up to timeout_ms, or -errno. An overrun
    // is recovered inside.
    virtual int32_t read(uint8_t* frames, uint32_t n, int timeout_ms) = 0;
    // Starts playback that holds fewer frames than the start threshold
    virtual void start() {}
};

// A sound card's speaker and microphone streams
class AudioCard {
public:
    virtual ~AudioCard() = default;
    virtual bool has_playback() const = 0;
    virtual bool has_capture() const = 0;
    // A stream, or nullptr and err a negative errno (-EBUSY: another
    // process holds the device)
    virtual std::unique_ptr<PcmStream> open(bool capture, int& err) = 0;
};

// cfg's card through /dev/snd with cfg.controls set, or nullptr and why
std::unique_ptr<AudioCard> open_alsa_card(const LinuxAudioConfig& cfg, std::string& why);

}  // namespace linux_audio

// The Board audio methods for a Linux board. Return values are the
// Cardputer's: samples or HAL_OK, HAL_ERR_INVAL for a rate outside 8000 to
// 48000 Hz, HAL_ERR_BUSY when another call or another process has the
// device, HAL_ERR_IO when it fails, HAL_ERR_NODEV without the device.
class LinuxAudio {
public:
    explicit LinuxAudio(LinuxAudioConfig cfg);
    LinuxAudio(LinuxAudioConfig cfg, std::unique_ptr<linux_audio::AudioCard> card);
    ~LinuxAudio();
    LinuxAudio(const LinuxAudio&) = delete;
    LinuxAudio& operator=(const LinuxAudio&) = delete;

    // false: no audio on this board, and why
    bool open(std::string& why);
    bool has_speaker() const;
    bool has_microphone() const;

    int32_t tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct);
    // samples may sit at any address
    int32_t play(const int16_t* samples, uint32_t count, uint32_t rate, uint32_t volume_pct);
    int32_t mic_read(int16_t* out, uint32_t max_samples, uint32_t timeout_ms);
    void mic_set_gain(uint32_t gain);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kube32::hal
