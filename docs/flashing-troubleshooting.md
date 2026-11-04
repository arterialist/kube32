# Flashing and the serial console

## Flashing

The [browser installer](flash.md) installs a release without a build or any tools.

`kube32 flash` writes the firmware of a release to one board:

```
kube32 flash [--port <serial port>] [--version X.Y.Z]
```

- Downloads the firmware parts of the installed `kube32`'s GitHub release, or of `--version`: the bootloader, partition table, `otadata` and application listed in `kube32-firmware-esp32s3-<version>-flash.json`. It checks each file against its `.sha256` file and the SHA-256 in the list, and keeps them in the cache directory ([Install](install.md#releases-and-versions)).
- Uses `--port`, else `KUBE32_SERIAL_PORT`, else the one connected port with USB vendor ID `0x303a` (native USB-Serial/JTAG) or `0x1a86` (CH340). With several boards connected, pass `--port`.
- Writes each part at its offset with esptool at 921600 baud, and again at 460800 baud when that fails. `--baud` sets one rate. Writes paced blocks on CH340 bridges and resets the board as `scripts/flash.sh` does (below), reading the same `KUBE32_FLASH_BEFORE` and `KUBE32_FLASH_AFTER`.
- The parts leave NVS alone, so the board keeps its stored settings: Wi-Fi, node name and cluster. `--erase-settings` writes the release's merged image from `0x0` instead, which erases them.
- `--file <image>` writes a local merged image from `0x0` instead of a release, which also erases the stored settings.
- Prints the console commands that set the board up, and offers to open `kube32 console`.

### Flashing a build from source

Build once, then flash one board at a time:

```
pio run -e esp32s3
bash scripts/flash.sh <serial port> 921600
```

`scripts/flash.sh <serial port> [baud]` defaults to `460800` baud and reads `KUBE32_SERIAL_PORT` when you omit the port argument. The script locates `pio` on `PATH`, then in `$PLATFORMIO_CORE_DIR/penv/bin` or `~/.platformio/penv/bin`. It runs `esptool` from PlatformIO's virtualenv or from a system `python3` with `esptool` installed.

- Writes `bootloader.bin` at `0x0`, `partitions.bin` at `0x8000`, an erased `otadata` at `0x11000`, and `firmware.bin` into the `ota_0` app slot at `0x20000` from `.pio/build/esp32s3/` in `dio` mode with an 8 MB flash size, using the offsets from `partitions.csv`. The board boots the flashed image. This leaves NVS and the object store journal untouched. Runs `pio run` if the binaries are missing.
- Writes 256-byte blocks with a 1.5 ms pause on CH340 USB-UART bridges, vendor ID `0x1a86`. Uses `esptool` defaults on native USB-Serial/JTAG ports.
- Resets native USB boards through the RTC watchdog with `--after watchdog-reset`, and resets CH340 boards with `--after hard-reset`.
- Reads `KUBE32_FLASH_BEFORE` for `--before`, defaulting to `default-reset`, and `KUBE32_FLASH_AFTER` for `--after`.

To enter ROM download mode from the serial console when USB auto-reset fails, send `K32_CMD:BOOTLOADER`, then flash with `KUBE32_FLASH_BEFORE=no-reset`.

Each node checks at boot that its interpreter computes 64-bit integer values right: a global, loads from memory, locals and arguments over a call, and a host call's result. The boot log prints `[KUBELET] interpreter check: ok`, and the Node annotation `kube32.io/interp-check` holds `ok` or `failed: <step>`. Check it after flashing a build made with another toolchain or other compiler flags:

```
kubectl get node <node> -o jsonpath='{.metadata.annotations.kube32\.io/interp-check}'
```

Flash a board running the single-app partition table once with a current build to move it to the A/B layout; see [Firmware updates](firmware-updates.md#moving-a-board-to-this-layout). After that, update boards through the cluster with `kube32 firmware push` and `kube32 firmware rollout`.

### Quad I/O flash

The firmware builds for dual I/O (`dio`) flash reads, which every ESP32-S3 module supports. Many flash chips also read in quad I/O (`qio`), which doubles the bandwidth from flash to the cache: blobs read in place and code that runs from flash get faster. To build for it, set the mode in `sdkconfig.esp32s3` before `pio run`:

```
# CONFIG_ESPTOOLPY_FLASHMODE_DIO is not set
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHMODE="qio"
```

or pick *Serial flasher config > Flash SPI mode > QIO* in `pio run -e esp32s3 -t menuconfig`. The second-stage bootloader sets the chip's quad enable bit and switches to quad reads, so the image header stays `dio` and `scripts/flash.sh` writes the image as usual.

A chip reads in QIO only if ESP-IDF knows how to set its quad enable bit. `esptool.py --port <serial port> flash_id` prints the chip's manufacturer and device IDs. These chips run kube32 in QIO, with the read speed of a mapped blob as `K32_CMD:BLOBREAD` reports it:

| Board | Flash chip | `dio` | `qio` |
| --- | --- | ---: | ---: |
| M5Stack Cardputer | GigaDevice | 3.9 MB/s | 7.0 MB/s |
| ESP32-S3 devkit, 8 MB | Puya, manufacturer `0x85`, device `0x2017` | 13 MB/s | 21 MB/s |

ESP-IDF drives the Puya chip as a generic chip, whose quad enable method works for it. A board whose chip does not boot a `qio` image takes the `dio` build again through download mode.

## Serial console

`kube32 console [--port <serial port>]` opens a board's console without resetting the board. It prints what the board writes and sends each line typed.

The firmware listens for newline-terminated commands up to 512 characters on USB serial at 115200 baud. Generic boards listen on both native USB-Serial/JTAG and UART0, answering on the port that received the command. The Cardputer profile listens on USB only.

After any reset other than power-on (such as a reboot, a panic, or a firmware update), the board disconnects from the USB bus for 100 ms before starting its console. This causes the host to re-enumerate the port, and programs holding the port, including `kube32 gateway`, reopen it. The board blocks on whichever console port received the last bytes and only polls the other port, so a request streaming in over USB loses no bytes while UART0 is idle.

Opening the USB port on a Cardputer or the CH340 port on a devkit resets the board. Wait for the boot log to finish before sending commands.

### Board profile

Profile commands write to the `k32` NVS namespace, return `K32_OK ..., rebooting`, and reboot the board:

| Command | Effect |
| --- | --- |
| `K32_CMD:WIFI:<ssid>:<password>` | Join a 2.4 GHz network as a station. The station picks the strongest access point with that name and roams to one at least 10 dB stronger when signal drops below -70 dBm. The SSID cannot contain `:`. `K32_CMD:WIFI:` erases credentials. |
| `K32_CMD:API:<host>` | Set the control plane address for a worker, default `192.168.4.1`. `auto` erases it. A worker that gets no answer there switches to the address the control plane announces over ESP-NOW. |
| `K32_CMD:AP:off` | Disable the `Kube32-Cluster` access point on the control plane. `on` erases the setting. |
| `K32_CMD:APPASS:<password>` | Set the `Kube32-Cluster` access point password (8 to 63 printable characters) on the control plane and on each worker that joins the access point. An empty value erases the password, and the control plane generates a replacement on its next boot. |
| `K32_CMD:APPASS` | Print the stored access point password as `K32_APPASS <password>`. |
| `K32_CMD:BOARD:cardputer` | Enable the Cardputer display, keypad, speaker, and microSD. `display` is an alias. `generic` erases it. |
| `K32_CMD:ROLE:control-plane` / `worker` | Override the PSRAM-based role. `auto` erases it. |
| `K32_CMD:NAME:<name>` | Set the node name to a DNS-1123 label, up to 63 characters of `a-z`, `0-9`, and `-`. `auto` erases it and falls back to `esp32-<mac>`. |
| `K32_CMD:TOKEN:<token>` | Set the API server and kubelet bearer token, 16 to 128 printable characters without spaces. `off` disables authentication. |
| `K32_CMD:REBOOT` | Reboot without changing settings. |
| `K32_CMD:BOOTLOADER` | Reboot into ROM download mode for flashing with `KUBE32_FLASH_BEFORE=no-reset`. |
| `K32_CMD:SYSRESERVE:<size>` | Set the PSRAM that the node keeps back for the system, from 16Ki to 1Mi (for example `256Ki`), from the next boot on and reboot. `auto` returns to the default of 192 KiB on a control plane and 64 KiB on a worker. See [hardware spec](hardware-spec.md#node-and-pod-memory-figures). |
| `K32_CMD:SDKHZ:<khz>` | Set the microSD card's SPI clock from the next boot on and reboot. `0` returns to the 20 MHz default. See [storage](storage.md#microsd-hardware-and-mount). |

### Diagnostics

| Command | Output |
| --- | --- |
| `K32_CMD:STATUS` | `K32_STATUS node=... role=... mac=... free_heap=... min_free_heap=... free_psram=... uptime_ms=... pods=... storage=... auth=... tls=...` |
| `K32_CMD:STORE` | `K32_STORE journal=... keys=... live_bytes=... capacity_bytes=... sectors=.../... pending=... records=... bytes=... moved=... via_ram=... unchanged=... errors=... refused_full=... refused_no_space=... refused_io=... refused_oom=... last_error=... revision=...` on the control plane, or `K32_STORE journal=none`. `refused_*` count journal appends refused by cause and `last_error` names the last cause (`none`, `full`, `too-large`, `no-space`, `io`, `out-of-memory`, `unreadable`). |
| `K32_CMD:CERT` | The API server's self-signed TLS certificate in PEM format as `K32_CERT <line>` lines followed by `K32_OK cert`. |
| `K32_CMD:FIRMWARE` | `K32_FIRMWARE version=... sha256=... slot=... state=Confirmed\|Verifying update_slot=... staged=... staged_sha256=... staged_version=...`: reports the running image and, on the control plane, the staged image. See [Firmware updates](firmware-updates.md). |
| `K32_CMD:SCAN` | Up to 20 `K32_AP ssid="..." rssi=... ch=... auth=...` lines for 2.4 GHz networks. |
| `K32_CMD:TASKS` | One `K32_TASK` line per FreeRTOS task with state, priority, core, free stack high-water mark, and run time. |
| `K32_CMD:TASKBT:<task name prefix>` | First 48 words of a blocked task's saved stack for `addr2line`. Exact name matches take precedence over prefix matches. |
| `K32_CMD:BTALL` | Every task's backtrace for `addr2line`. |
| `K32_CMD:THROW` | The housekeeping loop raises `std::bad_alloc` on its next pass and logs `[CLUSTER] housekeeping pass skipped: std::bad_alloc`; the node keeps running. |
| `K32_CMD:HEAP` | `K32_HEAP used\|free <=<size> <blocks> <bytes>` lines: internal heap blocks grouped by size. |
| `K32_CMD:HEAPMIN:start` / `K32_CMD:HEAPMIN` | `start` begins tracking the internal heap's lowest free size; `K32_CMD:HEAPMIN` prints `K32_OK heapmin <bytes>, internal free <bytes>, largest <bytes>` for the time since and stops. The `[MEM]` lines' minimum counts from boot. |
| `K32_CMD:HEAPWATCH:on` / `dump` / `off` | Firmware built with `CONFIG_HEAP_USE_HOOKS` only: `on` records every internal allocation with the task that made it; `dump` prints `K32_HEAPWATCH` lines with each task's bytes at the moment the recorded bytes peaked and now; `off` dumps and stops. |
| `K32_CMD:POOL` | `K32_POOL <offset> <size> used\|free` lines list the chunks of the pod pool block's small heap in address order. |
| `K32_CMD:BENCH:<kernel>:<n>:<runs>` | `K32_BENCH kernel=<kernel> n=<n> runs=<runs> us_min=<µs> us_total=<µs> sum=<checksum>`: runs a benchmark kernel natively: `fib`, `crc32`, `sieve`, `sort`, `mandel` or `json` (see [Benchmarks](benchmarks.md)). `K32_CMD:BENCH:hostcall:<calls>` times that many `esp_timer_get_time()` calls. |
| `K32_CMD:TCP` | One `K32_TCP` line per lwIP TCP connection with its state, local port, peer, and bytes unsent, unacknowledged, unread by the application, or out of order. |
| `K32_CMD:ACCESSLOG:on` / `off` | Log HTTP requests on the API server and ingress. |
| `K32_CMD:SD` | microSD mount state, capacity, and files under `/sd/kube32`. See [storage](storage.md). |
| `K32_CMD:BLOBS` | `K32_BLOBS medium=... location=... available=... capacity=... free=...` per blob store and `K32_BLOB` lines for its blobs. See [Blobs](storage.md#blobs). |
| `K32_CMD:BLOBREAD:<name>[:<chunk>]` | `K32_BLOBREAD <name> <medium> mapped\|read chunk=... bytes=... us=... kB_per_s=... sum=...`: reads a blob natively from start to end, through its mapping or in file reads of `<chunk>` bytes (512 to 16384, default 4096), and prints the throughput and a sum of its 32-bit words. |
| `K32_CMD:MIC:<ms>` | Records the microphone for 100 to 10000 ms and prints `K32_MIC window=... mean=... rms=... peak=...` for every 250 ms. See [Peripherals](peripherals.md#speaker-and-microphone). |
| `K32_CMD:MICGAIN:<n>` | The microphone's digital gain factor, 1 to 64 (16 by default), until the next boot. |
| `K32_CMD:KEY:<text>` | Queue keystrokes `\n`, `\b`, `\t`, and characters. |
| `K32_CMD:KEYPOP` | Pop one key from the queue. |
| `K32_CMD:TONE:<count>` | Play 1 to 200 short tones. |

### HTTP over the console

`kube32 gateway` carries HTTP over the console in streams (`include/app/console_relay.hpp`). Host lines end with `\n` only. Board frames start with the byte `0x1e` and the board writes each frame as a single unit. Log output can surround a frame, but it never splits one. CRCs use CRC-32 (zlib) in hexadecimal.

| From | Frame | Effect |
| --- | --- | --- |
| Host | `K32_RESET` | Ends every stream. The board answers `K32_READY <streams> <role>`. |
| Host | `K32_OPEN <id> <port> <length> <crc32>\n<request>` | Opens stream `<id>` and sends an HTTP request under 64 KiB with `Connection: close` to `<port>` on the control plane. A worker forwards the request over Wi-Fi. |
| Board | `K32_DATA <id> <length> <crc32>\n<bytes>` | Carries the next bytes of the response. |
| Board | `K32_END <id> <0\|1>` | Ends the response: `0` when complete, `1` when cut short. |
| Host | `K32_CLOSE <id>` | Indicates that the client closed the connection. The board closes its connection. |
| Board | `K32_ERR crc <id>` | Indicates a request CRC mismatch. The host resends the request. |

A board with PSRAM runs up to 10 streams at once. A board without PSRAM runs up to 2. The board answers `503` when the host opens a stream beyond the limit. If the server sends nothing on a stream for 5 minutes, the board ends the stream with `K32_END <id> 1`.

`K32_HTTP <port> <length> [<crc32>]\n<request>` sends a single request. The board returns `K32_RESP <length>\n<raw response>` when the response finishes, or `K32_ERR crc` on a CRC mismatch. The board caps responses at 256 KiB.

Opening a native USB-Serial/JTAG or CH340 port with RTS high and DTR low resets the board. Clear RTS before DTR, as `kube32 gateway` does.

### Improv Wi-Fi

The console also speaks the [Improv Wi-Fi serial protocol](https://www.improv-wifi.com/serial/), which the [browser installer](flash.md) uses. A packet starts a line with `IMPROV`. The board answers:

| Request | Answer |
| --- | --- |
| Current state | `Provisioned` with the API server URL when the station holds an address on the network the board has credentials for, `Ready` otherwise |
| Device information | `kube32`, the firmware version, `ESP32-S3` and the node name |
| Wi-Fi networks | One result per network name in range |
| Wi-Fi settings | Joins the network next to the cluster access point for up to 20 seconds. On success it answers `Provisioned` with `http://<ip>:6443` on a control plane (no URL on a worker), stores the credentials as `K32_CMD:WIFI` does, and reboots 3 seconds later. Otherwise it answers `Unable to connect` and returns to its previous network. |

## Troubleshooting

| Symptom | Resolution |
| --- | --- |
| `esptool` reports "No serial data received" or the native USB console stops answering | Unplug and replug the board, or switch to the devkit's second USB port, the UART0 CH340 bridge, for console commands and flashing. Avoid repeated port open and close cycles with DTR/RTS toggling. |
| Board stays in download mode after flashing | Press the reset button or flash with `scripts/flash.sh`. |
| Garbled console output on the CH340 port | Set the baud rate to 115200 or use the native USB port. |
| Worker never registers | Check `[WORKER] kubelet -> http://<host>:6443` in the boot log and set the control plane address with `K32_CMD:API:<host>`. |
| `[NET] station not connected: reason 201` | No access point with the stored name is in range. Reason 15 or 204 is a wrong password. Store other credentials with `K32_CMD:WIFI:<ssid>:<password>`. |
| Control plane logs `http://192.168.4.1:6443` while joined to a station network | Disable the softAP with `K32_CMD:AP:off`. |
| Worker logs `[NET] no password for the cluster access point` | Read the password on the control plane with `K32_CMD:APPASS` and set it on the worker with `K32_CMD:APPASS:<password>`. |
| Keypad events do not reach the control plane across different access points | Verify the worker node object has an `InternalIP` for UDP transport. |
| Android Chrome cannot reach the board on LAN | Enable the Local network permission for Chrome on Android 17. |
| Cardputer flash is corrupted after flashing | Reflash without pressing G0. |
| Boot log shows `updates go to nowhere (no second app slot)` | The board uses the single-app partition table. Flash it once with `scripts/flash.sh`. |
| A board comes back on its other firmware after a rollout | The updated image reset or did not report Ready before confirming itself. Run `kubectl describe node <node>` to read the reason in the `FirmwareUpdate` condition; see [Rollback](firmware-updates.md#rollback). |
