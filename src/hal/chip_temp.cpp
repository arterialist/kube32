#include "hal/chip_temp.hpp"
#include "hal/hal_types.hpp"

#if defined(KUBE32_TARGET_ESP32)
#include "driver/temperature_sensor.h"
#include <cmath>
#include <mutex>
#else
#include "hal/mock_hal.hpp"
#endif

namespace kube32::hal {

#if defined(KUBE32_TARGET_ESP32)

static std::mutex s_mutex;
static temperature_sensor_handle_t s_sensor = nullptr;

int32_t chip_temp_read(int32_t& out_milli_c) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_sensor) {
        // -10 to 80 C: the range with the smallest error (under 1 C)
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        temperature_sensor_handle_t sensor = nullptr;
        if (temperature_sensor_install(&cfg, &sensor) != ESP_OK) return HAL_ERR_NODEV;
        if (temperature_sensor_enable(sensor) != ESP_OK) {
            temperature_sensor_uninstall(sensor);
            return HAL_ERR_NODEV;
        }
        s_sensor = sensor;
    }
    float celsius = 0;
    if (temperature_sensor_get_celsius(s_sensor, &celsius) != ESP_OK) return HAL_ERR_IO;
    out_milli_c = static_cast<int32_t>(std::lround(celsius * 1000.0f));
    return HAL_OK;
}

#else

int32_t chip_temp_read(int32_t& out_milli_c) {
    return MockHal::instance().chip_temp_read(out_milli_c);
}

#endif

} // namespace kube32::hal
