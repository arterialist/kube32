#if defined(KUBE32_TARGET_ESP32)

#include "hal/tft_keyboard_hardware.hpp"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdio>

static const char* TAG = "Kube32Hardware";

namespace kube32::hal {

static spi_device_handle_t spi_display = nullptr;
static volatile char last_key_pressed = '\0';

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
// ST7789 Low-Level SPI Communication
// ---------------------------------------------------------------------------
static void lcd_cmd(uint8_t cmd) {
    gpio_set_level((gpio_num_t)PIN_LCD_DC, 0); // Command mode
    spi_transaction_t t;
    std::memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_buffer = &cmd;
    spi_device_polling_transmit(spi_display, &t);
}

static void lcd_data(const uint8_t* data, int len) {
    if (len <= 0) return;
    gpio_set_level((gpio_num_t)PIN_LCD_DC, 1); // Data mode
    spi_transaction_t t;
    std::memset(&t, 0, sizeof(t));
    t.length = len * 8;
    t.tx_buffer = data;
    spi_device_polling_transmit(spi_display, &t);
}

static void lcd_data_byte(uint8_t byte) {
    lcd_data(&byte, 1);
}

static void set_window(int x0, int y0, int x1, int y1) {
    // ST7789 240x135 in Landscape (MADCTL 0x70) offset:
    // X offset = 40, Y offset = 53
    uint16_t sx = x0 + 40;
    uint16_t ex = x1 + 40;
    uint16_t sy = y0 + 53;
    uint16_t ey = y1 + 53;

    lcd_cmd(0x2A); // CASET
    uint8_t data_x[] = { (uint8_t)(sx >> 8), (uint8_t)(sx & 0xFF), (uint8_t)(ex >> 8), (uint8_t)(ex & 0xFF) };
    lcd_data(data_x, sizeof(data_x));

    lcd_cmd(0x2B); // RASET
    uint8_t data_y[] = { (uint8_t)(sy >> 8), (uint8_t)(sy & 0xFF), (uint8_t)(ey >> 8), (uint8_t)(ey & 0xFF) };
    lcd_data(data_y, sizeof(data_y));
}

// ---------------------------------------------------------------------------
// Display Initialization & Primitives
// ---------------------------------------------------------------------------
void init_display_and_backlight() {
    ESP_LOGI(TAG, "Initializing ST7789 LCD & Backlight on GPIO %d...", PIN_LCD_BACKLIGHT);

    // 1. Backlight GPIO 38
    gpio_reset_pin((gpio_num_t)PIN_LCD_BACKLIGHT);
    gpio_set_direction((gpio_num_t)PIN_LCD_BACKLIGHT, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_LCD_BACKLIGHT, 1); // Turn ON backlight!

    // 2. Control pins: DC, RST, and CS
    gpio_reset_pin((gpio_num_t)PIN_LCD_DC);
    gpio_set_direction((gpio_num_t)PIN_LCD_DC, GPIO_MODE_OUTPUT);

    gpio_reset_pin((gpio_num_t)PIN_LCD_RST);
    gpio_set_direction((gpio_num_t)PIN_LCD_RST, GPIO_MODE_OUTPUT);

    gpio_reset_pin((gpio_num_t)PIN_LCD_CS);
    gpio_set_direction((gpio_num_t)PIN_LCD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_LCD_CS, 1);

    // Hardware Reset Pulse
    gpio_set_level((gpio_num_t)PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level((gpio_num_t)PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    // 3. SPI Bus Configuration
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2
    };
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = 20 * 1000 * 1000, // 20 MHz SPI for reliable ST7789 comms
        .spics_io_num = -1, // Manual CS control to keep CS low across RAMWR and pixel streaming
        .queue_size = 7
    };
    spi_bus_add_device(SPI2_HOST, &devcfg, &spi_display);

    // 4. ST7789 Initialization Sequence (with CS held LOW)
    gpio_set_level((gpio_num_t)PIN_LCD_CS, 0);

    lcd_cmd(0x01); // SWRESET
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_cmd(0x11); // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(0x3A); // COLMOD
    lcd_data_byte(0x55); // 16-bit RGB565

    lcd_cmd(0x36); // MADCTL
    lcd_data_byte(0x70); // Landscape 240x135

    lcd_cmd(0x21); // INVON (Inversion On for IPS screen)

    lcd_cmd(0x13); // NORON
    vTaskDelay(pdMS_TO_TICKS(10));

    lcd_cmd(0x29); // DISPON
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_set_level((gpio_num_t)PIN_LCD_CS, 1);

    // Clear Screen & Draw Boot Screen
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

    ESP_LOGI(TAG, "ST7789 Display Initialized Successfully. Backlight ON.");
}

void set_backlight(bool enabled) {
    gpio_set_level((gpio_num_t)PIN_LCD_BACKLIGHT, enabled ? 1 : 0);
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    gpio_set_level((gpio_num_t)PIN_LCD_CS, 0); // Assert CS LOW

    set_window(x, y, x + w - 1, y + h - 1);
    lcd_cmd(0x2C); // RAMWR

    // Line buffer in big-endian RGB565
    int pixels = w * h;
    uint16_t color_be = (color << 8) | (color >> 8);
    uint16_t buffer[128];
    for (int i = 0; i < 128; ++i) buffer[i] = color_be;

    while (pixels > 0) {
        int chunk = (pixels > 128) ? 128 : pixels;
        lcd_data((uint8_t*)buffer, chunk * 2);
        pixels -= chunk;
    }

    gpio_set_level((gpio_num_t)PIN_LCD_CS, 1); // Deassert CS HIGH
}

void display_clear(uint16_t color) {
    display_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
}

void display_draw_char(int x, int y, char c, uint16_t color, uint16_t bg) {
    if (c < 32 || c > 126) c = 32;
    const uint8_t* col_bytes = FONT_5X7[c - 32];

    uint16_t char_buf[6 * 8];
    uint16_t color_be = (color << 8) | (color >> 8);
    uint16_t bg_be = (bg << 8) | (bg >> 8);

    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 5; ++col) {
            bool on = (row < 7) && (col_bytes[col] & (1 << row));
            char_buf[row * 6 + col] = on ? color_be : bg_be;
        }
        char_buf[row * 6 + 5] = bg_be; // 1-pixel gap
    }

    gpio_set_level((gpio_num_t)PIN_LCD_CS, 0);
    set_window(x, y, x + 5, y + 7);
    lcd_cmd(0x2C); // RAMWR
    lcd_data((uint8_t*)char_buf, sizeof(char_buf));
    gpio_set_level((gpio_num_t)PIN_LCD_CS, 1);
}

void display_draw_string(int x, int y, const char* str, uint16_t color, uint16_t bg) {
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
}

// ---------------------------------------------------------------------------
// Acoustic Synthesizer (LEDC PWM on Speaker Pin 42)
// ---------------------------------------------------------------------------
void init_speaker() {
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t chan_conf = {
        .gpio_num = PIN_SPEAKER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&chan_conf);
}

void play_tone(uint32_t freq_hz, uint32_t duration_ms) {
    if (freq_hz == 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        return;
    }
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq_hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 128); // 50% square wave
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void play_boot_chime() {
    play_tone(523, 80); // C5
    vTaskDelay(pdMS_TO_TICKS(20));
    play_tone(659, 80); // E5
    vTaskDelay(pdMS_TO_TICKS(20));
    play_tone(784, 80); // G5
    vTaskDelay(pdMS_TO_TICKS(20));
    play_tone(1046, 150); // C6
}

// ---------------------------------------------------------------------------
// Integrated 56-Key Matrix Keyboard Scanner
// ---------------------------------------------------------------------------
static const gpio_num_t KB_ROWS[] = {
    (gpio_num_t)13, (gpio_num_t)15, (gpio_num_t)3, (gpio_num_t)4,
    (gpio_num_t)5, (gpio_num_t)6, (gpio_num_t)7
};

// Keymap: 8 columns x 7 rows
static const char KEYMAP[8][7] = {
    { '`', '1', '2', '3', '4', '5', '6' },
    { '7', '8', '9', '0', '-', '=', '\b' },
    { '\t', 'q', 'w', 'e', 'r', 't', 'y' },
    { 'u', 'i', 'o', 'p', '[', ']', '\\' },
    { 'a', 's', 'd', 'f', 'g', 'h', 'j' },
    { 'k', 'l', ';', '\'', '\n', 'z', 'x' },
    { 'c', 'v', 'b', 'n', 'm', ',', '.' },
    { '/', ' ', ' ', ' ', ' ', ' ', ' ' }
};

static void keyboard_task(void* pvParameters) {
    (void)pvParameters;
    char prev_key = '\0';

    while (true) {
        char detected = '\0';

        // Scan 8 columns using 74HC138 demux (A0=8, A1=9, A2=11)
        for (int col = 0; col < 8; ++col) {
            gpio_set_level((gpio_num_t)PIN_KB_A0, (col & 1));
            gpio_set_level((gpio_num_t)PIN_KB_A1, (col & 2) >> 1);
            gpio_set_level((gpio_num_t)PIN_KB_A2, (col & 4) >> 2);
            esp_rom_delay_us(5); // Stabilization delay

            // Read rows (Active LOW)
            for (int r = 0; r < 7; ++r) {
                if (gpio_get_level(KB_ROWS[r]) == 0) {
                    detected = KEYMAP[col][r];
                    break;
                }
            }
            if (detected != '\0') break;
        }

        if (detected != '\0' && detected != prev_key) {
            last_key_pressed = detected;
            printf("[KEYBOARD] Key Pressed: '%c'\n", detected);

            // Audio click feedback
            play_tone(2200, 10);

            // Display on bottom line of physical LCD
            char buf[32];
            std::snprintf(buf, sizeof(buf), "KEY: '%c' (0x%02X)", detected, (uint8_t)detected);
            display_fill_rect(8, 120, 200, 10, COLOR_BG_DARK);
            display_draw_string(8, 120, buf, COLOR_CYAN, COLOR_BG_DARK);
        }
        prev_key = detected;

        vTaskDelay(pdMS_TO_TICKS(25)); // 40 Hz scan rate
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
