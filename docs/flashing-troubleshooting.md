# Flashing and the serial console

## Flashing

Build once, then flash one board at a time:

```
pio run -e esp32s3
bash scripts/flash.sh <serial port> 921600
```

`scripts/flash.sh <serial port> [baud]` defaults to `460800` baud and reads `KUBE32_SERIAL_PORT` when you omit the port argument. The script locates `pio` on `PATH`, then in `$PLATFORMIO_CORE_DIR/penv/bin` or `~/.platformio/penv/bin`. It runs `esptool` from PlatformIO's virtualenv or from a system `python3` with `esptool` installed.

- Writes `bootloader.bin` at `0x0`, `partitions.bin` at `0x8000`, and `firmware.bin` at `0x20000` from `.pio/build/esp32s3/` in `dio` mode with an 8 MB flash size. Runs `pio run` if the binaries are missing.
- Writes 256-byte blocks with a 1.5 ms pause on CH340 USB-UART bridges, vendor ID `0x1a86`. Uses `esptool` defaults on native USB-Serial/JTAG ports.
- Resets native USB boards through the RTC watchdog with `--after watchdog-reset`, and resets CH340 boards with `--after hard-reset`.
- Reads `KUBE32_FLASH_BEFORE` for `--before`, defaulting to `default-reset`, and `KUBE32_FLASH_AFTER` for `--after`.

To enter ROM download mode from the serial console when USB auto-reset fails, send `K32_CMD:BOOTLOADER`, then flash with `KUBE32_FLASH_BEFORE=no-reset`.

## Serial console

The firmware listens for newline-terminated commands up to 512 characters on USB serial at 115200 baud. Generic boards listen on both native USB-Serial/JTAG and UART0, answering on the port that received the command. The Cardputer profile listens on USB only.

Opening the USB port on a Cardputer or the CH340 port on a devkit resets the board. Wait for the boot log to finish before sending commands.

### Board profile

Profile commands write to the `k32` NVS namespace, return `K32_OK ..., rebooting`, and reboot the board:

| Command | Effect |
| --- | --- |
| `K32_CMD:WIFI:<ssid>:<password>` | Join a 2.4 GHz network as a station. The station picks the strongest access point with that name and roams to one at least 10 dB stronger when signal drops below -70 dBm. The SSID cannot contain `:`. `K32_CMD:WIFI:` erases credentials. |
| `K32_CMD:API:<host>` | Set the control plane address for a worker, default `192.168.4.1`. `auto` erases it. |
| `K32_CMD:AP:off` | Disable the `Kube32-Cluster` access point on the control plane. `on` erases the setting. |
| `K32_CMD:BOARD:cardputer` | Enable the Cardputer display, keypad, speaker, and microSD. `display` is an alias. `generic` erases it. |
| `K32_CMD:ROLE:control-plane` / `worker` | Override the PSRAM-based role. `auto` erases it. |
| `K32_CMD:NAME:<name>` | Set the node name to a DNS-1123 label, up to 63 characters of `a-z`, `0-9`, and `-`. `auto` erases it and falls back to `esp32-<mac>`. |
| `K32_CMD:TOKEN:<token>` | Set the API server and kubelet bearer token, 16 to 128 printable characters without spaces. `off` disables authentication. |
| `K32_CMD:REBOOT` | Reboot without changing settings. |
| `K32_CMD:BOOTLOADER` | Reboot into ROM download mode for flashing with `KUBE32_FLASH_BEFORE=no-reset`. |

### Diagnostics

| Command | Output |
| --- | --- |
| `K32_CMD:STATUS` | `K32_STATUS node=... role=... mac=... free_heap=... min_free_heap=... free_psram=... uptime_ms=... pods=... storage=... auth=... tls=...` |
| `K32_CMD:STORE` | `K32_STORE journal=... keys=... live_bytes=... capacity_bytes=... sectors=.../... pending=... records=... bytes=... moved=... unchanged=... errors=... revision=...` on the control plane, or `K32_STORE journal=none`. |
| `K32_CMD:CERT` | The API server's self-signed TLS certificate in PEM format as `K32_CERT <line>` lines followed by `K32_OK cert`. |
| `K32_CMD:SCAN` | Up to 20 `K32_AP ssid="..." rssi=... ch=... auth=...` lines for 2.4 GHz networks. |
| `K32_CMD:TASKS` | One `K32_TASK` line per FreeRTOS task with state, priority, core, free stack high-water mark, and run time. |
| `K32_CMD:TASKBT:<task name prefix>` | First 48 words of a blocked task's saved stack for `addr2line`. Exact name matches take precedence over prefix matches. |
| `K32_CMD:BTALL` | Every task's backtrace for `addr2line`. |
| `K32_CMD:HEAP` | `K32_HEAP used\|free <=<size> <blocks> <bytes>` lines: internal heap blocks grouped by size. |
| `K32_CMD:POOL` | `K32_POOL <offset> <size> used\|free` lines list the chunks of the pod pool block's small heap in address order. |
| `K32_CMD:TCP` | One `K32_TCP` line per lwIP TCP connection with its state, local port, peer, and bytes unsent, unacknowledged, unread by the application, or out of order. |
| `K32_CMD:ACCESSLOG:on` / `off` | Log HTTP requests on the API server and ingress. |
| `K32_CMD:SD` | microSD mount state, capacity, and files under `/sd/kube32`. See [storage](storage.md). |
| `K32_CMD:KEY:<text>` | Queue keystrokes `\n`, `\b`, `\t`, and characters. |
| `K32_CMD:KEYPOP` | Pop one key from the queue. |
| `K32_CMD:TONE:<count>` | Play 1 to 200 short tones. |

### HTTP over the console

`tools/kube32_serial_gateway.py` carries HTTP over the console in streams (`include/app/console_relay.hpp`). Host lines end with `\n` only. Board frames start with the byte `0x1e` and the board writes each frame as a single unit. Log output can surround a frame, but it never splits one. CRCs use CRC-32 (zlib) in hexadecimal.

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

Opening a native USB-Serial/JTAG or CH340 port with RTS high and DTR low resets the board. Clear RTS before DTR, as `tools/kube32_serial_gateway.py` does.

## Troubleshooting

| Symptom | Resolution |
| --- | --- |
| `esptool` reports "No serial data received" or the native USB console stops answering | Unplug and replug the board, or switch to the devkit's second USB port, the UART0 CH340 bridge, for console commands and flashing. Avoid repeated port open and close cycles with DTR/RTS toggling. |
| Board stays in download mode after flashing | Press the reset button or flash with `scripts/flash.sh`. |
| Garbled console output on the CH340 port | Set the baud rate to 115200 or use the native USB port. |
| Worker never registers | Check `[WORKER] kubelet -> http://<host>:6443` in the boot log and set the control plane address with `K32_CMD:API:<host>`. |
| Control plane logs `http://192.168.4.1:6443` while joined to a station network | Disable the softAP with `K32_CMD:AP:off`. |
| Keypad events do not reach the control plane across different access points | Verify the worker node object has an `InternalIP` for UDP transport. |
| Android Chrome cannot reach the board on LAN | Enable the Local network permission for Chrome on Android 17. |
| Cardputer flash is corrupted after flashing | Reflash without pressing G0. |
