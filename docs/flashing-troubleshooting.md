# Flashing & Hardware Troubleshooting

<div class="blueprint-card">
  <strong>Flashing Engine</strong>: <code>scripts/flash.sh</code> &amp; <code>scripts/flash_dual_boards.py</code><br>
  <strong>Driver Considerations</strong>: CH343 / CH340 USB-UART FIFO buffering &amp; Native USB-JTAG CDC<br>
  <strong>Baud Rates</strong>: 460,800 baud (Cardputer) / 115,200 baud with paced block transfers (DevKit)
</div>

This document records the empirical hardware solutions established during physical silicon testing of Kube32 across diverse ESP32-S3 boards.

---

## 1. Automated Dual-Board Flashing

The unified flashing daemon automatically discovers connected ESP32-S3 boards and flashes them sequentially with appropriate pacing:

```bash
# Run the automated flashing daemon
python3 scripts/flash_dual_boards.py
```

### Manual Flashing Commands
To flash a specific board individually:

```bash
# Flash Node 1 (Cardputer via native USB-JTAG)
bash scripts/flash.sh /dev/cu.usbmodem2101 460800

# Flash Node 2 (DevKit via CH343 USB-UART)
bash scripts/flash.sh /dev/cu.usbmodem578E0184461 460800
```

---

## 2. Hardware Quirks & Solutions

### A. CH343 / CH340 FIFO Buffer Overrun
**Symptom**: During flash block upload, `esptool` encounters packet checksum errors or hangs at 1% to 15%:
```
A fatal error occurred: Packet content transfer stopped (received 8 bytes)
```
**Root Cause**: The CH343 USB-to-UART bridge on many DevKit boards has a limited hardware FIFO. Large 4KB write blocks overrun the receiver buffer.

**Solution**:
1. Limit RAM block transfer size to 256 bytes:
   ```python
   esptool.loader.ESPLoader.ESP_RAM_BLOCK = 256
   esptool.loader.StubMixin.FLASH_WRITE_SIZE = 256
   ```
2. Inject a 1.5ms pacing delay between successive write packets:
   ```python
   def paced_flash_block(self, data, seq, timeout=20.0, encrypted=False):
       res = orig_flash_block(self, data, seq, timeout=20.0, encrypted=encrypted)
       time.sleep(0.0015)
       return res
   ```

### B. M5Stack Cardputer Power Switch & USB Detection
**Symptom**: The Cardputer screen turns on and operates, but macOS does not show `/dev/cu.usbmodem2101` in `/dev/cu.*` or `ioreg`.

**Root Cause**:
1. The Cardputer has an internal 120mAh LiPo battery. When powered on via battery, the ESP32-S3 boots, but the native USB controller requires VBUS power from the USB-C cable to enumerate.
2. The mechanical slide switch on the left edge controls the power rail. If the switch is DOWN (OFF), the USB controller is not powered even if the battery is supplying the screen.

**Solution**:
Push the slide switch **UP (ON)** and ensure the USB-C cable is fully seated into the recessed connector cutout.

### C. Native USB DTR/RTS Assertion Reset
**Symptom**: Opening the serial port resets the board into ROM download mode instead of normal SPI boot:
```
rst:0x15 (USB_UART_CHIP_RESET), boot:0x0 (DOWNLOAD_BOOT)
```

**Solution**:
Configure `dtr=False, rts=False` before opening the serial port with `pyserial`:
```python
import serial

s = serial.Serial()
s.port = "/dev/cu.usbmodem2101"
s.baudrate = 115200
s.dtr = False
s.rts = False
s.open()
```

---

## 3. ESP32-S3 Boot Strapping Modes

At power-on, the ESP32-S3 ROM prints the boot strap register. Decode values as follows:

| Boot Register | Mode | Meaning | Expected State |
| :--- | :--- | :--- | :--- |
| `boot:0x8` | `SPI_FAST_FLASH_BOOT` | Normal production boot from SPI Flash | **Expected for Node 2** |
| `boot:0x2b` | `SPI_FAST_FLASH_BOOT` | Production boot with USB-JTAG enabled | **Expected for Node 1** |
| `boot:0x0` | `DOWNLOAD_BOOT` | ROM UART/USB flashing mode | Active only during flashing |
| `boot:0x3` | `DOWNLOAD_BOOT` | Strap pin pulled LOW for flashing | Active only during flashing |

If the board remains stuck in `boot:0x0`, trigger a hard hardware reset by toggling RTS:
```python
s.rts = True
time.sleep(0.1)
s.rts = False
```
