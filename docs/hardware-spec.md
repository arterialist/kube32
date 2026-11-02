# Hardware

Kube32 runs on ESP32-S3 microcontrollers with 8 MB of flash. One firmware image boots both cluster roles:

| Role | Reference board | PSRAM | Console |
| --- | --- | --- | --- |
| Control plane | Generic ESP32-S3 devkit | 2 MB, quad SPI | Native USB-Serial/JTAG, and UART0 on GPIO 43/44 |
| Worker | M5Stack Cardputer | None | Native USB-Serial/JTAG only. GPIO 43 drives the speaker |

A board with PSRAM defaults to the control plane. A board without PSRAM defaults to a worker. Override the role in NVS with `K32_CMD:ROLE:control-plane|worker|auto`.

## Board profiles and labels

Select the peripheral profile with `K32_CMD:BOARD:cardputer|generic`:

| Profile | Labels | Allocatable peripherals | Default bus pins |
| --- | --- | --- | --- |
| `generic` | `kube32.io/board: generic` | 33 GPIO, 10 ADC, 2 I2C, 2 UART, chip temperature | I2C0 on GPIO 8/9, UART1 on GPIO 17/18 |
| `cardputer` | `kube32.io/board: cardputer` | 11 GPIO, 3 ADC, 1 I2C, 1 UART, chip temperature | I2C0 or UART1 on Grove port GPIO 2/1 |

Control plane nodes also carry the `node-role.kubernetes.io/control-plane` label, and worker nodes carry `node-role.kubernetes.io/worker`.

## Cardputer pins

| Function | GPIO |
| --- | --- |
| ST7789 LCD, 240x135, on SPI2 | MOSI 35, SCLK 36, CS 37, DC 34, RST 33, backlight 38 |
| NS4168 I2S speaker | BCLK 41, DATA 42, LRCK 43 |
| Key matrix | address lines 8, 9, 11; row inputs 13, 15, 3, 4, 5, 6, 7 |
| microSD on SPI3, 20 MHz | MOSI 14, MISO 39, SCK 40, CS 12 |

## Memory budget

The firmware reserves contiguous pod memory at boot before Wi-Fi initializes:

| Board type | Boot reservation | Pod allocation |
| --- | --- | --- |
| With PSRAM | One 1,284 KiB PSRAM block, 64 KiB shared interpreter stack in PSRAM | Serves a large linear memory of at least half the block from its start, up to 1,280 KiB, and every other pod allocation from its end down. When the block is full, pods take PSRAM above the system reserve, never internal RAM. The kubelet admits a pod against the block's free part plus that PSRAM. Serializes all pod calls on the 64 KiB stack |
| Without PSRAM | 80 KiB internal RAM pool block holding the 28 KiB shared interpreter stack, 24 KiB system heap reserve, 1 KiB log ring per pod | Serves all pod allocations from the pool block first, falling back to the general heap when full. The kubelet admits a pod only when its estimated footprint fits in the pool block. Serializes all pod calls on the 28 KiB stack (`app_main` pods require PSRAM). The kubelet refuses API responses over 16 KB |

Without PSRAM, network buffers come from internal RAM. The Wi-Fi station uses a 576-byte MTU, ~650 bytes per frame, with 12 dynamic RX and 12 dynamic TX buffers. Each socket holds up to 4 unread and 2 out-of-order segments. The pod HTTP server queues one connection, keeps two idle, and returns 503 when free internal heap is under 10 KB.

A pod's module loads only into pieces of the pool block left free by earlier pods. When a module fails to load twice for memory, the kubelet stops pods with a smaller memory limit once so the module loads first, then restarts them; a pod never stops for a smaller one.

Check free internal RAM and PSRAM on the serial console with `K32_CMD:STATUS`.

## Node and pod memory figures

| Figure | Board with PSRAM | Board without PSRAM |
| --- | --- | --- |
| `capacity` `memory` | Internal heap plus PSRAM | Internal heap |
| `capacity` `esp32.io/psram` | PSRAM | not reported |
| `allocatable` `memory` | The pool block plus the PSRAM that was free above the system reserve when the kubelet first reported | The pool block |
| `allocatable` `esp32.io/psram` | Same as `allocatable` `memory` | not reported |
| `kube32.io/memory-free` annotation | Free part of the pool block plus PSRAM above the system reserve | Free part of the pool block |
| `kube32.io/heap-free` annotation | Free internal RAM | Free internal RAM |
| `kube32.io/psram-free` annotation | Free PSRAM | not reported |

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

## Configuration notes

Settings in `platformio.ini` and `sdkconfig.defaults`:

- Target CPU clock is 240 MHz. Flash runs at 80 MHz, written with `--flash-mode dio` by `scripts/flash.sh`.
- PSRAM is configured for 80 MHz quad SPI via `CONFIG_SPIRAM_MODE_QUAD=y` and `CONFIG_SPIRAM_IGNORE_NOTFOUND=y` so the same image boots on boards without PSRAM.

## Limitations

- Boards with octal PSRAM require `CONFIG_SPIRAM_MODE_OCT=y` in `sdkconfig.defaults`.
- Pods cannot claim strapping, USB, flash/PSRAM, or board profile peripheral pins. See [Capability firewall](architecture/capability-firewall.md#reserved-pins).
