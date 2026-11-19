#include "hal/board.hpp"

#if defined(KUBE32_TARGET_ESP32)

#include "hal/sd_card.hpp"
#include "hal/tft_keyboard_hardware.hpp"

#if CONFIG_IDF_TARGET_ESP32S31
#include "hal/addressable_led.hpp"
#include "hal/es8311_audio.hpp"
#endif

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

// The M5Stack AtomS3 Lite: no screen, no PSRAM. It breaks out the Grove port
// (GPIO 2 and 1) and six header pins (5, 6, 7, 8, 38, 39); its button is on
// GPIO 41 (low when pressed), its IR LED on GPIO 4 and its WS2812 LED on
// GPIO 35. Pods may claim only the pins a user can reach and the button.
class AtomS3LiteBoard final : public Board {
public:
    const char* name() const override { return "atoms3-lite"; }

    bool init() override { return true; }

    void describe(BoardProfile& profile) const override {
        constexpr uint64_t reachable = (1ULL << 1) | (1ULL << 2) | (1ULL << 5) | (1ULL << 6) | (1ULL << 7) |
                                       (1ULL << 8) | (1ULL << 38) | (1ULL << 39) | (1ULL << 41);
        profile.name = name();
        profile.reserved_gpio_mask = KUBE32_CHIP_GPIO_MASK & ~reachable;
        profile.i2c_bus_mask = 0x1;
        profile.i2c_pins[0] = {2, 1};     // Grove SDA, SCL
        profile.i2c_pins[1] = {};
        profile.uart_port_mask = 0x2;
        profile.uart_pins[1] = {2, 1};    // Grove TX, RX
        profile.rmt_tx_pin_mask = 1ULL << 4;
        profile.rmt_tx_default_pin = 4;   // IR LED
        profile.onboard_devices = 0;
    }
};

#if CONFIG_IDF_TARGET_ESP32S31
// The Espressif ESP32-S31-Function-CoreBoard-1 (ESP32-S31-WROOM-3 module): an
// ES8311 codec with an NS4150B amplifier and an on-board microphone, a WS2812
// LED on GPIO 60 (through a level shifter, not inverted), the boot button on
// GPIO 61, and a gigabit Ethernet PHY. Header J2 breaks out GPIO 0-3, 4,
// 20-25, 35-40, 42-49 and 58-61. The PHY takes GPIO 4-19 (GPIO 4 is its
// interrupt line, also on the header), the codec 50-57, the USB-to-UART
// bridge 58 and 59 (UART0) and the LED 60. Pods may claim the other header
// pins; 36 and 37 (strapping) and 61 as inputs only.
class S31CoreBoard final : public Board {
public:
    const char* name() const override { return "s31-coreboard"; }

    bool init() override {
        audio_ok_ = audio_.init();
        led_ok_ = led_.init();
        return true;
    }

    void describe(BoardProfile& profile) const override {
        constexpr uint64_t header = 0xFULL | (0x3FULL << 20) | (0x3FULL << 35) | (0xFFULL << 42) | (1ULL << 61);
        profile.name = name();
        profile.reserved_gpio_mask = KUBE32_CHIP_GPIO_MASK & ~header;
        profile.i2c_bus_mask = 0x3;
        profile.i2c_pins[0] = {2, 3};     // J2 pins 7 and 10
        profile.i2c_pins[1] = {};
        profile.uart_port_mask = 0x6;
        profile.uart_pins[1] = {38, 39};  // J2 pins 22 and 21
        profile.uart_pins[2] = {};
        profile.rmt_tx_board_channels = led_ok_ ? 1 : 0;
        profile.onboard_devices = (audio_ok_ ? KUBE32_CAP_SPEAKER | KUBE32_CAP_MICROPHONE : 0) | (led_ok_ ? KUBE32_CAP_LED : 0);
    }

    int32_t tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct) override {
        return audio_ok_ ? audio_.tone(freq_hz, duration_ms, volume_pct) : HAL_ERR_NODEV;
    }
    int32_t audio_play(const int16_t* samples, uint32_t count, uint32_t sample_rate, uint32_t volume_pct) override {
        return audio_ok_ ? audio_.play(samples, count, sample_rate, volume_pct) : HAL_ERR_NODEV;
    }
    int32_t mic_read(int16_t* out, uint32_t max_samples, uint32_t timeout_ms) override {
        return audio_ok_ ? audio_.mic_read(out, max_samples, timeout_ms) : HAL_ERR_NODEV;
    }
    void mic_set_gain(uint32_t gain) override { audio_.mic_set_gain(gain); }
    int32_t led_set(uint32_t index, uint32_t rgb) override { return led_ok_ ? led_.set(index, rgb) : HAL_ERR_NODEV; }

private:
    // ES8311 control on ESP_I2C_SDA/SCL, I2S on I2S_MCLK/SCLK/LRCK/DSDIN/ASDOUT,
    // NS4150B enable on PA_CTRL (schematic nets)
    Es8311Audio audio_{{51, 50, 0x18, 52, 53, 55, 56, 54, 57}};
    AddressableLed led_{60, 1};
    bool audio_ok_{false};
    bool led_ok_{false};
};
#endif  // CONFIG_IDF_TARGET_ESP32S31

}  // namespace

Board* board_for(std::string_view name) {
    static CardputerBoard cardputer;
    static AtomS3LiteBoard atoms3_lite;
    if (name == "cardputer" || name == "display") return &cardputer;
    if (name == "atoms3-lite") return &atoms3_lite;
#if CONFIG_IDF_TARGET_ESP32S31
    static S31CoreBoard s31_coreboard;
    if (name == "s31-coreboard") return &s31_coreboard;
#endif
    return nullptr;
}

}  // namespace kube32::hal

#endif  // KUBE32_TARGET_ESP32
