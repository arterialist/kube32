# Device drivers

A driver pod operates one part for a [Device](../../docs/devices.md). On each tick it reads the part. It serves the readings over HTTP on the pod's port and publishes them on the Device's channel topic.

The Device controller creates the pod on the node that has the part, with the environment and grants below. It also creates a Service with the Device's name. From any node, apps reach the part through that Service, or through their own Service that selects driver pods by label. For an `OnDemand` Device (the default), the pod runs only while the Device is in use ([Activation](../../docs/devices.md#activation)).

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

The period is the default reading period. A Device's `intervalMs` sets `DEVICE_INTERVAL_MS` and the pod's `kube32.io/tick-interval`. The node ticks the driver at that period.

The I2C units connect to a Grove port. On the Cardputer, the Grove port has SDA on GPIO 2 and SCL on GPIO 1. A unit can also connect to a channel of an M5Stack PaHub on that port. Each I2C Device names its bus, address and PaHub channel. The node selects the channel before each transfer.

## HTTP API

All drivers serve the same API on port 7000:

| Request | Answer |
| --- | --- |
| `GET /` | `200 {"device":"kitchen-env","driver":"bme688","node":"<node>","ok":true,"at":<unix ms>,"uptime_ms":<ms>,"readings":{...}}`. `at` and `uptime_ms` are the times of the reading: the node's wall clock (`0` while the clock is not set) and its uptime. While the part does not answer: `503 {"device":...,"driver":...,"node":...,"ok":false,"error":"no answer at 0x77 on bus 0 (error -19)"}`. |
| `GET /describe` | `200 {"driver":"bme688","readings":{"temperature_c":"C",...},"actions":[...]}` |
| `POST /` | `{"action":"<name>",...}`. `200` with the `GET /` body when the action is done. `400 {"ok":false,"error":"..."}` for an unknown action or bad arguments. `503` when the part does not answer. |

The driver sends events on `DEVICE_TOPIC` with the `GET /` body:

- Each reading of `bme688`, `miniscale` and `chip-temp`.
- Each change of `rfid`, `button` and `switch`. `button` and `switch` also send a periodic report.
- The part going silent.

An event carries at most 200 bytes. If the body is longer, the driver omits `uptime_ms`, then `node`, then `device` and `driver`. The topic still names the device. To subscribe, set the pod annotation `kube32.io/event-topics: device.<name>`; the subscription keeps the driver of an `OnDemand` Device running. To stream the topic through an Ingress, use `kube32.io/event-stream`.

When a part is absent or does not answer, the driver tries again on each tick. It logs and publishes the first failure, and then nothing until the part answers again. The driver writes at most one error line a minute. A pod without `hardware.kube32.io/ipc` serves HTTP and logs one time that it cannot publish.

## Environment

| Variable | Meaning |
| --- | --- |
| `DEVICE_NAME` | The Device (default: the driver's name). |
| `NODE_NAME` | The node that has the part. |
| `DEVICE_TOPIC` | Topic for readings (default `device.<DEVICE_NAME>`). |
| `DEVICE_INTERVAL_MS` | Reading period. |
| `I2C_BUS`, `I2C_ADDR` | I2C drivers: bus (default `0`) and address in hex, `0x77` or `77`. |
| `PIN_PIN` | GPIO drivers: the GPIO number. |
| `CONFIG_<KEY>` | Driver settings. Each driver section lists its settings. |

An I2C driver uses `esp32_i2c_write` and `esp32_i2c_read` on its address. It never sees the multiplexer. A GPIO driver uses `esp32_gpio_set_mode`, `esp32_gpio_read` and `esp32_gpio_write` on its pin.

## Memory

Each driver has 3 KiB of linear memory (`memory: 3Ki`): a 512-byte stack and the static data. The build checks this size after the link step. The kubelet estimates the part of a board's pod pool that a pod uses from its module. `tests/test_drivers.cpp` prints these estimates:

| Driver | Wasm | Pool estimate |
| --- | --- | --- |
| `bme688` | 7.6 KiB | 28.6 KiB |
| `miniscale` | 5.8 KiB | 22.6 KiB |
| `rfid` | 6.5 KiB | 25.6 KiB |
| `chip-temp` | 3.8 KiB | 15.8 KiB |
| `button` | 4.9 KiB | 19.8 KiB |
| `switch` | 5.5 KiB | 22.0 KiB |

The estimates for `bme688`, `miniscale` and `rfid` add up to about 77 KiB. On the Cardputer, the three drivers load in 69 KB. Thus an ENV Pro, a Mini Scales unit and an RFID 2 run together on one Cardputer. They fit in the 80 KiB pool block of a board without PSRAM.

## bme688

The driver operates a Bosch BME688, or the earlier BME680, on I2C at `0x77`. With SDO low, the address is `0x76`. The M5Stack ENV Pro unit has a BME688 at `0x77`.

Each tick starts a forced-mode measurement. The measurement uses 2x oversampling for temperature, 16x for pressure and 1x for humidity, with no IIR filter. Then it runs one heater step and measures the gas resistance at the end of the step. The node ticks the driver every 10 ms until the result is ready, which takes about 43 ms plus the heater time. The driver never blocks the interpreter while it waits. The driver calculates the heater target from the last temperature that it measured.

The chip id at `0xD0` must read `0x61`. The variant at `0xF0` selects the gas registers and formula: `0x01` for the BME688, `0x00` for the BME680. The compensation is the integer code of the Bosch BME68x Sensor API, under its BSD-3-Clause license. The notice is in `bme688/bme688.c`.

| Setting | Default | Meaning |
| --- | --- | --- |
| `CONFIG_HEATER_C` | `300` | Heater temperature, `200` to `400` °C. |
| `CONFIG_HEATER_MS` | `100` | Heater time, `1` to `4032` ms. |
| `CONFIG_GAS` | `on` | `off` keeps the heater off and omits `gas_ohms`. |

`gas_ohms` is `null` when the heater did not reach its temperature or the gas reading is not valid. The gas resistance is a raw value. It decreases when volatile compounds increase. A new sensor must operate for some hours before its value becomes stable.

## miniscale

The M5Stack Unit Mini Scales has a 5 kg load cell and an HX711 behind an STM32F030. It is on I2C at `0x26` and uses protocol v3. Firmware v4 also reports negative weights.

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

Each tick reads `0x60` and `0x00`. The unit keeps its own calibration.

| Action | Effect |
| --- | --- |
| `{"action":"tare"}` | The current load becomes zero. The next reading is relative to it. |
| `{"action":"led","r":0,"g":32,"b":0}` | Sets the RGB LED, `0` to `255` for each color. A missing color is `0`. |

## rfid

The driver reads ISO 14443-A tags with 4, 7 and 10-byte UIDs. It uses an MFRC522-compatible reader on I2C, for example the M5Stack RFID 2 unit (WS1850S at `0x28`). The reader code comes from the `rfid` example (`apps/rfid/mfrc522.h`).

Each tick wakes the tags in the field, reads the UID of one tag and halts that tag. The driver publishes a tag that comes into the field. It also publishes when the field becomes empty, which is after three polls with no tag. `tag` is the last UID in lowercase hex, or `null` before the first tag. `present` tells whether the tag is still in the field. If the reader does not answer, the driver sets it up again on the next tick.

## chip-temp

The driver reads the ESP32-S3's internal temperature sensor with `esp32_chip_temp_read`. The resource `hardware.kube32.io/chip-temp` grants this call. The sensor measures the die, which is warmer than the room while the CPU and radio work. Thus the reading tells how warm the board is. The readings have a resolution of a tenth of a degree.

Each node has this sensor, so discovery does not create these Devices. Write a Device for each node that you want to read. The simulator reports 40.0 °C.

## button

The driver reads a push button or another contact on a GPIO input. Each tick reads the pin. A new level counts only after it stays for `CONFIG_DEBOUNCE_MS`. Until then, the node ticks the driver every 10 ms. The tick period sets how fast the driver finds a press. The debounce time sets how long the driver waits before it accepts the press.

The driver publishes each change. It also publishes the state every `CONFIG_REPORT_MS` when nothing changes. `presses` counts the presses since the driver started.

| Setting | Default | Meaning |
| --- | --- | --- |
| `PIN_PIN` | | The GPIO (required). |
| `CONFIG_PULL` | `up` | `up`, `down`, or `none`. |
| `CONFIG_ACTIVE` | `low` | The level of a pressed button: `low` or `high`. |
| `CONFIG_DEBOUNCE_MS` | `30` | How long a new level must stay. |
| `CONFIG_REPORT_MS` | `60000` | Period of reports when nothing changes. `0` publishes changes only. |

Wiring: connect the button between the pin and ground, and set `CONFIG_PULL=up` and `CONFIG_ACTIVE=low`.

## switch

The driver operates a GPIO output that drives a relay, a MOSFET or an LED. It publishes each change, and it publishes the state again on each tick. Each tick also drives the pin again. Thus if something else resets the pin, the pin goes back to its state. The driver sets the level before it makes the pin an output, so the pin starts in its state.

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

A driver is one C file that includes `common/driver.h`. This header implements the API above, the environment, the events and the error lines. The file defines these macros:

- `DRIVER`: its catalog name.
- `DESCRIBE`: the `GET /describe` body.
- `INTERVAL_MS`.
- `I2C_DEFAULT_ADDR` for an I2C part, or `GPIO_DRIVER` for a pin.
- `ACTIONS`, if the driver has actions.

The file implements these functions:

```c
static void start(void);            /* k32_init, after the environment is read */
static int  poll(uint32_t now);     /* k32_tick: 1 to be ticked again in 10 ms, else 0 */
static void put_readings(void);     /* the members of "readings", with key() and put_*() */
static int  act(const char* name, uint32_t n, const char* body, const char* end);   /* with ACTIONS */
```

After a reading, `poll` calls `dev_ok(now)`, and then `publish()` to send the reading. After a failure, it calls `dev_fail(now, "what failed", error)`. A three-line `Makefile` builds the driver with `common/driver.mk`. `tests/test_drivers.cpp` runs each driver against simulated parts on the stub I2C bus and GPIO pins.

Keep long calculations in separate functions (`__attribute__((noinline))`). On a board without PSRAM, a long function that clang inlines into `k32_tick` can use more native stack than a call has (see [Benchmarks](../../docs/benchmarks.md#native-stack)).
