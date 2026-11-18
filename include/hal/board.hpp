#pragma once

// A board's on-board peripherals behind one interface: the pod API, the boot
// screen and the console's diagnostics go through it. The board profile in
// NVS (K32_CMD:BOARD:) selects the implementation by name.

#include "hal/capability_firewall.hpp"
#include "hal/hal_types.hpp"

#include <cstddef>
#include <cstdint>
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
    // The pins the board keeps, the buses it offers and its on-board devices
    virtual void describe(BoardProfile& profile) const = 0;

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
};

// The implementation for a profile name, nullptr for a board without
// on-board peripherals ("generic", an empty name, an unknown name)
Board* board_for(std::string_view name);

}  // namespace kube32::hal
