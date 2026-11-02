# Install from the browser

The installer on this page writes the latest kube32 release to an ESP32-S3 board over USB and then sets up the board's Wi-Fi. It needs no toolchain, only a desktop browser with Web Serial, such as Google Chrome or Microsoft Edge.

<p>
<esp-web-install-button manifest="../firmware/latest/manifest.json">
  <button slot="activate" class="md-button md-button--primary">Connect and install</button>
  <span slot="unsupported">This browser has no Web Serial. Open the page in Google Chrome or Microsoft Edge on a desktop computer.</span>
  <span slot="not-allowed">The installer needs this page over HTTPS.</span>
</esp-web-install-button>
</p>
<p data-firmware-missing hidden>No release is published yet. Build and flash the firmware with PlatformIO as <a href="../flashing-troubleshooting/">Flashing and the serial console</a> describes.</p>

The installer offers kube32 <span data-firmware-version>(latest release)</span>.

## Install

1. Connect the board with a USB data cable. A devkit installs through either of its ports: the native USB port or the USB-UART bridge (CH340).
2. Press **Connect and install** and pick the board's serial port.
3. Choose **Install kube32** and confirm.
4. When the install finishes, set up Wi-Fi as the next section describes.

The installer is for a first install. It writes one image at offset `0x0`: the bootloader, the partition table, an empty `otadata` partition, and the firmware in the first app slot, the layout `scripts/flash.sh` writes. The image runs on to the end of the firmware and fills the gaps between those parts with `0xff`, so it erases the NVS partition at `0x9000` to `0x10fff`: writing it erases the stored settings, and the board starts without Wi-Fi credentials, node name, token or board profile. The object store journal and node storage lie past the end of the image and keep their contents. On a board that runs other firmware, the installer offers to erase the whole flash first.

## Wi-Fi

After the install, the board starts and the installer offers **Connect to Wi-Fi**. It lists the 2.4 GHz networks the board sees and sends the chosen network's name and password to the board over the serial port, using the Improv Wi-Fi serial protocol.

The board joins the network next to its cluster access point. Once it holds an address, it stores the credentials, and the installer shows the control plane's API server, `http://<control-plane-ip>:6443`, under **Visit device**. A few seconds later the board restarts on that network, as after `K32_CMD:WIFI`. A board that cannot join within 20 seconds answers **Unable to connect** and stays on the network it had.

A board on its native USB port leaves the USB bus for a moment when it starts, and the browser loses the port:

- If the installer does not offer Wi-Fi after the install, close it, press **Connect and install** again, pick the port, and choose **Connect to Wi-Fi**.
- After the Wi-Fi setup, the installer reports that the device disconnected when the board restarts. The board is on the network at the address shown.

The devkit's CH340 port stays open through restarts.

## Board settings

The installer sets up Wi-Fi only. Set the board profile and a worker's control plane address over the serial console, in the installer's **Logs & console** or a terminal at 115200 baud:

```text
K32_CMD:BOARD:cardputer          # Cardputer worker
K32_CMD:API:<control-plane-ip>   # worker
K32_CMD:AP:off                   # control plane
```

`K32_CMD:WIFI:<ssid>:<password>` sets Wi-Fi from the console. [Flashing and the serial console](flashing-troubleshooting.md#serial-console) lists every command, and the [quick start](quickstart.md#2-provision-over-the-serial-console) walks through a two-board cluster.

## Download mode

The installer resets the board into its ROM download mode over USB. When it gets no answer from the board:

- Send `K32_CMD:BOOTLOADER` from **Logs & console** or a terminal, then install.
- On a devkit, hold BOOT, press and release RESET, release BOOT, then install.
- When the native USB port stops answering, unplug and replug the board, or install through the devkit's CH340 port.
- When a Cardputer's flash is corrupted after an install, install again without pressing G0.

A board that stays in download mode after the install starts the firmware when you press RESET.

## Updates

Updates keep the stored settings:

- Over the network, a [firmware rollout](firmware-updates.md) installs the release's `kube32-firmware-esp32s3-<version>-app.bin` on every node in turn.
- Over USB, write the release's parts, each at its own offset: bootloader at `0x0`, partition table at `0x8000`, `otadata` at `0x11000` and the app at `0x20000`, as `kube32-firmware-esp32s3-<version>-flash.json` lists them. The parts skip NVS. `scripts/flash.sh` writes the same parts from a local build.
