#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#if defined(KUBE32_TARGET_ESP32)

namespace kube32::hal {

// Pinout of the M5Stack Cardputer (board profile "cardputer"): display,
// speaker and keyboard matrix
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
constexpr int PIN_MIC_CLK       = 43;   // SPM1423 PDM clock: shares the speaker's LRCK pin
constexpr int PIN_MIC_DATA      = 46;

constexpr int PIN_KB_A0         = 8;
constexpr int PIN_KB_A1         = 9;
constexpr int PIN_KB_A2         = 11;

constexpr int LCD_WIDTH         = 240;
constexpr int LCD_HEIGHT        = 135;

// RGB565 colors
constexpr uint16_t COLOR_BLACK   = 0x0000;
constexpr uint16_t COLOR_WHITE   = 0xFFFF;
constexpr uint16_t COLOR_CYAN    = 0x07FF;
constexpr uint16_t COLOR_GREEN   = 0x07E0;
constexpr uint16_t COLOR_NAVY    = 0x000F;
constexpr uint16_t COLOR_BG_DARK = 0x10A2; // #141218

// Display. Rendering streams dirty rectangles through a 4 KB line buffer
// (no full framebuffer), so the driver costs ~5 KB of internal RAM.
bool init_display_and_backlight();
void set_backlight(bool enabled);
void display_fill_rect(int x, int y, int w, int h, uint16_t color);
// w x h RGB565 pixels, row after row, to (x, y); the part outside the screen is dropped
void display_blit(int x, int y, int w, int h, const uint16_t* pixels);
void display_clear(uint16_t color = COLOR_BG_DARK);
// 6x8 cell font; strings longer than the panel are clipped
void display_draw_string(int x, int y, const char* str, uint16_t color = COLOR_WHITE, uint16_t bg = COLOR_BG_DARK);
void display_draw_string_n(int x, int y, const char* str, size_t len, uint16_t color, uint16_t bg);
void display_draw_bar(int x, int y, int w, int h, uint32_t pct, uint16_t fg, uint16_t bg);
bool display_available();

// Speaker (NS4168 over I2S) and microphone (SPM1423, PDM). They share
// GPIO 43 and I2S0, so one runs at a time; the speaker comes first. Every
// sample reaches the amplifier at most at a quarter of full scale.
// init_speaker() sets up both; the channels exist only while sound moves.
bool init_speaker();
void play_tone(uint32_t freq_hz, uint32_t duration_ms, uint8_t volume_pct = 20);
// Queues `count` mono samples at `sample_rate` (8000..48000 Hz, at most one
// second a call) at volume_pct of the ceiling: count, or -22 (bad rate),
// -16 (busy), -5 (I2S error)
int32_t audio_play(const int16_t* samples, uint32_t count, uint32_t sample_rate, uint32_t volume_pct);
// Up to max_samples of 16 kHz mono PCM from the microphone, waiting up to
// timeout_ms: the count, or -16 while the speaker still plays, -5 on an
// I2S error. max_samples 0 stops the microphone.
int32_t mic_read(int16_t* out, uint32_t max_samples, uint32_t timeout_ms);
// Digital gain of k32_mic_read after the DC offset is removed (1 to 64, 16 by default)
void mic_set_gain(uint32_t gain);

// GPIOs driven by the display, speaker and keyboard drivers above, for the
// board profile's reserved pins.
uint64_t peripheral_gpio_mask();

// Keyboard matrix. Key presses queue up (16 deep); pods drain the queue.
bool init_keyboard_scanner();
int  keyboard_pop();          // ASCII code, or -1 when empty
void keyboard_inject(char c);  // queue a key as if the matrix produced it (console K32_CMD:KEY)
char get_last_key_pressed();

} // namespace kube32::hal

#endif // KUBE32_TARGET_ESP32
