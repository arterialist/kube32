#pragma once

// An ES8311 mono codec on I2S with a class-D amplifier behind an enable pin:
// the speaker and microphone of k32_tone, k32_audio_play and k32_mic_read,
// with the Cardputer's semantics (hal/tft_keyboard_hardware.hpp). The codec
// is set up once over I2C at init, for MCLK at 256 times the sample rate:
// that setup holds at every rate, so the I2C controller is free again for
// pods afterwards. The speaker and the microphone share one I2S controller
// and its clocks, so one of them runs at a time and the speaker comes first.
// A channel exists only while sound moves, and the amplifier is on only while
// the speaker's channel runs.

#include <cstdint>

#if defined(KUBE32_TARGET_ESP32)
#include "driver/i2s_types.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

namespace kube32::hal {

struct Es8311Pins {
    int i2c_sda;
    int i2c_scl;
    uint8_t i2c_addr;   // 7-bit
    int mclk;
    int bclk;
    int ws;
    int dout;           // to the codec's DAC (DSDIN)
    int din;            // from the codec's ADC (ASDOUT)
    int pa_enable;      // amplifier enable, high for on
};

class Es8311Audio {
public:
    explicit Es8311Audio(const Es8311Pins& pins) : pins_(pins) {}

    // Sets the codec up: false when it does not answer on I2C
    bool init();
    // Square-wave tone at volume_pct of the ceiling (2 s at most): HAL_OK,
    // HAL_ERR_BUSY, HAL_ERR_IO
    int32_t tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct);
    // k32_audio_play: the count queued, or a negative HAL_ERR_*
    int32_t play(const int16_t* samples, uint32_t count, uint32_t sample_rate, uint32_t volume_pct);
    // k32_mic_read: samples copied, or a negative HAL_ERR_*; max_samples 0
    // stops the microphone
    int32_t mic_read(int16_t* out, uint32_t max_samples, uint32_t timeout_ms);
    // Digital gain of mic_read after the DC offset is removed (1 to 64)
    void mic_set_gain(uint32_t gain);

private:
    Es8311Pins pins_;
#if defined(KUBE32_TARGET_ESP32)
    enum class Mode : uint8_t { Off, Speaker, Mic };

    bool codec_setup();
    void pins_idle();
    void off_locked();
    void arm_idle(uint64_t after_us);
    static void idle_cb(void* arg);
    bool speaker_on_locked(uint32_t rate);
    bool speaker_write_locked(uint32_t n);
    bool mic_on_locked();
    void mic_condition(int16_t* pcm, size_t n);
    int32_t lock(uint32_t timeout_ms);

    SemaphoreHandle_t mutex_{nullptr};
    esp_timer_handle_t idle_{nullptr};
    Mode mode_{Mode::Off};
    i2s_chan_handle_t tx_{nullptr};
    i2s_chan_handle_t rx_{nullptr};
    uint32_t tx_rate_{0};
    uint64_t play_until_us_{0};
    uint64_t mic_last_read_us_{0};
    uint32_t mic_gain_{1};
    int32_t mic_dc_q8_{0};
    uint32_t mic_settle_{0};
    int16_t scratch_[256]{};   // samples on their way to the speaker or from the microphone (under mutex_)
#endif
};

}  // namespace kube32::hal
