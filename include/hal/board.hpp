#pragma once

// A board's on-board peripherals behind one interface: the pod API, the boot
// screen and the console's diagnostics go through it. The board profile in
// NVS (K32_CMD:BOARD:) selects the implementation by name.

#include "hal/capability_firewall.hpp"
#include "hal/hal_types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace kube32::hal {

class Board {
public:
    virtual ~Board() = default;

    // The kube32.io/board label of the node
    virtual const char* name() const = 0;
    // Brings up the peripherals; false when they did not answer (the
    // display, on a board with one), and the node then offers pods none of them
    virtual bool init() = 0;
    // Stops DMA/peripherals cleanly before software reset
    virtual void shutdown() {}
    // The pins the board keeps, the buses it offers and its on-board devices
    virtual void describe(BoardProfile& profile) const = 0;
    // Extra board/hardware labels to publish on the Node object
    virtual void extra_labels(std::map<std::string, std::string>&) const {}
    // True when the board has the Cardputer-wired microSD SPI slot
    virtual bool has_sd_card_slot() const { return false; }
    // True when UART0 pins are wired to on-board peripherals (console uses USB only)
    virtual bool usb_only_console() const { return false; }

    // A board without a screen answers a zero size and HAL_ERR_NODEV
    virtual DisplayInfo display_info() const { return {}; }
    virtual int32_t display_text(int32_t, int32_t, const char*, size_t, uint16_t, uint16_t) { return HAL_ERR_NODEV; }
    virtual int32_t display_rect(int32_t, int32_t, int32_t, int32_t, uint16_t) { return HAL_ERR_NODEV; }
    virtual int32_t display_bar(int32_t, int32_t, int32_t, int32_t, uint32_t, uint16_t, uint16_t) { return HAL_ERR_NODEV; }
    virtual int32_t display_blit(int32_t, int32_t, int32_t, int32_t, const uint16_t*) { return HAL_ERR_NODEV; }

    // The next key, or HAL_ERR_AGAIN; a board without keys answers HAL_ERR_NODEV
    virtual int32_t key_read() { return HAL_ERR_NODEV; }
    virtual void key_inject(char) {}
    // Points touching the panel now, or a negative HAL_ERR_*
    virtual int32_t touch_read(TouchPoint*, uint32_t) { return HAL_ERR_NODEV; }

    virtual int32_t tone(uint32_t, uint32_t, uint32_t) { return HAL_ERR_NODEV; }
    virtual int32_t audio_play(const int16_t*, uint32_t, uint32_t, uint32_t) { return HAL_ERR_NODEV; }
    virtual int32_t mic_read(int16_t*, uint32_t, uint32_t) { return HAL_ERR_NODEV; }
    virtual void mic_set_gain(uint32_t) {}

    // Sets addressable LED `index` to 0xRRGGBB: HAL_OK, HAL_ERR_INVAL for an
    // index past the board's LEDs, HAL_ERR_BUSY or HAL_ERR_IO
    virtual int32_t led_set(uint32_t, uint32_t) { return HAL_ERR_NODEV; }
    // Sets vibration motor strength (0..100%)
    virtual int32_t motor_set(uint32_t) { return HAL_ERR_NODEV; }
};

#if defined(KUBE32_TARGET_ESP32)
// The implementation for a profile name, nullptr for a board without
// on-board peripherals ("generic", an empty name, an unknown name)
Board* board_for(std::string_view name);
// Auto-detected board when NVS "board" is unset (e.g. eFuse-identified ESP-Mosaico)
Board* default_board();
#else
inline Board* board_for(std::string_view) { return nullptr; }
inline Board* default_board() { return nullptr; }
#endif

}  // namespace kube32::hal
