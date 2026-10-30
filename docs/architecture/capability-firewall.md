# Capability firewall

The capability firewall in `src/hal/capability_firewall.cpp` controls which host functions and peripherals a pod may use.

At admission, the firewall converts the pod manifest into a grant, claims requested pins, channels, and buses, and installs ESP-IDF bus drivers. If a pin or bus is reserved or owned by another pod, admission fails with `CapabilityAdmissionFailed`. When you remove the pod, the firewall removes its bus drivers and releases its claims.

On every call, guarded host functions check the calling pod's grant. An ungranted call returns `K32_ERR_PERM`, which is `-1`, and logs `[FIREWALL] denied <pod>: ...`.

## Resource grants

The firewall reads `resources.requests`, or `resources.limits` if `requests` is absent. Every `esp32.io/` key also accepts the `esp.io/` prefix.

| Resource | `securityContext.capabilities.add` | Grants |
| --- | --- | --- |
| `hardware.kube32.io/ipc: "1"` | `CHANNEL_IPC`, `IPC` | `k32_event_emit`, `kube32_channel_open` |
| None | `SERVICE_CALL` | `k32_http_call` |
| `hardware.kube32.io/tft-display: "1"` | `DISPLAY`, `TFT_DISPLAY` | `k32_display_text`, `k32_display_rect`, `k32_display_bar` |
| `hardware.kube32.io/keyboard: "1"` | `KEYBOARD` | `k32_key_read` |
| `hardware.kube32.io/speaker: "1"` | `SPEAKER`, `AUDIO` | `k32_tone` |
| `hardware.kube32.io/sd: "1"` plus volume claim | `SD`, `SD_CARD` | `k32_fs_*` in the pod directory, see [storage](../storage.md) |
| `esp32.io/gpio-<N>` | None | Pin N read and write, value ignored |
| `esp32.io/gpio: "<count>"` | None | Free pins, lowest numbers first |
| `esp32.io/adc1-channel-<0..9>` or `esp32.io/adc: "<count>"` | None | ADC1 channels |
| `esp32.io/i2c-bus-<0\|1>` or `esp32.io/i2c: "<count>"` | None | I2C buses, `esp32_i2c_read` and `esp32_i2c_write` |
| `esp32.io/uart-<1\|2>` or `esp32.io/uart: "<count>"` | None | UART ports, `esp32_uart_read` and `esp32_uart_write` |
| `esp32.io/rmt-tx: "1"`, `esp32.io/rmt-rx: "1"` | None | One RMT (infrared) channel per direction, `esp32_rmt_send` and `esp32_rmt_receive` |

A volume declared with `persistentVolumeClaim` grants `k32_fs_*` on the node storage root without `hardware.kube32.io/sd`. Adding `hardware.kube32.io/sd: "1"`, `SD`, or a `kube32.io/sd` volume requires a mounted microSD card. Unrecognized capability names in `securityContext.capabilities.add` are ignored.

## Buses and pin annotations

Claiming an I2C bus or UART port claims its two GPIO pins and installs the ESP-IDF `i2c_master` or `uart_driver_install` driver while the pod runs. Calling `esp32_i2c_write` with length `0` probes an address, returning `0` on ACK or `-19` when no device answers.

Set pins and speeds in `metadata.annotations`, or omit the annotation to use the board profile defaults:

* `kube32.io/i2c-bus-<0|1>: "sda=<pin>,scl=<pin>[,hz=<1000..1000000>]"` with a default of `100000` Hz
* `kube32.io/uart-<1|2>: "tx=<pin>,rx=<pin>[,baud=<300..5000000>]"` with a default of `115200` baud

Buses without board defaults require an annotation. See `examples/i2cscan.yaml` and `apps/i2cscan`.

RMT channels take their pin the same way, from `kube32.io/rmt-tx: "pin=<n>[,carrier=<Hz>][,duty=<percent>]"` and `kube32.io/rmt-rx: "pin=<n>[,active=low|high][,idle_us=<n>]"` or the board default (GPIO 44, the IR LED, for TX on the `cardputer` profile; no other defaults). The firewall admits at most four channels of each direction, and the driver is created while the pod runs. See [peripherals](../peripherals.md#infrared-rmt), `examples/irremote.yaml`, and `apps/irremote`.

## Board profiles and reserved pins

The ESP32-S3 provides GPIO 0 to 21 and 26 to 48. `KUBE32_RESERVED_GPIO_MASK` in `include/hal/capability_firewall.hpp` reserves pins 0, 19, 20, 26 to 32, 43, and 44 on every board for boot strapping, native USB, SPI flash, quad PSRAM, and UART0.

The board profile, set at boot from NVS key `board` via `K32_CMD:BOARD:cardputer|generic`, reserves on-board peripheral pins and sets node capacity:

| Profile | GPIO | ADC1 | I2C | UART | RMT TX / RX | Default pins |
| --- | --- | --- | --- | --- | --- | --- |
| `generic` | 33 | 10 | 2 | 2 | 4 / 4 | I2C0 on GPIO 8/9, UART1 on GPIO 17/18 |
| `cardputer` | 11 | 3 | 1 | 1 | 4 / 4 | Grove port GPIO 2/1 shared by I2C0 and UART1; RMT TX on GPIO 44 (IR LED) |

The `cardputer` profile reserves pins 33 to 38 for the LCD, 41 and 42 for I2S, 3 to 9, 11, 13, and 15 for the key matrix, and 12, 14, 39, and 40 for the microSD bus. It offers GPIO 44, a UART0 pin the board wires only to its IR LED, to RMT TX channels. An ADC1 channel is claimable only when its pin, `GPIO channel + 1`, is claimable.

## Limitations

* `KUBE32_RESERVED_GPIO_MASK` does not block strapping pins 3, 45, and 46 or octal PSRAM pins 33 to 37.
* Claiming an ADC1 channel does not claim its GPIO pin.
