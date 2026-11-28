#pragma once

// WS2812 addressable RGB LEDs on one data pin (k32_led_set). The LEDs keep
// one RMT TX channel from init on: BoardProfile::rmt_tx_board_channels takes
// it out of what pods may claim. Between frames the channel holds the data
// pin low, the LEDs' idle level.

#include <cstdint>

#if defined(KUBE32_TARGET_ESP32)
#include "driver/rmt_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

namespace kube32::hal {

class AddressableLed {
public:
    static constexpr uint32_t kMaxLeds = 8;

    AddressableLed(int gpio, uint32_t count) : gpio_(gpio), count_(count < kMaxLeds ? count : kMaxLeds) {}

    // Takes the RMT channel and turns the LEDs off: false when that fails
    bool init();
    // Sets LED `index` to 0xRRGGBB and sends the frame: HAL_OK, HAL_ERR_INVAL
    // for an index past the LEDs, HAL_ERR_NODEV before init, HAL_ERR_BUSY,
    // HAL_ERR_IO
    int32_t set(uint32_t index, uint32_t rgb);
    uint32_t count() const { return count_; }

private:
    int32_t send_locked();

    int gpio_;
    uint32_t count_;
    uint8_t grb_[3 * kMaxLeds]{};   // wire order: green, red, blue
#if defined(KUBE32_TARGET_ESP32)
    SemaphoreHandle_t mutex_{nullptr};
    rmt_channel_handle_t chan_{nullptr};
    rmt_encoder_handle_t encoder_{nullptr};
#endif
};

}  // namespace kube32::hal
