#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace kube32::hal {

// Real macOS speaker playback through CoreAudio AudioQueue (AudioToolbox).
// Non-blocking: tone() and play() resample to 48 kHz and append a voice to
// a thread-safe polyphonic mixer so overlapping strums/chords mix smoothly
// without stalling the Wasm or HTTP event loops.
class MacAudio {
public:
    MacAudio();
    ~MacAudio();
    MacAudio(const MacAudio&) = delete;
    MacAudio& operator=(const MacAudio&) = delete;

    bool open(std::string& why);
    void close();
    bool is_open() const;

    int32_t tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct);
    // samples may sit at any alignment in guest linear memory
    int32_t play(const int16_t* samples, uint32_t count, uint32_t sample_rate, uint32_t volume_pct);

    uint64_t played_calls() const;
    uint64_t played_samples() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kube32::hal
