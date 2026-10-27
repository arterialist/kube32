# Flashing and troubleshooting

<div class="blueprint-card">
  <strong>Flashing utilities</strong>: <code>scripts/flash.sh</code> and PlatformIO / esptool.<br>
  <strong>Supported interfaces</strong>: Native ESP32-S3 USB-CDC/JTAG and external USB-to-UART bridges.<br>
  <strong>Recommended baud rates</strong>: 460,800 baud (high speed) or 115,200 baud (recovery).
</div>

This page documents flashing procedures, serial connection options, and boot recovery for ESP32-S3 boards.

---

## 1. Flashing procedures

### Flashing via the helper script
The repository includes a helper script that flashes the bootloader, partition table, and firmware in a single command:

```bash
# Flash Node 1 (Control plane)
bash scripts/flash.sh /dev/ttyACM0 460800

# Flash Node 2 (Worker node)
bash scripts/flash.sh /dev/ttyACM1 460800
```

!!! tip
    If your workstation runs macOS, replace `/dev/ttyACM*` with `/dev/cu.usbmodem*` or `/dev/cu.usbserial*`. On Windows, use the appropriate `COM*` port.

### Flashing directly via PlatformIO
You can also flash directly using PlatformIO:

```bash
pio run -e esp32s3 -t upload --upload-port /dev/ttyACM0
```

---

## 2. Common troubleshooting steps

### Entering download mode manually
If the flashing tool cannot communicate with the chip:

1. Press and hold the **BOOT** button (connected to GPIO 0).
2. Press and release the **RESET** (or EN) button.
3. Release the **BOOT** button.
4. Run the flash command again.

### High-speed USB transfer pacing
Certain USB-to-UART bridge controllers experience buffer overruns when flashing large binaries at 460,800 baud or higher.

If flashing stalls or reports `Packet content transfer stopped`:

1. Use 256-byte flash block sizing:
   ```python
   esptool.loader.ESPLoader.ESP_RAM_BLOCK = 256
   esptool.loader.StubMixin.FLASH_WRITE_SIZE = 256
   ```
2. Introduce a 1.5 millisecond pacing delay between write blocks:
   ```python
   def paced_flash_block(self, data, seq, timeout=20.0, encrypted=False):
       res = orig_flash_block(self, data, seq, timeout=20.0, encrypted=encrypted)
       time.sleep(0.0015)
       return res
   ```
   The included `scripts/flash.sh` script applies this pacing automatically.

### Preventing unintended resets on serial open
When opening a serial monitor on native USB-CDC or USB-JTAG interfaces, operating system drivers often assert DTR or RTS lines, which can reset the chip into download mode:

```
rst:0x15 (USB_UART_CHIP_RESET), boot:0x0 (DOWNLOAD_BOOT)
```

To connect without triggering a reset, configure `dtr=False, rts=False` before opening the port:

```python
import serial

s = serial.Serial()
s.port = "/dev/ttyACM0"
s.baudrate = 115200
s.dtr = False
s.rts = False
s.open()
```

---

## 3. Boot strap modes

During power-on reset, the ESP32-S3 ROM prints its boot strap register status. Use this table to diagnose the chip state:

| Boot value | Mode name | Status |
| :--- | :--- | :--- |
| `boot:0x8` | `SPI_FAST_FLASH_BOOT` | Normal SPI flash boot. |
| `boot:0x2b` | `SPI_FAST_FLASH_BOOT` | Normal boot with native USB-JTAG enabled. |
| `boot:0x0` | `DOWNLOAD_BOOT` | ROM download mode. Awaiting firmware upload. |
| `boot:0x3` | `DOWNLOAD_BOOT` | GPIO 0 held low during reset. |

If a board remains stuck in `boot:0x0` after flashing, toggle the RTS line or press the physical RESET button to initiate normal execution:

```python
s.rts = True
time.sleep(0.1)
s.rts = False
```
