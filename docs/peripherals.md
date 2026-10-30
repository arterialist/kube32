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
| `esp32.io/rmt-tx: "1"` | None | One RMT (infrared) transmit channel and its pin: `esp32_rmt_send` |
| `esp32.io/rmt-rx: "1"` | None | One RMT (infrared) receive channel and its pin: `esp32_rmt_receive` |
| `hardware.kube32.io/tft-display: "1"` | `DISPLAY`, `TFT_DISPLAY` | `k32_display_text`, `k32_display_rect`, `k32_display_bar` |
| `hardware.kube32.io/keyboard: "1"` | `KEYBOARD` | `k32_key_read` |
| `hardware.kube32.io/speaker: "1"` | `SPEAKER`, `AUDIO` | `k32_tone` |
| `hardware.kube32.io/ipc: "1"` | `CHANNEL_IPC`, `IPC` | `k32_event_emit`, `kube32_channel_*` |
| `hardware.kube32.io/sd: "1"` | `SD`, `SD_CARD` | `k32_fs_*` in the pod's volume claim ([storage](storage.md)) |

If a requested pin, channel, or bus is reserved or owned by another pod, admission fails with `CapabilityAdmissionFailed`. Ungranted calls at runtime return `K32_ERR_PERM` (`-1`) and log `[FIREWALL] denied <pod>: ...`.

## Host functions

### GPIO, ADC, I2C, UART, and infrared

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
| `esp32_rmt_send` | `i(ii)` | `int32_t esp32_rmt_send(const uint16_t* durations_us, uint32_t count)` | Sends one IR frame of `count` mark and space durations on the pod's RMT TX channel ([infrared](#infrared-rmt)). Returns `count` once the frame is out, or negative error. |
| `esp32_rmt_receive` | `i(ii)` | `int32_t esp32_rmt_receive(uint16_t* out_us, uint32_t cap)` | Copies the oldest frame the pod's RMT RX channel captured, without waiting. Returns its duration count, `K32_ERR_AGAIN` when none is queued, or negative error. |
| `esp32_delay_ms` | `v(i)` | `void esp32_delay_ms(uint32_t ms)` | Sleeps for `ms` milliseconds in 20 ms slices. Requires no grant. |
| `esp32_get_time_us` | `I()` | `int64_t esp32_get_time_us(void)` | Returns microseconds since boot. Requires no grant. |

Error codes (`include/runtime/k32_app_abi.h`, `include/hal/hal_types.hpp`):

* `K32_OK`, `0`, success
* `K32_ERR_PERM`, `-1`, capability not granted to this pod
* `K32_ERR_IO`, `-5`, physical I/O error
* `K32_ERR_TOOBIG`, `-7`, IR frame longer than the buffer
* `K32_ERR_AGAIN`, `-11`, no IR frame queued
* `K32_ERR_NOMEM`, `-12`, out of memory
* `K32_ERR_FAULT`, `-14`, buffer pointer and length outside linear memory
* `K32_ERR_NODEV`, `-19`, bus not installed or no device acknowledged
* `K32_ERR_INVAL`, `-22`, invalid bus, port, or 7-bit address above `0x7f`; empty, overlong, or zero-duration IR frame
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

After a failed I2C transfer, the driver clears the bus with `i2c_master_bus_reset` (nine SCL pulses followed by a STOP condition) so a device stuck mid-byte releases SDA before the next transfer. The node console logs `[HAL] I2C<n>: <error> talking to 0x<address>, bus cleared (<count> times)` for the first three clears after driver installation and every hundredth clear after.

### I2C address probing

Calling `esp32_i2c_write(bus, addr, ptr, 0)` with `len` set to `0` probes `addr` using `i2c_master_probe` with a 50 ms timeout:

* Returns `0` (`K32_OK`) when a device at `addr` acknowledges.
* Returns `-19` (`K32_ERR_NODEV`) when no device answers.

### Device discovery

Each node probes its board's default I2C pins (I2C0 on GPIO 8/9 on a devkit, I2C0 on the Grove port GPIO 2/1 on the Cardputer) before it registers and every 10 seconds after (`src/hal/i2c_discovery.cpp`). Each address that acknowledges a zero-length write becomes a node label, so a pod can select the node with that device attached:

| Label | Value |
| --- | --- |
| `i2c.kube32.io/0x<address>` | `bus-<n>`, the lowest bus the address answers on |

```yaml
spec:
  nodeSelector:
    i2c.kube32.io/0x28: bus-0
```

```
kubectl --context kube32 get nodes -L i2c.kube32.io/0x28
```

* A scan runs only when no pod holds the controller or its pins. While a pod holds a bus, the node keeps the labels from the last scan. Pod admission waits while a scan holds the bus (one scan takes milliseconds).
* If the bus lines read low with internal pull-ups enabled, something other than an idle I2C bus drives them. The node skips the scan and reports no devices on that bus.
* The kubelet patches label changes onto its Node: it adds labels for detected devices and removes labels for devices that stop answering. When a node re-registers after a reboot, it removes `i2c.kube32.io/` labels it no longer finds.
* As in Kubernetes, removing a label does not evict a running pod.

## Infrared (RMT)

The ESP32-S3 RMT peripheral sends and captures pulse trains, the signal of infrared remote controls. A pod claims one transmit channel (`esp32.io/rmt-tx: "1"`), one receive channel (`esp32.io/rmt-rx: "1"`), or one of each. The chip has four channels of each direction; a node advertises at most four of each and only as many as it has pins for, so a profile without a claimable pin for a direction advertises none of it.

### Pins and settings

```yaml
metadata:
  annotations:
    kube32.io/rmt-tx: "pin=2,carrier=38000,duty=33"
    kube32.io/rmt-rx: "pin=1,active=low,idle_us=12000"
```

| Annotation | Setting | Default | Meaning |
| --- | --- | --- | --- |
| `kube32.io/rmt-tx` | `pin` | Board default | Output pin |
| | `carrier` | `38000` | Frequency in Hz of the carrier that modulates every mark, `10000` to `500000`; `0` sends a mark as a steady high level |
| | `duty` | `33` | Carrier duty cycle in percent, `1` to `99` |
| `kube32.io/rmt-rx` | `pin` | Board default | Input pin |
| | `active` | `low` | Line level during a mark: `low` for IR demodulating receivers, which pull their output low while they see the carrier; `high` for a non-inverted signal |
| | `idle_us` | `12000` | A level that lasts this long ends the frame, `1000` to `32767` |

| Board profile | Allocatable counts | Default TX pin | Default RX pin |
| --- | --- | --- | --- |
| `generic` | 4 RMT TX, 4 RMT RX | None | None |
| `cardputer` | 4 RMT TX, 4 RMT RX | GPIO 44, the built-in IR LED | None |

Channel pins are claimed like bus pins: admission fails when the chip or the board reserves the pin, when another pod holds it, or when the claim names no pin and the board has no default (`RMT RX has no default pin on the cardputer board: set annotation kube32.io/rmt-rx: "pin=<n>"`).

GPIO 44 is the UART0 RX pad, reserved on every board by `KUBE32_RESERVED_GPIO_MASK`. The `cardputer` profile offers it to RMT TX channels only (`BoardProfile::rmt_tx_pin_mask`): the board wires its IR LED there and nothing else, and its console runs on the native USB port, so no UART0 driver reads the pad. It stays unavailable as a GPIO and as an RX pin. On the `generic` profile the pad belongs to the USB-UART bridge and cannot be claimed.

With an M5Stack Unit IR in the Cardputer's Grove port, the unit's receiver output is GPIO 1 (white wire, active low) and its LED input is GPIO 2 (yellow wire). A receiver on GPIO 1 takes the Grove port, so `I2C0` and `UART1` cannot be claimed beside it; a pod that only sends through the built-in LED on GPIO 44 leaves the port free for them.

### Frames

A frame is alternating mark and space durations in microseconds, mark first, each from 1 to 65535.

`esp32_rmt_send` transmits one frame and returns when it is out. The host turns the durations into RMT symbols while the frame plays, reading them from the pod's linear memory, so a send allocates nothing. A frame holds at most `K32_RMT_MAX_DURATIONS` (512) durations and lasts at most `K32_RMT_MAX_FRAME_US` (500 ms). The calling pod waits for the frame (an NEC frame takes 68 ms); on boards without PSRAM, where all pods run on one interpreter, every other pod's call waits as well.

The receiver is armed from admission until the pod's channels are released. It captures with 1 µs resolution and ignores pulses shorter than 1.25 µs. Each captured frame goes into the channel's queue, which holds up to `K32_RMT_RX_QUEUE_FRAMES` (8) frames and `K32_RMT_RX_QUEUE_DURATIONS` (512) durations in all. A frame that finds the queue full, or that has more than 512 durations, is dropped, and the pod's next `esp32_rmt_receive` call writes the count to the pod's log:

```
[kube32] RMT receiver dropped 2 frame(s) on a full queue
```

`esp32_rmt_receive` never waits. It returns the oldest frame with its marks normalized to the `active` setting, `K32_ERR_AGAIN` when the queue is empty, or `K32_ERR_TOOBIG` when the frame has more durations than `cap`; the first `cap` durations are then copied and the frame leaves the queue. A buffer of 512 durations holds any frame. A pod that polls the queue from `k32_tick` and returns `1` after taking a frame is ticked again within about 10 ms, so it keeps up with a remote's repeat codes.

### Channel lifecycle and memory

After admission succeeds, `src/hal/pod_rmt.cpp` creates the channels with `rmt_new_tx_channel` and `rmt_new_rx_channel` from `driver/rmt_tx.h` and `driver/rmt_rx.h` (1 MHz resolution, one 48-symbol memory block each, no DMA), applies the carrier with `rmt_apply_carrier`, and arms the receiver with `rmt_receive`. The receiver is armed again from the RMT interrupt at the end of every frame. When the pod stops or is deleted, `unregister_pod` disables and deletes the channels, frees the queue, and resets the pins with `gpio_reset_pin`. If a channel cannot be created, the pod is not admitted and nothing it claimed stays installed.

A receive channel allocates its 1 KiB queue and a 192-byte capture block from internal RAM; each channel's driver objects take a few hundred bytes more. A node pays only for channels its pods have claimed, and the node log reports what each channel cost when it was created:

```
[HAL] RMT RX installed for default/irremote: GPIO 1, active low, idle 12000 us, <n> bytes of internal RAM
```

The firmware builds the RMT driver's interrupt handlers and encoders into flash instead of IRAM (`CONFIG_RMT_*_IN_IRAM` off in `sdkconfig.defaults`): an IR symbol lasts hundreds of microseconds, so a cache miss costs nothing.

On the host simulator the channels keep the same lifecycle and queue. `MockHal::get_rmt_tx_frames(pin)` returns the frames sent on a pin, `hal::rmt_inject(pin, ...)` hands a frame to the receiver on a pin, and `MockHal::set_rmt_loopback(true)` delivers every frame sent to the node's receivers, as an LED facing them would.

### Example: IR remote

`apps/irremote/irremote.c` decodes NEC codes, including repeat codes and 16-bit extended addresses, and logs any other frame as raw durations. It publishes the codes on the channel topic `ir.received` and sends the codes it receives on `ir.send`, as `POST /send` bodies, or periodically from `IR_SEND_CODE`:

```
nec 0x04 0x08        NEC address and command
nec repeat           the NEC repeat code
raw 9000,4500,560    durations in microseconds, mark first
```

It polls the receiver from `k32_tick` and never blocks, so it runs on the Cardputer's shared interpreter. Without the `esp32.io/rmt-rx` claim it logs `irremote: no esp32.io/rmt-rx claim: not receiving, sending only` once and only sends.

`examples/irremote.yaml` receives and sends through an M5Stack Unit IR on the Cardputer's Grove port: the receiver uses GPIO 1 and the unit's LED uses GPIO 2. The transmit channel holds GPIO 2 low between frames. Without the transmit channel, the pin floats high, the unit's LED conducts, and its receiver reports noise frames. `examples/irremote-tx.yaml` sends through the Cardputer's built-in LED on GPIO 44 and leaves the Grove port free:

```yaml
metadata:
  name: irremote
  annotations:
    kube32.io/rmt-rx: "pin=1"              # Unit IR receiver, Grove white
    kube32.io/rmt-tx: "pin=2"              # Unit IR LED, Grove yellow
    kube32.io/event-topics: ir.send
spec:
  nodeSelector:
    kube32.io/board: cardputer
  containers:
    - name: irremote
      image: wasm://embedded/irremote
      resources:
        requests:
          memory: 12Ki
          esp32.io/rmt-tx: "1"
          esp32.io/rmt-rx: "1"
          hardware.kube32.io/ipc: "1"      # publishes on ir.received
        limits:
          memory: 12Ki
```

```
make -C apps/irremote
kubectl --context kube32 apply -f examples/irremote.yaml
kubectl --context kube32 logs -f irremote
```

### Example: RFID reader

`apps/rfid/rfid.c` reads ISO 14443-A tags (4, 7, and 10-byte UIDs) through an MFRC522-compatible I2C reader, such as the M5Stack RFID 2 unit (WS1850S, address `0x28`). Each tick wakes tags in the field (WUPA), selects one through the anticollision cascade, reads its UID, and halts it. The app runs from `k32_tick` with `kube32.io/tick-interval: 200ms`, so it runs on the Cardputer's shared interpreter in 4 KiB of linear memory.

* When a tag enters the field, the app logs it (`rfid: tag 04a1b2c3d4e580 sak 0x00`) and publishes its hex UID to the `rfid.tag` channel topic. When a tag stays out of the field for three polls, the app logs that it left.
* `TAG_<UID>` (UID in uppercase hex) maps a tag to a message published on `ACTION_TOPIC` (default `ir.send`, which `irremote` sends as an IR code). `TAG_DEFAULT` maps all other tags.
* `GET` returns the reader state, poll counters, and last eight tags as JSON.
* If the reader stops answering, the next tick reinitializes it.

`examples/rfid.yaml` runs the app as a Deployment on the node labeled `i2c.kube32.io/0x28: bus-0` ([device discovery](#device-discovery)), with a Service and an Ingress that serves the JSON status on `/rfid` and tag events on `/rfid/events`:

```
make -C apps/rfid
kubectl --context kube32 apply -f examples/rfid.yaml
kubectl --context kube32 logs -l app=rfid -f
```

## Reserved pins

The ESP32-S3 provides GPIO 0 to 21 and 26 to 48. The firewall blocks claims on reserved pins (`include/hal/capability_firewall.hpp`):

* Every board (`KUBE32_RESERVED_GPIO_MASK`) reserves GPIO `0` for boot strapping, `19` and `20` for native USB, `26` to `32` for SPI flash and quad PSRAM, and `43` and `44` for the UART0 console. The `cardputer` profile offers GPIO `44` to RMT TX channels for its IR LED.
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
