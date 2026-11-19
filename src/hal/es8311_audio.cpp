#include "hal/es8311_audio.hpp"

#if defined(KUBE32_TARGET_ESP32)

#include "hal/hal_types.hpp"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

#include <algorithm>
#include <cstring>

namespace kube32::hal {

namespace {

const char* TAG = "es8311";

// Every sample the amplifier receives is scaled to at most this percent of
// full scale (K32_SPEAKER_MAX_PCT)
constexpr uint32_t kSpeakerMaxPct = 25;
constexpr uint32_t kSpeakerDmaDescs = 6;
constexpr uint32_t kSpeakerDmaFrames = 256;
constexpr uint64_t kSpeakerTailUs = 30000;      // the amplifier stops this long after the last sample
constexpr uint32_t kMicDmaDescs = 4;
constexpr uint32_t kMicDmaFrames = 256;         // 64 ms at 16 kHz
constexpr uint32_t kMicRate = 16000;
constexpr uint64_t kMicIdleUs = 1000000;        // the microphone stops 1 s after the last read
// The ADC's high-pass filter removes most of the offset; what is left is
// tracked and removed as on the Cardputer, and the first 100 ms after the
// microphone starts read as silence while the ADC settles
constexpr int32_t kMicDcShift = 10;
constexpr int32_t kMicSettleShift = 6;
constexpr uint32_t kMicSettleSamples = 1600;
// Microphone gain in the codec: the PGA at 30 dB and the ADC's digital scale
// at 42 dB put a quiet room's floor at about 150 RMS
constexpr uint8_t kMicPga = 0x1A;               // REG14: analog input 1, PGA 30 dB
constexpr uint8_t kAdcScale = 0x07;             // REG16: 42 dB, in 6 dB steps

// The codec's registers in the order they are written: slave mode, MCLK from
// its pin at 256 times the sample rate (pre-divider and multiplier 1, ADC and
// DAC dividers 1, LRCK divider 256, oversampling 16), 16-bit I2S on both
// serial ports, the ADC on both slots, both converters powered and unmuted,
// the DAC at 0 dB
struct Reg {
    uint8_t addr;
    uint8_t value;
};
constexpr Reg kSetup[] = {
    {0x44, 0x08}, {0x44, 0x08},   // I2C noise filter; the first write after power-up may be lost
    {0x01, 0x30}, {0x02, 0x00}, {0x03, 0x10}, {0x16, kAdcScale}, {0x04, 0x10}, {0x05, 0x00},
    {0x0B, 0x00}, {0x0C, 0x00}, {0x10, 0x1F}, {0x11, 0x7F},
    {0x00, 0x80},                 // clock state machine on, slave
    {0x01, 0x3F},                 // every clock on, MCLK from the pin
    {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xFF},
    {0x13, 0x10}, {0x1B, 0x0A}, {0x1C, 0x6A},
    {0x09, 0x0C}, {0x0A, 0x0C},   // 16-bit I2S in and out, not muted
    {0x17, 0xBF}, {0x0E, 0x02}, {0x12, 0x00}, {0x14, kMicPga}, {0x0D, 0x01}, {0x15, 0x40},
    {0x37, 0x08}, {0x45, 0x00},
    {0x32, 0xBF}, {0x31, 0x00},   // DAC volume 0 dB, unmuted
};

}  // namespace

bool Es8311Audio::codec_setup() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = -1;
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(pins_.i2c_sda);
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(pins_.i2c_scl);
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "no I2C controller for the codec");
        return false;
    }
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = pins_.i2c_addr;
    dev_cfg.scl_speed_hz = 100000;
    i2c_master_dev_handle_t dev = nullptr;
    bool ok = i2c_master_bus_add_device(bus, &dev_cfg, &dev) == ESP_OK;
    // Chip ID registers 0xFD and 0xFE read 0x83 and 0x11
    uint8_t id[2] = {};
    for (uint8_t i = 0; ok && i < 2; ++i) {
        const uint8_t reg = static_cast<uint8_t>(0xFD + i);
        ok = i2c_master_transmit_receive(dev, &reg, 1, &id[i], 1, 100) == ESP_OK;   // timeout in ms
    }
    if (ok) {
        for (const Reg& r : kSetup) {
            const uint8_t buf[2] = {r.addr, r.value};
            if (i2c_master_transmit(dev, buf, sizeof(buf), 100) != ESP_OK && r.addr != 0x44) {
                ok = false;
                break;
            }
        }
    }
    if (dev) i2c_master_bus_rm_device(dev);
    i2c_del_master_bus(bus);
    if (!ok) {
        ESP_LOGE(TAG, "the codec does not answer at 0x%02x (SDA %d, SCL %d)", pins_.i2c_addr, pins_.i2c_sda, pins_.i2c_scl);
        return false;
    }
    ESP_LOGI(TAG, "codec at 0x%02x (id %02x%02x)", pins_.i2c_addr, id[0], id[1]);
    return true;
}

void Es8311Audio::pins_idle() {
    gpio_set_level(static_cast<gpio_num_t>(pins_.pa_enable), 0);
    for (int pin : {pins_.mclk, pins_.bclk, pins_.ws, pins_.dout}) {
        gpio_reset_pin(static_cast<gpio_num_t>(pin));
        gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT);
        gpio_set_level(static_cast<gpio_num_t>(pin), 0);
    }
}

bool Es8311Audio::init() {
    // The amplifier stays off until the speaker's channel runs
    gpio_reset_pin(static_cast<gpio_num_t>(pins_.pa_enable));
    gpio_set_direction(static_cast<gpio_num_t>(pins_.pa_enable), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(pins_.pa_enable), 0);
    if (!codec_setup()) return false;
    mutex_ = xSemaphoreCreateMutex();
    esp_timer_create_args_t args = {};
    args.callback = &Es8311Audio::idle_cb;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "k32_audio";
    if (!mutex_ || esp_timer_create(&args, &idle_) != ESP_OK) {
        ESP_LOGE(TAG, "audio: no mutex or timer");
        return false;
    }
    pins_idle();
    ESP_LOGI(TAG, "speaker and microphone on I2S (MCLK %d, BCLK %d, LRCK %d, DOUT %d, DIN %d), amplifier on GPIO %d, at most %u%% of full scale",
             pins_.mclk, pins_.bclk, pins_.ws, pins_.dout, pins_.din, pins_.pa_enable, (unsigned)kSpeakerMaxPct);
    return true;
}

void Es8311Audio::off_locked() {
    // The amplifier goes off before its input stops
    gpio_set_level(static_cast<gpio_num_t>(pins_.pa_enable), 0);
    if (tx_) {
        i2s_channel_disable(tx_);
        i2s_del_channel(tx_);
        tx_ = nullptr;
        tx_rate_ = 0;
    }
    if (rx_) {
        i2s_channel_disable(rx_);
        i2s_del_channel(rx_);
        rx_ = nullptr;
    }
    if (mode_ != Mode::Off) pins_idle();
    mode_ = Mode::Off;
}

void Es8311Audio::arm_idle(uint64_t after_us) {
    esp_timer_stop(idle_);
    esp_timer_start_once(idle_, after_us);
}

// Stops whichever channel has nothing left to do
void Es8311Audio::idle_cb(void* arg) {
    auto* self = static_cast<Es8311Audio*>(arg);
    if (xSemaphoreTake(self->mutex_, 0) != pdTRUE) {
        self->arm_idle(20000);   // busy: a call is moving samples
        return;
    }
    const uint64_t now = esp_timer_get_time();
    if (self->mode_ == Mode::Speaker) {
        if (now >= self->play_until_us_ + kSpeakerTailUs) self->off_locked();
        else self->arm_idle(self->play_until_us_ + kSpeakerTailUs - now);
    } else if (self->mode_ == Mode::Mic) {
        if (now >= self->mic_last_read_us_ + kMicIdleUs) self->off_locked();
        else self->arm_idle(self->mic_last_read_us_ + kMicIdleUs - now);
    }
    xSemaphoreGive(self->mutex_);
}

static i2s_std_gpio_config_t std_pins(const Es8311Pins& p, bool tx) {
    i2s_std_gpio_config_t g = {};
    g.mclk = static_cast<gpio_num_t>(p.mclk);
    g.bclk = static_cast<gpio_num_t>(p.bclk);
    g.ws = static_cast<gpio_num_t>(p.ws);
    g.dout = tx ? static_cast<gpio_num_t>(p.dout) : I2S_GPIO_UNUSED;
    g.din = tx ? I2S_GPIO_UNUSED : static_cast<gpio_num_t>(p.din);
    return g;
}

// The speaker's channel at `rate`, running, the amplifier on
bool Es8311Audio::speaker_on_locked(uint32_t rate) {
    if (mode_ == Mode::Mic) off_locked();
    if (tx_ && tx_rate_ == rate) return true;
    if (tx_) {
        // Another sample rate: the queued samples play out first
        const uint64_t now = esp_timer_get_time();
        if (play_until_us_ > now) vTaskDelay(pdMS_TO_TICKS((play_until_us_ - now) / 1000 + 1));
        off_locked();
    }
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = kSpeakerDmaDescs;
    chan_cfg.dma_frame_num = kSpeakerDmaFrames;
    // Zero a descriptor once the DMA has sent it: on underrun the ring
    // sends silence, not its stale contents
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &tx_, nullptr) != ESP_OK) {
        tx_ = nullptr;
        return false;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = std_pins(pins_, true),
    };
    if (i2s_channel_init_std_mode(tx_, &std_cfg) != ESP_OK || i2s_channel_enable(tx_) != ESP_OK) {
        i2s_del_channel(tx_);
        tx_ = nullptr;
        pins_idle();
        return false;
    }
    gpio_set_level(static_cast<gpio_num_t>(pins_.pa_enable), 1);
    tx_rate_ = rate;
    mode_ = Mode::Speaker;
    play_until_us_ = esp_timer_get_time();
    return true;
}

// Queues the samples in scratch_[0, n), already scaled
bool Es8311Audio::speaker_write_locked(uint32_t n) {
    size_t written = 0;
    if (i2s_channel_write(tx_, scratch_, n * sizeof(int16_t), &written, 1000) != ESP_OK) return false;   // timeout in ms
    const uint64_t now = esp_timer_get_time();
    const uint64_t ring_us = 1000000ull * kSpeakerDmaDescs * kSpeakerDmaFrames / tx_rate_;
    const uint64_t queued_us = 1000000ull * n / tx_rate_;
    // What is in the ring plays within its length from now
    play_until_us_ = std::min(std::max(play_until_us_, now) + queued_us, now + ring_us);
    return true;
}

int32_t Es8311Audio::lock(uint32_t timeout_ms) {
    if (!mutex_) return HAL_ERR_NODEV;
    return xSemaphoreTake(mutex_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? HAL_OK : HAL_ERR_BUSY;
}

int32_t Es8311Audio::play(const int16_t* samples, uint32_t count, uint32_t sample_rate, uint32_t volume_pct) {
    if (sample_rate < 8000 || sample_rate > 48000) return HAL_ERR_INVAL;
    if (count > sample_rate) count = sample_rate;   // one second per call
    if (volume_pct > 100) volume_pct = 100;
    if (count == 0) return 0;
    if (int32_t rc = lock(2000)) return rc;
    if (!speaker_on_locked(sample_rate)) {
        xSemaphoreGive(mutex_);
        return HAL_ERR_IO;
    }
    // Q15 gain: volume_pct of the ceiling
    const int32_t gain = static_cast<int32_t>((32768u * kSpeakerMaxPct * volume_pct) / 10000u);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(samples);
    bool ok = true;
    for (uint32_t done = 0; ok && done < count;) {
        const uint32_t n = std::min<uint32_t>(count - done, sizeof(scratch_) / sizeof(scratch_[0]));
        // The pod's buffer may sit at any address: copy it whole first
        std::memcpy(scratch_, src + 2 * done, n * sizeof(int16_t));
        for (uint32_t i = 0; i < n; ++i) scratch_[i] = static_cast<int16_t>((scratch_[i] * gain) >> 15);
        ok = speaker_write_locked(n);
        done += n;
    }
    arm_idle(play_until_us_ - std::min(play_until_us_, (uint64_t)esp_timer_get_time()) + kSpeakerTailUs);
    xSemaphoreGive(mutex_);
    return ok ? static_cast<int32_t>(count) : HAL_ERR_IO;
}

int32_t Es8311Audio::tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct) {
    if (freq_hz == 0 || duration_ms == 0 || volume_pct == 0) return HAL_OK;
    if (duration_ms > 2000) duration_ms = 2000;
    if (volume_pct > 100) volume_pct = 100;
    if (int32_t rc = lock(2000)) return rc;
    const uint32_t rate = 16000;
    if (!speaker_on_locked(rate)) {
        xSemaphoreGive(mutex_);
        return HAL_ERR_IO;
    }
    int period = static_cast<int>(rate / freq_hz);
    if (period < 2) period = 2;
    const int total = static_cast<int>((rate * duration_ms) / 1000);
    const int amplitude = static_cast<int>((32767u * kSpeakerMaxPct * volume_pct) / 10000u);
    int ramp = total / 8 > 64 ? 64 : total / 8;
    if (ramp < 1) ramp = 1;
    int phase = 0, produced = 0;
    bool ok = true;
    while (ok && produced < total) {
        const int n = std::min<int>(total - produced, sizeof(scratch_) / sizeof(scratch_[0]));
        for (int i = 0; i < n; ++i, ++produced) {
            int a = amplitude;
            if (produced < ramp) a = amplitude * produced / ramp;
            else if (total - produced < ramp) a = amplitude * (total - produced) / ramp;
            scratch_[i] = static_cast<int16_t>((phase < period / 2) ? a : -a);
            phase = (phase + 1) % period;
        }
        ok = speaker_write_locked(static_cast<uint32_t>(n));
    }
    arm_idle(play_until_us_ - std::min(play_until_us_, (uint64_t)esp_timer_get_time()) + kSpeakerTailUs);
    xSemaphoreGive(mutex_);
    return ok ? HAL_OK : HAL_ERR_IO;
}

bool Es8311Audio::mic_on_locked() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = kMicDmaDescs;
    chan_cfg.dma_frame_num = kMicDmaFrames;
    if (i2s_new_channel(&chan_cfg, nullptr, &rx_) != ESP_OK) {
        rx_ = nullptr;
        return false;
    }
    // Both slots: in mono mode the ESP32-S31 still stores both of them. The
    // codec puts its ADC on both and mic_read keeps the left one.
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kMicRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = std_pins(pins_, false),
    };
    if (i2s_channel_init_std_mode(rx_, &std_cfg) != ESP_OK || i2s_channel_enable(rx_) != ESP_OK) {
        i2s_del_channel(rx_);
        rx_ = nullptr;
        pins_idle();
        return false;
    }
    mode_ = Mode::Mic;
    mic_settle_ = kMicSettleSamples;
    return true;
}

void Es8311Audio::mic_condition(int16_t* pcm, size_t n) {
    const int32_t gain = static_cast<int32_t>(mic_gain_);
    int32_t dc = mic_dc_q8_;
    for (size_t i = 0; i < n; ++i) {
        const int32_t x = pcm[i];
        if (mic_settle_) {
            if (mic_settle_-- == kMicSettleSamples) dc = x << 8;
            dc += ((x << 8) - dc) >> kMicSettleShift;
            pcm[i] = 0;
            continue;
        }
        dc += ((x << 8) - dc) >> kMicDcShift;
        const int32_t y = (x - (dc >> 8)) * gain;
        pcm[i] = static_cast<int16_t>(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
    mic_dc_q8_ = dc;
}

void Es8311Audio::mic_set_gain(uint32_t gain) {
    mic_gain_ = gain < 1 ? 1 : gain > 64 ? 64 : gain;
}

int32_t Es8311Audio::mic_read(int16_t* out, uint32_t max_samples, uint32_t timeout_ms) {
    if (max_samples > kMicRate) max_samples = kMicRate;
    if (timeout_ms > 1000) timeout_ms = 1000;
    const uint64_t deadline = esp_timer_get_time() + 1000ull * timeout_ms;
    if (int32_t rc = lock(timeout_ms)) return rc;
    if (max_samples == 0) {
        if (mode_ == Mode::Mic) off_locked();
        xSemaphoreGive(mutex_);
        return 0;
    }
    // Playback keeps the controller until its samples have played
    while (mode_ == Mode::Speaker && esp_timer_get_time() < play_until_us_) {
        if (esp_timer_get_time() >= deadline) {
            xSemaphoreGive(mutex_);
            return HAL_ERR_BUSY;
        }
        xSemaphoreGive(mutex_);
        vTaskDelay(pdMS_TO_TICKS(10));
        if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return HAL_ERR_BUSY;
    }
    if (mode_ != Mode::Mic) {
        off_locked();
        if (!mic_on_locked()) {
            xSemaphoreGive(mutex_);
            return HAL_ERR_IO;
        }
    }
    // Stereo frames through scratch_: the left sample of each goes to `out`,
    // which may sit at any address
    uint32_t got = 0;
    esp_err_t err = ESP_OK;
    while (got < max_samples) {
        const uint64_t now = esp_timer_get_time();
        const uint32_t wait_ms = deadline > now ? static_cast<uint32_t>((deadline - now) / 1000) : 0;
        const uint32_t frames = std::min<uint32_t>(max_samples - got, sizeof(scratch_) / sizeof(scratch_[0]) / 2);
        size_t bytes = 0;
        err = i2s_channel_read(rx_, scratch_, frames * 2 * sizeof(int16_t), &bytes, wait_ms);   // timeout in ms
        const uint32_t n = static_cast<uint32_t>(bytes / (2 * sizeof(int16_t)));
        for (uint32_t i = 0; i < n; ++i) scratch_[i] = scratch_[2 * i];
        mic_condition(scratch_, n);
        std::memcpy(reinterpret_cast<uint8_t*>(out) + 2 * got, scratch_, n * sizeof(int16_t));
        got += n;
        if (err != ESP_OK) break;
    }
    mic_last_read_us_ = esp_timer_get_time();
    arm_idle(kMicIdleUs);
    xSemaphoreGive(mutex_);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) return HAL_ERR_IO;
    return static_cast<int32_t>(got);
}

}  // namespace kube32::hal

#endif  // KUBE32_TARGET_ESP32
