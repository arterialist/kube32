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
| `generic` | `kube32.io/board: generic` | 33 GPIO, 10 ADC, 2 I2C, 2 UART | I2C0 on GPIO 8/9, UART1 on GPIO 17/18 |
| `cardputer` | `kube32.io/board: cardputer` | 11 GPIO, 3 ADC, 1 I2C, 1 UART | I2C0 or UART1 on Grove port GPIO 2/1 |

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
| With PSRAM | One 1,284 KiB PSRAM block, 64 KiB shared interpreter stack in PSRAM | Serves one large pod requesting at least half the block, up to 1,280 KiB of linear memory. Allocations over 4 KB and interpreter structures use remaining PSRAM. Serializes all pod calls on the 64 KiB stack |
| Without PSRAM | 67 KiB internal RAM pool block, 40 KiB shared interpreter stack, 16 KiB system heap reserve, 1 KiB log ring per pod | Serves all pod allocations from the 67 KiB pool block first, falling back to the general heap when full. The kubelet admits a pod only when its estimated footprint fits with the reserve kept. Serializes all pod calls on the 40 KiB stack (`app_main` pods require PSRAM). The kubelet refuses API responses over 16 KB |

Check free internal RAM and PSRAM on the serial console with `K32_CMD:STATUS`.

## Partition table

Layout in `partitions.csv` for 8 MB flash:

| Name | Offset | Size | Use |
| --- | --- | --- | --- |
| `nvs` | `0x9000` | 32 KB | Node configuration, Wi-Fi, role, board profile, name, API address, token, TLS credentials |
| `phy_init` | `0x11000` | 4 KB | Radio calibration |
| `factory` | `0x20000` | 3 MB | Application image |
| `storage` | `0x320000` | 3.5 MB | FAT filesystem with wear levelling at `/data`, used when no microSD card is mounted |
| `journal` | `0x6A0000` | 1 MB | Raw object store journal (`data`, subtype `0x40`), used when no microSD card is mounted |
| `reserved` | `0x7A0000` | 384 KB | Unused |

## Configuration notes

Settings in `platformio.ini` and `sdkconfig.defaults`:

- Target CPU clock is 240 MHz. Flash runs at 80 MHz, written with `--flash-mode dio` by `scripts/flash.sh`.
- PSRAM is configured for 80 MHz quad SPI via `CONFIG_SPIRAM_MODE_QUAD=y` and `CONFIG_SPIRAM_IGNORE_NOTFOUND=y` so the same image boots on boards without PSRAM.

## Limitations

- Boards with octal PSRAM require `CONFIG_SPIRAM_MODE_OCT=y` in `sdkconfig.defaults`.
- There is no OTA partition. Flash firmware updates over USB.
- Pods cannot claim strapping, USB, flash/PSRAM, or board profile peripheral pins. See [Capability firewall](architecture/capability-firewall.md#reserved-pins).
