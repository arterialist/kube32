# Hardware

Kube32 runs on ESP32-S3 microcontrollers with 8 MB of flash. One firmware image boots both cluster roles:

| Role | Reference board | PSRAM | Console |
| --- | --- | --- | --- |
| Control plane | Generic ESP32-S3 devkit | 2 MB, quad SPI | Native USB-Serial/JTAG, and UART0 on GPIO 43/44 |
| Worker | M5Stack Cardputer | None | Native USB-Serial/JTAG only. GPIO 43 drives the speaker |

A board with PSRAM defaults to the control plane. A board without PSRAM defaults to a worker. Override the role in NVS with `K32_CMD:ROLE:control-plane|worker|auto`.

## Board profiles and labels

Select the peripheral profile with `K32_CMD:BOARD:cardputer|atoms3-lite|generic`:

| Profile | Labels | Allocatable peripherals | Default bus pins |
| --- | --- | --- | --- |
| `generic` | `kube32.io/board: generic` | 33 GPIO, 10 ADC, 2 I2C, 2 UART, chip temperature | I2C0 on GPIO 8/9, UART1 on GPIO 17/18 |
| `cardputer` | `kube32.io/board: cardputer` | 11 GPIO, 3 ADC, 1 I2C, 1 UART, chip temperature | I2C0 or UART1 on Grove port GPIO 2/1 |
| `atoms3-lite` | `kube32.io/board: atoms3-lite` | 9 GPIO, 6 ADC, 1 I2C, 1 UART, chip temperature | I2C0 or UART1 on Grove port GPIO 2/1; RMT TX on GPIO 4 (IR LED) |

Control plane nodes also carry the `node-role.kubernetes.io/control-plane` label, and worker nodes carry `node-role.kubernetes.io/worker`.

## Cardputer pins

| Function | GPIO |
| --- | --- |
| ST7789 LCD, 240x135, on SPI2 | MOSI 35, SCLK 36, CS 37, DC 34, RST 33, backlight 38 |
| NS4168 I2S speaker | BCLK 41, DATA 42, LRCK 43 |
| Key matrix | address lines 8, 9, 11; row inputs 13, 15, 3, 4, 5, 6, 7 |
| microSD on SPI3, 20 MHz | MOSI 14, MISO 39, SCK 40, CS 12 |

## AtomS3 Lite pins

The M5Stack AtomS3 Lite is an ESP32-S3 with 8 MB of flash and no PSRAM, so it joins as a worker. It has no screen and no microSD slot: node storage is the internal flash partition. The `atoms3-lite` profile offers pods only the pins a user can reach, plus the button:

| Function | GPIO |
| --- | --- |
| Grove port (HY2.0-4P) | yellow 2, white 1 |
| Header | 5, 6, 7, 8, 38, 39 |
| Button, low when pressed | 41 (`button` driver: `pull: up`, `active: low`) |
| IR LED, the default RMT TX pin | 4 |
| WS2812 RGB LED, reserved | 35 |

```sh
# on the Atom's console
K32_CMD:BOARD:atoms3-lite
```

## Memory budget

The firmware reserves contiguous pod memory at boot before Wi-Fi initializes:

| Board type | Boot reservation | Pod allocation |
| --- | --- | --- |
| With PSRAM | One 1,284 KiB PSRAM block, 64 KiB shared interpreter stack in PSRAM | Serves a large linear memory of at least half the block from its start, up to 1,280 KiB, and every other pod allocation from its end down. When the block is full, pods take PSRAM above the system reserve, never internal RAM. The kubelet admits a pod against the block's free part plus that PSRAM. Ticks and events run on two pod workers, one per core (the core 0 worker on its own 68 KiB stack, taken from the block: allocatable memory and the largest linear memory are 68 KiB smaller); other pod calls run one at a time on the 64 KiB stack |
| Without PSRAM | 80 KiB internal RAM pool block holding the 28 KiB shared interpreter stack, 24 KiB system heap reserve, 1 KiB log ring per pod | Serves all pod allocations from the pool block first, falling back to the general heap when full. The kubelet admits a pod only when its estimated footprint fits in the pool block. Serializes all pod calls on the 28 KiB stack (`app_main` pods require PSRAM). The kubelet refuses API responses over 16 KB |

Without PSRAM, network buffers come from internal RAM. The Wi-Fi station uses a 576-byte MTU, ~650 bytes per frame, with 12 dynamic RX and 12 dynamic TX buffers. Each socket holds up to 4 unread and 2 out-of-order segments. The pod HTTP server queues one connection, keeps two idle, and returns 503 when free internal heap is under 10 KB.

A pod's module loads only into pieces of the pool block left free by earlier pods. When a module fails to load twice for memory, the kubelet stops pods with a smaller memory limit once so the module loads first, then restarts them; a pod never stops for a smaller one.

Check free internal RAM and PSRAM on the serial console with `K32_CMD:STATUS`.

## Node and pod memory figures

| Figure | Board with PSRAM | Board without PSRAM |
| --- | --- | --- |
| `capacity` `memory` | Internal heap plus PSRAM | Internal heap |
| `capacity` `esp32.io/psram` | PSRAM | not reported |
| `allocatable` `memory` | The pool block plus the PSRAM that was free above the system-reserved PSRAM when the kubelet first reported | The pool block |
| `allocatable` `esp32.io/psram` | Same as `allocatable` `memory` | not reported |
| `kube32.io/memory-free` annotation | Free part of the pool block plus PSRAM above the system-reserved PSRAM | Free part of the pool block |
| `kube32.io/system-reserved` annotation | The system-reserved PSRAM | The system reserve of internal RAM |
| `kube32.io/heap-free` annotation | Free internal RAM | Free internal RAM |
| `kube32.io/psram-free` annotation | Free PSRAM | not reported |

The system-reserved PSRAM is what the node keeps for its own allocations and never offers to pods: the object store, the JSON of list and watch responses, the controllers' working sets, the console relay and firmware staging. `capacity` less `allocatable` covers it, as `system-reserved` and `kube-reserved` do in Kubernetes. A control plane keeps 192 KiB and a worker with PSRAM keeps 64 KiB. `K32_CMD:SYSRESERVE:<size>` sets another figure from the next boot on (16 KiB to 1 MiB), and `kube32-sim --system-reserved` sets it for the modelled heap. `K32_CMD:PODCORES:<1|2>` sets the number of [pod workers](architecture/wasm-runtime.md#pod-workers) from the next boot on (2 by default on a board with PSRAM).

The 192 KiB come from the control plane's peak use of PSRAM above its boot level. A list copies every object it returns and the controllers each hold a working set of that size, so the peak grows with the number of pods: it measured 109 KiB with a few pods and passed 122 KiB with the node full. 192 KiB leave half again as much as that peak, and a node that needs more raises the figure. When PSRAM runs out anyway, a controller pass or an API request that cannot allocate ends with a log line (`pass skipped: std::bad_alloc`, or a `503` Status for the request) and the next pass runs a second later.

A control plane also sets 64 KiB of PSRAM aside at boot for its large allocations. Its heap holds thousands of small objects that live for minutes (stored objects, buffers, task stacks), and once they are spread over PSRAM the free memory lies in pieces of a few KiB, while a list, a copy of a large object or a controller's working set needs one block of 4 to 60 KiB. Allocations over 4 KiB go to the region first and to the heap when it has no room. Only large blocks share the region, and most of them live for one request or one pass, so it stays in large pieces. The API server's workers allocate their request scratch at start, so a request whose objects are under 4 KiB needs no larger block. Allocatable `memory` leaves the region out. The `[MEM]` console lines report the largest free PSRAM block and, for the region, its free bytes, its largest free block and the allocations it had no room for.

Allocatable figures stay the same while pods start and stop. The scheduler subtracts what bound pods request, and the kubelet admits each pod against what is free now. The kubelet writes the annotations with each heartbeat, in bytes. `MemoryPressure` compares free internal RAM with 24 KiB.

```bash
kubectl get node <node> -o jsonpath='{.status.allocatable.memory} {.metadata.annotations.kube32\.io/memory-free}'
```

`kubectl top pod` reports every byte that a pod's runtime holds: wasm3's structures, compiled code, value stack and linear memory as allocated, allocator headers included, the module's bytes when the image is not embedded, and the stack of its thread when it runs `app_main`. The runtime charges each allocation to the pod it is made for and credits it back when it is freed, on whichever thread frees it. An allocation in the pool block keeps its owner in the chunk header that it has anyway. An allocation in the general heap or PSRAM carries an 8-byte header. `kubectl top node` reports internal RAM and PSRAM in use, less the pool block's free part, on the base of `capacity` `memory`.

## Partition table

Layout in `partitions.csv` for 8 MB flash:

| Name | Offset | Size | Use |
| --- | --- | --- | --- |
| `nvs` | `0x9000` | 32 KB | Node configuration, Wi-Fi, role, board profile, name, API address, token, TLS credentials |
| `otadata` | `0x11000` | 8 KB | Selects the active boot slot and tracks the confirmation state of an updated image |
| `phy_init` | `0x13000` | 4 KB | Radio calibration |
| `ota_0` | `0x20000` | 3.25 MB | App slot |
| `ota_1` | `0x360000` | 3.25 MB | App slot: a [firmware update](firmware-updates.md) writes to the inactive slot |
| `journal` | `0x6A0000` | 1 MB | Raw object store journal (`data`, subtype `0x40`), used when no microSD card is mounted |
| `storage` | `0x7A0000` | 384 KB | FAT filesystem with wear levelling at `/data`, used when no microSD card is mounted |

## Other targets

The firmware also builds with ESP-IDF 6.1 for the ESP32-C3 (`sdkconfig.defaults.esp32c3`, 4 MB flash, one app slot, no firmware rollouts) and the ESP32-P4 (`sdkconfig.defaults.esp32p4`), with `idf.py -DIDF_TARGET=<target>` as in [CONTRIBUTING.md](https://github.com/arterialist/kube32/blob/main/CONTRIBUTING.md#firmware). Neither image has run on a board yet. An ESP32-C3 image built with `sdkconfig.defaults.qemu` boots in Espressif's QEMU (`-nic user,model=open_eth`), joins a control plane over the emulated Ethernet and runs pods. The ESP32-P4 has no radio, so a P4 node has no network in this firmware yet.

## Linux boards

On a Linux single-board computer, `kube32-sim` runs a node that gives pods the board's own devices through the same pod API as a Cardputer. `--board` names a preset (`cardputer-zero`), and [flags](cli-reference.md#linux-boards) describe any other board. A device that does not open gets one log line, and the Node does not advertise it.

| Pods use | The node reaches |
| --- | --- |
| `hardware.kube32.io/tft-display`: `k32_display_*` | The framebuffer, mapped, at 16 bits a pixel (RGB565) or 32 bits (XRGB8888 and the other channel layouts the driver reports). `k32_display_info` answers the panel's size, and every call clips to it. Text uses the Cardputer's 6x8 font. |
| `hardware.kube32.io/keyboard`: `k32_key_read` | The evdev keyboard, grabbed so that key presses reach only the pods. A press returns the code a Cardputer returns: ASCII, `\n` for Enter, `\b` for Backspace and Delete, `\t` for Tab, `27` for Esc, and `U`, `D`, `L`, `R` for the arrows. Shift and Caps Lock work as on a US keyboard. A held key returns its code once, and function and media keys return none. |
| `esp32.io/i2c-bus-<n>`, `kube32.io/i2c-device`, discovery | `/dev/i2c-N` with `I2C_RDWR`. The kernel sets the bus's pins and speed, so a claim takes no pins, and a `kube32.io/i2c-bus-<n>` annotation is refused. An address whose `/sys/bus/i2c/devices/<N>-00<hh>/driver` exists belongs to a kernel driver: a pod that holds the bus finds no device there (`K32_ERR_NODEV`), a device grant that names it is refused, and discovery skips it. |
| `hardware.kube32.io/chip-temp`: `esp32_chip_temp_read` | The thermal zone's `temp`, the SoC's temperature. |

The node offers no GPIO, ADC, UART or RMT, because the kernel owns the SoC's pins, and no speaker or microphone. Its Node has no `kube32.io/simulator` label, so the Device controller places drivers on its buses as on a board's. The board serves every node of the process: run it as a worker, or standalone with `--workers ""`.

The node keeps the panel and the keyboard from its start and shows the ready screen, the node's address and the cluster's app URL, until a pod draws. On a board with a screen owner (`--screen-owner`, a launcher's systemd user unit), they stay with the launcher until a pod draws or reads a key. The node then stops the launcher, clears the panel and grabs the keyboard. Stopping the launcher also frees what else it holds: on the Cardputer Zero, the audio server keeps the speaker busy while the launcher runs. Once no pod holds the display, keyboard or speaker grant, the node lets go of them within a second and starts the launcher again.

### Cardputer Zero

The M5Stack Cardputer Zero (Raspberry Pi CM0, 320x170 ST7789 panel, TCA8418 keyboard, Debian) runs a node with `--board cardputer-zero`:

| Device | Path |
| --- | --- |
| Panel, 320x170 RGB565 | `/dev/fb_lcd`, the board's udev name for the ST7789 framebuffer |
| Keyboard | `/dev/input/cardputer-zero-internal`, keymap `cardputer-zero` |
| Backlight | `/sys/class/backlight/backlight`, turned on when the node takes the panel and finds it off |
| Pod I2C bus 0 | `/dev/i2c-1`. The board's own parts on it belong to kernel drivers: codec `0x10`, RTC `0x32`, keyboard `0x34`, IO expander `0x4f`, fuel gauge `0x55` and IMU `0x68` |
| Chip temperature | `/sys/class/thermal/thermal_zone0` |
| Screen owner | `APPLaunch.service`, the board's launcher |

The keyboard's Sym layer sends key codes that the board's console keymap turns into symbols, and the `cardputer-zero` keymap returns those symbols: Sym+Q is `~`, Sym+1 is `!`. Fn+F, Fn+Z, Fn+X and Fn+C are the arrows.

A pod that draws for the Cardputer's 240x135 screen without asking `k32_display_info` draws in the panel's top left corner, on black.

Install the node as a systemd user unit. Copy a `linux-arm64` `kube32-sim` (a [release](releases.md) archive, or `docker build --platform linux/arm64 --target sim-binary --output type=local,dest=dist .` on another machine) to the board, then on the board:

```sh
mkdir -p ~/.local/bin ~/.config/kube32 ~/.config/systemd/user
cp kube32-sim ~/.local/bin/
cp packaging/cardputer-zero/kube32-node.service ~/.config/systemd/user/
cat > ~/.config/kube32/node.env <<'EOF'
KUBE32_JOIN=http://<control plane address>:6443
KUBE32_NODE_NAME=cardputer-zero
EOF
systemctl --user daemon-reload
systemctl --user enable --now kube32-node
journalctl --user -u kube32-node -f
```

The unit starts with the user's systemd instance, which the board's image starts at boot (`loginctl enable-linger`). The node keeps its storage in `~/.local/share/kube32`. When the node stops, the unit starts the launcher again in case the pods had the screen.

## Console over USB OTG

On a board whose USB port is wired to the USB OTG peripheral rather than USB-Serial/JTAG, the node's console runs as a CDC-ACM port. Build with `CONFIG_KUBE32_CONSOLE_USB_OTG` (`sdkconfig.defaults.usb-otg`: `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.usb-otg"`). `K32_CMD:*` lines, the log stream and `tools/kube32_serial_gateway.py` run over it without a USB-serial adapter. It is off by default: the default console stays USB-Serial/JTAG.

- The port is TinyUSB CDC-ACM (Espressif VID 303A, product string `Kube32 Console`), full speed on the ESP32-S3 and high speed where the chip's controller is. It has no reset wiring: DTR and RTS only tell the node a host opened the port, so `esptool --before default-reset` cannot reach it.
- Boot and panic output stays on UART0. Output after `app_main` starts also goes to a 4 KiB ring (`CONFIG_KUBE32_CONSOLE_USB_OTG_RING_BYTES`). The first host to open the port gets it, oldest first, after a line saying how many bytes were lost when the ring overflowed. The second-stage bootloader's lines, output printed before `app_main`, and panic dumps go to UART0 only.
- Nothing waits for a host: output with nobody reading stays in the ring, the oldest whole lines dropping first; a slow host loses new log lines, but frames (the gateway's data) wait up to their timeout and are never split.
- `K32_CMD:BOOTLOADER` detaches from USB and resets into the ROM's download mode. Run esptool on the ROM's own port with `--before no-reset`: on the ESP32-S3 that is the USB-Serial/JTAG port, which comes back in place of the CDC port.
- A boot after a crash stays on USB-Serial/JTAG on chips where OTG and USB-Serial/JTAG share the USB PHY, so a crash loop cannot take the port away. `CONFIG_KUBE32_CONSOLE_USB_OTG_GUARD_SECONDS` restarts the node on USB-Serial/JTAG when no console line arrives over OTG for that long after boot, for bringing up a board with no other way back.
- The port takes 7 KB of internal RAM statically and 7 to 8 KB when it starts (measured on an ESP32-S3: tasks, the ring and TinyUSB's buffers), which is tight on a board without PSRAM.

## Configuration notes

Settings in `platformio.ini` and `sdkconfig.defaults`:

- Target CPU clock is 240 MHz. Flash runs at 80 MHz, written with `--flash-mode dio` by `scripts/flash.sh`.
- PSRAM is configured for 80 MHz quad SPI via `CONFIG_SPIRAM_MODE_QUAD=y` and `CONFIG_SPIRAM_IGNORE_NOTFOUND=y` so the same image boots on boards without PSRAM.

## Limitations

- Boards with octal PSRAM require `CONFIG_SPIRAM_MODE_OCT=y` in `sdkconfig.defaults`.
- Pods cannot claim strapping, USB, flash/PSRAM, or board profile peripheral pins. See [Capability firewall](architecture/capability-firewall.md#reserved-pins).
