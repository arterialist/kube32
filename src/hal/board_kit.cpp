// ESP-Mosaico CoreBoard HAL: 480x480 QSPI CO5300 AMOLED display, CST9220 touch,
// ES8311 speaker/microphone, onboard I2C0 sensors (BMI270, dual BMM150, BQ27220),
// vibration motor (GPIO 8), AI button (GPIO 7), subboard power & I2C1 sharing,
// and expansion module bridge (Camera + Interaction modules).

#include "hal/board.hpp"

#if defined(KUBE32_TARGET_ESP32) && CONFIG_IDF_TARGET_ESP32S31

#include "hal/mosaico_modules.hpp"
#include "hal/pod_buses.hpp"

#include "bsp/audio.h"
#include "bsp/display.h"
#include "bsp/esp_mosaico.h"
#include "bsp/motor.h"
#include "bsp/power.h"
#include "bsp/subboard.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_system.h"

extern "C" uint8_t esp_lcd_touch_cst9220_last_reported_points(esp_lcd_touch_handle_t tp);
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace kube32::hal {

Board* kit_board();

namespace {

const char* const TAG = "KitBoard";

#include "font_5x7.inc"

constexpr int kWidth = BSP_LCD_H_RES;
constexpr int kHeight = BSP_LCD_V_RES;
// Must stay <= BSP_LCD_DMA_CHUNK_LINES (8) so every transfer fits in one QSPI
// DMA chunk within spi_bus_initialize's max_transfer_sz (480 * 8 * 2 = 7680 B).
constexpr int kChunkRows = 8;
constexpr uint32_t kMicRate = 16000;
// Safety ceiling: every sample reaches the amplifier at most at 25% of full scale
// with the ES8311 output volume at 40. Never raise this ceiling.
constexpr uint32_t kSpeakerCeilingPct = 25;
constexpr int kSpeakerDefaultVolume = 40;
constexpr float kMicInputGainDb = 30.0f;
constexpr int32_t kMicDcShift = 10;
constexpr int32_t kMicSettleShift = 6;
constexpr uint32_t kMicSettleSamples = 1600;

inline uint16_t swap16(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }

// Non-destructive I2C register read through the BSP's I2C0 master bus handle.
bool i2c0_read_reg(i2c_master_bus_handle_t bus, uint8_t addr, uint8_t reg, uint8_t* out, size_t len) {
    if (!bus) return false;
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = addr;
    dc.scl_speed_hz = 400000;
    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK || !dev) return false;
    const esp_err_t e = i2c_master_transmit_receive(dev, &reg, 1, out, len, 100);
    i2c_master_bus_rm_device(dev);
    return e == ESP_OK;
}

bool i2c0_write_reg(i2c_master_bus_handle_t bus, uint8_t addr, uint8_t reg, uint8_t val) {
    if (!bus) return false;
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = addr;
    dc.scl_speed_hz = 400000;
    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK || !dev) return false;
    const uint8_t buf[2] = {reg, val};
    const esp_err_t e = i2c_master_transmit(dev, buf, sizeof(buf), 100);
    i2c_master_bus_rm_device(dev);
    return e == ESP_OK;
}

class KitBoard final : public Board {
public:
    const char* name() const override { return "esp-mosaico"; }

    bool init() override {
        if (initialized_) return panel_ != nullptr;

        // 1. Detect V1.0 vs V1.2 from eFuse USER_DATA before touching buses/pins.
        if (bsp_board_variant_get(&variant_) == ESP_OK) {
            variant_ok_ = true;
            ESP_LOGI(TAG, "ESP-Mosaico variant: %s", variant_ == BSP_BOARD_VARIANT_V1_0 ? "v1.0" : "v1.2");
        } else {
            variant_ = BSP_BOARD_VARIANT_V1_2;
            ESP_LOGW(TAG, "eFuse board variant not programmed; assuming v1.2");
        }

        // 2. Power rails: initialize GPIO 60 (VCC_3V3_CTRL, active-low) and ramp up
        //    so display, touch, codec, onboard sensors, and left/right slots power up cleanly.
        if (bsp_power_init() != ESP_OK) {
            ESP_LOGW(TAG, "bsp_power_init failed");
        }
        if (bsp_power_set_vcc_3v3(true) != ESP_OK) {
            ESP_LOGW(TAG, "bsp_power_set_vcc_3v3(true) failed");
        }

        // 3. Onboard I2C0 (V1.2: GPIO 56/3; V1.0: GPIO 0/1). Borrow handle into
        //    pod_buses so kube32 discovery and pods share the BSP bus without conflicts.
        if (bsp_i2c_init() == ESP_OK && bsp_i2c_get_handle()) {
            i2c0_ready_ = true;
            set_board_i2c_bus(0, bsp_i2c_get_handle(), 400000);
            probe_onboard_sensors();
        } else {
            ESP_LOGW(TAG, "I2C0 bus did not start");
        }

        // 4. 480x480 CO5300 QSPI AMOLED display (SPI2_HOST).
        esp_lcd_panel_handle_t panel = nullptr;
        const bsp_display_config_t display_cfg = BSP_DISPLAY_DEFAULT_CONFIG();
        if (bsp_display_new(&display_cfg, &panel) == ESP_OK && panel) {
            panel_ = panel;
            bsp_display_on();
            bsp_display_brightness_set(60);
            chunk_ = static_cast<uint16_t*>(heap_caps_malloc(
                static_cast<size_t>(kWidth) * kChunkRows * sizeof(uint16_t),
                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            done_ = xSemaphoreCreateBinary();
            if (chunk_ && done_) {
                const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = on_flush_done};
                esp_lcd_panel_io_register_event_callbacks(bsp_display_get_panel_io(), &cbs, done_);
            } else {
                ESP_LOGE(TAG, "no DMA memory or semaphore for display");
                panel_ = nullptr;
            }
        } else {
            ESP_LOGE(TAG, "CO5300 display did not start");
        }

        // 5. CST9220 capacitive touch on I2C0 (0x5A, INT GPIO 6).
        if (bsp_touch_new(BSP_LCD_ROTATION_DEFAULT, &touch_) != ESP_OK) {
            ESP_LOGW(TAG, "CST9220 touch panel did not start");
            touch_ = nullptr;
        }

        // 6. ES8311 speaker & microphone (I2C0 0x19, I2S0 BCK=37 DOUT=52 DIN=40 WS=49 MCLK=54, PA=45).
        if (bsp_audio_init(nullptr) == ESP_OK) {
            speaker_ = bsp_audio_codec_speaker_init();
            mic_ = bsp_audio_codec_microphone_init();
        }
        if (!speaker_) ESP_LOGW(TAG, "ES8311 speaker did not start");
        if (!mic_) ESP_LOGW(TAG, "ES8311 microphone did not start");

        // 7. Vibration motor (GPIO 8, active-high MOSFET) & AI button (GPIO 7, active-low).
        if (bsp_motor_init() == ESP_OK) {
            bsp_motor_set(false);
            motor_ready_ = true;
        } else {
            ESP_LOGW(TAG, "vibration motor GPIO %d did not init", static_cast<int>(BSP_MOTOR_GPIO));
        }
        {
            gpio_config_t btn = {};
            btn.pin_bit_mask = (1ULL << BSP_BUTTON_AI_GPIO);
            btn.mode = GPIO_MODE_INPUT;
            btn.pull_up_en = GPIO_PULLUP_ENABLE;
            btn.pull_down_en = GPIO_PULLDOWN_DISABLE;
            btn.intr_type = GPIO_INTR_DISABLE;
            if (gpio_config(&btn) == ESP_OK) {
                ai_button_ready_ = true;
            }
        }

        // 8. Subboard power/address-select & I2C1 (V1.2: GPIO 0/1) + expansion module discovery.
        //    I2C1 stays exclusively owned by mosaico_module_mgr + Camera SCCB.
        if (bsp_subboard_init() != ESP_OK) {
            ESP_LOGW(TAG, "bsp_subboard_init failed");
        }
        mosaico_modules_init();

        // 9. Register shutdown handler so every esp_restart() cleanly drains LCD/camera DMA,
        //    silences speaker PA, and stops vibration motor before reset.
        if (!shutdown_registered_) {
            esp_register_shutdown_handler(on_esp_shutdown);
            shutdown_registered_ = true;
        }

        initialized_ = true;
        return panel_ != nullptr;
    }

    void shutdown() override {
        if (shutting_down_.exchange(true)) return;
        // Stop expansion modules first (camera DVP DMA + interaction LEDs).
        mosaico_modules_shutdown();
        // Turn off vibration motor.
        if (motor_ready_) bsp_motor_set(false);
        // Stop audio and mute NS4150B amplifier.
        {
            std::lock_guard<std::mutex> lock(audio_mutex_);
            if (input_open_ && mic_) {
                esp_codec_dev_close(mic_);
                input_open_ = false;
            }
            if (output_rate_ && speaker_) {
                esp_codec_dev_close(speaker_);
                output_rate_ = 0;
            }
            gpio_set_level(BSP_AUDIO_PA_CTRL, 0);
        }
        // Drain any in-flight CO5300 SPI2 DMA transfer and put display to sleep.
        if (panel_) {
            std::lock_guard<std::mutex> lock(mutex_);
            bsp_display_off();
        }
    }

    void describe(BoardProfile& profile) const override {
        profile.name = name();
        // Internal CoreBoard pins:
        //   SENSOR_INT 2, TOUCH_INT 6, CO5300 QSPI 9/35/36/42/43/44/50/51,
        //   SPI3 NAND 20-25, USJ 33-34, ES8311 I2S+PA 37/40/45/49/52/54,
        //   POWER_SWITCH 57, UART0 58-59, VCC_3V3_CTRL 60, Boot 61,
        //   Subboard EEPROM A0 selects 14 (left) and 39 (right).
        // Right-slot Interaction GPIOs (5, 10, 11, 38, 46, 47) and vibration motor
        // GPIO 8 remain unreserved here; attached modules reserve their active pins
        // via mosaico_modules_describe() below.
        uint64_t reserved = pins({
            2, 6, 9, 14, 20, 21, 22, 23, 24, 25,
            33, 34, 35, 36, 37, 39, 40, 42, 43, 44, 45,
            49, 50, 51, 52, 54, 57, 58, 59, 60, 61,
        });
        // I2C0 (V1.2: 56/3, V1.0: 0/1) is shared with kube32 for onboard sensors
        // (BMI270 @ 0x69, BMM150 @ 0x11/0x12, BQ27220 @ 0x55). On V1.2, I2C1
        // (GPIO 0/1) is owned exclusively by mosaico_module_mgr + Camera SCCB
        // and omitted from i2c_bus_mask so I2cDiscovery never contends with it.
        reserved |= pins({0, 1, 3, 56});
        profile.i2c_bus_mask = i2c0_ready_ ? 0x1 : 0;
        profile.i2c_fixed_mask = profile.i2c_bus_mask;
        profile.i2c_pins[0] = (variant_ == BSP_BOARD_VARIANT_V1_0)
                                  ? BusPins{BSP_I2C_SDA_V1_0, BSP_I2C_SCL_V1_0}
                                  : BusPins{BSP_I2C_SDA_V1_2, BSP_I2C_SCL_V1_2};
        profile.i2c_pins[1] = {};
        profile.i2c_reserved[0].set(0x19);  // ES8311 codec
        profile.i2c_reserved[0].set(0x5A);  // CST9220 touch
        if (variant_ == BSP_BOARD_VARIANT_V1_0) {
            profile.i2c_reserved[0].set(BSP_SUBBOARD_EEPROM_ADDR_LEFT);
            profile.i2c_reserved[0].set(BSP_SUBBOARD_EEPROM_ADDR_RIGHT);
        }
        profile.reserved_gpio_mask = reserved;
        // AI button (GPIO 7) pulls to GND when pressed: allow input-only claims safely.
        profile.input_only_gpio_mask = (1ULL << BSP_BUTTON_AI_GPIO);
        profile.uart_port_mask = 0;

        if (!initialized_) {
            profile.onboard_devices = KUBE32_CAP_DISPLAY | KUBE32_CAP_TOUCH |
                                      KUBE32_CAP_SPEAKER | KUBE32_CAP_MICROPHONE |
                                      KUBE32_CAP_IMU | KUBE32_CAP_MOTOR;
        } else {
            uint32_t devs = 0;
            if (panel_) devs |= KUBE32_CAP_DISPLAY;
            if (touch_) devs |= KUBE32_CAP_TOUCH;
            if (speaker_) devs |= KUBE32_CAP_SPEAKER;
            if (mic_) devs |= KUBE32_CAP_MICROPHONE;
            if (imu_present_) devs |= KUBE32_CAP_IMU;
            if (motor_ready_) devs |= KUBE32_CAP_MOTOR;
            profile.onboard_devices = devs;
            mosaico_modules_describe(profile);
        }
    }

    void extra_labels(std::map<std::string, std::string>& labels) const override {
        labels["kube32.io/board-revision"] = (variant_ == BSP_BOARD_VARIANT_V1_0) ? "v1.0" : "v1.2";
        if (imu_present_) labels["kube32.io/imu"] = "bmi270";
        if (mag0_present_ || mag1_present_) labels["kube32.io/magnetometer"] = "bmm150";
        if (battery_present_) labels["kube32.io/battery"] = "bq27220";
        if (motor_ready_) labels["kube32.io/motor"] = "true";
        mosaico_modules_labels(labels);
    }

    DisplayInfo display_info() const override { return {kWidth, kHeight, KUBE32_DISPLAY_RGB565}; }

    int32_t display_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) override {
        return fill(x, y, w, h, swap16(color));
    }

    int32_t display_bar(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pct, uint16_t fg, uint16_t bg) override {
        if (w <= 0 || h <= 0) return HAL_ERR_INVAL;
        const int32_t filled = static_cast<int32_t>(std::min<uint32_t>(pct, 100)) * w / 100;
        if (filled > 0) {
            const int32_t rc = fill(x, y, filled, h, swap16(fg));
            if (rc != HAL_OK) return rc;
        }
        if (w - filled > 0) {
            return fill(x + filled, y, w - filled, h, swap16(bg));
        }
        return HAL_OK;
    }

    int32_t display_text(int32_t x, int32_t y, const char* text, size_t len, uint16_t fg, uint16_t bg) override {
        if (!panel_ || !chunk_ || shutting_down_.load(std::memory_order_relaxed)) return HAL_ERR_NODEV;
        if (len == 0) return HAL_OK;
        // Render the entire visible 8-pixel-tall text run into chunk_ in one DMA transfer.
        const int32_t total_w = static_cast<int32_t>(std::min<size_t>(len, static_cast<size_t>(kWidth / 6 + 2))) * 6;
        const int32_t x0 = std::max<int32_t>(x, 0);
        const int32_t y0 = std::max<int32_t>(y, 0);
        const int32_t x1 = std::min<int32_t>(x + total_w, kWidth);
        const int32_t y1 = std::min<int32_t>(y + 8, kHeight);
        if (x1 <= x0 || y1 <= y0) return HAL_OK;

        const int32_t bw = x1 - x0;
        const int32_t bh = y1 - y0;
        const uint16_t f = swap16(fg);
        const uint16_t b = swap16(bg);

        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down_.load(std::memory_order_relaxed)) return HAL_ERR_NODEV;
        for (int32_t py = 0; py < bh; ++py) {
            const int32_t row = (y0 - y) + py;
            uint16_t* dst = chunk_ + py * bw;
            for (int32_t px = 0; px < bw; ++px) {
                const int32_t rel_x = (x0 - x) + px;
                const size_t ci = static_cast<size_t>(rel_x / 6);
                const int col = rel_x % 6;
                bool on = false;
                if (ci < len && col < 5 && row < 7) {
                    const unsigned char ch = static_cast<unsigned char>(text[ci]);
                    const uint8_t* glyph = (ch >= 32 && ch <= 126) ? FONT_5X7[ch - 32] : FONT_5X7[0];
                    on = (glyph[col] & (1 << row)) != 0;
                }
                dst[px] = on ? f : b;
            }
        }
        draw_chunk_locked(x0, y0, x1, y1);
        return HAL_OK;
    }

    int32_t display_blit(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* pixels) override {
        if (!panel_ || !chunk_ || shutting_down_.load(std::memory_order_relaxed)) return HAL_ERR_NODEV;
        if (w <= 0 || h <= 0 || !pixels) return HAL_ERR_INVAL;
        const int32_t x0 = std::max<int32_t>(x, 0);
        const int32_t y0 = std::max<int32_t>(y, 0);
        const int32_t x1 = std::min<int32_t>(x + w, kWidth);
        const int32_t y1 = std::min<int32_t>(y + h, kHeight);
        if (x1 <= x0 || y1 <= y0) return HAL_OK;

        const int32_t bw = x1 - x0;
        const int32_t sx_off = x0 - x;
        const uint8_t* src_bytes = reinterpret_cast<const uint8_t*>(pixels);

        for (int32_t row = y0; row < y1;) {
            const int32_t n = std::min<int32_t>(y1 - row, kChunkRows);
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutting_down_.load(std::memory_order_relaxed)) return HAL_ERR_NODEV;
            int32_t out = 0;
            for (int32_t r = 0; r < n; ++r) {
                const int32_t sy = (row + r) - y;
                const uint8_t* line = src_bytes + static_cast<size_t>(sy) * static_cast<size_t>(w) * 2U +
                                      static_cast<size_t>(sx_off) * 2U;
                for (int32_t c = 0; c < bw; ++c) {
                    uint16_t v;
                    std::memcpy(&v, line + static_cast<size_t>(c) * 2U, 2);
                    chunk_[out++] = swap16(v);
                }
            }
            draw_chunk_locked(x0, row, x1, row + n);
            row += n;
        }
        return HAL_OK;
    }

    int32_t touch_read(TouchPoint* out, uint32_t max) override {
        if (!touch_ || shutting_down_.load(std::memory_order_relaxed)) return HAL_ERR_NODEV;
        if (max == 0 || !out) return 0;
        const esp_err_t err = esp_lcd_touch_read_data(touch_);
        if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) return HAL_ERR_IO;
        esp_lcd_touch_point_data_t pts[10] = {};
        uint8_t count = 0;
        const uint8_t want = static_cast<uint8_t>(std::min<uint32_t>(max, 10));
        if (esp_lcd_touch_get_data(touch_, pts, &count, want) != ESP_OK) return HAL_ERR_IO;
        for (uint8_t i = 0; i < count; ++i) {
            out[i] = {pts[i].x, pts[i].y, pts[i].track_id, pts[i].strength};
        }
        return count;
    }

    uint8_t touch_last_raw_points() const {
        return touch_ ? esp_lcd_touch_cst9220_last_reported_points(touch_) : 0;
    }

    int32_t tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct) override {
        if (!speaker_ || shutting_down_.load(std::memory_order_relaxed)) return HAL_ERR_NODEV;
        if (freq_hz == 0 || duration_ms == 0 || volume_pct == 0) return HAL_OK;
        if (duration_ms > 2000) duration_ms = 2000;
        if (volume_pct > 100) volume_pct = 100;

        constexpr uint32_t kRate = 16000;
        const uint32_t total = (kRate * duration_ms) / 1000;
        if (total == 0) return HAL_OK;

        std::lock_guard<std::mutex> lock(audio_mutex_);
        if (shutting_down_.load(std::memory_order_relaxed)) return HAL_ERR_NODEV;
        if (!open_output_locked(kRate)) return HAL_ERR_IO;

        uint32_t period = kRate / freq_hz;
        if (period < 2) period = 2;
        const int32_t amplitude = static_cast<int32_t>((32767U * kSpeakerCeilingPct * volume_pct) / 10000U);
        const uint32_t ramp = std::max<uint32_t>(1, std::min<uint32_t>(64, total / 8));

        uint32_t phase = 0;
        uint32_t produced = 0;
        while (produced < total) {
            const uint32_t n = std::min<uint32_t>(total - produced, kAudioChunkSamples);
            for (uint32_t i = 0; i < n; ++i, ++produced) {
                int32_t a = amplitude;
                if (produced < ramp) a = amplitude * static_cast<int32_t>(produced) / static_cast<int32_t>(ramp);
                else if (total - produced < ramp) a = amplitude * static_cast<int32_t>(total - produced) / static_cast<int32_t>(ramp);
                const int16_t s = static_cast<int16_t>((phase < period / 2) ? a : -a);
                audio_stereo_[2 * i] = s;
                audio_stereo_[2 * i + 1] = s;
                phase = (phase + 1) % period;
            }
            if (esp_codec_dev_write(speaker_, audio_stereo_, static_cast<int>(n * 2 * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
                return HAL_ERR_IO;
            }
        }
        return HAL_OK;
    }

    int32_t audio_play(const int16_t* samples, uint32_t count, uint32_t sample_rate, uint32_t volume_pct) override {
        if (!speaker_ || shutting_down_.load(std::memory_order_relaxed)) return HAL_ERR_NODEV;
        if (sample_rate < 8000 || sample_rate > 48000) return HAL_ERR_INVAL;
        if (count == 0) return 0;
        if (count > sample_rate) count = sample_rate;
        if (!samples) return HAL_ERR_INVAL;
        if (volume_pct > 100) volume_pct = 100;

        std::lock_guard<std::mutex> lock(audio_mutex_);
        if (shutting_down_.load(std::memory_order_relaxed)) return HAL_ERR_NODEV;
        if (!open_output_locked(sample_rate)) return HAL_ERR_IO;

        const int32_t gain_q15 = static_cast<int32_t>((32768U * kSpeakerCeilingPct * volume_pct) / 10000U);
        const uint8_t* src = reinterpret_cast<const uint8_t*>(samples);
        for (uint32_t done = 0; done < count;) {
            const uint32_t n = std::min<uint32_t>(count - done, kAudioChunkSamples);
            for (uint32_t i = 0; i < n; ++i) {
                int16_t s;
                std::memcpy(&s, src + static_cast<size_t>(done + i) * 2U, 2);
                const int16_t scaled = static_cast<int16_t>((static_cast<int32_t>(s) * gain_q15) >> 15);
                audio_stereo_[2 * i] = scaled;
                audio_stereo_[2 * i + 1] = scaled;
            }
            if (esp_codec_dev_write(speaker_, audio_stereo_, static_cast<int>(n * 2 * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
                return HAL_ERR_IO;
            }
            done += n;
        }
        return static_cast<int32_t>(count);
    }

    int32_t mic_read(int16_t* out, uint32_t max_samples, uint32_t timeout_ms) override {
        (void)timeout_ms;
        if (!mic_ || shutting_down_.load(std::memory_order_relaxed)) return HAL_ERR_NODEV;
        std::lock_guard<std::mutex> lock(audio_mutex_);
        if (shutting_down_.load(std::memory_order_relaxed)) return HAL_ERR_NODEV;
        if (max_samples == 0) {
            if (input_open_) {
                esp_codec_dev_close(mic_);
                input_open_ = false;
            }
            return 0;
        }
        if (!out) return HAL_ERR_INVAL;
        if (max_samples > kMicRate) max_samples = kMicRate;
        if (!open_input_locked()) return HAL_ERR_IO;

        uint8_t* dst = reinterpret_cast<uint8_t*>(out);
        uint32_t got = 0;
        while (got < max_samples) {
            const uint32_t n = std::min<uint32_t>(max_samples - got, kMicChunkSamples);
            if (esp_codec_dev_read(mic_, audio_stereo_, static_cast<int>(n * 2 * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
                return got > 0 ? static_cast<int32_t>(got) : HAL_ERR_IO;
            }
            // ESP32-S31 I2S RX runs in stereo mode; keep left slot and condition DC/gain.
            for (uint32_t i = 0; i < n; ++i) mic_mono_[i] = audio_stereo_[2 * i];
            condition_mic_locked(mic_mono_, n);
            std::memcpy(dst + static_cast<size_t>(got) * 2U, mic_mono_, n * sizeof(int16_t));
            got += n;
        }
        return static_cast<int32_t>(got);
    }

    void mic_set_gain(uint32_t gain) override {
        mic_gain_ = gain < 1 ? 1 : (gain > 64 ? 64 : gain);
    }

    int32_t led_set(uint32_t index, uint32_t rgb) override {
        return mosaico_modules_led_set(index, rgb);
    }

    int32_t motor_set(uint32_t percent) override {
        if (!motor_ready_) return HAL_ERR_NODEV;
        if (percent > 100) percent = 100;
        return bsp_motor_set_strength(static_cast<uint8_t>(percent)) == ESP_OK ? HAL_OK : HAL_ERR_IO;
    }

private:
    static void on_esp_shutdown() {
        kit_board()->shutdown();
    }

    static uint64_t pins(std::initializer_list<int> list) {
        uint64_t mask = 0;
        for (int p : list) mask |= 1ULL << p;
        return mask;
    }

    static bool IRAM_ATTR on_flush_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(ctx), &woken);
        return woken == pdTRUE;
    }

    void probe_onboard_sensors() {
        i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
        if (!bus) return;
        uint8_t chip_id = 0;
        if (i2c0_read_reg(bus, BSP_IMU_I2C_ADDR, 0x00, &chip_id, 1)) {
            imu_present_ = (chip_id == 0x24);
            ESP_LOGI(TAG, "I2C0 BMI270 at 0x%02x: chip_id=0x%02x (%s)", BSP_IMU_I2C_ADDR, chip_id,
                     imu_present_ ? "ok" : "unexpected");
        }
        // BMM150 powers up in suspend mode (reg 0x4B bit 0 = 0); enable power bit to read chip ID (0x40 == 0x32).
        auto probe_bmm = [&](uint8_t addr) -> bool {
            if (i2c_master_probe(bus, addr, 50) != ESP_OK) return false;
            (void)i2c0_write_reg(bus, addr, 0x4B, 0x01);
            vTaskDelay(pdMS_TO_TICKS(3));
            uint8_t id = 0;
            const bool ok = i2c0_read_reg(bus, addr, 0x40, &id, 1) && id == 0x32;
            ESP_LOGI(TAG, "I2C0 BMM150 at 0x%02x: chip_id=0x%02x (%s)", addr, id, ok ? "ok" : "probed");
            return true;
        };
        mag0_present_ = probe_bmm(BSP_BMM150_ADDR_0);
        mag1_present_ = probe_bmm(BSP_BMM150_ADDR_1);

        uint8_t vbuf[2] = {};
        if (i2c0_read_reg(bus, BSP_BATTERY_I2C_ADDR, 0x08, vbuf, 2)) {
            battery_present_ = true;
            const uint16_t mv = static_cast<uint16_t>(vbuf[0] | (vbuf[1] << 8));
            ESP_LOGI(TAG, "I2C0 BQ27220 at 0x%02x: %u mV", BSP_BATTERY_I2C_ADDR, static_cast<unsigned>(mv));
        }
    }

    // Caller holds mutex_.
    void draw_chunk_locked(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        if (x1 <= x0 || y1 <= y0 || !panel_ || !chunk_ || !done_) return;
        // Drain any stale completion before submitting.
        (void)xSemaphoreTake(done_, 0);
        if (esp_lcd_panel_draw_bitmap(panel_, x0, y0, x1, y1, chunk_) == ESP_OK) {
            xSemaphoreTake(done_, pdMS_TO_TICKS(100));
        }
    }

    int32_t fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t swapped) {
        if (!panel_ || !chunk_ || shutting_down_.load(std::memory_order_relaxed)) return HAL_ERR_NODEV;
        if (w <= 0 || h <= 0) return (w == 0 || h == 0) ? HAL_OK : HAL_ERR_INVAL;
        const int32_t x0 = std::max<int32_t>(x, 0);
        const int32_t x1 = std::min<int32_t>(x + w, kWidth);
        const int32_t y0 = std::max<int32_t>(y, 0);
        const int32_t y1 = std::min<int32_t>(y + h, kHeight);
        if (x1 <= x0 || y1 <= y0) return HAL_OK;

        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down_.load(std::memory_order_relaxed)) return HAL_ERR_NODEV;
        const int32_t width = x1 - x0;
        for (int32_t row = y0; row < y1;) {
            const int32_t n = std::min<int32_t>(kChunkRows, y1 - row);
            for (int32_t i = 0; i < width * n; ++i) chunk_[i] = swapped;
            draw_chunk_locked(x0, row, x1, row + n);
            row += n;
        }
        return HAL_OK;
    }

    bool open_output_locked(uint32_t rate) {
        if (input_open_) {
            esp_codec_dev_close(mic_);
            input_open_ = false;
        }
        if (output_rate_ == rate) return true;
        if (output_rate_) {
            esp_codec_dev_close(speaker_);
            output_rate_ = 0;
        }
        esp_codec_dev_sample_info_t fs = {};
        fs.bits_per_sample = 16;
        fs.channel = 2;
        fs.sample_rate = rate;
        if (esp_codec_dev_open(speaker_, &fs) != ESP_CODEC_DEV_OK) {
            return false;
        }
        esp_codec_dev_set_out_vol(speaker_, kSpeakerDefaultVolume);
        output_rate_ = rate;
        return true;
    }

    bool open_input_locked() {
        if (input_open_) return true;
        if (output_rate_) {
            esp_codec_dev_close(speaker_);
            output_rate_ = 0;
        }
        esp_codec_dev_sample_info_t fs = {};
        fs.bits_per_sample = 16;
        fs.channel = 2;
        fs.sample_rate = kMicRate;
        if (esp_codec_dev_open(mic_, &fs) != ESP_CODEC_DEV_OK) return false;
        esp_codec_dev_set_in_gain(mic_, kMicInputGainDb);
        mic_settle_ = kMicSettleSamples;
        input_open_ = true;
        return true;
    }

    void condition_mic_locked(int16_t* pcm, size_t n) {
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
            pcm[i] = static_cast<int16_t>(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
        }
        mic_dc_q8_ = dc;
    }

    static constexpr uint32_t kAudioChunkSamples = 256;
    static constexpr uint32_t kMicChunkSamples = 128;

    bool initialized_{false};
    bool shutdown_registered_{false};
    std::atomic<bool> shutting_down_{false};
    bsp_board_variant_t variant_{BSP_BOARD_VARIANT_V1_2};
    bool variant_ok_{false};
    bool i2c0_ready_{false};
    bool imu_present_{false};
    bool mag0_present_{false};
    bool mag1_present_{false};
    bool battery_present_{false};
    bool motor_ready_{false};
    bool ai_button_ready_{false};

    esp_lcd_panel_handle_t panel_{nullptr};
    uint16_t* chunk_{nullptr};
    SemaphoreHandle_t done_{nullptr};
    std::mutex mutex_;
    esp_lcd_touch_handle_t touch_{nullptr};
    esp_codec_dev_handle_t speaker_{nullptr};
    esp_codec_dev_handle_t mic_{nullptr};
    std::mutex audio_mutex_;
    int16_t audio_stereo_[kAudioChunkSamples * 2]{};
    int16_t mic_mono_[kMicChunkSamples]{};
    uint32_t output_rate_{0};
    bool input_open_{false};
    uint32_t mic_gain_{1};
    int32_t mic_dc_q8_{0};
    uint32_t mic_settle_{0};
};

}  // namespace

Board* kit_board() {
    static KitBoard board;
    return &board;
}

uint8_t kit_touch_last_raw_points() {
    return static_cast<const KitBoard*>(kit_board())->touch_last_raw_points();
}

Board* kit_board_detect() {
    bsp_board_variant_t variant;
    return bsp_board_variant_get(&variant) == ESP_OK ? kit_board() : nullptr;
}

// Console diagnostic helper for onboard I2C0 sensors & buttons on ESP-Mosaico.
void kit_print_sensors(void (*write_line)(const char*, size_t, void*), void* ctx) {
    char line[192];
    auto emit = [&](int n) {
        if (n > 0) write_line(line, static_cast<size_t>(std::min<int>(n, static_cast<int>(sizeof(line) - 1))), ctx);
    };
    bsp_board_variant_t variant = BSP_BOARD_VARIANT_V1_2;
    const bool v_ok = bsp_board_variant_get(&variant) == ESP_OK;
    emit(snprintf(line, sizeof(line), "K32_SENSOR board=esp-mosaico variant=%s ai_btn=%d boot_btn=%d\n",
                  v_ok ? (variant == BSP_BOARD_VARIANT_V1_0 ? "v1.0" : "v1.2") : "unknown",
                  gpio_get_level(BSP_BUTTON_AI_GPIO) == BSP_BUTTON_ACTIVE_LEVEL ? 1 : 0,
                  gpio_get_level(BSP_BUTTON_BOOT_GPIO) == BSP_BUTTON_ACTIVE_LEVEL ? 1 : 0));

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        emit(snprintf(line, sizeof(line), "K32_ERR i2c0 not initialized\n"));
        return;
    }
    uint8_t id = 0;
    if (i2c0_read_reg(bus, BSP_IMU_I2C_ADDR, 0x00, &id, 1)) {
        uint8_t pwr = 0;
        (void)i2c0_read_reg(bus, BSP_IMU_I2C_ADDR, 0x7D, &pwr, 1);
        emit(snprintf(line, sizeof(line), "K32_SENSOR imu=bmi270 addr=0x%02x chip_id=0x%02x pwr_ctrl=0x%02x\n",
                      BSP_IMU_I2C_ADDR, id, pwr));
    } else {
        emit(snprintf(line, sizeof(line), "K32_SENSOR imu=absent\n"));
    }
    for (int idx = 0; idx < 2; ++idx) {
        const uint8_t addr = idx == 0 ? BSP_BMM150_ADDR_0 : BSP_BMM150_ADDR_1;
        uint8_t mag_id = 0;
        if (i2c0_read_reg(bus, addr, 0x40, &mag_id, 1)) {
            emit(snprintf(line, sizeof(line), "K32_SENSOR mag%d=bmm150 addr=0x%02x chip_id=0x%02x\n", idx, addr, mag_id));
        } else {
            emit(snprintf(line, sizeof(line), "K32_SENSOR mag%d=absent addr=0x%02x\n", idx, addr));
        }
    }
    uint8_t vbuf[2] = {}, ibuf[2] = {}, tbuf[2] = {}, socbuf[2] = {};
    if (i2c0_read_reg(bus, BSP_BATTERY_I2C_ADDR, 0x08, vbuf, 2)) {
        (void)i2c0_read_reg(bus, BSP_BATTERY_I2C_ADDR, 0x0C, ibuf, 2);
        (void)i2c0_read_reg(bus, BSP_BATTERY_I2C_ADDR, 0x06, tbuf, 2);
        (void)i2c0_read_reg(bus, BSP_BATTERY_I2C_ADDR, 0x2C, socbuf, 2);
        const uint16_t mv = static_cast<uint16_t>(vbuf[0] | (vbuf[1] << 8));
        const int16_t ma = static_cast<int16_t>(ibuf[0] | (ibuf[1] << 8));
        const uint16_t temp_dk = static_cast<uint16_t>(tbuf[0] | (tbuf[1] << 8));
        const float temp_c = static_cast<float>(temp_dk) / 10.0f - 273.15f;
        const uint16_t soc = static_cast<uint16_t>(socbuf[0] | (socbuf[1] << 8));
        emit(snprintf(line, sizeof(line), "K32_SENSOR battery=bq27220 addr=0x%02x mv=%u ma=%d soc=%u temp_c=%.1f\n",
                      BSP_BATTERY_I2C_ADDR, static_cast<unsigned>(mv), static_cast<int>(ma),
                      static_cast<unsigned>(soc), static_cast<double>(temp_c)));
    } else {
        emit(snprintf(line, sizeof(line), "K32_SENSOR battery=absent\n"));
    }
}

}  // namespace kube32::hal

#endif  // KUBE32_TARGET_ESP32 && CONFIG_IDF_TARGET_ESP32S31
