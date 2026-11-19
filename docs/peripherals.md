# Peripherals

WebAssembly pods access board peripherals through host functions in the `env` import module (`src/runtime/host_functions.cpp`, `src/runtime/app_pod.cpp`, `include/runtime/k32_app_abi.h`). The capability firewall in `src/hal/capability_firewall.cpp` gates every hardware call against the pod's resource requests. On the host simulator, calls route to `src/hal/mock_hal.cpp`.

## Resource requests

The firewall reads `resources.requests`, or `resources.limits` when `requests` is absent. Every `esp32.io/` key also accepts the `esp.io/` prefix.

| Resource | `securityContext.capabilities.add` | Grants |
| --- | --- | --- |
| `esp32.io/gpio-<N>: "1"` | None | Read, write, and configure GPIO pin `N` |
| `esp32.io/gpio-input-<N>: "1"` | None | Read GPIO pin `N` and set it as an input, with or without a pull. The pod cannot drive the pin. A pod can claim GPIO `0`, the boot button, in this way |
| `esp32.io/gpio: "<count>"` | None | `<count>` free claimable GPIO pins, lowest numbers first |
| `esp32.io/adc1-channel-<0..9>: "1"` | None | Read ADC1 channel `0..9`, which samples GPIO `channel + 1` on the ESP32-S3. The ESP32-S31 has channels `0..7`, on GPIO 42 to 49 |
| `esp32.io/adc: "<count>"` | None | `<count>` free claimable ADC1 channels |
| `esp32.io/i2c-bus-<0\|1>: "1"` | None | I2C controller `0` or `1` and its SDA/SCL pins |
| Annotation `kube32.io/i2c-device: "bus=<n>,addr=0x<hh>[,mux=0x<hh>:<channel>]"` | None | One address on I2C bus `n`. Other pods' grants share the bus. With `mux`, the address is behind a PCA9548A channel ([shared I2C buses](devices.md#shared-i2c-buses)) |
| `esp32.io/i2c: "<count>"` | None | `<count>` free I2C controllers |
| `esp32.io/uart-<1\|2>: "1"` | None | UART port `1` or `2` and its TX/RX pins |
| `esp32.io/uart: "<count>"` | None | `<count>` free UART ports |
| `esp32.io/rmt-tx: "1"` | None | One RMT (infrared) transmit channel and its pin: `esp32_rmt_send` |
| `esp32.io/rmt-rx: "1"` | None | One RMT (infrared) receive channel and its pin: `esp32_rmt_receive` |
| `hardware.kube32.io/tft-display: "1"` | `DISPLAY`, `TFT_DISPLAY` | `k32_display_text`, `k32_display_rect`, `k32_display_bar`, `k32_display_blit`, `k32_display_blit_pal`, `k32_display_info` |
| `hardware.kube32.io/touch: "1"` | `TOUCH` | `k32_touch_read` |
| `hardware.kube32.io/keyboard: "1"` | `KEYBOARD` | `k32_key_read` |
| `hardware.kube32.io/speaker: "1"` | `SPEAKER`, `AUDIO` | `k32_tone`, `k32_audio_play` |
| `hardware.kube32.io/microphone: "1"` | `MICROPHONE`, `MIC` | `k32_mic_read` |
| `hardware.kube32.io/led: "1"` | `LED` | `k32_led_set` |
| `hardware.kube32.io/ipc: "1"` | `CHANNEL_IPC`, `IPC` | `k32_event_emit`, `kube32_channel_*` |
| `hardware.kube32.io/sd: "1"` | `SD`, `SD_CARD` | `k32_fs_*` in the pod's volume claim ([storage](storage.md)) |
| `hardware.kube32.io/chip-temp: "1"` | `CHIP_TEMP` | `esp32_chip_temp_read` |

If a requested pin, channel, or bus is reserved or owned by another pod, admission fails with `CapabilityAdmissionFailed`. Ungranted calls at runtime return `K32_ERR_PERM` (`-1`) and log `[FIREWALL] denied <pod>: ...`.

### Device grants on the Node

Pods share each device that a `hardware.kube32.io/*` resource grants, and a pod holds a grant at most once. A node therefore advertises each grant it can give with a count of its `pods` capacity, in both `capacity` and `allocatable`:

| Resource | Advertised by |
| --- | --- |
| `hardware.kube32.io/ipc` | Every node |
| `hardware.kube32.io/chip-temp` | Every node: the chip's internal sensor, a stand-in in the simulator. A node on a [Linux board](hardware-spec.md#linux-boards) reads its SoC's thermal zone and advertises the grant only when the zone can be read |
| `hardware.kube32.io/tft-display`, `keyboard`, `speaker`, `microphone`, `touch`, `led` | Boards with the devices on board (the Cardputer profile has a display, keyboard, speaker and microphone; the `s31-coreboard` profile a speaker, microphone and RGB LED; a Linux board, the framebuffer, keyboard and sound card that opened) |
| `hardware.kube32.io/sd` | Nodes with a mounted microSD card ([storage](storage.md)) |

A resource that a node no longer has (a card taken out, a board profile changed between boots) is set to `null` in the next status write and leaves the Node. The scheduler does not filter on these resources; the firewall checks the grants at admission.

## Host functions

### GPIO, ADC, I2C, UART, and infrared

Declared in `include/runtime/k32_app_abi.h` and linked in `src/runtime/host_functions.cpp`:

| Import | Wasm signature | C declaration | Behavior |
| --- | --- | --- | --- |
| `esp32_gpio_set_mode` | `i(ii)` | `int32_t esp32_gpio_set_mode(uint32_t pin, uint32_t mode)` | Configures `pin`: `0` input, `1` output, `2` input pull-up, `3` input pull-down. Returns `0` or negative error. |
| `esp32_gpio_write` | `i(ii)` | `int32_t esp32_gpio_write(uint32_t pin, uint32_t value)` | Sets output level (`0` or `1`) on `pin`. Returns `0` or negative error. |
| `esp32_gpio_read` | `i(i)` | `int32_t esp32_gpio_read(uint32_t pin)` | Returns input level (`0` or `1`) on `pin`, or negative error. |
| `esp32_adc_read` | `i(i)` | `int32_t esp32_adc_read(uint32_t channel)` | One-shot read on ADC1 `channel` (`0..9`, 12 dB attenuation; the ESP32-S31's ADC has one input range). Returns the raw 12-bit reading or negative error. |
| `esp32_chip_temp_read` | `i(i)` | `int32_t esp32_chip_temp_read(int32_t* out_milli_c)` | Stores the chip's internal temperature in milli-degrees Celsius and returns `0`, or a negative error. The sensor measures the die, which is warmer than the air around it. The simulator reports 40.0 °C. |
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

### Cardputer display, keypad, speaker, and microphone

Nodes running the `cardputer` board profile, selected with `K32_CMD:BOARD:cardputer`, expose the M5Stack Cardputer's built-in 240x135 ST7789 display, 56-key matrix, NS4168 I2S speaker, and SPM1423 PDM microphone (`src/runtime/app_pod.cpp`, `src/hal/tft_keyboard_hardware.cpp`). On nodes without this profile, these functions return `K32_ERR_NODEV` (`-19`). The `s31-coreboard` profile serves the speaker and microphone functions with the ESP32-S31-Function-CoreBoard-1's codec ([below](#speaker-and-microphone-on-the-esp32-s31-function-coreboard-1)). A node on a [Linux board](hardware-spec.md#linux-boards) serves the display, keyboard, speaker and microphone functions with the board's own panel, keyboard and sound card, the display at the panel's size.

| Import | Wasm signature | C declaration | Behavior |
| --- | --- | --- | --- |
| `k32_display_text` | `i(iiiiii)` | `int32_t k32_display_text(int32_t x, int32_t y, const char* text, uint32_t len, uint32_t fg565, uint32_t bg565)` | Draws up to 64 bytes of ASCII `text` at `x, y` in a 6x8 cell font using RGB565 colors. |
| `k32_display_rect` | `i(iiiii)` | `int32_t k32_display_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color565)` | Fills rectangle `x, y, w, h` with RGB565 `color565`. |
| `k32_display_blit` | `i(iiiii)` | `int32_t k32_display_blit(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* rgb565)` | Copies `w x h` RGB565 pixels, row after row, to `x, y`; the part outside the screen is dropped. `w` and `h` are 1 to 1024. |
| `k32_display_blit_pal` | `i(iiiiii)` | `int32_t k32_display_blit_pal(int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t* idx, const uint16_t* pal565)` | The same from 8-bit palette indices through 256 RGB565 entries: a paletted frame (such as one the [raster kernels](app-development.md#raster-kernels) draw) reaches the screen without the pod converting it. |
| `k32_display_bar` | `i(iiiiiii)` | `int32_t k32_display_bar(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pct, uint32_t fg565, uint32_t bg565)` | Draws a horizontal bar at `x, y, w, h` filled to `pct` from `0` to `100`. |
| `k32_display_info` | `i(iii)` | `int32_t k32_display_info(int32_t* width, int32_t* height, int32_t* format)` | Stores the panel's size in pixels and the format the drawing calls take (`1`, RGB565). A pod that asks draws on any panel: 240x135 on the Cardputer, 320x170 on the Cardputer Zero. |
| `k32_key_read` | `i()` | `int32_t k32_key_read(void)` | Pops the next key code from the 16-entry scanner queue, or `K32_ERR_AGAIN` (`-11`) when empty. Printable keys return ASCII, Enter `\n`, Backspace `\b`, Tab `\t`, Esc `27`, and the arrows `U`, `D`, `L` and `R` (Fn with `;`, `.`, `,` and `/` on the Cardputer). |
| `k32_tone` | `i(iii)` | `int32_t k32_tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct)` | Plays a square-wave tone at `freq_hz` for `duration_ms`, capped at `2000`, at `volume_pct` (`0` to `100`) of the speaker's ceiling. |
| `k32_audio_play` | `i(iiii)` | `int32_t k32_audio_play(const int16_t* samples, uint32_t count, uint32_t sample_rate, uint32_t volume_pct)` | Queues `count` signed 16-bit mono samples at `sample_rate` (`8000` to `48000` Hz), at most one second of them per call, at `volume_pct` of the ceiling. Returns the number queued once they are in the output buffer. |
| `k32_mic_read` | `i(iii)` | `int32_t k32_mic_read(int16_t* out, uint32_t max_samples, uint32_t timeout_ms)` | Copies up to `max_samples` (at most `16000`) samples of 16 kHz signed 16-bit mono PCM from the microphone to `out`, waiting up to `timeout_ms` (at most `1000`). Returns the number copied. |

You can test the keypad, speaker and microphone from the serial console with `K32_CMD:KEY:<text>`, `K32_CMD:KEYPOP`, `K32_CMD:TONE:<n>` and `K32_CMD:MIC:<ms>`, which records for `<ms>` milliseconds and prints the mean, RMS and peak of every 250 ms.

### Speaker and microphone

The speaker never plays louder than `K32_SPEAKER_MAX_PCT` (25) percent of full scale: the firmware scales every sample of `k32_tone` and `k32_audio_play` to `volume_pct` of that ceiling, so `volume_pct` `100` is the loudest a pod can play.

`k32_audio_play` returns once its samples are in the output buffer; the last 96 ms or so play after it returns. A call that follows before they have played continues the sound without a gap, so a pod streams audio a buffer at a time. The amplifier gets no clock once the samples have played out.

`k32_mic_read` starts the microphone, which records into a 64 ms buffer between calls and stops one second after the last one; `max_samples` `0` stops it at once. The samples come with the PDM decimator's DC offset removed (a high-pass at 2.5 Hz) and 24 dB of gain, saturating at full scale: speech at 30 cm peaks at several thousand, and a quiet room's floor is about 270 RMS. The first 100 ms after the microphone starts read as silence while the offset settles. `K32_CMD:MICGAIN:<n>` sets the gain factor (1 to 64, 16 by default) until the next boot. Pods that hold the grant share one stream of samples.

The microphone's clock (GPIO 43) is the speaker's word clock, and the microphone's data line is GPIO 46. Only I2S0 has a PDM receiver, so the two share one controller and one runs at a time. The speaker comes first: playback stops a recording, and `k32_mic_read` waits for playback to end and returns `K32_ERR_BUSY` (`-16`) when it is still playing after `timeout_ms`. A channel exists only while sound moves; between sounds the node holds the speaker's pins low and keeps no audio buffers.

### Speaker and microphone on a Linux board

On a Linux board such as the Cardputer Zero, `kube32-sim` reaches the sound card through the kernel's ALSA devices in `/dev/snd` (`src/hal/linux/linux_audio.cpp`), and the four imports keep the semantics above: the same ceiling, the same return values, a 16 kHz microphone with a 64 ms buffer, and the speaker before the microphone. The node converts to the format the codec takes (48 kHz stereo on the Cardputer Zero's ES8389) and back. What differs:

* Other processes may use the card: `k32_tone` and `k32_audio_play` return `K32_ERR_BUSY` while one holds the speaker, such as a PipeWire server with a client stream, and `k32_mic_read` while one holds the microphone.
* `k32_tone` returns `K32_ERR_IO` when the card fails a write, where the Cardputer returns `0` in every case.
* The Cardputer Zero's output sounds only about 200 ms after a stream starts, so a sound after a pause begins with that much silence. The output queue grows by the same amount, so a call never waits longer than on the Cardputer, and the stream stays open for a second after a sound, so the next one starts at once.
* The board profile sets the codec's mixer controls when the node starts and puts them back when it stops.

### Speaker and microphone on the ESP32-S31-Function-CoreBoard-1

The `s31-coreboard` profile plays through the board's ES8311 codec and the NS4150B amplifier behind its speaker connector, and records from its on-board microphone through the same codec ([pins](hardware-spec.md#esp32-s31-function-coreboard-1)). The four imports keep the semantics above: the same ceiling, the same return values, a 16 kHz microphone with a 64 ms buffer whose first 100 ms read as silence, channels that exist only while sound moves, and the speaker before the microphone, since the codec's converters share one I2S controller and its clocks. What differs:

* The node sets the codec up over I2C once at boot, for a master clock at 256 times the sample rate, a setup that serves every rate: the codec takes no I2C controller from pods.
* The amplifier is on only while the speaker's channel runs.
* The codec amplifies the microphone by 72 dB (30 dB analog, 42 dB digital) and removes its DC offset with a high-pass filter. A quiet room's floor reads about 150 RMS. `K32_CMD:MICGAIN:<n>` multiplies the samples on top of that (1 to 64, 1 by default).
* `k32_tone` returns `K32_ERR_IO` when the codec's I2S channel fails and `K32_ERR_BUSY` when other calls keep the speaker for 2 seconds, where the Cardputer returns `0`.

### RGB LED

A pod with `hardware.kube32.io/led: "1"` or the `LED` capability sets the board's addressable RGB LEDs. Pods that hold the grant share them, and an LED keeps its color until the next call.

| Import | Wasm signature | C declaration | Behavior |
| --- | --- | --- | --- |
| `k32_led_set` | `i(ii)` | `int32_t k32_led_set(uint32_t index, uint32_t rgb)` | Sets LED `index` (`0` on a board with one) to the color `0xRRGGBB`, `0` for off. Returns `0`, `K32_ERR_INVAL` for an index past the board's LEDs, or `K32_ERR_NODEV` on a node without one. |

The `s31-coreboard` profile has one WS2812 LED, on GPIO 60. The node keeps one RMT TX channel for it from boot, so the board offers pods three RMT TX channels, and sends each color to the LED as a WS2812 frame. The LED has an import of its own because WS2812 bits last 0.3 and 0.9 microseconds, and the pods' RMT channels count durations in whole microseconds.

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
| `s31-coreboard` | 22 GPIO, 8 ADC, 2 I2C (`0`, `1`), 2 UART (`1`, `2`), 3 RMT TX | `I2C0` SDA 2, SCL 3; `I2C1` none; `UART1` TX 38, RX 39; `UART2` none |
| `cardputer-zero` (Linux) | 1 I2C (`0`) | `I2C0` is `/dev/i2c-1`, on the pins the kernel set up |

On the Cardputer, `I2C0` and `UART1` share the Grove port pins GPIO 2 and GPIO 1, so only one of the two can be claimed at a time. On a [Linux board](hardware-spec.md#linux-boards) the kernel sets each bus's pins and speed: a claim takes no pins, and a `kube32.io/i2c-bus-<n>` annotation is refused.

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

Each node probes the default I2C pins of its board (`src/hal/i2c_discovery.cpp`). These pins are I2C0 on GPIO 8/9 on a devkit and I2C0 on the Grove port, GPIO 2/1, on the Cardputer. The node probes before it registers, and then every 10 seconds. Each address that acknowledges a zero-length write becomes a node label. Thus a pod can select the node that has the device. Behind a PCA9548A multiplexer, for example the M5Stack PaHub, the scan probes each channel separately. It identifies the parts of the driver catalog, and these parts become [Devices](devices.md#discovery):

| Label | Value |
| --- | --- |
| `i2c.kube32.io/0x<address>` | `bus-<n>`, the lowest bus the address answers on, or `bus-<n>.mux-<hh>.<channel>` for a part behind a multiplexer |

```yaml
spec:
  nodeSelector:
    i2c.kube32.io/0x28: bus-0
```

```
kubectl --context kube32 get nodes -L i2c.kube32.io/0x28
```

* A scan runs only when no pod claims the whole controller or holds its pins. While a pod holds a bus, the node keeps the labels from the last scan. While a scan holds a free bus, pod admission waits. With a multiplexer, this takes a few hundred milliseconds. The node scans a bus that device grants share while the pods that share it run.
* If the bus lines read low with internal pull-ups enabled, something other than an idle I2C bus drives them. The node skips the scan and reports no devices on that bus.
* The kubelet patches label changes onto its Node: it adds labels for detected devices and removes labels for devices that stop answering. When a node re-registers after a reboot, it removes `i2c.kube32.io/` labels it no longer finds.
* As in Kubernetes, removing a label does not evict a running pod.

## Infrared (RMT)

The ESP32-S3 RMT peripheral sends and captures pulse trains, the signal of infrared remote controls. A pod claims one transmit channel (`esp32.io/rmt-tx: "1"`), one receive channel (`esp32.io/rmt-rx: "1"`), or one of each. The chip has four channels of each direction (the ESP32-S31 too); a node advertises at most four of each and only as many as it has pins for, so a profile without a claimable pin for a direction advertises none of it. A board that keeps a TX channel for its own device advertises one fewer: the `s31-coreboard` profile keeps one for its [RGB LED](#rgb-led). A Linux board offers one channel of each direction on its own infrared devices ([below](#infrared-on-a-linux-board)).

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
| `cardputer-zero` (Linux) | 1 RMT TX, 1 RMT RX | The board's IR LED, no pin | The board's IR receiver, no pin |

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

### Infrared on a Linux board

A [Linux board](hardware-spec.md#linux-boards) runs the channels on the kernel's LIRC devices: `--ir-tx` names the transmitter and `--ir-rx` the receiver, and the `cardputer-zero` preset sets both. The node advertises one RMT TX channel when the transmitter opens and one RMT RX channel when the receiver opens. The kernel wires the LED and the receiver, so a claim takes no pin, and an annotation that names one is refused:

```
RMT RX runs on the cardputer-zero board's own receiver: annotation kube32.io/rmt-rx cannot set a pin
```

The other settings apply where the driver supports them:

| Setting | On a Linux board |
| --- | --- |
| `carrier`, `duty` | Set with `LIRC_SET_SEND_CARRIER` and `LIRC_SET_SEND_DUTY_CYCLE` before every frame. When the driver refuses a value, the pod is not admitted and the event names the device's error. A driver without these controls sends with its own carrier, and the node logs that when it opens the device. |
| `active` | Ignored. The kernel driver knows the receiver's polarity and reports every mark as a pulse. |
| `idle_us` | The receiver's timeout (`LIRC_SET_REC_TIMEOUT`) while the pod holds the channel, and the longest space inside a frame. The device's previous timeout comes back when the channel is released. |

Frames follow the rules [above](#frames): at most 512 durations, at most 500 ms for a frame sent, and the same queue, drop counts and `K32_ERR_AGAIN`. `esp32_rmt_send` writes the frame up to its last mark in `LIRC_MODE_PULSE`, as the kernel takes an odd count of durations. The write returns once the driver has sent the frame, and the call then waits out a final space. The receiver reads `LIRC_MODE_MODE2` samples on a thread of its own from admission until release. A frame starts with a pulse, samples of one kind in a row make one duration, and a duration above 65535 µs counts as 65535. The driver's timeout report, or a space of `idle_us`, ends the frame without that space. A frame in progress when the driver reports an overflow is lost. Samples that arrived before the claim are discarded.

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

`examples/webhook.yaml` forwards the `rfid.tag` and `ir.received` topics to an HTTPS endpoint as JSON ([`apps/webhook`](https://github.com/arterialist/kube32/tree/main/apps/webhook)).

The `rfid` [device driver](https://github.com/arterialist/kube32/tree/main/apps/drivers) runs the same reader code.

## Reserved pins

The ESP32-S3 provides GPIO 0 to 21 and 26 to 48. The firewall blocks claims on reserved pins (`include/hal/chip_caps.hpp`):

* Every board (`KUBE32_RESERVED_GPIO_MASK`) reserves GPIO `0` for boot strapping, `19` and `20` for native USB, `26` to `32` for SPI flash and quad PSRAM, and `43` and `44` for the UART0 console. The `cardputer` profile offers GPIO `44` to RMT TX channels for its IR LED. A pod can claim GPIO `0` as an input pin (`esp32.io/gpio-input-0`) to read the boot button.
* The `cardputer` profile also reserves LCD SPI pins `33` to `38`, I2S speaker pins `41` to `43`, microphone data pin `46`, keyboard matrix pins `3` to `9`, `11`, `13`, and `15`, and microSD SPI pins `12`, `14`, `39`, and `40`.
* The `atoms3-lite` profile offers only its Grove pins `1` and `2`, header pins `5` to `8`, `38` and `39`, and button pin `41`. It reserves the rest, including RGB LED pin `35`, and offers IR LED pin `4` to RMT TX channels.

An ADC1 channel `N` from `0` to `9` is claimable only when its pad, GPIO `N + 1`, is claimable by the board profile.

The ESP32-S31 provides GPIO 0 to 61 except 29 and 41. Every ESP32-S31 board reserves GPIO `26` to `28` and `30` to `32` for SPI flash (the PSRAM has pins of its own), `33` and `34` for native USB, `36`, `37`, `60` and `61` for boot strapping, and `58` and `59` for the UART0 console. A pod can claim GPIO `36`, `37` and `61`, the boot button of most boards, as input pins. The `s31-coreboard` profile also reserves the Ethernet PHY's pins `4` to `19`, the codec's `50` to `57` and RGB LED pin `60`. ADC1 channel `N` from `0` to `7` samples GPIO `N + 42`.

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
