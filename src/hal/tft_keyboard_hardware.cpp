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
#include <cstring>
#include <cstdio>

static const char* TAG = "Kube32Hardware";

namespace kube32::hal {

static esp_lcd_panel_io_handle_t s_io_handle = nullptr;
static esp_lcd_panel_handle_t s_panel_handle = nullptr;
static uint16_t* s_framebuffer = nullptr;
static volatile char last_key_pressed = '\0';
static i2s_chan_handle_t s_tx_chan = nullptr;

// ---------------------------------------------------------------------------
// Basic 5x7 ASCII Bitmap Font
// ---------------------------------------------------------------------------
static const uint8_t FONT_5X7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // 32 (Space)
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // 33 !
    {0x00, 0x07, 0x00, 0x07, 0x00}, // 34 "
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // 35 #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // 36 $
    {0x23, 0x13, 0x08, 0x64, 0x62}, // 37 %
    {0x36, 0x49, 0x55, 0x22, 0x50}, // 38 &
    {0x00, 0x05, 0x03, 0x00, 0x00}, // 39 '
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // 40 (
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // 41 )
    {0x14, 0x08, 0x3E, 0x08, 0x14}, // 42 *
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // 43 +
    {0x00, 0x50, 0x30, 0x00, 0x00}, // 44 ,
    {0x08, 0x08, 0x08, 0x08, 0x08}, // 45 -
    {0x00, 0x60, 0x60, 0x00, 0x00}, // 46 .
    {0x20, 0x10, 0x08, 0x04, 0x02}, // 47 /
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 48 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 49 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 50 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 51 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 52 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 53 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 54 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 55 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 56 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 57 9
    {0x00, 0x36, 0x36, 0x00, 0x00}, // 58 :
    {0x00, 0x56, 0x36, 0x00, 0x00}, // 59 ;
    {0x08, 0x14, 0x22, 0x41, 0x00}, // 60 <
    {0x14, 0x14, 0x14, 0x14, 0x14}, // 61 =
    {0x00, 0x41, 0x22, 0x14, 0x08}, // 62 >
    {0x02, 0x01, 0x51, 0x09, 0x06}, // 63 ?
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // 64 @
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // 65 A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // 66 B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 67 C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // 68 D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // 69 E
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // 70 F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // 71 G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 72 H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // 73 I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // 74 J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // 75 K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // 76 L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // 77 M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // 78 N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 79 O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // 80 P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 81 Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // 82 R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // 83 S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // 84 T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 85 U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 86 V
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, // 87 W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 88 X
    {0x07, 0x08, 0x70, 0x08, 0x07}, // 89 Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // 90 Z
    {0x00, 0x7F, 0x41, 0x41, 0x00}, // 91 [
    {0x02, 0x04, 0x08, 0x10, 0x20}, // 92 backslash
    {0x00, 0x41, 0x41, 0x7F, 0x00}, // 93 ]
    {0x04, 0x02, 0x01, 0x02, 0x04}, // 94 ^
    {0x40, 0x40, 0x40, 0x40, 0x40}, // 95 _
    {0x00, 0x01, 0x02, 0x04, 0x00}, // 96 `
    {0x20, 0x54, 0x54, 0x54, 0x78}, // 97 a
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // 98 b
    {0x38, 0x44, 0x44, 0x44, 0x20}, // 99 c
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // 100 d
    {0x38, 0x54, 0x54, 0x54, 0x18}, // 101 e
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // 102 f
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, // 103 g
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // 104 h
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // 105 i
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // 106 j
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // 107 k
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // 108 l
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // 109 m
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // 110 n
    {0x38, 0x44, 0x44, 0x44, 0x38}, // 111 o
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // 112 p
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // 113 q
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // 114 r
    {0x48, 0x54, 0x54, 0x54, 0x20}, // 115 s
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // 116 t
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // 117 u
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // 118 v
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // 119 w
    {0x44, 0x28, 0x10, 0x28, 0x44}, // 120 x
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // 121 y
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // 122 z
    {0x00, 0x08, 0x36, 0x41, 0x00}, // 123 {
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // 124 |
    {0x00, 0x41, 0x36, 0x08, 0x00}, // 125 }
    {0x08, 0x08, 0x2A, 0x1C, 0x08}  // 126 ~
};

// ---------------------------------------------------------------------------
// Display Initialization & Primitives (ESP-IDF esp_lcd ST7789 Engine)
// ---------------------------------------------------------------------------
void init_display_and_backlight() {
    ESP_LOGI(TAG, "Initializing ST7789 Backlight on GPIO %d...", PIN_LCD_BACKLIGHT);

    // 1. Backlight GPIO 38: direct HIGH output (3.3V power switch rail)
    gpio_reset_pin((gpio_num_t)PIN_LCD_BACKLIGHT);
    gpio_set_direction((gpio_num_t)PIN_LCD_BACKLIGHT, GPIO_MODE_OUTPUT);
    gpio_set_drive_capability((gpio_num_t)PIN_LCD_BACKLIGHT, GPIO_DRIVE_CAP_3);
    gpio_set_level((gpio_num_t)PIN_LCD_BACKLIGHT, 1);

    // 2. Allocate 240x135 RGB565 Framebuffer in DMA-capable internal memory
    if (!s_framebuffer) {
        s_framebuffer = (uint16_t*)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_framebuffer) {
            std::memset(s_framebuffer, 0, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
        }
    }

    // 3. Configure SPI Master Bus on SPI2_HOST
    ESP_LOGI(TAG, "Configuring SPI master bus (MOSI: %d, SCLK: %d, CS: %d, DC: %d, RST: %d)...",
             PIN_LCD_MOSI, PIN_LCD_SCLK, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = PIN_LCD_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = PIN_LCD_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return;
    }

    // 4. Create Panel IO SPI Handle
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = PIN_LCD_CS;
    io_config.dc_gpio_num = PIN_LCD_DC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = 20 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &s_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(ret));
        return;
    }

    // 5. Create ST7789 Panel Device Handle
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = PIN_LCD_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_config.bits_per_pixel = 16;
    ret = esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(ret));
        return;
    }

    // 6. Initialize ST7789, Invert IPS Colors, Set 240x135 Landscape Viewport
    esp_lcd_panel_reset(s_panel_handle);
    esp_lcd_panel_init(s_panel_handle);
    esp_lcd_panel_invert_color(s_panel_handle, true);
    esp_lcd_panel_swap_xy(s_panel_handle, true);
    esp_lcd_panel_mirror(s_panel_handle, true, false);
    esp_lcd_panel_set_gap(s_panel_handle, 40, 53);
    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    // Initial Screen Draw
    display_clear(COLOR_BG_DARK);

    // Header bar
    display_fill_rect(0, 0, LCD_WIDTH, 20, 0x194B); // Navy header
    display_draw_string(8, 6, "KUBE32 v1.33 SILICON MESH", COLOR_CYAN, 0x194B);

    display_draw_string(8, 30, "STATUS: Control-Plane Active", COLOR_GREEN, COLOR_BG_DARK);
    display_draw_string(8, 48, "NODE:   esp32-node-01 (Master)", COLOR_WHITE, COLOR_BG_DARK);
    display_draw_string(8, 66, "IP:     192.168.4.1 (SoftAP)", COLOR_WHITE, COLOR_BG_DARK);
    display_draw_string(8, 84, "HEAP:   48,400 B | PSRAM: 2MB", COLOR_CYAN, COLOR_BG_DARK);
    display_draw_string(8, 102, "PODS:   hardware-controller", COLOR_WHITE, COLOR_BG_DARK);
    display_draw_string(8, 120, "READY:  Listening for k8s...", COLOR_GREEN, COLOR_BG_DARK);

    ESP_LOGI(TAG, "ST7789 Display Initialized Successfully via esp_lcd. Backlight ON.");
}

void set_backlight(bool enabled) {
    gpio_set_level((gpio_num_t)PIN_LCD_BACKLIGHT, enabled ? 1 : 0);
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (!s_framebuffer) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int r = y; r < y + h; ++r) {
        for (int c = x; c < x + w; ++c) {
            s_framebuffer[r * LCD_WIDTH + c] = color;
        }
    }
    if (s_panel_handle) {
        esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, LCD_WIDTH, LCD_HEIGHT, s_framebuffer);
    }
}

void display_clear(uint16_t color) {
    if (!s_framebuffer) return;
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; ++i) {
        s_framebuffer[i] = color;
    }
    if (s_panel_handle) {
        esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, LCD_WIDTH, LCD_HEIGHT, s_framebuffer);
    }
}

void display_draw_char(int x, int y, char c, uint16_t color, uint16_t bg) {
    if (!s_framebuffer) return;
    if (c < 32 || c > 126) c = 32;
    const uint8_t* col_bytes = FONT_5X7[c - 32];

    for (int row = 0; row < 8; ++row) {
        int py = y + row;
        if (py < 0 || py >= LCD_HEIGHT) continue;
        for (int col = 0; col < 5; ++col) {
            int px = x + col;
            if (px < 0 || px >= LCD_WIDTH) continue;
            bool on = (row < 7) && (col_bytes[col] & (1 << row));
            s_framebuffer[py * LCD_WIDTH + px] = on ? color : bg;
        }
        int px = x + 5;
        if (px >= 0 && px < LCD_WIDTH) {
            s_framebuffer[py * LCD_WIDTH + px] = bg;
        }
    }
}

void display_draw_string(int x, int y, const char* str, uint16_t color, uint16_t bg) {
    if (!s_framebuffer) return;
    int cur_x = x;
    int cur_y = y;
    while (*str) {
        if (*str == '\n') {
            cur_x = x;
            cur_y += 10;
        } else {
            display_draw_char(cur_x, cur_y, *str, color, bg);
            cur_x += 6;
        }
        str++;
    }
    if (s_panel_handle) {
        esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, LCD_WIDTH, LCD_HEIGHT, s_framebuffer);
    }
}

// ---------------------------------------------------------------------------
// Acoustic Synthesizer (NS4168 I2S Mono Audio Amplifier)
// ---------------------------------------------------------------------------
void init_speaker() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return;
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
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return;
    }
    i2s_channel_enable(s_tx_chan);
    ESP_LOGI(TAG, "NS4168 I2S speaker online (BCLK: %d, LRCK: %d, DOUT: %d)", PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DATA);
}

void play_tone(uint32_t freq_hz, uint32_t duration_ms, uint8_t volume_pct) {
    if (!s_tx_chan) return;

    // Silence buffer
    int16_t silence[256];
    std::memset(silence, 0, sizeof(silence));
    size_t bytes_written = 0;

    if (freq_hz == 0 || duration_ms == 0 || volume_pct == 0) {
        // Flush silence across multiple DMA descriptors to guarantee complete silence
        for (int k = 0; k < 8; ++k) {
            i2s_channel_write(s_tx_chan, silence, sizeof(silence), &bytes_written, pdMS_TO_TICKS(50));
        }
        return;
    }

    const uint32_t sample_rate = 16000;
    int samples_per_period = sample_rate / freq_hz;
    if (samples_per_period < 2) samples_per_period = 2;

    int total_samples = (sample_rate * duration_ms) / 1000;
    int16_t buf[128];
    int buf_len = sizeof(buf) / sizeof(int16_t);

    // Scale amplitude safely from volume_pct (0..100)
    // At 100% vol: amplitude = 4000 (well-controlled, clear)
    // At 20% vol:  amplitude = 800 (gentle, quiet click/beep)
    if (volume_pct > 100) volume_pct = 100;
    int16_t amplitude = (int16_t)((4000L * volume_pct) / 100L);
    if (amplitude < 100 && volume_pct > 0) amplitude = 100;

    int phase = 0;
    int remaining = total_samples;
    // Play exactly once for duration_ms
    while (remaining > 0) {
        int chunk = (remaining > buf_len) ? buf_len : remaining;
        for (int i = 0; i < chunk; ++i) {
            buf[i] = (phase < samples_per_period / 2) ? amplitude : -amplitude;
            phase = (phase + 1) % samples_per_period;
        }
        i2s_channel_write(s_tx_chan, buf, chunk * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(100));
        remaining -= chunk;
    }

    // Immediately flush silence so the DMA buffer does not loop the tone
    for (int k = 0; k < 6; ++k) {
        i2s_channel_write(s_tx_chan, silence, sizeof(silence), &bytes_written, pdMS_TO_TICKS(50));
    }
}

void play_boot_chime() {
    // Silent on boot
}

// ---------------------------------------------------------------------------
// Integrated 56-Key Matrix Keyboard Scanner
// ---------------------------------------------------------------------------
static const gpio_num_t KB_ROWS[] = {
    (gpio_num_t)13, (gpio_num_t)15, (gpio_num_t)3, (gpio_num_t)4,
    (gpio_num_t)5, (gpio_num_t)6, (gpio_num_t)7
};

struct KeyMapChart_t {
    uint8_t x_1;
    uint8_t x_2;
};

static const KeyMapChart_t X_MAP_CHART[7] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7}, {8, 9}, {10, 11}, {12, 13}
};

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

struct KeyCoord {
    int x;
    int y;
};

static void keyboard_task(void* pvParameters) {
    (void)pvParameters;
    static char prev_keys[16] = {0};
    static int prev_key_count = 0;

    while (true) {
        KeyCoord active_coords[16];
        int active_count = 0;

        // 1. Scan 8 demultiplexer lines via 74HC138 (A0=8, A1=9, A2=11)
        for (int i = 0; i < 8; ++i) {
            gpio_set_level((gpio_num_t)PIN_KB_A0, (i & 1));
            gpio_set_level((gpio_num_t)PIN_KB_A1, (i & 2) >> 1);
            gpio_set_level((gpio_num_t)PIN_KB_A2, (i & 4) >> 2);
            esp_rom_delay_us(10); // Settle propagation delay

            // Read 7 active-low column pins
            for (int j = 0; j < 7; ++j) {
                if (gpio_get_level(KB_ROWS[j]) == 0) {
                    if (active_count < 16) {
                        int kx = (i > 3) ? X_MAP_CHART[j].x_1 : X_MAP_CHART[j].x_2;
                        int ky = (i > 3) ? (i - 4) : i;
                        ky = 3 - ky; // Invert to align with layout rows 0..3
                        active_coords[active_count++] = { kx, ky };
                    }
                }
            }
        }

        // 2. Identify active modifier keys
        bool shift_active = false;
        bool fn_active = false;
        bool ctrl_active = false;

        for (int k = 0; k < active_count; ++k) {
            int x = active_coords[k].x;
            int y = active_coords[k].y;
            if (y == 2 && x == 0) fn_active = true;
            if (y == 2 && x == 1) shift_active = true;
            if (y == 3 && x == 0) ctrl_active = true;
        }

        // 3. Process pressed non-modifier keys
        char current_pressed[16] = {0};
        int current_count = 0;

        for (int k = 0; k < active_count; ++k) {
            int x = active_coords[k].x;
            int y = active_coords[k].y;

            // Skip pure modifiers
            if (y == 2 && (x == 0 || x == 1)) continue;
            if (y == 3 && (x >= 0 && x <= 2)) continue;
            if (x < 0 || x >= 14 || y < 0 || y >= 4) continue;

            char ch = '\0';
            if (fn_active) {
                if (y == 0 && x == 0) ch = 27; // Esc
                else if (y == 2 && x == 11) ch = 'U'; // Up
                else if (y == 3 && x == 10) ch = 'L'; // Left
                else if (y == 3 && x == 11) ch = 'D'; // Down
                else if (y == 3 && x == 12) ch = 'R'; // Right
                else ch = KEYMAP_NORMAL[y][x];
            } else if (shift_active) {
                ch = KEYMAP_SHIFTED[y][x];
            } else {
                ch = KEYMAP_NORMAL[y][x];
            }

            if (ch != '\0' && current_count < 16) {
                current_pressed[current_count++] = ch;
            }
        }

        // 4. Edge-detect newly pressed keys (transition from unpressed -> pressed)
        for (int i = 0; i < current_count; ++i) {
            char ch = current_pressed[i];
            bool was_pressed = false;
            for (int p = 0; p < prev_key_count; ++p) {
                if (prev_keys[p] == ch) {
                    was_pressed = true;
                    break;
                }
            }

            if (!was_pressed) {
                last_key_pressed = ch;
                printf("[KEYBOARD] Key Pressed: '%c'\n", ch);
                fflush(stdout);

                // Update physical LCD display
                char buf[32];
                if (ch == '\b') {
                    std::snprintf(buf, sizeof(buf), "KEY: [BACKSPACE]");
                } else if (ch == '\n') {
                    std::snprintf(buf, sizeof(buf), "KEY: [ENTER]");
                } else if (ch == '\t') {
                    std::snprintf(buf, sizeof(buf), "KEY: [TAB]");
                } else if (ch == ' ') {
                    std::snprintf(buf, sizeof(buf), "KEY: [SPACE]");
                } else {
                    std::snprintf(buf, sizeof(buf), "KEY: '%c' (0x%02X)", ch, (uint8_t)ch);
                }
                display_fill_rect(8, 120, 220, 12, COLOR_BG_DARK);
                display_draw_string(8, 120, buf, COLOR_CYAN, COLOR_BG_DARK);
            }
        }

        // Save state for next cycle
        for (int i = 0; i < current_count; ++i) {
            prev_keys[i] = current_pressed[i];
        }
        prev_key_count = current_count;

        vTaskDelay(pdMS_TO_TICKS(20)); // 50 Hz debounce scan rate
    }
}

void init_keyboard_scanner() {
    // Column outputs
    gpio_config_t col_cfg = {
        .pin_bit_mask = (1ULL << PIN_KB_A0) | (1ULL << PIN_KB_A1) | (1ULL << PIN_KB_A2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&col_cfg);

    // Row inputs with internal pull-up
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

    xTaskCreatePinnedToCore(keyboard_task, "k32_kb_scan", 3072, nullptr, 4, nullptr, 1);
    ESP_LOGI(TAG, "Keyboard 56-key matrix scanner started on Core 1.");
}

char get_last_key_pressed() {
    return last_key_pressed;
}

} // namespace kube32::hal

#endif // KUBE32_TARGET_ESP32
