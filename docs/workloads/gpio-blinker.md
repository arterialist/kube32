# GPIO and other peripherals

`tools/kube32-cli check` points here when a module imports any of the
`esp32_*` peripheral functions. This page says what those functions do
today. In short: the code exists, but no example pod and no hardware test in
this repository uses it.

Earlier versions of this page described a "GPIO blinker" pod and showed its
log output. No such image was ever built, so that page was removed.

## The functions

Linked from `src/runtime/host_functions.cpp`:

| Import | Signature | Board behaviour |
| --- | --- | --- |
| `esp32_gpio_set_mode(pin, mode)` | `i(ii)` | `gpio_config` on a claimed pin |
| `esp32_gpio_write(pin, level)` | `i(ii)` | `gpio_set_level` on a claimed pin |
| `esp32_gpio_read(pin)` | `i(i)` | `gpio_get_level` on a claimed pin |
| `esp32_adc_read(channel)` | `i(i)` | ADC1 one-shot read on a claimed channel |
| `esp32_i2c_write`, `esp32_i2c_read` | `i(iiii)` | fail with an I/O error (-5): the firmware never installs an I2C driver |
| `esp32_uart_write`, `esp32_uart_read` | `i(iii)` | fail: the firmware never installs a UART1 or UART2 driver |
| `esp32_delay_ms(ms)` | `v(i)` | task delay, no grant needed |
| `esp32_get_time_us()` | `I()` | milliseconds since boot times 1000 |

The simulator routes the same calls to a mock HAL (`src/hal/mock_hal.cpp`).
The C++ tests (`test_capability_firewall`, `test_hal_adversarial`) check the
firewall rules against that mock.

## Granting access

Pins, ADC channels and buses are requested as resources, for example
`esp32.io/gpio-10: "1"` for pin 10; the full list is on the
[capability firewall](../architecture/capability-firewall.md) page. A call on
a pin the pod has not claimed returns -1.

Before wiring anything to a Cardputer, read the list of pins the firewall does
not protect on that page. Driving one of them from a pod can disturb the
display, keypad, speaker or card.
