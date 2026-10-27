# Flashing and troubleshooting

<div class="blueprint-card">
  <strong>Flashing scripts</strong>: <code>scripts/flash.sh</code> and <code>scripts/flash_dual_boards.py</code>.<br>
  <strong>Known chip interfaces</strong>: CH343 USB-UART and native ESP32-S3 USB-JTAG.<br>
  <strong>Baud rates</strong>: 460,800 baud on Cardputer, 115,200 baud on DevKit.
</div>

This page documents hardware workarounds discovered while testing Kube32 on physical development boards.

---

## 1. Flashing scripts

The flashing daemon scans for connected boards and flashes them in sequence:

```bash
python3 scripts/flash_dual_boards.py
```

### Flashing boards individually
To flash a specific board:

```bash
# Flash Node 1 (Cardputer via native USB-JTAG)
bash scripts/flash.sh /dev/cu.usbmodem2101 460800

# Flash Node 2 (DevKit via CH343 USB-UART)
bash scripts/flash.sh /dev/cu.usbmodem578E0184461 460800
```

---

## 2. Common hardware issues and fixes

### CH343 and CH340 buffer overruns
**Problem**: Flashing stalls at 1% or fails with `Packet content transfer stopped`.

**Cause**: The CH343 USB-to-UART bridge on many dev boards drops packets when transfer blocks exceed 256 bytes.

**Fix**:
1. Limit RAM block transfer size to 256 bytes:
   ```python
   esptool.loader.ESPLoader.ESP_RAM_BLOCK = 256
   esptool.loader.StubMixin.FLASH_WRITE_SIZE = 256
   ```
2. Add a 1.5 millisecond sleep between write blocks:
   ```python
   def paced_flash_block(self, data, seq, timeout=20.0, encrypted=False):
       res = orig_flash_block(self, data, seq, timeout=20.0, encrypted=encrypted)
       time.sleep(0.0015)
       return res
   ```

### Cardputer power switch and USB enumeration
**Problem**: The Cardputer screen lights up, but macOS does not show `/dev/cu.usbmodem2101`.

**Cause**:
1. The Cardputer has a small battery. When running on battery alone, the native USB controller does not enumerate because it needs VBUS voltage from the cable.
2. The mechanical switch on the left edge controls the power rail. If it is switched down, the USB controller stays unpowered.

**Fix**:
Push the slide switch UP and push the USB-C cable firmly into the recessed socket.

### Native USB reset into download mode
**Problem**: Opening a serial connection resets the board into ROM download mode instead of normal boot:
```
rst:0x15 (USB_UART_CHIP_RESET), boot:0x0 (DOWNLOAD_BOOT)
```

**Fix**:
Set `dtr=False, rts=False` before opening the port in Python:
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

## 3. Boot strap modes

When the ESP32-S3 starts, the ROM prints its boot strap register. Use this table to check the mode:

| Boot value | Mode name | Status |
| :--- | :--- | :--- |
| `boot:0x8` | `SPI_FAST_FLASH_BOOT` | Normal SPI boot. Expected on Node 2. |
| `boot:0x2b` | `SPI_FAST_FLASH_BOOT` | Normal boot with USB-JTAG active. Expected on Node 1. |
| `boot:0x0` | `DOWNLOAD_BOOT` | ROM download mode. Used during flashing. |
| `boot:0x3` | `DOWNLOAD_BOOT` | Strap pin pulled low. Used during flashing. |

If a board remains stuck in `boot:0x0`, toggle the RTS line to trigger a reboot:
```python
s.rts = True
time.sleep(0.1)
s.rts = False
```
