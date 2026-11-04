#if defined(KUBE32_TARGET_ESP32)

#include "hal/tft_keyboard_hardware.hpp"
#include <initializer_list>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_timer.h"
#include "esp_rom_gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_io_spi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

static const char* TAG = "Kube32Hardware";

namespace kube32::hal {

static esp_lcd_panel_io_handle_t s_io_handle = nullptr;
static esp_lcd_panel_handle_t s_panel_handle = nullptr;
static SemaphoreHandle_t s_lcd_mutex = nullptr;
static SemaphoreHandle_t s_lcd_flushed = nullptr;   // given when a line buffer transfer ends
static i2s_chan_handle_t s_tx_chan = nullptr;
static bool s_display_ok = false;

// Line buffer: up to 8 rows of the full panel width (240 * 8 * 2 = 3840 bytes),
// DMA capable so esp_lcd can push it straight to the SPI peripheral.
constexpr int LINEBUF_ROWS = 8;
static uint16_t* s_linebuf = nullptr;

// ---------------------------------------------------------------------------
// 5x7 ASCII font (ASCII 32..126)
// ---------------------------------------------------------------------------
static const uint8_t FONT_5X7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x79, 0x41, 0x3E}, {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x49, 0x49, 0x7A},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x7F, 0x20, 0x18, 0x20, 0x7F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78}, {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7F}, {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x0C, 0x52, 0x52, 0x52, 0x3E},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3D, 0x00}, {0x7F, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78}, {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7C, 0x14, 0x14, 0x14, 0x08}, {0x08, 0x14, 0x14, 0x18, 0x7C}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C}, {0x3C, 0x40, 0x30, 0x40, 0x3C},
    {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C}, {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00},
    {0x00, 0x00, 0x7F, 0x00, 0x00}, {0x00, 0x41, 0x36, 0x08, 0x00}, {0x08, 0x08, 0x2A, 0x1C, 0x08}
};

static inline uint16_t swap16(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }

struct LcdLock {
    LcdLock() { if (s_lcd_mutex) xSemaphoreTake(s_lcd_mutex, portMAX_DELAY); }
    ~LcdLock() { if (s_lcd_mutex) xSemaphoreGive(s_lcd_mutex); }
};

static bool IRAM_ATTR on_flushed(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_lcd_flushed, &woken);
    return woken == pdTRUE;
}

// Pushes the line buffer rows [0, rows) to panel rows [y, y+rows) at columns
// [x, x+w) and waits until the transfer has read the buffer, so the next
// draw can reuse it. Callers run pods (a HUD draws from its tick): a transfer
// that does not end turns the display off instead of blocking them, and the
// next draw would wait for the SPI bus forever.
static void flush_rows(int x, int y, int w, int rows) {
    if (!s_panel_handle || !s_display_ok || rows <= 0 || w <= 0) return;
    if (esp_lcd_panel_draw_bitmap(s_panel_handle, x, y, x + w, y + rows, s_linebuf) != ESP_OK) return;
    if (xSemaphoreTake(s_lcd_flushed, pdMS_TO_TICKS(200)) != pdTRUE) {
        s_display_ok = false;
        ESP_LOGE(TAG, "LCD transfer did not end within 200 ms: display off");
    }
}

bool init_display_and_backlight() {
    gpio_reset_pin((gpio_num_t)PIN_LCD_BACKLIGHT);
    gpio_set_direction((gpio_num_t)PIN_LCD_BACKLIGHT, GPIO_MODE_OUTPUT);
    gpio_set_drive_capability((gpio_num_t)PIN_LCD_BACKLIGHT, GPIO_DRIVE_CAP_3);
    gpio_set_level((gpio_num_t)PIN_LCD_BACKLIGHT, 1);

    if (!s_lcd_mutex) s_lcd_mutex = xSemaphoreCreateMutex();
    if (!s_lcd_flushed) s_lcd_flushed = xSemaphoreCreateBinary();
    if (!s_linebuf) {
        s_linebuf = static_cast<uint16_t*>(heap_caps_malloc(LCD_WIDTH * LINEBUF_ROWS * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (!s_linebuf) {
            ESP_LOGE(TAG, "line buffer allocation failed");
            return false;
        }
    }

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = PIN_LCD_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = PIN_LCD_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = LCD_WIDTH * LINEBUF_ROWS * sizeof(uint16_t);
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = PIN_LCD_CS;
    io_config.dc_gpio_num = PIN_LCD_DC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 4;
    io_config.on_color_trans_done = on_flushed;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &s_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = PIN_LCD_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_config.bits_per_pixel = 16;
    ret = esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_lcd_panel_reset(s_panel_handle);
    esp_lcd_panel_init(s_panel_handle);
    esp_lcd_panel_invert_color(s_panel_handle, true);
    esp_lcd_panel_swap_xy(s_panel_handle, true);
    esp_lcd_panel_mirror(s_panel_handle, true, false);
    esp_lcd_panel_set_gap(s_panel_handle, 40, 53);
    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    s_display_ok = true;
    display_clear(COLOR_BG_DARK);
    display_fill_rect(0, 0, LCD_WIDTH, 18, 0x194B);
    display_draw_string(6, 5, "KUBE32  booting", COLOR_CYAN, 0x194B);
    ESP_LOGI(TAG, "ST7789 ready (dirty-rect driver, %d-row line buffer)", LINEBUF_ROWS);
    return true;
}

bool display_available() { return s_display_ok; }

void set_backlight(bool enabled) {
    gpio_set_level((gpio_num_t)PIN_LCD_BACKLIGHT, enabled ? 1 : 0);
}

static bool clip(int& x, int& y, int& w, int& h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    return (w > 0 && h > 0);
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (!s_display_ok || !clip(x, y, w, h)) return;
    LcdLock lock;
    uint16_t c = swap16(color);
    int band = LINEBUF_ROWS;
    for (int i = 0; i < w * band; ++i) s_linebuf[i] = c;
    for (int row = 0; row < h; row += band) {
        int rows = (h - row < band) ? (h - row) : band;
        flush_rows(x, y + row, w, rows);
    }
}

void display_clear(uint16_t color) {
    display_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
}

void display_blit(int x, int y, int w, int h, const uint16_t* pixels) {
    if (!s_display_ok || !pixels || w <= 0 || h <= 0) return;
    // clip, keeping the source rows and columns that stay on the screen
    const int src_stride = w;
    int cx = x, cy = y, cw = w, ch = h;
    if (!clip(cx, cy, cw, ch)) return;
    const int skip_x = cx - x, skip_y = cy - y;
    LcdLock lock;
    for (int row = 0; row < ch; row += LINEBUF_ROWS) {
        const int rows = (ch - row < LINEBUF_ROWS) ? (ch - row) : LINEBUF_ROWS;
        for (int r = 0; r < rows; ++r) {
            const uint16_t* src = pixels + static_cast<size_t>(skip_y + row + r) * src_stride + skip_x;
            uint16_t* dst = s_linebuf + static_cast<size_t>(r) * cw;
            for (int i = 0; i < cw; ++i) {
                uint16_t v;
                memcpy(&v, src + i, sizeof v);   // the pod's pixels may be unaligned
                dst[i] = swap16(v);
            }
        }
        flush_rows(cx, cy + row, cw, rows);
    }
}

void display_draw_string_n(int x, int y, const char* str, size_t len, uint16_t color, uint16_t bg) {
    if (!s_display_ok || !str || len == 0) return;
    if (y >= LCD_HEIGHT || y + 8 <= 0 || x >= LCD_WIDTH) return;
    LcdLock lock;
    uint16_t fg16 = swap16(color);
    uint16_t bg16 = swap16(bg);
    size_t pos = 0;
    int cx = x;
    while (pos < len && cx < LCD_WIDTH) {
        size_t n = len - pos;
        if (n > 40) n = 40;
        int w = static_cast<int>(n) * 6;
        if (cx + w > LCD_WIDTH) w = LCD_WIDTH - cx;
        if (w <= 0) break;
        for (int row = 0; row < 8; ++row) {
            uint16_t* line = s_linebuf + row * w;
            for (int col = 0; col < w; ++col) {
                int ci = col / 6;
                int cc = col % 6;
                unsigned char ch = static_cast<unsigned char>(str[pos + ci]);
                if (ch < 32 || ch > 126) ch = 32;
                bool on = (cc < 5) && (row < 7) && (FONT_5X7[ch - 32][cc] & (1 << row));
                line[col] = on ? fg16 : bg16;
            }
        }
        int rows = 8;
        int yy = y < 0 ? 0 : y;
        if (yy + rows > LCD_HEIGHT) rows = LCD_HEIGHT - yy;
        flush_rows(cx, yy, w, rows);
        cx += w;
        pos += n;
    }
}

void display_draw_string(int x, int y, const char* str, uint16_t color, uint16_t bg) {
    if (!str) return;
    const char* line = str;
    int cy = y;
    while (*line) {
        const char* nl = std::strchr(line, '\n');
        size_t len = nl ? static_cast<size_t>(nl - line) : std::strlen(line);
        display_draw_string_n(x, cy, line, len, color, bg);
        if (!nl) break;
        line = nl + 1;
        cy += 10;
    }
}

void display_draw_bar(int x, int y, int w, int h, uint32_t pct, uint16_t fg, uint16_t bg) {
    if (pct > 100) pct = 100;
    int filled = static_cast<int>((static_cast<uint32_t>(w) * pct) / 100);
    if (filled > 0) display_fill_rect(x, y, filled, h, fg);
    if (filled < w) display_fill_rect(x + filled, y, w - filled, h, bg);
}

// ---------------------------------------------------------------------------
// Audio: the NS4168 speaker (I2S) and the SPM1423 microphone (PDM)
// ---------------------------------------------------------------------------
// GPIO 43 is the speaker's word clock and the microphone's PDM clock, so the
// one I2S controller with a PDM receiver (I2S0) runs either the speaker or
// the microphone. A channel is created when it is needed and deleted when the
// other one takes over or it goes idle: its DMA buffers take internal RAM
// only while sound moves, and the amplifier gets no clock between sounds.
// The speaker comes first: playback stops a recording, and a recording waits
// for playback to end.
//
// Every sample the amplifier receives is scaled to at most kSpeakerMaxPct
// percent of full scale.
static constexpr uint32_t kSpeakerMaxPct = 25;            // K32_SPEAKER_MAX_PCT
static constexpr uint32_t kSpeakerDmaDescs = 6;
static constexpr uint32_t kSpeakerDmaFrames = 256;        // 1536 samples in the ring
static constexpr uint32_t kMicDmaDescs = 4;
static constexpr uint32_t kMicDmaFrames = 256;            // 64 ms at 16 kHz
static constexpr uint32_t kMicRate = 16000;
static constexpr uint64_t kMicIdleUs = 1000000;           // the microphone stops 1 s after the last read
// The PDM decimator's output sits at a DC offset of about -1,650 and speech
// at 30 cm peaks at a few hundred (the SPM1423 is -22 dBFS at 94 dB SPL).
// k32_mic_read returns it with the offset removed (a one-pole high-pass at
// 2.5 Hz) and 24 dB of gain, saturating: speech at 30 cm peaks at several
// thousand, a quiet room's floor is about 270 rms. The decimator settles
// after the microphone starts: its first 100 ms read as silence while the
// offset is found quickly.
static constexpr int32_t kMicDcShift = 10;                // offset tracking: 1/1024 of each sample
static constexpr int32_t kMicSettleShift = 6;             // while settling: 1/64
static constexpr uint32_t kMicSettleSamples = 1600;       // 100 ms
static uint32_t s_mic_gain = 16;
static int32_t s_mic_dc_q8 = 0;
static uint32_t s_mic_settle = 0;                         // samples left to settle
static constexpr uint64_t kSpeakerTailUs = 30000;         // the amplifier stops this long after the last sample

enum class AudioMode : uint8_t { Off, Speaker, Mic };
static SemaphoreHandle_t s_audio_mutex = nullptr;
static AudioMode s_audio_mode = AudioMode::Off;
static i2s_chan_handle_t s_rx_chan = nullptr;
static uint32_t s_tx_rate = 0;
static uint64_t s_play_until_us = 0;     // when the samples queued so far have played
static uint64_t s_mic_last_read_us = 0;
static esp_timer_handle_t s_audio_idle = nullptr;
static int16_t s_audio_scratch[256];     // samples on their way to the speaker (under s_audio_mutex)

// The pins the channels drive, held low while neither runs (GPIO 43 is
// also the UART0 TX pad: it leaves the UART here)
static void audio_pins_low() {
    for (int pin : {PIN_I2S_BCLK, PIN_I2S_DATA, PIN_I2S_LRCK}) {
        esp_rom_gpio_pad_select_gpio(static_cast<uint32_t>(pin));
        gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT);
        gpio_set_level(static_cast<gpio_num_t>(pin), 0);
    }
}

static void audio_off_locked() {
    if (s_tx_chan) {
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = nullptr;
        s_tx_rate = 0;
    }
    if (s_rx_chan) {
        i2s_channel_disable(s_rx_chan);
        i2s_del_channel(s_rx_chan);
        s_rx_chan = nullptr;
    }
    if (s_audio_mode != AudioMode::Off) audio_pins_low();
    s_audio_mode = AudioMode::Off;
}

static void arm_audio_idle(uint64_t after_us) {
    if (!s_audio_idle) return;
    esp_timer_stop(s_audio_idle);
    esp_timer_start_once(s_audio_idle, after_us);
}

// Stops whichever channel has nothing left to do
static void audio_idle_cb(void*) {
    if (xSemaphoreTake(s_audio_mutex, 0) != pdTRUE) {
        arm_audio_idle(20000);   // busy: a call is moving samples
        return;
    }
    const uint64_t now = esp_timer_get_time();
    if (s_audio_mode == AudioMode::Speaker) {
        if (now >= s_play_until_us + kSpeakerTailUs) audio_off_locked();
        else arm_audio_idle(s_play_until_us + kSpeakerTailUs - now);
    } else if (s_audio_mode == AudioMode::Mic) {
        if (now >= s_mic_last_read_us + kMicIdleUs) audio_off_locked();
        else arm_audio_idle(s_mic_last_read_us + kMicIdleUs - now);
    }
    xSemaphoreGive(s_audio_mutex);
}

bool init_speaker() {
    s_audio_mutex = xSemaphoreCreateMutex();
    esp_timer_create_args_t args = {};
    args.callback = &audio_idle_cb;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "k32_audio";
    if (!s_audio_mutex || esp_timer_create(&args, &s_audio_idle) != ESP_OK) {
        ESP_LOGE(TAG, "audio: no mutex or timer");
        return false;
    }
    audio_pins_low();
    ESP_LOGI(TAG, "speaker on I2S0 (BCLK %d, LRCK %d, DOUT %d, at most %u%% of full scale), microphone PDM (CLK %d, DATA %d)",
             PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DATA, (unsigned)kSpeakerMaxPct, PIN_MIC_CLK, PIN_MIC_DATA);
    return true;
}

// The speaker's channel at `rate`, running
static bool speaker_on_locked(uint32_t rate) {
    if (s_audio_mode == AudioMode::Mic) audio_off_locked();
    if (s_tx_chan && s_tx_rate == rate) return true;
    if (s_tx_chan) {
        // Another sample rate: the queued samples play out first
        const uint64_t now = esp_timer_get_time();
        if (s_play_until_us > now) vTaskDelay(pdMS_TO_TICKS((s_play_until_us - now) / 1000 + 1));
        audio_off_locked();
    }
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = kSpeakerDmaDescs;
    chan_cfg.dma_frame_num = kSpeakerDmaFrames;
    // Zero a descriptor once the DMA has sent it. Without this the ring
    // replays its stale contents on underrun and the amplifier hums.
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &s_tx_chan, nullptr) != ESP_OK) {
        s_tx_chan = nullptr;
        return false;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)PIN_I2S_BCLK,
            .ws   = (gpio_num_t)PIN_I2S_LRCK,
            .dout = (gpio_num_t)PIN_I2S_DATA,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(s_tx_chan, &std_cfg) != ESP_OK || i2s_channel_enable(s_tx_chan) != ESP_OK) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = nullptr;
        audio_pins_low();
        return false;
    }
    s_tx_rate = rate;
    s_audio_mode = AudioMode::Speaker;
    s_play_until_us = esp_timer_get_time();
    return true;
}

// Queues the samples in s_audio_scratch[0, n), already scaled
static bool speaker_write_locked(uint32_t n) {
    size_t written = 0;
    if (i2s_channel_write(s_tx_chan, s_audio_scratch, n * sizeof(int16_t), &written, 1000) != ESP_OK) return false;   // timeout in ms
    const uint64_t now = esp_timer_get_time();
    const uint64_t ring_us = 1000000ull * kSpeakerDmaDescs * kSpeakerDmaFrames / s_tx_rate;
    const uint64_t queued_us = 1000000ull * n / s_tx_rate;
    // What is in the ring plays within its length from now
    s_play_until_us = std::min(std::max(s_play_until_us, now) + queued_us, now + ring_us);
    return true;
}

static int32_t audio_lock(uint32_t timeout_ms) {
    if (!s_audio_mutex) return -19;   // K32_ERR_NODEV: no speaker on this board
    return xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? 0 : -16;   // K32_ERR_BUSY
}

int32_t audio_play(const int16_t* samples, uint32_t count, uint32_t sample_rate, uint32_t volume_pct) {
    if (sample_rate < 8000 || sample_rate > 48000) return -22;   // K32_ERR_INVAL
    if (count > sample_rate) count = sample_rate;               // one second per call
    if (volume_pct > 100) volume_pct = 100;
    if (count == 0) return 0;
    if (int32_t rc = audio_lock(2000)) return rc;
    if (!speaker_on_locked(sample_rate)) {
        xSemaphoreGive(s_audio_mutex);
        return -5;   // K32_ERR_IO
    }
    // Q15 gain: volume_pct of the ceiling
    const int32_t gain = static_cast<int32_t>((32768u * kSpeakerMaxPct * volume_pct) / 10000u);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(samples);
    bool ok = true;
    for (uint32_t done = 0; ok && done < count;) {
        const uint32_t n = std::min<uint32_t>(count - done, sizeof(s_audio_scratch) / sizeof(s_audio_scratch[0]));
        // The pod's buffer may sit at any address: copy it whole first
        std::memcpy(s_audio_scratch, src + 2 * done, n * sizeof(int16_t));
        for (uint32_t i = 0; i < n; ++i) s_audio_scratch[i] = static_cast<int16_t>((s_audio_scratch[i] * gain) >> 15);
        ok = speaker_write_locked(n);
        done += n;
    }
    arm_audio_idle(s_play_until_us - std::min(s_play_until_us, (uint64_t)esp_timer_get_time()) + kSpeakerTailUs);
    xSemaphoreGive(s_audio_mutex);
    return ok ? static_cast<int32_t>(count) : -5;
}

void play_tone(uint32_t freq_hz, uint32_t duration_ms, uint8_t volume_pct) {
    if (freq_hz == 0 || duration_ms == 0 || volume_pct == 0) return;
    if (duration_ms > 2000) duration_ms = 2000;
    if (volume_pct > 100) volume_pct = 100;
    if (audio_lock(2000) != 0) return;
    const uint32_t rate = 16000;
    if (!speaker_on_locked(rate)) {
        xSemaphoreGive(s_audio_mutex);
        return;
    }
    int period = static_cast<int>(rate / freq_hz);
    if (period < 2) period = 2;
    const int total = static_cast<int>((rate * duration_ms) / 1000);
    const int amplitude = static_cast<int>((32767u * kSpeakerMaxPct * volume_pct) / 10000u);
    int ramp = total / 8 > 64 ? 64 : total / 8;
    if (ramp < 1) ramp = 1;
    int phase = 0, produced = 0;
    while (produced < total) {
        const int n = std::min<int>(total - produced, sizeof(s_audio_scratch) / sizeof(s_audio_scratch[0]));
        for (int i = 0; i < n; ++i, ++produced) {
            int a = amplitude;
            if (produced < ramp) a = amplitude * produced / ramp;
            else if (total - produced < ramp) a = amplitude * (total - produced) / ramp;
            s_audio_scratch[i] = static_cast<int16_t>((phase < period / 2) ? a : -a);
            phase = (phase + 1) % period;
        }
        if (!speaker_write_locked(static_cast<uint32_t>(n))) break;
    }
    arm_audio_idle(s_play_until_us - std::min(s_play_until_us, (uint64_t)esp_timer_get_time()) + kSpeakerTailUs);
    xSemaphoreGive(s_audio_mutex);
}

static bool mic_on_locked() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = kMicDmaDescs;
    chan_cfg.dma_frame_num = kMicDmaFrames;
    if (i2s_new_channel(&chan_cfg, nullptr, &s_rx_chan) != ESP_OK) {
        s_rx_chan = nullptr;
        return false;
    }
    i2s_pdm_rx_config_t pdm = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(kMicRate),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {},
    };
    pdm.gpio_cfg.clk = static_cast<gpio_num_t>(PIN_MIC_CLK);
    pdm.gpio_cfg.din = static_cast<gpio_num_t>(PIN_MIC_DATA);
    pdm.gpio_cfg.invert_flags.clk_inv = false;
    if (i2s_channel_init_pdm_rx_mode(s_rx_chan, &pdm) != ESP_OK || i2s_channel_enable(s_rx_chan) != ESP_OK) {
        i2s_del_channel(s_rx_chan);
        s_rx_chan = nullptr;
        audio_pins_low();
        return false;
    }
    s_audio_mode = AudioMode::Mic;
    s_mic_settle = kMicSettleSamples;
    return true;
}

static void mic_condition(int16_t* pcm, size_t n) {
    const int32_t gain = static_cast<int32_t>(s_mic_gain);
    int32_t dc = s_mic_dc_q8;
    for (size_t i = 0; i < n; ++i) {
        const int32_t x = pcm[i];
        if (s_mic_settle) {
            if (s_mic_settle-- == kMicSettleSamples) dc = x << 8;
            dc += ((x << 8) - dc) >> kMicSettleShift;
            pcm[i] = 0;
            continue;
        }
        dc += ((x << 8) - dc) >> kMicDcShift;
        const int32_t y = (x - (dc >> 8)) * gain;
        pcm[i] = static_cast<int16_t>(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
    s_mic_dc_q8 = dc;
}

void mic_set_gain(uint32_t gain) {
    s_mic_gain = gain < 1 ? 1 : gain > 64 ? 64 : gain;
}

int32_t mic_read(int16_t* out, uint32_t max_samples, uint32_t timeout_ms) {
    if (max_samples > kMicRate) max_samples = kMicRate;
    if (timeout_ms > 1000) timeout_ms = 1000;
    const uint64_t deadline = esp_timer_get_time() + 1000ull * timeout_ms;
    if (int32_t rc = audio_lock(timeout_ms)) return rc;
    if (max_samples == 0) {
        if (s_audio_mode == AudioMode::Mic) audio_off_locked();
        xSemaphoreGive(s_audio_mutex);
        return 0;
    }
    // Playback keeps the pins until its samples have played
    while (s_audio_mode == AudioMode::Speaker && esp_timer_get_time() < s_play_until_us) {
        if (esp_timer_get_time() >= deadline) {
            xSemaphoreGive(s_audio_mutex);
            return -16;   // K32_ERR_BUSY
        }
        xSemaphoreGive(s_audio_mutex);
        vTaskDelay(pdMS_TO_TICKS(10));
        if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return -16;
    }
    if (s_audio_mode != AudioMode::Mic) {
        audio_off_locked();
        if (!mic_on_locked()) {
            xSemaphoreGive(s_audio_mutex);
            return -5;   // K32_ERR_IO
        }
    }
    const uint64_t now = esp_timer_get_time();
    const uint32_t wait_ms = deadline > now ? static_cast<uint32_t>((deadline - now) / 1000) : 0;
    size_t got = 0;
    // i2s_channel_read copies out of its DMA buffers: any destination address works
    esp_err_t err = i2s_channel_read(s_rx_chan, out, max_samples * sizeof(int16_t), &got, wait_ms);   // timeout in ms
    mic_condition(out, got / sizeof(int16_t));
    if (got == 0) {
        static uint32_t s_empty_reads = 0;
        if (s_empty_reads++ < 4) ESP_LOGW(TAG, "microphone: no samples in %u ms (%s)", (unsigned)wait_ms, esp_err_to_name(err));
    }
    s_mic_last_read_us = esp_timer_get_time();
    arm_audio_idle(kMicIdleUs);
    xSemaphoreGive(s_audio_mutex);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) return -5;
    return static_cast<int32_t>(got / sizeof(int16_t));
}

// ---------------------------------------------------------------------------
// 56-key matrix keyboard
// ---------------------------------------------------------------------------
static const gpio_num_t KB_ROWS[] = {
    (gpio_num_t)13, (gpio_num_t)15, (gpio_num_t)3, (gpio_num_t)4,
    (gpio_num_t)5, (gpio_num_t)6, (gpio_num_t)7
};

struct KeyMapChart_t { uint8_t x_1; uint8_t x_2; };
static const KeyMapChart_t X_MAP_CHART[7] = { {0, 1}, {2, 3}, {4, 5}, {6, 7}, {8, 9}, {10, 11}, {12, 13} };

static const char KEYMAP_NORMAL[4][14] = {
    { '`', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b' },
    { '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\\' },
    { 0,    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '\n' },
    { 0,    0,   0,   'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', ' '  }
};
static const char KEYMAP_SHIFTED[4][14] = {
    { '~', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b' },
    { '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '|' },
    { 0,    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '\n' },
    { 0,    0,   0,   'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', ' '  }
};

constexpr int KEYQ_SIZE = 16;
static volatile char s_keyq[KEYQ_SIZE];
static volatile uint8_t s_keyq_head = 0, s_keyq_tail = 0;
static volatile char s_last_key = '\0';

static void keyq_push(char c) {
    uint8_t next = static_cast<uint8_t>((s_keyq_head + 1) % KEYQ_SIZE);
    if (next == s_keyq_tail) return;
    s_keyq[s_keyq_head] = c;
    s_keyq_head = next;
}

int keyboard_pop() {
    if (s_keyq_tail == s_keyq_head) return -1;
    char c = s_keyq[s_keyq_tail];
    s_keyq_tail = static_cast<uint8_t>((s_keyq_tail + 1) % KEYQ_SIZE);
    return static_cast<unsigned char>(c);
}

char get_last_key_pressed() { return s_last_key; }

void keyboard_inject(char c) { s_last_key = c; keyq_push(c); }

struct KeyCoord { int x; int y; };

static void keyboard_task(void* pvParameters) {
    (void)pvParameters;
    static char prev_keys[16] = {0};
    static int prev_key_count = 0;

    while (true) {
        KeyCoord active_coords[16];
        int active_count = 0;

        for (int i = 0; i < 8; ++i) {
            gpio_set_level((gpio_num_t)PIN_KB_A0, (i & 1));
            gpio_set_level((gpio_num_t)PIN_KB_A1, (i & 2) >> 1);
            gpio_set_level((gpio_num_t)PIN_KB_A2, (i & 4) >> 2);
            esp_rom_delay_us(10);
            for (int j = 0; j < 7; ++j) {
                if (gpio_get_level(KB_ROWS[j]) == 0 && active_count < 16) {
                    int kx = (i > 3) ? X_MAP_CHART[j].x_1 : X_MAP_CHART[j].x_2;
                    int ky = (i > 3) ? (i - 4) : i;
                    ky = 3 - ky;
                    active_coords[active_count++] = { kx, ky };
                }
            }
        }

        bool shift_active = false, fn_active = false;
        for (int k = 0; k < active_count; ++k) {
            int x = active_coords[k].x, y = active_coords[k].y;
            if (y == 2 && x == 0) fn_active = true;
            if (y == 2 && x == 1) shift_active = true;
        }

        char current_pressed[16] = {0};
        int current_count = 0;
        for (int k = 0; k < active_count; ++k) {
            int x = active_coords[k].x, y = active_coords[k].y;
            if (y == 2 && (x == 0 || x == 1)) continue;
            if (y == 3 && (x >= 0 && x <= 2)) continue;
            if (x < 0 || x >= 14 || y < 0 || y >= 4) continue;
            char ch;
            if (fn_active) {
                if (y == 0 && x == 0) ch = 27;
                else if (y == 2 && x == 11) ch = 'U';
                else if (y == 3 && x == 10) ch = 'L';
                else if (y == 3 && x == 11) ch = 'D';
                else if (y == 3 && x == 12) ch = 'R';
                else ch = KEYMAP_NORMAL[y][x];
            } else if (shift_active) {
                ch = KEYMAP_SHIFTED[y][x];
            } else {
                ch = KEYMAP_NORMAL[y][x];
            }
            if (ch != '\0' && current_count < 16) current_pressed[current_count++] = ch;
        }

        for (int i = 0; i < current_count; ++i) {
            char ch = current_pressed[i];
            bool was_pressed = false;
            for (int p = 0; p < prev_key_count; ++p) if (prev_keys[p] == ch) { was_pressed = true; break; }
            if (!was_pressed) {
                s_last_key = ch;
                keyq_push(ch);
            }
        }
        for (int i = 0; i < current_count; ++i) prev_keys[i] = current_pressed[i];
        prev_key_count = current_count;

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

uint64_t peripheral_gpio_mask() {
    uint64_t mask = 0;
    for (int pin : {PIN_LCD_MOSI, PIN_LCD_SCLK, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST, PIN_LCD_BACKLIGHT,
                    PIN_I2S_BCLK, PIN_I2S_DATA, PIN_I2S_LRCK, PIN_SPEAKER, PIN_MIC_CLK, PIN_MIC_DATA, PIN_KB_A0, PIN_KB_A1, PIN_KB_A2}) {
        mask |= 1ULL << pin;
    }
    for (gpio_num_t row : KB_ROWS) mask |= 1ULL << static_cast<int>(row);
    return mask;
}

bool init_keyboard_scanner() {
    gpio_config_t col_cfg = {
        .pin_bit_mask = (1ULL << PIN_KB_A0) | (1ULL << PIN_KB_A1) | (1ULL << PIN_KB_A2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&col_cfg);
    uint64_t row_mask = 0;
    for (int r = 0; r < 7; ++r) row_mask |= (1ULL << KB_ROWS[r]);
    gpio_config_t row_cfg = {
        .pin_bit_mask = row_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&row_cfg);
    BaseType_t rc = xTaskCreatePinnedToCore(keyboard_task, "k32_kb", 2560, nullptr, 4, nullptr, 1);
    ESP_LOGI(TAG, "keyboard scanner %s", rc == pdPASS ? "started" : "FAILED");
    return rc == pdPASS;
}

} // namespace kube32::hal

#endif // KUBE32_TARGET_ESP32
