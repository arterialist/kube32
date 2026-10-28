# Hardware

Kube32 has run on exactly two boards. Anything else that is an ESP32-S3 with
8 MB of flash may work, but has not been tried.

| Role | Board | PSRAM | Console |
| --- | --- | --- | --- |
| Control plane, `esp32-node-01` | Generic ESP32-S3 devkit | 2 MB, quad SPI | native USB-Serial/JTAG, and UART0 through the devkit's CH340 bridge |
| Worker, `esp32-node-02` | M5Stack Cardputer | none | native USB-Serial/JTAG only (UART0's TX pin, GPIO 43, drives the speaker) |

The role follows from PSRAM. The Cardputer's peripherals are enabled by the
board profile stored with `K32_CMD:BOARD:cardputer`. Neither depends on the
MAC address.

## Firmware configuration

From `platformio.ini`, `sdkconfig.defaults` and `partitions.csv`:

| Item | Setting |
| --- | --- |
| Framework | ESP-IDF 5.5.3 through PlatformIO `espressif32 @ 6.13.0`, board definition `esp32-s3-devkitc-1` |
| CPU | 240 MHz |
| Flash | 8 MB, 80 MHz. `platformio.ini` says QIO; `scripts/flash.sh` writes the image with `--flash-mode dio` |
| PSRAM | quad SPI at 80 MHz, optional (`CONFIG_SPIRAM_IGNORE_NOTFOUND`). Allocations over 4 KB go to PSRAM when present; 32 KB of internal RAM is kept back for DMA and the system. Octal PSRAM would need a different `sdkconfig` and has not been tried |
| Network | lwIP with 32 sockets and 32 active TCP connections; TCP send buffer and window of 11,520 bytes; loopback enabled (the console relay uses it) |
| Watchdogs | task watchdog 5 s (idle task on core 0 checked), interrupt watchdog 300 ms, brownout detector at level 7 |
| C++ | exceptions on (`-fexceptions`); controller loops catch allocation failures instead of rebooting |

### Partition table

| Name | Offset | Size | Use |
| --- | --- | --- | --- |
| `nvs` | `0x9000` | 32 KB | board profile: Wi-Fi credentials, role, board, API address, access point switch |
| `phy_init` | `0x11000` | 4 KB | radio calibration |
| `factory` | `0x20000` | 2.5 MB | the application |
| `storage` | `0x2A0000` | 4 MB | declared, not used: nothing mounts it |
| `reserved` | `0x6A0000` | 1.4 MB | not used |

There is no OTA partition. Updating a board means flashing it over USB.

## Cardputer pins

| Function | GPIO |
| --- | --- |
| ST7789 LCD, 240x135, on SPI2 | MOSI 35, SCLK 36, CS 37, DC 34, RST 33, backlight 38 |
| NS4168 I2S speaker | BCLK 41, DATA 42, LRCK 43 |
| Key matrix | address lines 8, 9, 11; row inputs 13, 15, 3, 4, 5, 6, 7 |
| microSD on SPI3, 20 MHz | MOSI 14, MISO 39, SCK 40, CS 12 |

The display driver draws dirty rectangles through a 4 KB line buffer instead
of keeping a framebuffer. The speaker driver disables its I2S channel between
tones; without that, the DMA ring replayed and the amplifier hummed. The
capability firewall does not yet reserve these pins; see
[Capability firewall](architecture/capability-firewall.md#reserved-pins).

## Memory

The two boards have very different RAM budgets, so the firmware reserves
memory for pods early in boot, before Wi-Fi fragments the heap:

| Board | Reserved at boot |
| --- | --- |
| Without PSRAM | 67 KiB of internal RAM for one 64 KiB pod, a 40 KiB pod thread stack, and 1 KiB log rings per pod |
| With PSRAM | one 1,340 KiB PSRAM block for one large pod (Tessera's core declares 1,280 KiB of linear memory) |

The boot log prints `[MEM]` lines with the free internal RAM and PSRAM after
each stage, and again every 30 s. `K32_CMD:STATUS` prints them on demand.
Figures recorded in commit messages during development:

| Commit | Board | Figure |
| --- | --- | --- |
| `766a7c9` | control plane | internal RAM stayed above 120 KB with the 76 KB Tessera core loaded |
| `f54968a` | control plane | PSRAM free was down to 42 KB with the core pod loaded, before the ingress stacks were cut from 96 KB to 64 KB |
| `f54968a` | Cardputer | mounting the microSD card cost 28 KB of internal RAM before the FatFs buffer fix |

Built from commit `f737ab6`, `firmware.bin` is 2,262,864 bytes, 86.3 % of
the 2.5 MB application partition. Statically allocated RAM is 51,732 bytes,
as reported by `pio run`.
