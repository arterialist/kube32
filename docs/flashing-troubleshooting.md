# Flashing and the serial console

## Flashing

Build once, then flash each board in turn:

```
pio run -e esp32s3
bash scripts/flash.sh <serial port> 921600
```

What `scripts/flash.sh` does:

- **Writes three images** with esptool: `bootloader.bin` at `0x0`,
  `partitions.bin` at `0x8000` and `firmware.bin` at `0x20000`, all from
  `.pio/build/esp32s3/`. If they are missing it runs `pio run` first. The
  flash mode is `dio` and the size 8 MB.
- **Adapts to the port.** On a CH340 USB-UART bridge (USB vendor ID
  `0x1a86`) it writes 256-byte blocks with a 1.5 ms pause after each, because
  the bridge drops data otherwise. A native USB-Serial/JTAG port gets
  esptool's defaults; flashing one of the development boards that way took
  about 26 s, against 192 s with the paced path.
- **Resets the board afterwards.** Native USB boards are reset through the
  RTC watchdog (`--after watchdog-reset`); a reset through the USB line could
  leave the chip in download mode. CH340 boards get a hard reset.
- **Honours two environment variables:**
  - `KUBE32_FLASH_BEFORE`: esptool's `--before`, default `default-reset`. Use
    `no-reset` for a board already in download mode.
  - `KUBE32_FLASH_AFTER`: esptool's `--after`.

!!! warning "Fixed PlatformIO path"
    The script runs `pio` and PlatformIO's Python from
    `~/.platformio/penv/bin/`. On another machine, change
    those two paths, or run the same esptool command with the offsets above.

Flash one board at a time. Two esptool runs in parallel made the CH340 one
fail with "No more data to read from the serial port".

## Serial console

The firmware reads commands on its USB serial port (115200 baud; the native
USB port ignores the rate). One command per line, up to 512 characters.
Generic boards also listen on UART0, which is what a devkit's CH340 port is
wired to. The Cardputer profile listens on USB only.

Opening the port resets some boards: the Cardputer when its USB port is
opened, the devkit when its CH340 port is opened. Wait for the boot log to
end before typing.

### Board profile

Each of these commands stores a value in the `k32` NVS namespace, answers
`K32_OK ..., rebooting` and reboots the board:

| Command | Effect |
| --- | --- |
| `K32_CMD:WIFI:<ssid>:<password>` | join this network as a station. The SSID ends at the first `:`, so it may not contain one; the password may. `K32_CMD:WIFI:` with nothing after it erases the credentials |
| `K32_CMD:API:<host>` | the control plane's address, for a worker (default `192.168.4.1`); `auto` erases it |
| `K32_CMD:AP:off` | the control plane does not start its own `Kube32-Cluster` access point; `on` erases the setting |
| `K32_CMD:BOARD:cardputer` | enable the Cardputer's display, keypad, speaker and microSD (`display` is accepted too); `generic` erases it |
| `K32_CMD:ROLE:control-plane` / `worker` | override the PSRAM-based role; `auto` erases it |
| `K32_CMD:REBOOT` | reboot without changing anything |
| `K32_CMD:BOOTLOADER` | reboot into the ROM download mode, for flashing with `KUBE32_FLASH_BEFORE=no-reset` |

Credentials are stored only in the board's NVS. Nothing is written to the
repository or sent anywhere else.

### Diagnostics

| Command | Output |
| --- | --- |
| `K32_CMD:STATUS` | `K32_STATUS node=... role=... mac=... free_heap=... min_free_heap=... free_psram=... uptime_ms=... pods=...` |
| `K32_CMD:SCAN` | up to 20 `K32_AP ssid=... rssi=... ch=... auth=...` lines; the ESP32-S3 sees 2.4 GHz networks only |
| `K32_CMD:TASKS` | one `K32_TASK` line per FreeRTOS task: state, priority, core, free stack (high-water mark), run time |
| `K32_CMD:TASKBT:<task name prefix>` | the first 48 words of a blocked task's saved stack, for `addr2line` |
| `K32_CMD:ACCESSLOG:on` / `off` | log each HTTP request on the API server and the ingress: client, method, path, status, duration, connection disposition |
| `K32_CMD:SD` | microSD mount state, card figures and the files under `/sd/kube32` ([microSD card](sd-card.md)) |
| `K32_CMD:KEY:<text>` | queue keystrokes as if typed on the key matrix; `\n` is Enter, `\b` Backspace, `\t` Tab |
| `K32_CMD:KEYPOP` | take one key from that queue on the host side |
| `K32_CMD:TONE:<count>` | play 1 to 200 short tones from the console task, without a pod |

### HTTP over the console

A host can send an HTTP request through the console. The request must carry
`Connection: close`:

```
K32_HTTP <port> <length> [<crc32 hex>]\n<length bytes of HTTP request>
```

The board replays the request over loopback against its own server on that
port and answers `K32_RESP <length>\n<raw response>`. A worker forwards the
request to the control plane instead. A CRC mismatch answers `K32_ERR crc`.
Requests are limited to 64 KiB, and responses are cut at 256 KiB.

`tools/kube32_serial_gateway.py` wraps this protocol. It listens on local
ports and relays each request to a board over USB; by default local 6443
goes to the board's 6443 and local 8080 to the board's 80:

```
python3 tools/kube32_serial_gateway.py --port <serial port>
kubectl --server http://127.0.0.1:6443 get pods
```

It relays one request at a time and returns a response only once the board
has closed the connection. Watches, `kubectl logs -f` and k9s therefore do
not work through it; use the network for those.

## Troubleshooting

| Symptom | What helped |
| --- | --- |
| esptool reports "No serial data received", or the native USB console reads nothing, while the board keeps running | Unplug and replug the board. Commit `f54968a` fixed one cause (console output racing the USB driver). Repeated open and close cycles with DTR/RTS toggling have also wedged the port. |
| A board stays in download mode after flashing | Flash again with the current `scripts/flash.sh`, which resets native USB boards through the watchdog, or press reset |
| Garbled console output on the devkit's CH340 port | That bridge corrupts received data above 115200 baud; use the native USB port |
| The worker never registers | Check its boot line `[WORKER] kubelet -> http://<host>:6443` and set the address with `K32_CMD:API:` |
| `[CONTROL-PLANE] API server http://192.168.4.1:6443` although the board joined your network | The control plane's own access point is on, and that line shows its address. Store `K32_CMD:AP:off` |
| Keypad events stop at the worker | The boards are on different access points, so ESP-NOW cannot reach across. The UDP transport covers this case once both boards are registered; check that the worker's node object has an `InternalIP` |
| Android Chrome cannot open the board's page while other devices can | On Android 17, give Chrome the "Local network" permission. Without it, the phone drops LAN connections from that app |
| The Cardputer's flash is corrupted after flashing | G0 was pressed during the flash; flash again without touching it |
