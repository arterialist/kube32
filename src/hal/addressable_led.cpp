#include "hal/addressable_led.hpp"

#if defined(KUBE32_TARGET_ESP32)

#include "hal/hal_types.hpp"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

namespace kube32::hal {

namespace {

const char* TAG = "led";

// WS2812 bit timing at 10 MHz: a 0 is 0.3 us high and 0.9 us low, a 1 the
// reverse. The LEDs latch a frame once the line has been low for 280 us.
constexpr uint32_t kResolutionHz = 10000000;
constexpr uint32_t kLatchUs = 300;

}  // namespace

bool AddressableLed::init() {
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) return false;
    rmt_tx_channel_config_t cfg = {};
    cfg.gpio_num = static_cast<gpio_num_t>(gpio_);
    cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    cfg.resolution_hz = kResolutionHz;
    cfg.mem_block_symbols = 48;
    cfg.trans_queue_depth = 1;
    rmt_bytes_encoder_config_t ec = {};
    ec.bit0.level0 = 1;
    ec.bit0.duration0 = 3;
    ec.bit0.level1 = 0;
    ec.bit0.duration1 = 9;
    ec.bit1.level0 = 1;
    ec.bit1.duration0 = 9;
    ec.bit1.level1 = 0;
    ec.bit1.duration1 = 3;
    ec.flags.msb_first = 1;
    esp_err_t e = rmt_new_tx_channel(&cfg, &chan_);
    if (e == ESP_OK) e = rmt_new_bytes_encoder(&ec, &encoder_);
    if (e == ESP_OK) e = rmt_enable(chan_);
    if (e != ESP_OK) {
        if (encoder_) rmt_del_encoder(encoder_);
        if (chan_) rmt_del_channel(chan_);
        encoder_ = nullptr;
        chan_ = nullptr;
        ESP_LOGW(TAG, "no RMT TX channel for the LEDs on GPIO %d: %s", gpio_, esp_err_to_name(e));
        return false;
    }
    // The channel drives the line low from here on: the LEDs see a reset
    // before the first frame
    esp_rom_delay_us(kLatchUs);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const int32_t rc = send_locked();
    xSemaphoreGive(mutex_);
    ESP_LOGI(TAG, "%u addressable LED(s) on GPIO %d, RMT TX at 10 MHz", (unsigned)count_, gpio_);
    return rc == HAL_OK;
}

int32_t AddressableLed::set(uint32_t index, uint32_t rgb) {
    if (!chan_) return HAL_ERR_NODEV;
    if (index >= count_) return HAL_ERR_INVAL;
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(200)) != pdTRUE) return HAL_ERR_BUSY;
    grb_[3 * index] = static_cast<uint8_t>(rgb >> 8);
    grb_[3 * index + 1] = static_cast<uint8_t>(rgb >> 16);
    grb_[3 * index + 2] = static_cast<uint8_t>(rgb);
    const int32_t rc = send_locked();
    xSemaphoreGive(mutex_);
    return rc;
}

int32_t AddressableLed::send_locked() {
    rmt_transmit_config_t tc = {};
    tc.flags.eot_level = 0;
    esp_err_t e = rmt_transmit(chan_, encoder_, grb_, 3 * count_, &tc);
    if (e == ESP_OK) e = rmt_tx_wait_all_done(chan_, 100);   // timeout in ms
    // The next frame starts after this one has latched
    esp_rom_delay_us(kLatchUs);
    return e == ESP_OK ? HAL_OK : HAL_ERR_IO;
}

}  // namespace kube32::hal

#endif  // KUBE32_TARGET_ESP32
