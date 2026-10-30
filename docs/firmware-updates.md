# Firmware updates

The cluster updates the firmware of its own boards. You stage one image on the control plane. A rollout then updates every node in turn, starting with workers one at a time and ending with the control plane. Each node writes the image to its second app slot, checks the SHA-256, switches its boot slot, and reboots. The target image confirms itself only after the node reports Ready for 30 seconds. A reset before confirmation, or a failure to reach the API server, causes the bootloader to revert to the previous image. The rollout stops at the first node that fails.

```
kube32-cli firmware push .pio/build/esp32s3/firmware.bin
kube32-cli firmware rollout
```

## Partition layout

| Name | Type | Subtype | Offset | Size | Use |
| --- | --- | --- | --- | --- | --- |
| `nvs` | `data` | `nvs` | `0x9000` | 32 KB | Node configuration, kept by updates |
| `otadata` | `data` | `ota` | `0x11000` | 8 KB | Boot slot and the state of an unconfirmed image |
| `phy_init` | `data` | `phy` | `0x13000` | 4 KB | Radio calibration |
| `ota_0` | `app` | `ota_0` | `0x20000` | 3.25 MB | App slot |
| `ota_1` | `app` | `ota_1` | `0x360000` | 3.25 MB | App slot |
| `journal` | `data` | `0x40` | `0x6A0000` | 1 MB | Object store journal, kept by updates |
| `storage` | `data` | `fat` | `0x7A0000` | 384 KB | Node storage root without a microSD card |

The build fails if `firmware.bin` exceeds the app slot size (`tools/pio_slot_check.py`). The tool prints how much of the slot the image uses.

## Moving a board to this layout

A board flashed with the single-app partition table (`factory` at `0x20000`) requires a one-time USB flash to install the dual-slot table and a rollback-capable bootloader:

```
pio run -e esp32s3
bash scripts/flash.sh <serial port> 921600
```

`scripts/flash.sh` writes the bootloader at `0x0`, the partition table at `0x8000`, an erased `otadata` at `0x11000`, and the firmware into `ota_0` at `0x20000`. The NVS partition (name, role, board profile, Wi-Fi, token, TLS key) and the control plane journal partition keep their offsets and survive. The FAT `storage` partition moves. The node formats `storage` on the next boot, which erases internal flash files (pod volumes and container logs on a node without a card). The update leaves a microSD card untouched.

Any later USB flash with `scripts/flash.sh` also erases `otadata`. The board then boots the flashed image from `ota_0`, regardless of which slot ran before.

## Staging an image

`kube32-cli firmware push <firmware.bin>` uploads the image through the API server to the passive app slot on the control plane. It sends 16 KiB ranges by default (`--chunk`). The control plane holds one range in memory while writing it to flash. Each range stays under the 60 KiB request limit of `tools/kube32_serial_gateway.py`, so the command works over both USB serial and Wi-Fi. An interrupted upload resumes at the byte the control plane reports. Running the command again continues an unfinished upload of the same image. The control plane hashes the incoming data, compares the SHA-256 with the value from the CLI, and verifies that the slot holds a bootable image. The staged image survives a control plane restart.

You cannot stage an image while a rollout is in progress.

## Rolling out

`kube32-cli firmware rollout` creates a `FirmwareRollout` named `firmware` for the staged image and watches it until completion or failure (`--no-wait` returns immediately). The rollout controller on the control plane iterates through the nodes. It updates workers first in alphabetical order and updates the control plane last. On the first node that has not confirmed the target image, the controller sets the `kube32.io/firmware` annotation to the image SHA-256:

1. The kubelet reads the staged image size from the control plane. Between pod syncs, it downloads the image into its passive slot in ranges (32 KiB per request with PSRAM, 16 KiB without) and hashes the data as it arrives. A board without PSRAM streams the download through the 1 KiB HTTP client buffer without allocating memory for the full image.
2. When the SHA-256 matches and the slot holds a bootable image, the kubelet sets that slot for the next boot and reboots the node.
3. The bootloader boots the unconfirmed image. After the node registers and reports Ready in its heartbeats for 30 s without failing, the image confirms itself.
4. The controller advances to the next node once the current node reports the image as confirmed.

Last, the control plane writes its object store journal and boots the image directly from the staging slot. It restores its state from the journal partition, confirms the image the same way, and marks the rollout complete.

Each node has 15 minutes to confirm the target image (`kube32-cli firmware rollout --node-timeout <s>`, `spec.nodeTimeoutSeconds`).

## Rollback

| What happens to the target image | Result |
| --- | --- |
| A panic, a watchdog reset, a power cut, or any other reset before confirmation | The bootloader marks the image aborted and boots the previous slot |
| The image runs but fails to report Ready for 30 s within 5 minutes of boot (for example, it never reaches the API server) | The image marks itself invalid and reboots into the previous slot |
| The download, SHA-256 check, or image check fails | The node keeps its current slot and reports the failure |

On failure, the node records the failed image hash in `kube32.io/firmware-failed`. It sets its `FirmwareUpdate` condition to `RolledBack` or `UpdateFailed` with a reason, and it emits a Warning Event. The rollout stops in the `Failed` phase and leaves remaining nodes untouched. A node does not retry a failed image while its target annotation still names that image. Deleting the rollout (`kube32-cli firmware abort`) clears the target from the nodes. Starting another rollout with the same image retries the update.

## Status

`kube32-cli firmware status` prints the staged image, the rollout state, and a node table:

```
staged:  Staged  v0.3.0-12-g1a2b3c4 (5e0c2a9d41f7)  3,098,880/3,098,880 bytes  slot ota_1
rollout: Progressing  v0.3.0-12-g1a2b3c4 (5e0c2a9d41f7)  0/2 nodes  node esp32-node-02  updating node esp32-node-02: ...
NODE           ROLE           VERSION             IMAGE         SLOT   STATE      TARGET        UPDATE
esp32-node-02  worker         v0.3.0-9-g9f8e7d6   0d3b8a1c22e4  ota_0  Confirmed  5e0c2a9d41f7  Downloading: v0.3.0-12-g1a2b3c4 (5e0c2a9d41f7): 1440 KiB of 3027 KiB to ota_1
esp32-node-01  control-plane  v0.3.0-9-g9f8e7d6   0d3b8a1c22e4  ota_0  Confirmed  -             UpToDate: v0.3.0-9-g9f8e7d6 (0d3b8a1c22e4) on ota_0
```

With kubectl:

| Command | Shows |
| --- | --- |
| `kubectl get nodes -o wide` | `OS-IMAGE` is `kube32 <version> (ESP-IDF <version>)` |
| `kubectl get firmwarerollouts` | Version, image, phase, current node, updated nodes; `-o wide` adds the message |
| `kubectl describe node <node>` | The `FirmwareUpdate` condition and the firmware annotations |
| `kubectl get events` | `FirmwareRolloutStarted`, `FirmwareUpdateStarted`, `FirmwareDownloading`, `FirmwareActivated`, `FirmwareUpdated`, `FirmwareRolloutComplete`; Warnings `FirmwareRolledBack`, `FirmwareUpdateFailed`, `FirmwareRolloutFailed` |

On the serial console, `K32_CMD:FIRMWARE` returns `K32_FIRMWARE version=... sha256=... slot=... state=Confirmed|Verifying update_slot=... staged=... staged_sha256=... staged_version=...`. The boot log also prints a `[FIRMWARE]` line for the running image.

### Node annotations and condition

| Name | Set by | Value |
| --- | --- | --- |
| `kube32.io/firmware` | rollout controller | SHA-256 of the target image for the node |
| `kube32.io/firmware-version` | kubelet | Version of the running image |
| `kube32.io/firmware-sha256` | kubelet | SHA-256 of the running image file |
| `kube32.io/firmware-slot` | kubelet | `ota_0` or `ota_1` |
| `kube32.io/firmware-state` | kubelet | `Confirmed`, or `Verifying` before the image confirms itself |
| `kube32.io/firmware-failed` | kubelet | SHA-256 of the last image that failed on the node |

The `FirmwareUpdate` condition is `True` during an active update (reasons `Waiting`, `Downloading`, `Rebooting`, `Verifying`) and `False` otherwise (`UpToDate`, `UpdateFailed`, `RolledBack`).

## Image identity

The SHA-256 of the entire `firmware.bin` file identifies an image, matching the output of `shasum -a 256 firmware.bin`. Each node computes the SHA-256 of its running image at boot. The build embeds the version string after the `kube32-version:` marker. This value comes from `$KUBE32_VERSION` if set at build time, or from `git describe --tags --always --dirty` otherwise. `kube32-cli firmware info <firmware.bin>` prints both values.

```
KUBE32_VERSION=v0.4.0-rc1 pio run -e esp32s3
```

## API

| Request | Effect |
| --- | --- |
| `GET /apis/kube32.io/v1/firmware` | Returns the staged `FirmwareImage` with `status.phase` (`Empty`, `Receiving`, `Staged`, `Failed`), `sha256`, `size`, `received`, `version`, `slot`, `slotSize`, and `message` |
| `PUT /apis/kube32.io/v1/firmware/image?offset=<n>&size=<bytes>&sha256=<hex>` | Writes the request body at `offset`. Offset `0` starts an upload. Subsequent ranges must start at `received` (`409 Conflict` returns the expected offset). The server streams the body to flash, so ranges can be any size. Returns `422` if the SHA-256 or image check fails |
| `GET /apis/kube32.io/v1/firmware/image?sha256=<hex>&offset=<n>&length=<n>` | Reads bytes from the staged image, up to 256 KiB per request. Returns `409` if a different image is staged |
| `/apis/kube32.io/v1/firmwarerollouts[/<name>]` | Manages `FirmwareRollout` resources (cluster-scoped, short name `fwr`): get, list, watch, create, update, patch, delete. Accepts `spec.sha256` (required) and `spec.nodeTimeoutSeconds`. The controller updates `status.phase` (`Progressing`, `Complete`, `Failed`), `version`, `node`, `updatedNodes`, `totalNodes`, and `message`. `Complete` and `Failed` are terminal states. Only one rollout runs at a time |

All firmware endpoints require standard API authentication.

```yaml
apiVersion: kube32.io/v1
kind: FirmwareRollout
metadata:
  name: firmware
spec:
  sha256: 5e0c2a9d41f7...
  nodeTimeoutSeconds: 900
```

## Testing rollback

Building with `-DKUBE32_FIRMWARE_FAULT` produces an image that panics 10 seconds after booting in an unconfirmed state, before confirmation can occur. When flashed over USB, the same image runs normally. Rolling out this fault build tests bootloader rollback on the first worker:

```
PLATFORMIO_BUILD_FLAGS=-DKUBE32_FIRMWARE_FAULT KUBE32_VERSION=fault-test pio run -e esp32s3
cp .pio/build/esp32s3/firmware.bin /tmp/fault.bin
pio run -e esp32s3
kube32-cli firmware push /tmp/fault.bin
kube32-cli firmware rollout
```

## Simulator

`kube32-sim` stores node slots as files under `--data-dir` (`firmware/` for the local node, `firmware-<worker>/` for in-process workers). It creates `ota_0.bin`, `ota_1.bin`, and `otadata`, and it runs the same state machine. Before an update, a simulated node reports the version and SHA-256 of the `kube32-sim` executable. When an in-process worker switches slots, it restarts its kubelet. When the local node switches slots, `kube32-sim` exits and boots from the target slot on the next launch (pass `--enable-flash` to persist the object store).

## Limitations

- Nodes must run firmware with A/B slots. A node flashed with the single-app partition table reports no firmware annotations and causes the rollout to time out.
- A node that joins the cluster after a completed rollout keeps its current firmware until the next rollout.
