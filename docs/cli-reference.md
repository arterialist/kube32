# Tools

Use `kubectl` and k9s to manage the cluster. Use the tools below to run the host simulator, build and ship WebAssembly pods, flash firmware, reach a board over USB, and load-test the ingress.

| Tool | Purpose |
| --- | --- |
| `build/kube32-sim` | Run control-plane and worker nodes as host processes |
| `tools/kube32-cli` | Build, check, deploy, embed, and push Wasm pods and node storage files |
| `scripts/flash.sh` | Build and flash firmware to an ESP32-S3 board |
| `tools/kube32_serial_gateway.py` | Forward local HTTP ports to a board over USB serial |
| `tools/tessera_loadtest.py` | Run concurrent HTTP load tests against a Tessera ingress |
| `tools/embed_assets.py`, `tools/pio_embed.py` | Generate embedded Wasm image and web asset tables |

## kube32-sim

`build/kube32-sim` runs Kube32 nodes on the host. With no flags, one process runs the control plane, its kubelet, and one in-process worker. It ticks pods whose `k32_tick` returns `> 0` every 10 ms and others at 1 Hz, matching the boards. Unknown flags exit with code 2 and the usage text.

| Flag | Default | Description |
| --- | --- | --- |
| `--role <role>` | `standalone` | Node role: `standalone`, `control-plane`, or `worker`. |
| `--port <port>` | `6443` | API server port. |
| `--join <url>` | `""` | Worker: API server URL to register with, or `auto` for mDNS discovery. |
| `--token <token>` | `$KUBE32_API_TOKEN` | API bearer token. |
| `--tls` | off | Serve HTTPS alongside plain HTTP on `--port`; key and certificate are stored in `<data-dir>/kube32/tls/`. |
| `--egress-ca <file>` | none | PEM roots that pods' `https://` calls trust besides the operating system's (a private CA). Control plane and standalone roles. |
| `--data-dir <dir>` | `/tmp/kube32_<node>_data` | Object store and node storage root (unless `KUBE32_SD_ROOT` is set). Each node's firmware slots are files under this directory (`firmware/`, `firmware-<worker>/`). See [Firmware updates](firmware-updates.md#simulator). |
| `--enable-flash` | off | Persist the object store journal in `<data-dir>/kube32/store/journal.img`. |
| `--node-name <name>` | `esp32-master` | Control-plane or standalone node name. |
| `--kubelet-name <name>` | `esp32-node-01` | Kubelet node name in `standalone` mode. |
| `--workers <a,b,...>` | `esp32-node-02` | In-process worker node names in `standalone` mode; pass `""` for a single control-plane node with no in-process workers (for a separate worker process or a physical board). |
| `--board <board>` | `generic` | `kube32.io/board` label for this process's kubelet. |
| `--worker-board <board>` | `cardputer` | `kube32.io/board` label for in-process workers. |
| `--node-ip <ip>` | `127.0.0.1` | Pod IP address advertised to other nodes; pass a LAN address so physical boards can reach pods on the simulator. |
| `--psram <size>` | `1284Ki`; workers reserve none | PSRAM pool to model (for example `8Mi`). Passing `0` explicitly models a board without PSRAM (see below). |
| `--heap-budget <size>` | `51Ki` | General heap available to pods and the system when `--psram 0` is set. |
| `--ingress-port <port>` | off (`0`) | Ingress controller HTTP port. |
| `--pod-port <port>` | `7000` | Pod HTTP server port on every node. |
| `--node-id <id>` | `0` | Node ID for UDP IPC channels. |
| `--ipc-port <port>` | `7400 + <node-id>` | UDP port for IPC channels. |
| `--peer <name:id:port>` | none | Static UDP IPC peer (repeatable). |
| `--lease-interval <ms>` | `5000` | Node heartbeat interval in milliseconds. |
| `--not-ready-timeout <ms>` | `15000` | Mark a node `NotReady` after this many milliseconds without a heartbeat. |
| `--eviction-timeout <ms>` | `30000` | Add `node.kubernetes.io/unreachable:NoExecute` after this many milliseconds without a heartbeat; pods are evicted once their matching `tolerationSeconds` (default `300` s) elapses. |
| `--reconcile-interval <ms>` | `1000` | Controller and scheduler loop interval in milliseconds. |
| `-h`, `--help` | | Print all options with defaults and exit `0`. |

### Modeling boards without PSRAM

`--psram 0` models a board without PSRAM and reports no `esp32.io/psram` capacity. Pods share an 80 KiB pool block and one 28 KiB interpreter stack. The kubelet admits a pod against the free part of the pool block plus `--heap-budget`, default `51Ki`, minus the 24 KiB system reserve, and caps pod allocations in the general heap at the heap budget minus that reserve. The board itself admits pods on the pool block alone, while the simulator also counts the heap budget because its 64-bit `wasm3` structures are larger than the board's. Node allocatable memory equals the heap budget plus the pool block. Without `--psram`, `standalone` and `control-plane` nodes behave as PSRAM boards.

The simulator is a 64-bit host build where `wasm3` runtime structures are about 2.3x larger than on the 32-bit ESP32-S3. A pod set that fits on a `--psram 0` simulator fits on the board, while the reverse is not guaranteed.

## kube32-cli

`tools/kube32-cli` requires Python 3 with the standard library. See [Writing app pods](app-development.md) for usage walkthroughs.

| Command | Description |
| --- | --- |
| `build <dir>` | Compile a C, Rust, or AssemblyScript directory to `.wasm` and check it. Flags: `--lang c\|c-make\|rust\|assemblyscript`, `--name <name>`, `-o <path>`, `--artifact <path>`, `--memory <qty>`, default `64Ki`, `--stack <qty>`, default `16Ki`, `--target <triple>`, default `wasm32-unknown-unknown`, `-v`. |
| `check <file>` | Validate a `.wasm` module against the app ABI and the wasm3 opcode set. Flag: `--memory <qty>`. |
| `deploy <file>` | Create a Pod via `kubectl create`, carrying the module in `kube32.io/wasm-base64` unless `--image` is set. Flags: `--name <name>`, `--node <node>`, `--namespace <ns>`, default `default`, `--memory <qty>`, default `64Ki`, `--psram <qty>`, `--hardware <list>`, `--sd-claim <claim>`, `--env K=V`, `--image <wasm://...>`, `--port <port>`, default `7000`, `--replace`, `--timeout <sec>`, default `60`, `--dry-run`, `--context`, `--kubeconfig`, `--server`, `--kubectl`. |
| `embed <file>` | Copy a checked module to `images/<name>.wasm` for `wasm://embedded/<name>:v1`. Flags: `--name <name>`, `--force`, `--images-dir <dir>`. |
| `sd-push <file> (--to <mount point> \| --node <node>)` | Copy a checked module to `kube32/images/<name>[-<tag>].wasm` for `wasm://sd/<name>[:<tag>]`, either to a locally mounted card with `--to <dir>` or over the network to a node's storage with `--node <node>`. Flags: `--to <dir>`, `--node <node>`, `--name <name>`, `--tag <tag>`, `--context`, `--kubeconfig`, `--server`, `--kubectl`. |
| `push <file\|dir> --node <node> [--path <path>]` | Upload a file or directory tree over the network to `<storage-root>/kube32/<path>` via `kubectl replace --raw /api/v1/nodes/<node>/proxy/storage/<path>` (kubeconfig credentials apply). Default `--path` is `images/<filename>` for a file or `images` for a directory, uploading files recursively to `<path>/<relative-path>`. Prints one line per file plus a directory total, stops at the first error, and exits `1` if a directory is empty. Flags: `--node <node>`, `--path <path>`, `--context`, `--kubeconfig`, `--server`, `--kubectl`. |

Firmware commands (see [Firmware updates](firmware-updates.md)) reach the API server through `kubectl --raw` (kubeconfig credentials apply) and accept `--context`, `--kubeconfig`, `--server`, and `--kubectl`:

| Command | Description |
| --- | --- |
| `firmware push <firmware.bin>` | Check the image (an ESP32 image whose last 32 bytes are its SHA-256) and upload it into the control plane's update slot in ranges, resuming where an interrupted upload stopped. Does nothing when the image is already staged. Flags: `--chunk <qty>`, default `16Ki` (the control plane holds a range in memory while writing it to flash), `--retries <n>`, default `5`. |
| `firmware rollout` | Create the `FirmwareRollout` `firmware` for the staged image, replacing a finished one, and print progress until it completes or fails. Flags: `--sha256 <hex>` (refuses unless it matches the staged image), `--node-timeout <s>`, `--no-wait`, `--timeout <s>`, default `3600`, `--interval <s>`, default `5`. |
| `firmware status` | Print the staged image, the rollout, and a node table showing version, image, slot, confirmation state, target, and `FirmwareUpdate` condition. |
| `firmware abort` | Delete the rollout. Nodes that have not switched keep their running firmware. |
| `firmware info <firmware.bin>` | Print the version and SHA-256 of an image file. |

Exit codes:

| Code | Meaning |
| --- | --- |
| 0 | Success |
| 1 | Input or toolchain error |
| 2 | Usage error |
| 3 | ABI check failed |
| 4 | `kubectl` failed |
| 5 | Pod already exists during `deploy` (pass `--replace`), or a firmware rollout is in progress during `firmware rollout` |
| 6 | The firmware rollout failed or did not finish within `--timeout` |

## flash.sh

Build if needed and flash one board over serial, see [Flashing](flashing-troubleshooting.md):

```
scripts/flash.sh <serial port> [baud]
```

- Uses `<serial port>` or `KUBE32_SERIAL_PORT`, and defaults to `460800` baud.
- Locates `pio` on `PATH` or in `${PLATFORMIO_CORE_DIR:-~/.platformio}/penv/bin`, and `esptool` in PlatformIO's virtualenv or system `python3`.
- Runs `pio run -e esp32s3` if `bootloader.bin`, `partitions.bin`, or `firmware.bin` is missing from `.pio/build/esp32s3/`.
- Writes the bootloader at `0x0`, the partition table at `0x8000`, an erased `otadata`, and the firmware into the first app slot, at the offsets in `partitions.csv`.
- Writes 256-byte paced blocks on CH340 bridges, vendor ID `0x1a86`. Uses `KUBE32_FLASH_BEFORE`, default `default-reset`, and `KUBE32_FLASH_AFTER`, default `hard-reset` on CH340 and `watchdog-reset` on native USB.

## kube32_serial_gateway.py

Requires `pyserial`. Listens on local TCP ports and relays each connection to a board as a stream over the USB console. See [HTTP over the console](flashing-troubleshooting.md#http-over-the-console). Streams run concurrently, so watches, `kubectl logs -f`, k9s, and Server-Sent Events run alongside other requests.

```
python3 tools/kube32_serial_gateway.py [--port <serial port>] [--map <local>:<device>]...
```

- Without `--port`, the gateway reads `KUBE32_SERIAL_PORT`, or queries each native USB-Serial/JTAG device (vendor ID `0x303a`) and CH340 bridge (vendor ID `0x1a86`) for its role and selects the control plane.
- Default port mappings are `6443:6443` for the API server and `8080:80` for the ingress, on `127.0.0.1`.
- A worker forwards streams to the control plane over Wi-Fi.
- The gateway opens the port without resetting the board.
- If a board specified with `--port` does not respond because it is still booting, the gateway retries up to six times at 3-second intervals.
- The gateway prints device output to stdout and sends lines from stdin to the console, such as `K32_CMD:STATUS`.
- When the board restarts, the gateway ends open streams and holds new requests until the board answers again. When the port disappears, the gateway reopens it.
- The gateway forwards chunked request bodies to the board with a `Content-Length` header, and responds with `413` to requests of 60 KiB or more.

## tessera_loadtest.py

Runs concurrent HTTP clients against a Tessera ingress and prints `p50`, `p90`, `p99`, and `max` latency percentiles, throughput, and status errors per endpoint:

```
python3 tools/tessera_loadtest.py http://<control-plane-address> --clients 4 --seconds 60 [--no-keepalive]
```

## Embedding assets

`tools/embed_assets.py` compiles `images/*.wasm` into `EmbeddedImage` entries keyed by file stem, and `images/web/<bundle>/<path>[.gz]` into `EmbeddedAsset` entries for `StaticBundle` ingress backends and `k32_resp_asset`:

```
tools/embed_assets.py --host-out build/generated
tools/embed_assets.py --esp
```

- `--host-out <dir>` writes `<dir>/embedded_blobs.cpp` for CMake host builds.
- `--esp` writes `src/embedded/embedded_blobs_esp.cpp` and `src/embedded/embedded_files.cmake`; PlatformIO invokes it via `tools/pio_embed.py`.
- Files ending in `.gz` are served at `/<path>` with `Content-Encoding: gzip`.
