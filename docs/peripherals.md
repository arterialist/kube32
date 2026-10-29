# Peripherals

WebAssembly pods access board peripherals through host functions in the `env` import module (`src/runtime/host_functions.cpp`, `src/runtime/app_pod.cpp`, `include/runtime/k32_app_abi.h`). The capability firewall in `src/hal/capability_firewall.cpp` gates every hardware call against the pod's resource requests. On the host simulator, calls route to `src/hal/mock_hal.cpp`.

## Resource requests

The firewall reads `resources.requests`, or `resources.limits` when `requests` is absent. Every `esp32.io/` key also accepts the `esp.io/` prefix.

| Resource | `securityContext.capabilities.add` | Grants |
| --- | --- | --- |
| `esp32.io/gpio-<N>: "1"` | None | Read, write, and configure GPIO pin `N` |
| `esp32.io/gpio: "<count>"` | None | `<count>` free claimable GPIO pins, lowest numbers first |
| `esp32.io/adc1-channel-<0..9>: "1"` | None | Read ADC1 channel `0..9` (samples GPIO `channel + 1`) |
| `esp32.io/adc: "<count>"` | None | `<count>` free claimable ADC1 channels |
| `esp32.io/i2c-bus-<0\|1>: "1"` | None | I2C controller `0` or `1` and its SDA/SCL pins |
| `esp32.io/i2c: "<count>"` | None | `<count>` free I2C controllers |
| `esp32.io/uart-<1\|2>: "1"` | None | UART port `1` or `2` and its TX/RX pins |
| `esp32.io/uart: "<count>"` | None | `<count>` free UART ports |
| `hardware.kube32.io/tft-display: "1"` | `DISPLAY`, `TFT_DISPLAY` | `k32_display_text`, `k32_display_rect`, `k32_display_bar` |
| `hardware.kube32.io/keyboard: "1"` | `KEYBOARD` | `k32_key_read` |
| `hardware.kube32.io/speaker: "1"` | `SPEAKER`, `AUDIO` | `k32_tone` |
| `hardware.kube32.io/ipc: "1"` | `CHANNEL_IPC`, `IPC` | `k32_event_emit`, `kube32_channel_*` |
| `hardware.kube32.io/sd: "1"` | `SD`, `SD_CARD` | `k32_fs_*` in the pod's volume claim ([storage](storage.md)) |

If a requested pin, channel, or bus is reserved or owned by another pod, admission fails with `CapabilityAdmissionFailed`. Ungranted calls at runtime return `K32_ERR_PERM` (`-1`) and log `[FIREWALL] denied <pod>: ...`.

## Host functions

### GPIO, ADC, I2C, and UART

Declared in `include/runtime/k32_app_abi.h` and linked in `src/runtime/host_functions.cpp`:

| Import | Wasm signature | C declaration | Behavior |
| --- | --- | --- | --- |
| `esp32_gpio_set_mode` | `i(ii)` | `int32_t esp32_gpio_set_mode(uint32_t pin, uint32_t mode)` | Configures `pin`: `0` input, `1` output, `2` input pull-up, `3` input pull-down. Returns `0` or negative error. |
| `esp32_gpio_write` | `i(ii)` | `int32_t esp32_gpio_write(uint32_t pin, uint32_t value)` | Sets output level (`0` or `1`) on `pin`. Returns `0` or negative error. |
| `esp32_gpio_read` | `i(i)` | `int32_t esp32_gpio_read(uint32_t pin)` | Returns input level (`0` or `1`) on `pin`, or negative error. |
| `esp32_adc_read` | `i(i)` | `int32_t esp32_adc_read(uint32_t channel)` | One-shot read on ADC1 `channel` (`0..9`, 12 dB attenuation). Returns raw reading or negative error. |
| `esp32_i2c_write` | `i(iiii)` | `int32_t esp32_i2c_write(uint32_t bus, uint32_t addr, const void* data, uint32_t len)` | Writes `len` bytes to 7-bit `addr` on `bus` (100 ms timeout). Returns bytes written or negative error. |
| `esp32_i2c_read` | `i(iiii)` | `int32_t esp32_i2c_read(uint32_t bus, uint32_t addr, void* buf, uint32_t len)` | Reads `len` bytes from 7-bit `addr` on `bus` (100 ms timeout). Returns bytes read or negative error. |
| `esp32_uart_write` | `i(iii)` | `int32_t esp32_uart_write(uint32_t port, const void* data, uint32_t len)` | Writes `len` bytes to UART `port` (`1` or `2`). Returns bytes queued or negative error. |
| `esp32_uart_read` | `i(iii)` | `int32_t esp32_uart_read(uint32_t port, void* buf, uint32_t max_len)` | Reads up to `max_len` bytes from UART `port` within 50 ms. Returns bytes read or negative error. |
| `esp32_delay_ms` | `v(i)` | `void esp32_delay_ms(uint32_t ms)` | Sleeps for `ms` milliseconds in 20 ms slices. Requires no grant. |
| `esp32_get_time_us` | `I()` | `int64_t esp32_get_time_us(void)` | Returns microseconds since boot. Requires no grant. |

Error codes (`include/runtime/k32_app_abi.h`, `include/hal/hal_types.hpp`):

* `K32_OK`, `0`, success
* `K32_ERR_PERM`, `-1`, capability not granted to this pod
* `K32_ERR_IO`, `-5`, physical I/O error
* `K32_ERR_NOMEM`, `-12`, out of memory
* `K32_ERR_FAULT`, `-14`, buffer pointer and length outside linear memory
* `K32_ERR_NODEV`, `-19`, bus not installed or no device acknowledged
* `K32_ERR_INVAL`, `-22`, invalid bus, port, or 7-bit address above `0x7f`
* `K32_ERR_TIMEOUT`, `-110`, transfer timed out

### Cardputer display, keypad, and speaker

Nodes running the `cardputer` board profile, selected with `K32_CMD:BOARD:cardputer`, expose the M5Stack Cardputer's built-in 240x135 ST7789 display, 56-key matrix, and NS4168 I2S speaker (`src/runtime/app_pod.cpp`, `src/hal/tft_keyboard_hardware.cpp`). On nodes without this profile, these functions return `K32_ERR_NODEV` (`-19`).

| Import | Wasm signature | C declaration | Behavior |
| --- | --- | --- | --- |
| `k32_display_text` | `i(iiiiii)` | `int32_t k32_display_text(int32_t x, int32_t y, const char* text, uint32_t len, uint32_t fg565, uint32_t bg565)` | Draws up to 64 bytes of ASCII `text` at `x, y` in a 6x8 cell font using RGB565 colors. |
| `k32_display_rect` | `i(iiiii)` | `int32_t k32_display_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color565)` | Fills rectangle `x, y, w, h` with RGB565 `color565`. |
| `k32_display_bar` | `i(iiiiiii)` | `int32_t k32_display_bar(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pct, uint32_t fg565, uint32_t bg565)` | Draws a horizontal bar at `x, y, w, h` filled to `pct` from `0` to `100`. |
| `k32_key_read` | `i()` | `int32_t k32_key_read(void)` | Pops the next ASCII key code from the 16-entry scanner queue, or `K32_ERR_AGAIN` (`-11`) when empty. |
| `k32_tone` | `i(iii)` | `int32_t k32_tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct)` | Plays a square-wave tone at `freq_hz` for `duration_ms`, capped at `2000`, and `volume_pct` from `0` to `100`. |

You can test the keypad and speaker from the serial console with `K32_CMD:KEY:<text>`, `K32_CMD:KEYPOP`, and `K32_CMD:TONE:<n>`.

## Bus configuration and driver lifecycle

### Pin annotations and board defaults

A pod that claims an I2C bus or UART port can configure its pins and speed with pod annotations:

```yaml
metadata:
  annotations:
    kube32.io/i2c-bus-0: "sda=8,scl=9,hz=100000"
    kube32.io/uart-1: "tx=17,rx=18,baud=115200"
```

* `kube32.io/i2c-bus-<0|1>` takes `sda=<pin>,scl=<pin>[,hz=<1000..1000000>]`. Default speed is `100000` Hz.
* `kube32.io/uart-<1|2>` takes `tx=<pin>,rx=<pin>[,baud=<300..5000000>]`. Default speed is `115200` baud, 8N1, no flow control, with a 256-byte RX ring buffer.

When the annotation is omitted, the bus uses the board profile's default pins. Claiming a bus with no default pins and no annotation fails admission.

| Board profile | Allocatable counts | Default bus pins |
| --- | --- | --- |
| `generic` | 33 GPIO, 10 ADC, 2 I2C (`0`, `1`), 2 UART (`1`, `2`) | `I2C0` SDA 8, SCL 9; `I2C1` none; `UART1` TX 17, RX 18; `UART2` none |
| `cardputer` | 11 GPIO, 3 ADC, 1 I2C (`0`), 1 UART (`1`) | `I2C0` SDA 2, SCL 1; `UART1` TX 2, RX 1 |

On the Cardputer, `I2C0` and `UART1` share the Grove port pins GPIO 2 and GPIO 1, so only one of the two can be claimed at a time.

### Driver lifecycle

Bus pins are claimed as GPIOs during admission before any generic `esp32.io/gpio: "<count>"` requests are allocated. If a pin is reserved or held by another pod, admission fails.

After admission succeeds, `src/hal/pod_buses.cpp` installs the ESP-IDF driver on the claimed pins:

* I2C uses `i2c_new_master_bus` from `driver/i2c_master.h` with internal pull-ups enabled.
* UART uses `uart_driver_install`, `uart_param_config`, and `uart_set_pin`.

When the pod stops or is deleted, `unregister_pod` deletes the driver with `i2c_del_master_bus` or `uart_driver_delete`, resets the GPIO pins with `gpio_reset_pin`, and releases the pin and bus claims.

### I2C address probing

Calling `esp32_i2c_write(bus, addr, ptr, 0)` with `len` set to `0` probes `addr` using `i2c_master_probe` with a 50 ms timeout:

* Returns `0` (`K32_OK`) when a device at `addr` acknowledges.
* Returns `-19` (`K32_ERR_NODEV`) when no device answers.

## Reserved pins

The ESP32-S3 provides GPIO 0 to 21 and 26 to 48. The firewall blocks claims on reserved pins (`include/hal/capability_firewall.hpp`):

* Every board (`KUBE32_RESERVED_GPIO_MASK`) reserves GPIO `0` for boot strapping, `19` and `20` for native USB, `26` to `32` for SPI flash and quad PSRAM, and `43` and `44` for the UART0 console.
* The `cardputer` profile also reserves LCD SPI pins `33` to `38`, I2S speaker pins `41` to `43`, keyboard matrix pins `3` to `9`, `11`, `13`, and `15`, and microSD SPI pins `12`, `14`, `39`, and `40`.

An ADC1 channel `N` from `0` to `9` is claimable only when its pad, GPIO `N + 1`, is claimable by the board profile.

## Example: I2C bus scanner

`apps/i2cscan/i2cscan.c` probes 7-bit addresses `0x08` to `0x77` on `I2C_BUS` using zero-length `esp32_i2c_write` calls and logs every responding address. When `SCAN_EVERY_MS` is set, it repeats after that delay. Otherwise it scans once and exits.

```c
/* One pass over the bus: the number of devices found, or a negative error */
static int32_t scan(uint32_t bus) {
    int32_t found = 0;
    for (uint32_t addr = 0x08; addr <= 0x77; addr++) {
        int32_t r = esp32_i2c_write(bus, addr, 0, 0);
        if (r == K32_ERR_NODEV) continue;   /* nobody acknowledged */
        struct line l = {{0}, 0};
        if (r < 0) {
            put(&l, "i2cscan: bus "); put_uint(&l, bus); put(&l, ": error "); put_int(&l, r);
            put(&l, " probing "); put_hex8(&l, addr);
            put(&l, r == K32_ERR_PERM ? " (bus not claimed by this pod)" : "");
            k32_log(l.buf, l.len);
            return r;
        }
        found++;
        put(&l, "i2cscan: bus "); put_uint(&l, bus); put(&l, ": device at "); put_hex8(&l, addr);
        k32_log(l.buf, l.len);
    }
    return found;
}
```

Manifest (`examples/i2cscan.yaml`):

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: i2cscan
  namespace: default
  # annotations:
  #   kube32.io/i2c-bus-0: "sda=8,scl=9,hz=400000"
spec:
  restartPolicy: Never
  containers:
    - name: i2cscan
      image: wasm://embedded/i2cscan
      env:
        - name: I2C_BUS
          value: "0"
        - name: SCAN_EVERY_MS
          value: "10000"
      resources:
        requests:
          memory: 64Ki
          esp32.io/i2c-bus-0: "1"
        limits:
          memory: 64Ki                 # one Wasm page
```

Build and run:

```
make -C apps/i2cscan
kubectl --context kube32 apply -f examples/i2cscan.yaml
kubectl --context kube32 logs i2cscan
```

## Limitations

* The reserved pin mask does not block strapping pins 3, 45, and 46 or octal PSRAM pins 33 to 37 on the `generic` profile.
* Claiming an ADC1 channel does not claim its underlying GPIO pin, so another pod can claim the same pad as a GPIO.
