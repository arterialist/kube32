#if defined(KUBE32_TARGET_ESP32)

#include "hal/tft_keyboard_hardware.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_io_spi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <cstring>
#include <cstdio>

static const char* TAG = "Kube32Hardware";

namespace kube32::hal {

static esp_lcd_panel_io_handle_t s_io_handle = nullptr;
static esp_lcd_panel_handle_t s_panel_handle = nullptr;
static SemaphoreHandle_t s_lcd_mutex = nullptr;
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

// Pushes the line buffer rows [0, rows) to panel rows [y, y+rows) at columns [x, x+w)
static void flush_rows(int x, int y, int w, int rows) {
    if (!s_panel_handle || rows <= 0 || w <= 0) return;
    esp_lcd_panel_draw_bitmap(s_panel_handle, x, y, x + w, y + rows, s_linebuf);
}

bool init_display_and_backlight() {
    gpio_reset_pin((gpio_num_t)PIN_LCD_BACKLIGHT);
    gpio_set_direction((gpio_num_t)PIN_LCD_BACKLIGHT, GPIO_MODE_OUTPUT);
    gpio_set_drive_capability((gpio_num_t)PIN_LCD_BACKLIGHT, GPIO_DRIVE_CAP_3);
    gpio_set_level((gpio_num_t)PIN_LCD_BACKLIGHT, 1);

    if (!s_lcd_mutex) s_lcd_mutex = xSemaphoreCreateMutex();
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
// Speaker
// ---------------------------------------------------------------------------
bool init_speaker() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 128;
    // Zero a descriptor once the DMA has sent it. Without this the ring replays
    // its stale contents on underrun and the amplifier hums forever after the
    // first beep.
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return false;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
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
    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return false;
    }
    // The channel stays disabled while idle: no bit clock, so the class-D
    // amplifier is silent and draws nothing between tones.
    ESP_LOGI(TAG, "I2S speaker ready (BCLK %d, LRCK %d, DOUT %d)", PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DATA);
    return true;
}

static SemaphoreHandle_t s_tone_mutex = nullptr;

void play_tone(uint32_t freq_hz, uint32_t duration_ms, uint8_t volume_pct) {
    if (!s_tx_chan) return;
    if (!s_tone_mutex) s_tone_mutex = xSemaphoreCreateMutex();
    if (xSemaphoreTake(s_tone_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

    int16_t silence[128];
    std::memset(silence, 0, sizeof(silence));
    size_t bytes_written = 0;

    if (freq_hz == 0 || duration_ms == 0 || volume_pct == 0) {
        xSemaphoreGive(s_tone_mutex);
        return;
    }
    if (i2s_channel_enable(s_tx_chan) != ESP_OK) {
        xSemaphoreGive(s_tone_mutex);
        return;
    }
    if (duration_ms > 2000) duration_ms = 2000;
    if (volume_pct > 100) volume_pct = 100;

    const uint32_t sample_rate = 16000;
    int period = static_cast<int>(sample_rate / freq_hz);
    if (period < 2) period = 2;
    int total = static_cast<int>((sample_rate * duration_ms) / 1000);
    int16_t amplitude = static_cast<int16_t>((12000L * volume_pct) / 100L);
    if (amplitude < 300) amplitude = 300;

    int16_t buf[128];
    int phase = 0;
    int remaining = total;
    int ramp = total / 8 > 64 ? 64 : total / 8;
    if (ramp < 1) ramp = 1;
    int produced = 0;
    while (remaining > 0) {
        int chunk = remaining > 128 ? 128 : remaining;
        for (int i = 0; i < chunk; ++i) {
            int a = amplitude;
            if (produced < ramp) a = amplitude * produced / ramp;
            else if (total - produced < ramp) a = amplitude * (total - produced) / ramp;
            buf[i] = static_cast<int16_t>((phase < period / 2) ? a : -a);
            phase = (phase + 1) % period;
            produced++;
        }
        i2s_channel_write(s_tx_chan, buf, chunk * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(100));
        remaining -= chunk;
    }
    // Flush the ring with silence, then stop the clock.
    for (int k = 0; k < 4; ++k) i2s_channel_write(s_tx_chan, silence, sizeof(silence), &bytes_written, pdMS_TO_TICKS(50));
    i2s_channel_disable(s_tx_chan);
    xSemaphoreGive(s_tone_mutex);
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
    static int traced = 0;
    if (traced < 8) { traced++; printf("[TRACE] keyboard matrix key 0x%02x\n", (unsigned)(unsigned char)c); }
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
