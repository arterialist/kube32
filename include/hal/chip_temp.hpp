#pragma once

#include <cstdint>

#if !defined(KUBE32_TARGET_ESP32)
#include <functional>
#endif

namespace kube32::hal {

// The ESP32-S3's internal temperature sensor (esp32_chip_temp_read, granted
// by hardware.kube32.io/chip-temp). The driver is installed on the first read
// and stays installed. It reads the die, which runs above the ambient
// temperature. Stores milli-degrees Celsius and returns HAL_OK, HAL_ERR_NODEV
// when the sensor cannot be installed, or HAL_ERR_IO. On the host the stub
// HAL answers (MockHal::set_chip_temp), or the host's own sensor.
int32_t chip_temp_read(int32_t& out_milli_c);

#if !defined(KUBE32_TARGET_ESP32)
// The host's own sensor in place of the stub HAL's (a Linux board's thermal
// zone); an empty function hands the reads back to the stub. Set before pods run.
void set_host_chip_temp(std::function<int32_t(int32_t& out_milli_c)> read);
#endif

} // namespace kube32::hal
