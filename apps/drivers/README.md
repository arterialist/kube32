# Device drivers

A driver pod runs one part for a [Device](../../docs/devices.md): it reads the part on each tick, serves the readings over HTTP on the pod's port, and publishes them on the device's channel topic. The Device controller creates the pod on the node the part is wired to, with the environment and grants below, and a Service named after the Device. Apps reach the part through that Service, or through their own Service selecting driver pods by label, from any node.

```sh
make -C apps/drivers        # writes images/driver-<name>.wasm (embedded as wasm://embedded/driver-<name>)
```

| Driver | Part | Bus | Readings | Actions | Period |
| --- | --- | --- | --- | --- | --- |
| [`bme688`](#bme688) | Bosch BME688 or BME680 (M5Stack ENV Pro) | I2C `0x77` (`0x76`) | `temperature_c`, `pressure_hpa`, `humidity_pct`, `gas_ohms` | | 5 s |
| [`miniscale`](#miniscale) | M5Stack Unit Mini Scales | I2C `0x26` | `weight_g`, `raw` | `tare`, `led` | 1 s |
| [`rfid`](#rfid) | MFRC522-compatible reader (M5Stack RFID 2) | I2C `0x28` | `tag`, `present` | | 200 ms |
| [`chip-temp`](#chip-temp) | The ESP32-S3's temperature sensor | internal | `temperature_c` | | 10 s |
| [`button`](#button) | Push button or contact | GPIO input | `pressed`, `presses` | | 50 ms |
| [`switch`](#switch) | Relay, MOSFET, or LED | GPIO output | `on` | `set`, `toggle` | 30 s |

The period is the default reading period. A Device's `intervalMs` sets both `DEVICE_INTERVAL_MS` and the pod's `kube32.io/tick-interval`, which makes the node tick the driver.

The I2C units plug into a Grove port (on the Cardputer: SDA GPIO 2, SCL GPIO 1) or into a channel of an M5Stack PaHub on that port. Each I2C Device names its bus, address, and PaHub channel, and the node selects the channel before each transfer.

## HTTP API

Every driver serves the same API on port 7000:

| Request | Answer |
| --- | --- |
| `GET /` | `200 {"device":"kitchen-env","driver":"bme688","node":"<node>","ok":true,"at":<unix ms>,"uptime_ms":<ms>,"readings":{...}}`. `at` and `uptime_ms` are the times of the reading: the node's wall clock (`0` while it is not set) and its uptime. While the part does not answer: `503 {"device":...,"driver":...,"node":...,"ok":false,"error":"no answer at 0x77 on bus 0 (error -19)"}`. |
| `GET /describe` | `200 {"driver":"bme688","readings":{"temperature_c":"C",...},"actions":[...]}` |
| `POST /` | `{"action":"<name>",...}`. `200` with the `GET /` body once done, `400 {"ok":false,"error":"..."}` for an unknown action or bad arguments, `503` when the part does not answer. |

Events go out on `DEVICE_TOPIC` with the `GET /` body: each reading of `bme688`, `miniscale`, and `chip-temp`; each change of `rfid`, `button`, and `switch`, with a periodic report of the last two; and the part going silent. An event carries at most 200 bytes: a longer body leaves out `uptime_ms`, then `node`, then `device` and `driver` (the topic names the device). Subscribe with the pod annotation `kube32.io/event-topics: device.<name>`, or stream the topic through an Ingress with `kube32.io/event-stream`.

A part that is absent or stops answering is retried on every tick. The first failure is logged and published, then nothing until it answers again; error lines are limited to one a minute. A pod that lacks `hardware.kube32.io/ipc` serves HTTP and logs once that it cannot publish.

## Environment

| Variable | Meaning |
| --- | --- |
| `DEVICE_NAME` | The Device (default: the driver's name). |
| `NODE_NAME` | The node the part is wired to. |
| `DEVICE_TOPIC` | Topic for readings (default `device.<DEVICE_NAME>`). |
| `DEVICE_INTERVAL_MS` | Reading period. |
| `I2C_BUS`, `I2C_ADDR` | I2C drivers: bus (default `0`) and address in hex, `0x77` or `77`. |
| `PIN_PIN` | GPIO drivers: the GPIO number. |
| `CONFIG_<KEY>` | Driver settings, listed with each driver. |

An I2C driver talks to its address with `esp32_i2c_write` and `esp32_i2c_read` and never sees the multiplexer. A GPIO driver uses `esp32_gpio_set_mode`, `esp32_gpio_read`, and `esp32_gpio_write` on its pin.

## Memory

Each driver's linear memory is 3 KiB (`memory: 3Ki`): a 512-byte stack and the static data, checked after linking. What a pod takes from a board's pod pool is the kubelet's estimate from the module, which `tests/test_drivers.cpp` prints:

| Driver | Wasm | Pool estimate |
| --- | --- | --- |
| `bme688` | 7.3 KiB | 26.8 KiB |
| `miniscale` | 5.8 KiB | 22.6 KiB |
| `rfid` | 6.5 KiB | 25.6 KiB |
| `chip-temp` | 3.8 KiB | 15.8 KiB |
| `button` | 4.9 KiB | 19.8 KiB |
| `switch` | 5.5 KiB | 22.0 KiB |

The `bme688`, `miniscale`, and `rfid` drivers together take about 75 KiB, so an ENV Pro, a Mini Scales unit, and an RFID 2 run on one Cardputer, within the 80 KiB pool block of a board without PSRAM.

## bme688

Bosch BME688, and the BME680 before it, on I2C at `0x77` (`0x76` with SDO low). The M5Stack ENV Pro unit carries a BME688 at `0x77`.

Each tick starts a forced-mode measurement: temperature at 2x oversampling, pressure at 16x, humidity at 1x, no IIR filter, then one heater step and the gas resistance at its end. The driver is ticked every 10 ms until the result is in, about 43 ms plus the heater time, and never blocks the interpreter while it waits. The heater target is computed from the last temperature measured.

The chip id at `0xD0` must read `0x61`. The variant at `0xF0` selects the gas registers and formula: `0x01` BME688, `0x00` BME680. Compensation is the integer code of the Bosch BME68x Sensor API, under its BSD-3-Clause license (notice in `bme688/bme688.c`).

| Setting | Default | Meaning |
| --- | --- | --- |
| `CONFIG_HEATER_C` | `300` | Heater temperature, `200` to `400` °C. |
| `CONFIG_HEATER_MS` | `100` | Heater time, `1` to `4032` ms. |
| `CONFIG_GAS` | `on` | `off` leaves the heater off and `gas_ohms` out. |

`gas_ohms` is `null` when the heater did not reach its temperature or the gas reading is not valid. The gas resistance is raw: it falls as volatile compounds rise, and a new sensor needs some hours of operation before it settles.

## miniscale

M5Stack Unit Mini Scales: a 5 kg load cell and an HX711 behind an STM32F030, on I2C at `0x26`, protocol v3. Firmware v4 also reports negative weights.

| Register | Content |
| --- | --- |
| `0x00` | Raw ADC value, int32, little-endian |
| `0x10` | Weight in grams, float |
| `0x20` | The unit's button: `0` pressed, `1` released |
| `0x30` | LED: R, G, B |
| `0x40` | Gap (calibration, counts per gram), float |
| `0x50` | Offset: write `1` to tare |
| `0x60` | Weight x 100 in grams, int32, little-endian |
| `0xFE` | Firmware version |
| `0xFF` | The unit's I2C address |

Each tick reads `0x60` and `0x00`. The unit keeps its calibration itself.

| Action | Effect |
| --- | --- |
| `{"action":"tare"}` | The current load becomes zero; the next reading is relative to it. |
| `{"action":"led","r":0,"g":32,"b":0}` | Sets the RGB LED, `0` to `255` per color (absent: `0`). |

## rfid

ISO 14443-A tags (4, 7, and 10-byte UIDs) through an MFRC522-compatible reader on I2C, such as the M5Stack RFID 2 unit (WS1850S at `0x28`). The reader code is the `rfid` example's (`apps/rfid/mfrc522.h`).

Each tick wakes the tags in the field, reads the UID of one, and halts it. A tag that comes into the field is published, and so is the field going empty (no tag for three polls). `tag` is the last UID in lowercase hex, `null` before the first; `present` says whether it is still in the field. A reader that stops answering is set up again on the next tick.

## chip-temp

The ESP32-S3's internal temperature sensor, read with `esp32_chip_temp_read` (granted by `hardware.kube32.io/chip-temp`). It measures the die, which runs above the room's temperature while the CPU and radio work; it tells how warm the board runs. Readings are in tenths of a degree. Every node has the sensor, so discovery does not create these Devices; write one for a node. The simulator reports 40.0 °C.

## button

A push button or any contact on a GPIO input. Each tick reads the pin. A new level counts once it has held for `CONFIG_DEBOUNCE_MS`; until then, the driver is ticked every 10 ms. The tick period is the time to notice a press, and the debounce time is the time to trust it. Each change is published, and the state is published again every `CONFIG_REPORT_MS` without a change. `presses` counts presses since the driver started.

| Setting | Default | Meaning |
| --- | --- | --- |
| `PIN_PIN` | | The GPIO (required). |
| `CONFIG_PULL` | `up` | `up`, `down`, or `none`. |
| `CONFIG_ACTIVE` | `low` | The level of a pressed button: `low` or `high`. |
| `CONFIG_DEBOUNCE_MS` | `30` | How long a new level must hold. |
| `CONFIG_REPORT_MS` | `60000` | Period of reports without a change; `0` publishes changes only. |

Wiring: a button between the pin and ground with `CONFIG_PULL=up` and `CONFIG_ACTIVE=low`.

## switch

A GPIO output driving a relay, a MOSFET, or an LED. Each change is published, and the state is published again every tick. Each tick also drives the pin again, so a pin something else reset returns to its state. The level is set before the pin becomes an output, so it starts in its state.

| Setting | Default | Meaning |
| --- | --- | --- |
| `PIN_PIN` | | The GPIO (required). |
| `CONFIG_INITIAL` | `off` | The state at start: `off` or `on`. |
| `CONFIG_ACTIVE` | `high` | The level that means on: `high` or `low`. |

| Action | Effect |
| --- | --- |
| `{"action":"set","on":true}` | Switches on, or off with `false`. |
| `{"action":"toggle"}` | Switches to the other state. |

## Writing a driver

A driver is one C file that includes `common/driver.h`, which implements the API above, the environment, events, and the error lines. The file defines `DRIVER` (its catalog name), `DESCRIBE` (the `GET /describe` body), `INTERVAL_MS`, `I2C_DEFAULT_ADDR` for an I2C part or `GPIO_DRIVER` for a pin, and `ACTIONS` when it takes any. It implements:

```c
static void start(void);            /* k32_init, after the environment is read */
static int  poll(uint32_t now);     /* k32_tick: 1 to be ticked again in 10 ms, else 0 */
static void put_readings(void);     /* the members of "readings", with key() and put_*() */
static int  act(const char* name, uint32_t n, const char* body, const char* end);   /* with ACTIONS */
```

`poll` calls `dev_ok(now)` after a reading and `publish()` to send it, or `dev_fail(now, "what failed", error)`. A `Makefile` of three lines builds it with `common/driver.mk`. `tests/test_drivers.cpp` runs every driver against simulated parts on the stub I2C bus and GPIO pins.
