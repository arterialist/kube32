#if defined(__APPLE__)

#include "mac_audio.hpp"
#include "hal/hal_types.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace kube32::hal {

namespace {

constexpr uint32_t kOutRate = 48000;
constexpr uint32_t kNumBuffers = 3;
constexpr uint32_t kBufferFrames = 256;  // ~5.33 ms per buffer, ~10-16 ms total ring
constexpr size_t kMaxVoices = 12;
constexpr float kMasterGain = 0.80f;
constexpr double kTwoPi = 6.28318530717958647692;

struct Voice {
    std::vector<float> samples;
    size_t pos{0};
};

}  // namespace

struct MacAudio::Impl {
    AudioQueueRef queue{nullptr};
    AudioQueueBufferRef buffers[kNumBuffers]{};

    // Lifecycle lock (open / ensure_started / close). Never acquired in the
    // CoreAudio output callback so AudioQueueStop(queue, true) cannot deadlock.
    std::mutex life_mutex;
    bool opened{false};
    bool started{false};
    std::atomic<bool> closing{false};

    // Voice mixer lock (shared between play/tone and the CoreAudio callback).
    std::mutex mix_mutex;
    std::vector<Voice> voices;

    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> samples_played{0};

    void fill_buffer(AudioQueueBufferRef buf) {
        auto* out = static_cast<int16_t*>(buf->mAudioData);
        float mix[kBufferFrames];
        std::memset(mix, 0, sizeof(mix));

        {
            std::lock_guard<std::mutex> lk(mix_mutex);
            size_t i = 0;
            while (i < voices.size()) {
                Voice& v = voices[i];
                const size_t avail = v.samples.size() - v.pos;
                const size_t n = std::min<size_t>(avail, kBufferFrames);
                const float* src = v.samples.data() + v.pos;
                for (size_t k = 0; k < n; ++k) {
                    mix[k] += src[k];
                }
                v.pos += n;
                if (v.pos >= v.samples.size()) {
                    voices.erase(voices.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
        }

        for (uint32_t k = 0; k < kBufferFrames; ++k) {
            // Smooth soft clipping so overlapping strums never harsh-clip
            const float s = std::tanh(mix[k]);
            int32_t v = static_cast<int32_t>(std::lrintf(s * 32767.0f));
            if (v > 32767) v = 32767;
            else if (v < -32767) v = -32767;
            out[k] = static_cast<int16_t>(v);
        }
        buf->mAudioDataByteSize = kBufferFrames * sizeof(int16_t);
    }

    static void output_callback(void* user_data, AudioQueueRef aq, AudioQueueBufferRef buf) {
        auto* self = static_cast<Impl*>(user_data);
        if (!self || self->closing.load(std::memory_order_acquire)) return;

        self->fill_buffer(buf);

        if (!self->closing.load(std::memory_order_acquire)) {
            AudioQueueEnqueueBuffer(aq, buf, 0, nullptr);
        }
    }

    bool ensure_started() {
        std::lock_guard<std::mutex> lk(life_mutex);
        if (!opened || !queue || closing.load(std::memory_order_acquire)) return false;
        if (started) return true;

        for (uint32_t i = 0; i < kNumBuffers; ++i) {
            fill_buffer(buffers[i]);
            const OSStatus eq = AudioQueueEnqueueBuffer(queue, buffers[i], 0, nullptr);
            if (eq != noErr) {
                std::fprintf(stderr, "[AUDIO] macOS AudioQueueEnqueueBuffer failed: %d\n", static_cast<int>(eq));
                return false;
            }
        }
        const OSStatus st = AudioQueueStart(queue, nullptr);
        if (st != noErr) {
            std::fprintf(stderr, "[AUDIO] macOS AudioQueueStart failed: %d\n", static_cast<int>(st));
            return false;
        }
        started = true;
        return true;
    }

    void push_voice(Voice&& v) {
        std::lock_guard<std::mutex> lk(mix_mutex);
        if (voices.size() >= kMaxVoices) {
            voices.erase(voices.begin());
        }
        voices.push_back(std::move(v));
    }
};

MacAudio::MacAudio() : impl_(std::make_unique<Impl>()) {}

MacAudio::~MacAudio() {
    close();
}

bool MacAudio::open(std::string& why) {
    std::lock_guard<std::mutex> lk(impl_->life_mutex);
    if (impl_->opened) return true;

    impl_->closing.store(false, std::memory_order_release);

    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = static_cast<Float64>(kOutRate);
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    fmt.mFramesPerPacket = 1;
    fmt.mChannelsPerFrame = 1;
    fmt.mBitsPerChannel = 16;
    fmt.mBytesPerFrame = sizeof(int16_t);
    fmt.mBytesPerPacket = sizeof(int16_t);

    OSStatus err = AudioQueueNewOutput(&fmt, &Impl::output_callback, impl_.get(), nullptr, nullptr, 0, &impl_->queue);
    if (err != noErr || !impl_->queue) {
        why = "AudioQueueNewOutput failed (OSStatus " + std::to_string(static_cast<int>(err)) + ")";
        impl_->queue = nullptr;
        return false;
    }

    const UInt32 bytes = kBufferFrames * sizeof(int16_t);
    for (uint32_t i = 0; i < kNumBuffers; ++i) {
        err = AudioQueueAllocateBuffer(impl_->queue, bytes, &impl_->buffers[i]);
        if (err != noErr || !impl_->buffers[i]) {
            why = "AudioQueueAllocateBuffer failed (OSStatus " + std::to_string(static_cast<int>(err)) + ")";
            AudioQueueDispose(impl_->queue, true);
            impl_->queue = nullptr;
            for (uint32_t j = 0; j < kNumBuffers; ++j) impl_->buffers[j] = nullptr;
            return false;
        }
    }

    impl_->opened = true;
    impl_->started = false;
    return true;
}

void MacAudio::close() {
    impl_->closing.store(true, std::memory_order_release);
    AudioQueueRef q = nullptr;
    {
        std::lock_guard<std::mutex> lk(impl_->life_mutex);
        q = impl_->queue;
        impl_->queue = nullptr;
        impl_->opened = false;
        impl_->started = false;
        for (uint32_t i = 0; i < kNumBuffers; ++i) impl_->buffers[i] = nullptr;
    }
    if (q) {
        AudioQueueStop(q, true);
        AudioQueueDispose(q, true);
    }
    {
        std::lock_guard<std::mutex> lk(impl_->mix_mutex);
        impl_->voices.clear();
    }
}

bool MacAudio::is_open() const {
    std::lock_guard<std::mutex> lk(impl_->life_mutex);
    return impl_->opened;
}

int32_t MacAudio::tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct) {
    if (freq_hz == 0 || duration_ms == 0 || volume_pct == 0) return HAL_OK;
    if (!is_open()) return HAL_ERR_NODEV;

    if (duration_ms > 2000) duration_ms = 2000;
    if (volume_pct > 100) volume_pct = 100;

    const size_t total = static_cast<size_t>(kOutRate) * duration_ms / 1000;
    if (total == 0) return HAL_OK;

    const float amp = (static_cast<float>(volume_pct) / 100.0f) * 0.55f;
    const size_t ramp = std::min<size_t>(total / 6, kOutRate * 5 / 1000);  // 5 ms de-click ramp

    Voice v;
    v.samples.resize(total);
    for (size_t i = 0; i < total; ++i) {
        float env = 1.0f;
        if (ramp > 0 && i < ramp) {
            env = static_cast<float>(i) / static_cast<float>(ramp);
        } else if (ramp > 0 && total - 1 - i < ramp) {
            env = static_cast<float>(total - 1 - i) / static_cast<float>(ramp);
        }
        const double phase = kTwoPi * static_cast<double>(freq_hz) * static_cast<double>(i) / static_cast<double>(kOutRate);
        // Clean sine with gentle 2nd harmonic warmth
        const float wave = static_cast<float>(0.85 * std::sin(phase) + 0.15 * std::sin(2.0 * phase));
        v.samples[i] = amp * env * wave;
    }

    impl_->push_voice(std::move(v));
    if (!impl_->ensure_started()) return HAL_ERR_IO;
    impl_->calls.fetch_add(1, std::memory_order_relaxed);
    impl_->samples_played.fetch_add(total, std::memory_order_relaxed);
    return HAL_OK;
}

int32_t MacAudio::play(const int16_t* samples, uint32_t count, uint32_t sample_rate, uint32_t volume_pct) {
    if (sample_rate < 8000 || sample_rate > 48000) return HAL_ERR_INVAL;
    if (count == 0) return 0;
    if (count > sample_rate) count = sample_rate;
    if (volume_pct > 100) volume_pct = 100;
    if (!samples) return HAL_ERR_FAULT;
    if (!is_open()) return HAL_ERR_NODEV;

    // Copy from possibly unaligned guest linear memory
    std::vector<int16_t> raw(count);
    std::memcpy(raw.data(), samples, static_cast<size_t>(count) * sizeof(int16_t));

    const float scale = (static_cast<float>(volume_pct) / 100.0f) * (kMasterGain / 32768.0f);
    const size_t out_n = static_cast<size_t>((static_cast<uint64_t>(count) * kOutRate + sample_rate - 1) / sample_rate);

    Voice v;
    v.samples.resize(out_n);

    if (sample_rate == kOutRate) {
        for (size_t i = 0; i < out_n; ++i) {
            v.samples[i] = static_cast<float>(raw[i]) * scale;
        }
    } else {
        const double step = static_cast<double>(sample_rate) / static_cast<double>(kOutRate);
        for (size_t i = 0; i < out_n; ++i) {
            const double src_pos = static_cast<double>(i) * step;
            const size_t idx = static_cast<size_t>(src_pos);
            const float frac = static_cast<float>(src_pos - static_cast<double>(idx));
            const float s0 = static_cast<float>(raw[std::min<size_t>(idx, count - 1)]);
            const float s1 = static_cast<float>(raw[std::min<size_t>(idx + 1, count - 1)]);
            v.samples[i] = (s0 + frac * (s1 - s0)) * scale;
        }
    }

    // Subtle 1 ms fade-out at tail to avoid clicks if a buffer ends non-zero
    const size_t fade = std::min<size_t>(out_n / 8, kOutRate / 1000);
    for (size_t i = 0; i < fade; ++i) {
        const float w = static_cast<float>(fade - i) / static_cast<float>(fade);
        v.samples[out_n - fade + i] *= w;
    }

    impl_->push_voice(std::move(v));
    if (!impl_->ensure_started()) return HAL_ERR_IO;
    impl_->calls.fetch_add(1, std::memory_order_relaxed);
    impl_->samples_played.fetch_add(count, std::memory_order_relaxed);
    return static_cast<int32_t>(count);
}

uint64_t MacAudio::played_calls() const {
    return impl_->calls.load(std::memory_order_relaxed);
}

uint64_t MacAudio::played_samples() const {
    return impl_->samples_played.load(std::memory_order_relaxed);
}

}  // namespace kube32::hal

#endif  // defined(__APPLE__)
