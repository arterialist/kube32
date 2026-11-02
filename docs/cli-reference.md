# Tools

Use `kubectl` and k9s to manage the cluster. Use the tools below to run the host simulator, build and ship WebAssembly pods, flash firmware, reach a board over USB, and load-test the ingress.

| Tool | Purpose |
| --- | --- |
| `kube32` | Flash boards, open their serial console, run the simulator, create, build, check, deploy, embed, and push Wasm pods and node storage files, roll out firmware, checkpoint and move pods ([Install](install.md)) |
| `kube32-sim` | Run control-plane and worker nodes as host processes |
| `scripts/flash.sh` | Build and flash firmware to an ESP32-S3 board from a checkout |
| `tools/tessera_loadtest.py` | Run concurrent HTTP load tests against a Tessera ingress |
| `tools/embed_assets.py`, `tools/pio_embed.py` | Generate embedded Wasm image and web asset tables |

## kube32-sim

`kube32-sim` runs Kube32 nodes on the host. [`kube32 sim`](#board-and-simulator-commands) runs it, and a CMake build writes it to `build/kube32-sim`. With no flags, one process runs the control plane, its kubelet, and one in-process worker. It ticks pods whose `k32_tick` returns `> 0` every 10 ms and others at 1 Hz, matching the boards. Unknown flags exit with code 2 and the usage text.

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
| `--heap-budget <size>` | `51Ki` | General heap available to pods and the system when `--psram 0` is set. With `--psram`, the board's PSRAM besides the pool block (not modelled unless set). |
| `--sim-i2c <parts>` | none | Simulated I2C parts on bus 0 of this process's node, for example `pahub,bme688@2,miniscale@3,rfid@4`. The format is `<part>[:0x<address>][@<PaHub channel>]`, and the parts are `pahub`, `bme688`, `miniscale` and `rfid`. Discovery finds them and drivers read them ([Devices](devices.md#compute-on-a-laptop)). |
| `--ingress-port <port>` | off (`0`) | Ingress controller HTTP port. |
| `--pod-port <port>` | `7000` | Pod HTTP server port on every node. |
| `--node-id <id>` | `0` | Node ID for UDP IPC channels. |
| `--ipc-port <port>` | `7400 + <node-id>` | UDP port for IPC channels. |
| `--peer <name:id:port>` | none | Static UDP IPC peer (repeatable). |
| `--lease-interval <ms>` | `5000` | Node heartbeat interval in milliseconds. |
| `--not-ready-timeout <ms>` | `15000` | Mark a node `NotReady` after this many milliseconds without a heartbeat. |
| `--eviction-timeout <ms>` | `30000` | Add `node.kubernetes.io/unreachable:NoExecute` after this many milliseconds without a heartbeat; pods are evicted once their matching `tolerationSeconds` (default `300` s) elapses. |
| `--reconcile-interval <ms>` | `1000` | Shortest time between two passes of a controller or the scheduler, in milliseconds. They run a pass after a change and every 30 s without one. |
| `-h`, `--help` | | Print all options with defaults and exit `0`. |

### Modeling boards without PSRAM

`--psram 0` models a board without PSRAM and reports no `esp32.io/psram` capacity. Pods share an 80 KiB pool block and one 28 KiB interpreter stack. The kubelet admits a pod against the free part of the pool block plus `--heap-budget`, default `51Ki`, minus the 24 KiB system reserve, and caps pod allocations in the general heap at the heap budget minus that reserve. The board itself admits pods on the pool block alone, while the simulator also counts the heap budget because its 64-bit `wasm3` structures are larger than the board's. Node allocatable memory is the pool block plus the heap budget above the reserve, and the `kube32.io/memory-free` annotation is what is free of it now. Without `--psram`, `standalone` and `control-plane` nodes behave as PSRAM boards.

### Modeling boards with PSRAM

`--psram <size>` reserves the PSRAM pool block, and nodes report it as `esp32.io/psram`. Pods' code, runtime structures and smaller linear memories take from the block's end first. With `--heap-budget`, the simulator also models the board's PSRAM besides the block, which pods take from when the block is full. The node reports the block plus that PSRAM above a 16 KiB reserve as allocatable `memory` and `esp32.io/psram`, and the block plus the budget as `esp32.io/psram` capacity. For example, `--psram 1284Ki --heap-budget 320Ki` gives 1588 KiB of allocatable memory. Without `--heap-budget`, pods take the host's memory: allocatable `memory` is the block plus the host memory that was available when the kubelet first reported, and allocatable `esp32.io/psram` is the block.

`kubectl top node` reports the host's RAM in use, or with `--heap-budget` what pods hold of the modelled heap and of the pool block. `kubectl top pod` reports what each pod holds, with linear memory as the host maps it (whole host pages).

The simulator is a 64-bit host build where `wasm3` runtime structures are about 2.3x larger than on the 32-bit ESP32-S3. A pod set that fits on a `--psram 0` simulator fits on the board, while the reverse is not guaranteed.

## kube32

`uv tool install kube32` installs the `kube32` command, also under the name `kube32-cli` ([Install](install.md)). In a checkout of the repository, `tools/kube32-cli` runs the same commands from `python/src` with Python 3.9 or newer; the board commands there need `pyserial` and `esptool`. See [Writing app pods](app-development.md) for usage walkthroughs.

### Board and simulator commands

| Command | Description |
| --- | --- |
| `flash` | Write the firmware of a release to an ESP32-S3 board over USB: the bootloader, partition table, `otadata` and application of the installed `kube32`'s release, or of `--version X.Y.Z`, downloaded once, checked against their SHA-256, and written at their offsets with esptool at 921600 baud, then at 460800 when that fails. The board keeps its stored settings (NVS). Then prints the serial console commands that set the board up and offers to open the console. Flags: `--port <port>` (default `KUBE32_SERIAL_PORT`, or the one board connected: USB vendor ID `0x303a` for native USB, `0x1a86` for CH340), `--version X.Y.Z`, `--erase-settings` (the release's merged image from `0x0`, erasing stored settings), `--file <image>` (a merged image instead of a release), `--baud <rate>`, `--console`, `--no-console`. See [Flashing](flashing-troubleshooting.md#flashing). |
| `console` | Open a board's serial console at 115200 baud without resetting the board: print what the board writes and send each line typed, such as `K32_CMD:STATUS`. Reopens the port when the board restarts. `Ctrl-C` quits. Flag: `--port <port>`. |
| `gateway` | Serve a board's API server and ingress on local ports over its USB console. Flags: `--port <port>`, `--map <local>:<device>`. See [kube32 gateway](#kube32-gateway). |
| `sim [<args>...]` | Run `kube32-sim` with the arguments `kube32` does not take: `kube32-sim` from `PATH`, else the release's `kube32-sim-<version>-<macos\|linux>-<arm64\|x86_64>.tar.gz`, downloaded once and checked against its `.sha256` file. `kube32 sim -- --help` prints the simulator's options. Flags: `--version X.Y.Z` (run that release, also when `kube32-sim` is on `PATH`), `--docker` (run `ghcr.io/arterialist/kube32-sim:<version>` with the API server, ingress and pod ports of the arguments published on `127.0.0.1`). Passes `Ctrl-C` and `SIGTERM` to the simulator and exits with its exit code. |
| `version` | Print the version of `kube32`, and the cluster's `/version` read with `kubectl` when it answers. Flags: `--client`, `--context`, `--kubeconfig`, `--server`, `--kubectl`. |

`flash` and `sim` cache downloads in `~/Library/Caches/kube32` on macOS and `$XDG_CACHE_HOME/kube32` (default `~/.cache/kube32`) on Linux, or in `KUBE32_CACHE_DIR`. `KUBE32_RELEASES_URL` replaces `https://github.com/arterialist/kube32/releases/download`. A development build of `kube32` matches no release: pass `--version X.Y.Z`, `--file` to `flash`, or put `kube32-sim` on `PATH` for `sim`.

### App commands

| Command | Description |
| --- | --- |
| `new <dir> --lang c\|rust` | Create an app project in the new directory `<dir>`, whose name names the app, the module and the crate. C: a `Makefile` that compiles every `.c` file with `clang --target=wasm32 -Oz` and links `build/<name>.wasm` with `wasm-ld` or `rust-lld`, a request pod in `main.c`, and the ABI header in `include/runtime/k32_app_abi.h`. Rust: a `cdylib` crate with the counter example in `src/lib.rs`, depending on `kube32-sdk` of the same release, and `.cargo/config.toml` with the link flags. Flags: `--lang`, default `c`, `--sdk-path <dir>` (Rust: depend on the `kube32-sdk` crate in `<dir>`). A development build depends on the SDK in the Git repository. |
| `build [<dir>]` | Compile a C, Rust, or AssemblyScript directory, default the current one, to `.wasm` and check it. Runs `make` for a directory with a `Makefile` and `cargo build --release --target <triple>` for a crate. Flags: `--lang c\|c-make\|rust\|assemblyscript`, `--name <name>`, `-o <path>`, `--artifact <path>`, `--memory <qty>`, default `64Ki`, `--stack <qty>`, default `16Ki`, `--target <triple>`, default `wasm32-unknown-unknown`, `-v`. |
| `check <file>` | Validate a `.wasm` module against the app ABI and the wasm3 opcode set. Flag: `--memory <qty>`. |
| `deploy <file>` | Create a Pod via `kubectl create`, carrying the module in `kube32.io/wasm-base64` unless `--image` is set. The API server stores the module apart from the Pod, which keeps `kube32.io/wasm-sha256`. Flags: `--name <name>`, `--node <node>`, `--namespace <ns>`, default `default`, `--memory <qty>`, default `64Ki`, `--psram <qty>`, `--hardware <list>`, `--sd-claim <claim>`, `--env K=V`, `--image <wasm://...>`, `--port <port>`, default `7000`, `--replace`, `--timeout <sec>`, default `60`, `--dry-run`, `--context`, `--kubeconfig`, `--server`, `--kubectl`. |
| `embed <file>` | Copy a checked module to `images/<name>.wasm` of the kube32 checkout that contains the current directory, for `wasm://embedded/<name>:v1` in the next firmware or simulator build. Flags: `--name <name>`, `--force`, `--images-dir <dir>`. |
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
| `devices [<name>...]` | List the Devices of a namespace (`-n`, default `default`, or `-A`), or the named ones, with the latest reading of each driver. The command reads it with `GET /` from the driver pod's node, at its pod IP and `http` port. Thus this machine must be able to reach the node. A reading is a use: the command reports one for each `OnDemand` Device (the `activate` subresource) and waits for the drivers of `Idle` ones to start ([Activation](devices.md#activation)). Flags: `--timeout <s>` for each reading, default `3`; `--activation-timeout <s>`, default `10`; `--no-activate` lists without starting drivers. |

Pod migration commands (see [Pod migration](migration.md)) create and read Checkpoints through `kubectl --raw` (kubeconfig credentials apply) and accept `--context`, `--kubeconfig`, `--server`, and `--kubectl`:

| Command | Description |
| --- | --- |
| `checkpoint <pod>` | Create a `Save` Checkpoint named `<pod>-<suffix>` and wait until it is `Saved` or `Failed`. Prints the size and the node. The pod keeps running. Flags: `-n <ns>`, default `default`, `-c <container>`, `--no-wait`, `--timeout <s>`, default `60`. |
| `move <pod> [--to <node>]` | Create a `Move` Checkpoint and wait until it is `Completed` or `Failed`. Without `--to` the scheduler picks the node. Prints the move's message, or the reason and each blocker. Flags: `-n <ns>`, `-c <container>`, `--no-wait`, `--timeout <s>`, default `120`. |
| `checkpoints` | List the Checkpoints of a namespace (`-n`, default `default`, or `-A`), oldest first: name, pod, action, phase, node, target, size, trigger and message. |

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
| 7 | A checkpoint or move failed or did not finish within `--timeout` |

## flash.sh

Build if needed and flash one board over serial from a checkout, see [Flashing](flashing-troubleshooting.md):

```
scripts/flash.sh <serial port> [baud]
```

- Uses `<serial port>` or `KUBE32_SERIAL_PORT`, and defaults to `460800` baud.
- Locates `pio` on `PATH` or in `${PLATFORMIO_CORE_DIR:-~/.platformio}/penv/bin`, and `esptool` in PlatformIO's virtualenv or system `python3`.
- Runs `pio run -e esp32s3` if `bootloader.bin`, `partitions.bin`, or `firmware.bin` is missing from `.pio/build/esp32s3/`.
- Writes the bootloader at `0x0`, the partition table at `0x8000`, an erased `otadata`, and the firmware into the first app slot, at the offsets in `partitions.csv`.
- Writes 256-byte paced blocks on CH340 bridges, vendor ID `0x1a86`. Uses `KUBE32_FLASH_BEFORE`, default `default-reset`, and `KUBE32_FLASH_AFTER`, default `hard-reset` on CH340 and `watchdog-reset` on native USB.

## kube32 gateway

Listens on local TCP ports and relays each connection to a board as a stream over the USB console. See [HTTP over the console](flashing-troubleshooting.md#http-over-the-console). Streams run concurrently, so watches, `kubectl logs -f`, k9s, and Server-Sent Events run alongside other requests.

```
kube32 gateway [--port <serial port>] [--map <local>:<device>]...
```

In a checkout, `tools/kube32_serial_gateway.py` runs it from the source tree with `pyserial` installed.

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
