#pragma once

// What the chip offers pods, from ESP-IDF's capability headers. The simulator
// models an ESP32-S3.

#include <cstdint>
#include <string>

#if defined(ESP_PLATFORM)
#include "esp_bit_defs.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#endif

namespace kube32::hal {

#if defined(ESP_PLATFORM)
// The chip's pads
constexpr uint64_t KUBE32_CHIP_GPIO_MASK = SOC_GPIO_VALID_GPIO_MASK;
constexpr uint32_t KUBE32_ADC1_CHANNELS = SOC_ADC_CHANNEL_NUM(0);
// The HP I2C controllers pods may claim, and UART1/UART2 (UART0 is the console)
constexpr uint8_t KUBE32_CHIP_I2C_MASK = SOC_HP_I2C_NUM >= 2 ? 0x3 : 0x1;
constexpr uint8_t KUBE32_CHIP_UART_MASK = SOC_UART_HP_NUM >= 3 ? 0x6 : 0x2;
#else
// ESP32-S3 pads: GPIO 0-21 and 26-48 (the chip has no GPIO 22-25).
constexpr uint64_t KUBE32_CHIP_GPIO_MASK = ((1ULL << 22) - 1) | (((1ULL << 49) - 1) & ~((1ULL << 26) - 1));
constexpr uint32_t KUBE32_ADC1_CHANNELS = 10;
constexpr uint8_t KUBE32_CHIP_I2C_MASK = 0x3;
constexpr uint8_t KUBE32_CHIP_UART_MASK = 0x6;
#endif

// Reserved pins no pod claims: boot strapping, native USB, SPI flash and
// in-package PSRAM, and the UART0 console. KUBE32_INPUT_GPIO_MASK: reserved
// pins a pod may still claim as an input (esp32.io/gpio-input-<n>); reading
// a strapping pin, with a pull-up or not, leaves boot strapping as it is.
#if !defined(ESP_PLATFORM) || defined(CONFIG_IDF_TARGET_ESP32S3)
// Strapping 0, native USB 19-20, SPI flash and quad PSRAM 26-32, UART0 43-44.
// GPIO 0 is the boot button of most boards.
constexpr uint64_t KUBE32_RESERVED_GPIO_MASK =
    (1ULL << 0) | (0x7FULL << 26) | (1ULL << 43) | (1ULL << 44) | (1ULL << 19) | (1ULL << 20);
constexpr uint64_t KUBE32_INPUT_GPIO_MASK = 1ULL << 0;
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
// Strapping 2, 8, 9, SPI flash 11-17, native USB 18-19, UART0 20-21.
// GPIO 9 is the boot button of most boards.
constexpr uint64_t KUBE32_RESERVED_GPIO_MASK =
    (1ULL << 2) | (1ULL << 8) | (1ULL << 9) | (0x7FULL << 11) | (0x3ULL << 18) | (0x3ULL << 20);
constexpr uint64_t KUBE32_INPUT_GPIO_MASK = 1ULL << 9;
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
// Strapping and UART0 34-38, native USB 24-25. GPIO 35 is the boot button of most boards.
constexpr uint64_t KUBE32_RESERVED_GPIO_MASK = (0x1FULL << 34) | (0x3ULL << 24);
constexpr uint64_t KUBE32_INPUT_GPIO_MASK = 1ULL << 35;
#else
// The board profile reserves the pins its chip keeps
constexpr uint64_t KUBE32_RESERVED_GPIO_MASK = 0;
constexpr uint64_t KUBE32_INPUT_GPIO_MASK = 0;
#endif

constexpr uint64_t KUBE32_SAFE_GPIO_MASK = KUBE32_CHIP_GPIO_MASK & ~KUBE32_RESERVED_GPIO_MASK;

// The GPIO ADC1 channel `ch` samples, -1 when the channel has none
int adc1_channel_gpio(uint32_t ch);

// The chip's name for a person: "ESP32-S3" (the simulator's chip) for target
// esp32s3, from CONFIG_IDF_TARGET on the board
std::string chip_display_name();

}  // namespace kube32::hal
