# Contributing to Kube32

Thanks for taking a look. This file covers the one rule the project insists
on, how to build and test each part, how to test on hardware, and how commits
are written.

## The rule: nothing fake

Every statement in the code, the docs, the commit messages and the UI must be
true of what actually runs.

- A feature exists when it runs: on the boards for anything presented as
  running on the cluster, otherwise on the host simulator. Stubs, scripted
  demos and placeholder output are not features. Delete them rather than
  dress them up.
- Numbers are measured, never estimated or rounded up. Say where they were
  measured (board or simulator, which board, which build), and keep the raw
  figure so others can compare.
- An API response or a log line never invents a value. When a figure is not
  known, leave the field out or say "unknown"; do not fill in a plausible
  number. Where the code still does that today it is a bug; the known cases
  are listed in [docs/api-reference.md](docs/api-reference.md#known-inaccuracies).
- Docs show real commands with their real output. When something was run
  only on the simulator, or not at all, the page says so.
- Hardware claims need a hardware run. If you have no board, say the change
  is host-tested only. That is fine; claiming more is not.

Never commit secrets. Wi-Fi credentials are typed into the board's console
and live only in its NVS. Keep real SSIDs, passwords, MAC addresses and LAN
addresses out of the repository, logs pasted into docs included.

## Layout

See the table in [README.md](README.md#repository-layout). Firmware and
simulator share everything under `src/` and `include/`.
`src/main_esp32.cpp` and `src/main_host.cpp` assemble the components for
each target.

## Building

### Host simulator and unit tests

Needs CMake 3.16 or newer, a C++17 compiler (Apple Clang or GCC) and
Python 3.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
ctest --test-dir build --output-on-failure
```

The simulator's flags are parsed in `parse_sim_args` in `src/main_host.cpp`:
`--role standalone|control-plane|worker`, `--port`, `--ingress-port`,
`--pod-port`, `--ipc-port`, `--app`, `--core-pod`, `--data-dir`,
`--enable-flash`, `--join`, `--node-name`, `--node-id`, `--peer`, and the
lease and reconcile timings. `KUBE32_SD_ROOT=<dir>` gives it a simulated
microSD card.

The Wasm images in `images/` are embedded at configure time (a CMake glob).
After adding or removing a file there, re-run the configure step.

### Firmware

Needs [PlatformIO Core](https://platformio.org/install/cli) 6.1.
`platformio.ini` pins `espressif32 @ 6.13.0`, which installs ESP-IDF 5.5.3 and
its Xtensa toolchain on first use.

```
pio run -e esp32s3
```

A pre-build hook (`tools/pio_embed.py`) regenerates
`src/embedded/embedded_blobs_esp.cpp` from `images/`. The output goes to
`.pio/build/esp32s3/` (`bootloader.bin`, `partitions.bin`, `firmware.bin`).
The application partition is 2.5 MB; the build prints how much of it is used.

### Tessera

```
make -C apps/tessera/core test     # native build of the core + 1,244 checks
make -C apps/tessera/core          # wasm32 build -> apps/tessera/core/build/tessera-core.wasm
make -C apps/tessera/hud           # writes images/tessera-hud.wasm directly
python3 apps/tessera/web/build.py  # writes apps/tessera/web/dist/*.gz; exits 1 over the size budget
```

The firmware and the simulator embed the checked-in files under `images/`.
The core image and the web assets are copied there by hand. The core's README
lists the link settings used for the board image. Commit a rebuilt image
together with the source change that produced it.

### Rust SDK and tools

```
cd sdk/rust && cargo build --release --target wasm32-unknown-unknown
cd sdk/rust && cargo test -p kube32-sdk
python3 -m unittest discover -s tools/tests -v    # kube32-cli
```

The wasm32 target must come from rustup (`rustup target add
wasm32-unknown-unknown`); a distribution cargo earlier in `PATH` has no wasm32
standard library. [docs/app-development.md](docs/app-development.md) covers
`tools/kube32-cli`.

### Documentation

```
uv run --with "mkdocs-material>=9.5" mkdocs build --strict
```

## Tests

CI (`.github/workflows/ci.yml`) runs the host build and CTest on Ubuntu 24.04
and macOS 15, the Tessera core checks, the `kube32-cli` tests, the Rust SDK
build and tests, and the firmware build. Run the same commands locally before
sending a change.

Known failures, measured on the commit this file was written against:

- On macOS all 23 CTest programs pass.
- On Ubuntu 24.04 with GCC 13.3 (run in an arm64 container), 18 of 23 pass:
  - `test_channel_ipc` and `test_m4_adversarial_ipc` hang at process exit.
    `~ChannelManager` calls `VirtualChannelRoutingTable::instance().clear()`
    after that function-local static has been destroyed.
  - `test_m3_thread_names` fails: Linux rejects thread names longer than 15
    characters, and the OSAL does not truncate them.
  - `test_memory_adversarial` fails in `test_slab_concurrent_telemetry`.
  - `test_api_streams` fails in `test_router_watch_and_log_paths`: the second
    watch is refused because the first has not been released yet.

  The Linux CI job will stay red until these are fixed.

The Python suites under `tests/` (`tests/runner.py`, the `tier*` packages and
the `test_m*` scripts) predate the current runtime. Against the simulator,
tier 1 passes 29 of 30, tier 2 passes 29 of 29 and tier 3 passes 7 of 15.
They are not part of CI. Update or delete them; do not quote their results as
current.

## Testing on hardware

What has been learned on the two development boards (an ESP32-S3 devkit with
PSRAM and an M5Stack Cardputer):

- **Flash one board at a time**, with nothing else holding its serial port:
  `bash scripts/flash.sh <port> 921600`. A native USB-Serial/JTAG port takes
  about 26 s. A CH340 USB-UART bridge is much slower, and it corrupts
  received data above 115200 baud; prefer the native port.
- **Opening a port can reset the board.** The Cardputer resets when its
  USB-Serial/JTAG port is opened, and the devkit resets when its CH340 port is
  opened. Keep one long-lived reader per board writing to a file, and read the
  file instead of reopening the port.
- **When the automatic reset does not enter the bootloader**, send
  `K32_CMD:BOOTLOADER` on the console (drain pending output first). Then flash
  with `KUBE32_FLASH_BEFORE=no-reset bash scripts/flash.sh <port> 921600`.
- **If a native USB port stops answering** (esptool reports "No serial data
  received" and the console reads nothing), unplug and replug the board.
- **Do not press G0 on the Cardputer during a flash.** It corrupts the
  written image.
- **Useful console commands while testing**:
  - `K32_CMD:STATUS`: heap, PSRAM, uptime and pod count.
  - `K32_CMD:TASKS`: task states and stack high-water marks.
  - `K32_CMD:ACCESSLOG:on`: per-request HTTP log.
  - `K32_CMD:SD`: card state.
  - `K32_CMD:KEY:<text>`: inject keystrokes.

  All commands are listed in
  [docs/flashing-troubleshooting.md](docs/flashing-troubleshooting.md).
- `tools/tessera_loadtest.py <ingress URL>` measures latency percentiles
  against a running Tessera. Quote its output, with the board and build it
  ran against.

In the commit message, say what was verified and where, for example
"Verified on the Cardputer: ..." or "Host simulator only; not run on a
board."

## Commit messages

The history uses Conventional Commits with a scope:

```
type(scope[,scope]): what changed, comma-separated when several things did
```

- **Types** in use: `feat`, `fix`, `perf`, `chore`, `docs`, `test`, `style`.
- **Scopes** are the area touched, for example `api`, `kubelet`, `runtime`,
  `net`, `ipc`, `firmware`, `sd`, `tessera`, `devex`.
- **The subject** is lower case and has no trailing period.
- **The body** explains why. It is usually a bullet list with the measured
  before and after figures, and it ends with what was verified and where. See
  `git log` for examples (`f54968a`, `022d910`, `0565b3f`).

Keep unrelated changes in separate commits.

## Licensing

Kube32 is under the Apache License 2.0 (`LICENSE`). By contributing you agree
that your contribution is licensed under the same terms. Code copied from
elsewhere must come with a compatible license and keep its notices, as wasm3
does in `third_party/wasm3/`.
