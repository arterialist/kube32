// The kit's on-board peripherals: 480x480 QSPI AMOLED and touch panel, ES8311
// speaker and microphone, driven through the vendor BSP. Private branch only.

#include "hal/board.hpp"

#if defined(KUBE32_TARGET_ESP32) && CONFIG_IDF_TARGET_ESP32S31

#include "bsp/display.h"
#include "bsp/esp_mosaico.h"
#include "bsp/audio.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace kube32::hal {

namespace {

const char* const TAG = "KitBoard";

#include "font_5x7.inc"

constexpr int kWidth = BSP_LCD_H_RES;
constexpr int kHeight = BSP_LCD_V_RES;
constexpr int kChunkRows = 16;
constexpr uint32_t kMicRate = 16000;
// Every sample reaches the amplifier at most at a quarter of full scale
constexpr int kSpeakerCeilingPct = 25;
constexpr int kSpeakerDefaultVolume = 40;   // the codec's output volume, 0-100

inline uint16_t swap16(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }

class KitBoard final : public Board {
public:
    const char* name() const override { return "esp-mosaico"; }

    bool init() override {
        if (bsp_i2c_init() != ESP_OK) ESP_LOGW(TAG, "I2C bus did not start");
        esp_lcd_panel_handle_t panel = nullptr;
        const bsp_display_config_t display_cfg = BSP_DISPLAY_DEFAULT_CONFIG();
        if (bsp_display_new(&display_cfg, &panel) != ESP_OK || !panel) {
            ESP_LOGE(TAG, "display did not start");
            return false;
        }
        panel_ = panel;
        bsp_display_on();
        bsp_display_brightness_set(60);
        chunk_ = static_cast<uint16_t*>(heap_caps_malloc(static_cast<size_t>(kWidth) * kChunkRows * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (!chunk_) {
            ESP_LOGE(TAG, "no DMA memory for the panel");
            return false;
        }
        done_ = xSemaphoreCreateBinary();
        const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = on_flush_done};
        esp_lcd_panel_io_register_event_callbacks(bsp_display_get_panel_io(), &cbs, done_);
        if (bsp_touch_new(BSP_LCD_ROTATION_DEFAULT, &touch_) != ESP_OK) ESP_LOGW(TAG, "touch panel did not start");
        if (bsp_audio_init(nullptr) == ESP_OK) {
            speaker_ = bsp_audio_codec_speaker_init();
            mic_ = bsp_audio_codec_microphone_init();
        }
        if (!speaker_) ESP_LOGW(TAG, "speaker did not start");
        if (!mic_) ESP_LOGW(TAG, "microphone did not start");
        return true;
    }

    void describe(BoardProfile& profile) const override {
        profile.name = name();
        // USB 33-34 (USB-Serial/JTAG), UART0 58-59, display, codec, NAND,
        // I2C0 56/3, touch and sensor interrupts, power control
        profile.reserved_gpio_mask = pins({3, 2, 6, 7, 8, 9, 20, 21, 22, 23, 24, 25, 33, 34, 35, 36, 37, 40, 42, 43, 44, 45, 49, 50, 51, 52, 54, 56, 57, 58, 59, 60, 61});
        // I2C0 is the board's own bus (touch, codec, sensors); pods claim
        // devices on it by address, not the controller. I2C1 is the header.
        profile.i2c_bus_mask = 0x2;
        profile.i2c_pins[0] = {};
        profile.i2c_pins[1] = {0, 1};
        profile.uart_port_mask = 0;
        profile.onboard_devices = KUBE32_CAP_DISPLAY | KUBE32_CAP_TOUCH | KUBE32_CAP_SPEAKER | KUBE32_CAP_MICROPHONE;
    }

    DisplayInfo display_info() const override { return {kWidth, kHeight, KUBE32_DISPLAY_RGB565}; }

    int32_t display_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) override {
        return fill(x, y, w, h, swap16(color));
    }

    int32_t display_bar(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pct, uint16_t fg, uint16_t bg) override {
        if (w <= 0 || h <= 0) return HAL_ERR_INVAL;
        const int32_t filled = static_cast<int32_t>(std::min<uint32_t>(pct, 100)) * w / 100;
        fill(x, y, filled, h, swap16(fg));
        return fill(x + filled, y, w - filled, h, swap16(bg));
    }

    int32_t display_text(int32_t x, int32_t y, const char* text, size_t len, uint16_t fg, uint16_t bg) override {
        // One glyph cell at a time, 6x8 pixels
        const uint16_t f = swap16(fg), b = swap16(bg);
        uint16_t cell[6 * 8];
        for (size_t i = 0; i < len; ++i) {
            const int32_t cx = x + static_cast<int32_t>(i) * 6;
            if (cx >= kWidth) break;
            const unsigned char ch = static_cast<unsigned char>(text[i]);
            const uint8_t* glyph = ch >= 32 && ch <= 126 ? FONT_5X7[ch - 32] : FONT_5X7[0];
            for (int row = 0; row < 8; ++row) {
                for (int col = 0; col < 6; ++col) {
                    const bool on = col < 5 && row < 7 && (glyph[col] & (1 << row));
                    cell[row * 6 + col] = on ? f : b;
                }
            }
            blit_raw(cx, y, 6, 8, cell);
        }
        return HAL_OK;
    }

    int32_t display_blit(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* pixels) override {
        if (w <= 0 || h <= 0) return HAL_ERR_INVAL;
        // Pod memory, any alignment: copied into the DMA chunk, byte-swapped
        for (int32_t row = 0; row < h;) {
            const int32_t n = std::min<int32_t>(h - row, kChunkRows);
            std::lock_guard<std::mutex> lock(mutex_);
            int32_t out = 0;
            for (int32_t r = 0; r < n; ++r) {
                for (int32_t c = 0; c < w && c < kWidth; ++c) {
                    uint16_t v;
                    std::memcpy(&v, reinterpret_cast<const uint8_t*>(pixels) + (row + r) * w * 2 + c * 2, 2);
                    chunk_[out++] = swap16(v);
                }
            }
            flush(x, y + row, std::min<int32_t>(w, kWidth), n);
            row += n;
        }
        return HAL_OK;
    }

    int32_t touch_read(TouchPoint* out, uint32_t max) override {
        if (!touch_) return HAL_ERR_NODEV;
        if (esp_lcd_touch_read_data(touch_) != ESP_OK) return HAL_ERR_IO;
        uint16_t x[10], y[10], strength[10];
        uint8_t count = 0;
        const uint8_t want = static_cast<uint8_t>(std::min<uint32_t>(max, 10));
        esp_lcd_touch_get_coordinates(touch_, x, y, strength, &count, want);
        for (uint8_t i = 0; i < count; ++i) out[i] = {x[i], y[i], i, strength[i]};
        return count;
    }

    int32_t tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct) override {
        if (!speaker_) return HAL_ERR_NODEV;
        static const uint32_t kRate = 16000;
        const uint32_t samples = std::min<uint32_t>(kRate * duration_ms / 1000, kRate);
        std::unique_ptr<int16_t[]> buf(new (std::nothrow) int16_t[samples]);
        if (!buf) return HAL_ERR_NOMEM;
        const uint32_t half_period = freq_hz ? kRate / (2 * freq_hz) : kRate;
        const int16_t level = 12000;
        for (uint32_t i = 0; i < samples; ++i) buf[i] = ((i / (half_period ? half_period : 1)) & 1) ? level : -level;
        return audio_play(buf.get(), samples, kRate, volume_pct) >= 0 ? HAL_OK : HAL_ERR_IO;
    }

    int32_t audio_play(const int16_t* samples, uint32_t count, uint32_t sample_rate, uint32_t volume_pct) override {
        if (!speaker_) return HAL_ERR_NODEV;
        if (sample_rate < 8000 || sample_rate > 48000 || count > sample_rate) return HAL_ERR_INVAL;
        std::lock_guard<std::mutex> lock(audio_mutex_);
        if (!open_output(sample_rate)) return HAL_ERR_IO;
        const uint32_t scale = (std::min<uint32_t>(volume_pct, 100) * kSpeakerCeilingPct) / 100;
        int16_t buf[256];
        for (uint32_t done = 0; done < count;) {
            const uint32_t n = std::min<uint32_t>(count - done, 256);
            for (uint32_t i = 0; i < n; ++i) {
                int16_t s;
                std::memcpy(&s, reinterpret_cast<const uint8_t*>(samples) + (done + i) * 2, 2);
                buf[i] = static_cast<int16_t>((static_cast<int32_t>(s) * static_cast<int32_t>(scale)) / 100);
            }
            if (esp_codec_dev_write(speaker_, buf, static_cast<int>(n * 2)) != ESP_CODEC_DEV_OK) return HAL_ERR_IO;
            done += n;
        }
        return static_cast<int32_t>(count);
    }

    int32_t mic_read(int16_t* out, uint32_t max_samples, uint32_t timeout_ms) override {
        (void)timeout_ms;
        if (!mic_) return HAL_ERR_NODEV;
        std::lock_guard<std::mutex> lock(audio_mutex_);
        if (!open_input()) return HAL_ERR_IO;
        if (max_samples == 0) {
            esp_codec_dev_close(mic_);
            input_open_ = false;
            return 0;
        }
        if (esp_codec_dev_read(mic_, out, static_cast<int>(max_samples * 2)) != ESP_CODEC_DEV_OK) return HAL_ERR_IO;
        return static_cast<int32_t>(max_samples);
    }

private:
    static uint64_t pins(std::initializer_list<int> list) {
        uint64_t mask = 0;
        for (int p : list) mask |= 1ULL << p;
        return mask;
    }

    static bool on_flush_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(ctx), &woken);
        return woken == pdTRUE;
    }

    void flush(int32_t x, int32_t y, int32_t w, int32_t rows) {
        // The panel's frame stays inside the screen; the driver copies nothing,
        // so the chunk may only change after its transfer is done
        const int32_t x0 = std::max<int32_t>(x, 0), y0 = std::max<int32_t>(y, 0);
        const int32_t x1 = std::min<int32_t>(x + w, kWidth), y1 = std::min<int32_t>(y + rows, kHeight);
        if (x1 <= x0 || y1 <= y0) return;
        const uint16_t* data = chunk_ + (y0 - y) * w + (x0 - x);
        if (esp_lcd_panel_draw_bitmap(panel_, x0, y0, x1, y1, data) == ESP_OK) xSemaphoreTake(done_, pdMS_TO_TICKS(100));
    }

    int32_t fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t swapped) {
        if (w <= 0 || h <= 0 || !panel_) return w == 0 || h == 0 ? HAL_OK : HAL_ERR_INVAL;
        const int32_t x0 = std::max<int32_t>(x, 0), x1 = std::min<int32_t>(x + w, kWidth);
        if (x1 <= x0) return HAL_OK;
        std::lock_guard<std::mutex> lock(mutex_);
        for (int32_t row = std::max<int32_t>(y, 0); row < std::min<int32_t>(y + h, kHeight);) {
            const int32_t n = std::min<int32_t>(kChunkRows, std::min<int32_t>(y + h, kHeight) - row);
            const int32_t width = x1 - x0;
            for (int32_t i = 0; i < width * n; ++i) chunk_[i] = swapped;
            if (esp_lcd_panel_draw_bitmap(panel_, x0, row, x1, row + n, chunk_) == ESP_OK) xSemaphoreTake(done_, pdMS_TO_TICKS(100));
            row += n;
        }
        return HAL_OK;
    }

    void blit_raw(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* swapped) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::memcpy(chunk_, swapped, static_cast<size_t>(w) * h * 2);
        flush(x, y, w, h);
    }

    bool open_output(uint32_t rate) {
        if (input_open_) {
            esp_codec_dev_close(mic_);
            input_open_ = false;
        }
        if (output_rate_ == rate) return true;
        if (output_rate_) esp_codec_dev_close(speaker_);
        esp_codec_dev_sample_info_t fs = {};
        fs.bits_per_sample = 16;
        fs.channel = 1;
        fs.sample_rate = rate;
        if (esp_codec_dev_open(speaker_, &fs) != ESP_CODEC_DEV_OK) {
            output_rate_ = 0;
            return false;
        }
        esp_codec_dev_set_out_vol(speaker_, kSpeakerDefaultVolume);
        output_rate_ = rate;
        return true;
    }

    bool open_input() {
        if (input_open_) return true;
        if (output_rate_) {
            esp_codec_dev_close(speaker_);
            output_rate_ = 0;
        }
        esp_codec_dev_sample_info_t fs = {};
        fs.bits_per_sample = 16;
        fs.channel = 1;
        fs.sample_rate = kMicRate;
        if (esp_codec_dev_open(mic_, &fs) != ESP_CODEC_DEV_OK) return false;
        input_open_ = true;
        return true;
    }

    esp_lcd_panel_handle_t panel_{nullptr};
    uint16_t* chunk_{nullptr};
    SemaphoreHandle_t done_{nullptr};
    std::mutex mutex_;
    esp_lcd_touch_handle_t touch_{nullptr};
    esp_codec_dev_handle_t speaker_{nullptr};
    esp_codec_dev_handle_t mic_{nullptr};
    std::mutex audio_mutex_;
    uint32_t output_rate_{0};
    bool input_open_{false};
};

}  // namespace

Board* kit_board() {
    static KitBoard board;
    return &board;
}

}  // namespace kube32::hal

#endif  // S31
