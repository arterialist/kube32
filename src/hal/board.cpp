#include "hal/board.hpp"

#if defined(KUBE32_TARGET_ESP32)

#include "hal/sd_card.hpp"
#include "hal/tft_keyboard_hardware.hpp"

namespace kube32::hal {

namespace {

// The M5Stack Cardputer: ST7789 panel, matrix keyboard, I2S speaker and PDM
// microphone (hal/tft_keyboard_hardware.hpp)
class CardputerBoard final : public Board {
public:
    const char* name() const override { return "cardputer"; }

    bool init() override {
        const bool has_display = init_display_and_backlight();
        init_speaker();
        init_keyboard_scanner();
        return has_display;
    }

    void describe(BoardProfile& profile) const override {
        // Its only free connector is the Grove port (GPIO 2 and 1), so it
        // offers I2C0 or UART1 there. Its IR LED hangs on GPIO 44, the UART0
        // RX pad: the board wires nothing else to it and its console is the
        // USB port, so an RMT TX channel may drive it (the default TX pin)
        profile.name = name();
        profile.reserved_gpio_mask = peripheral_gpio_mask() | sd_gpio_mask();
        profile.i2c_bus_mask = 0x1;
        profile.i2c_pins[0] = {2, 1};     // Grove SDA, SCL
        profile.i2c_pins[1] = {};
        profile.uart_port_mask = 0x2;
        profile.uart_pins[1] = {2, 1};    // Grove TX, RX
        profile.rmt_tx_pin_mask = 1ULL << 44;
        profile.rmt_tx_default_pin = 44;  // IR LED
        profile.onboard_devices = KUBE32_CAP_DISPLAY | KUBE32_CAP_KEYBOARD | KUBE32_CAP_SPEAKER | KUBE32_CAP_MICROPHONE;
    }

    DisplayInfo display_info() const override { return {LCD_WIDTH, LCD_HEIGHT, KUBE32_DISPLAY_RGB565}; }

    int32_t display_text(int32_t x, int32_t y, const char* text, size_t len, uint16_t fg, uint16_t bg) override {
        display_draw_string_n(x, y, text, len, fg, bg);
        return HAL_OK;
    }
    int32_t display_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) override {
        display_fill_rect(x, y, w, h, color);
        return HAL_OK;
    }
    int32_t display_bar(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pct, uint16_t fg, uint16_t bg) override {
        display_draw_bar(x, y, w, h, pct, fg, bg);
        return HAL_OK;
    }
    int32_t display_blit(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* pixels) override {
        hal::display_blit(x, y, w, h, pixels);
        return HAL_OK;
    }

    int32_t key_read() override {
        const int k = keyboard_pop();
        return k < 0 ? HAL_ERR_AGAIN : k;
    }
    void key_inject(char c) override { keyboard_inject(c); }

    int32_t tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct) override {
        play_tone(freq_hz, duration_ms, static_cast<uint8_t>(volume_pct));
        return HAL_OK;
    }
    int32_t audio_play(const int16_t* samples, uint32_t count, uint32_t sample_rate, uint32_t volume_pct) override {
        return hal::audio_play(samples, count, sample_rate, volume_pct);
    }
    int32_t mic_read(int16_t* out, uint32_t max_samples, uint32_t timeout_ms) override {
        return hal::mic_read(out, max_samples, timeout_ms);
    }
    void mic_set_gain(uint32_t gain) override { hal::mic_set_gain(gain); }
};

}  // namespace

Board* board_for(std::string_view name) {
    static CardputerBoard cardputer;
    if (name == "cardputer" || name == "display") return &cardputer;
    return nullptr;
}

}  // namespace kube32::hal

#endif  // KUBE32_TARGET_ESP32
