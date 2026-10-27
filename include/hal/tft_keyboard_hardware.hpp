#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#if defined(KUBE32_TARGET_ESP32)

namespace kube32::hal {

// Standard Pinout for Integrated Display & Keyboard Silicon
constexpr int PIN_LCD_MOSI      = 35;
constexpr int PIN_LCD_SCLK      = 36;
constexpr int PIN_LCD_CS        = 37;
constexpr int PIN_LCD_DC        = 34;
constexpr int PIN_LCD_RST       = 33;
constexpr int PIN_LCD_BACKLIGHT = 38;

constexpr int PIN_I2S_BCLK      = 41;
constexpr int PIN_I2S_DATA      = 42;
constexpr int PIN_I2S_LRCK      = 43;
constexpr int PIN_SPEAKER       = 42;

constexpr int PIN_KB_A0         = 8;
constexpr int PIN_KB_A1         = 9;
constexpr int PIN_KB_A2         = 11;

constexpr int LCD_WIDTH         = 240;
constexpr int LCD_HEIGHT        = 135;

// Color 565 definitions
constexpr uint16_t COLOR_BLACK   = 0x0000;
constexpr uint16_t COLOR_WHITE   = 0xFFFF;
constexpr uint16_t COLOR_CYAN    = 0x07FF;
constexpr uint16_t COLOR_GREEN   = 0x07E0;
constexpr uint16_t COLOR_NAVY    = 0x000F;
constexpr uint16_t COLOR_BG_DARK = 0x10A2; // #141218 in RGB565

// Initialize ST7789 Display & Backlight
void init_display_and_backlight();

// Set Backlight Power (0 = Off, 1 = On)
void set_backlight(bool enabled);

// Display Drawing Primitives
void display_fill_rect(int x, int y, int w, int h, uint16_t color);
void display_clear(uint16_t color = COLOR_BG_DARK);
void display_draw_string(int x, int y, const char* str, uint16_t color = COLOR_WHITE, uint16_t bg = COLOR_BG_DARK);

// Speaker Acoustic Primitives
void init_speaker();
void play_tone(uint32_t freq_hz, uint32_t duration_ms);
void play_boot_chime();

// Keyboard Matrix Scanner
void init_keyboard_scanner();
char get_last_key_pressed();

} // namespace kube32::hal

#endif // KUBE32_TARGET_ESP32
