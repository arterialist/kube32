# Contributing to Kube32

## Development setup

Install the required toolchains for the components you work on:

| Component | Required tools |
| :--- | :--- |
| Host simulator and C++ tests | CMake 3.16 or newer, C++17 compiler, Python 3 |
| ESP32-S3 firmware | PlatformIO Core 6.1, `espressif32 @ 6.13.0`, ESP-IDF 5.5.3 |
| WebAssembly apps | `clang` with `wasm32` target support, `rust-lld` |
| Rust SDK | Rust via `rustup` with target `wasm32-unknown-unknown` |

Add the WebAssembly target to your Rust toolchain:

```bash
rustup target add wasm32-unknown-unknown
```

## Building

### Host simulator

Configure and build the simulator and unit test binaries:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

CMake embeds the files under `images/` at configure time. Re-run the `cmake -S . -B build` step after adding or removing files in `images/`.

Simulator flags in `src/main_host.cpp`: `--role standalone|control-plane|worker`, `--node-name`, `--node-id`, `--port`, `--join`, `--data-dir`, `--enable-flash`, `--ipc-port`, `--ingress-port`, `--pod-port`, `--peer`, `--kubelet-name`, `--workers`, `--board`, `--worker-board`, `--token`, `--psram`, `--lease-interval`, `--not-ready-timeout`, `--eviction-timeout`, `--reconcile-interval`. Set `KUBE32_SD_ROOT=<dir>` to simulate a microSD card.

### Firmware

Build the ESP32-S3 firmware image:

```bash
pio run -e esp32s3
```

The pre-build script `tools/pio_embed.py` generates `src/embedded/embedded_blobs_esp.cpp` from `images/`. Build artifacts `bootloader.bin`, `partitions.bin`, and `firmware.bin` are written to `.pio/build/esp32s3/`. The factory application partition is 3 MB.

### Example apps and Rust SDK

Build the Tessera pods and web bundle:

```bash
make -C apps/tessera
```

Build the Rust SDK and example modules:

```bash
cd sdk/rust && cargo build --release --target wasm32-unknown-unknown --locked
```

### Documentation

Build the documentation site with strict link checking:

```bash
uv run --with "mkdocs-material>=9.5" mkdocs build --strict
```

## Running tests

Run the local test suites before opening a pull request:

```bash
# C++ unit tests
ctest --test-dir build --output-on-failure

# Tessera native tests (db, api, web)
make -C apps/tessera test

# kube32-cli unit tests
python3 -m pytest tools/tests
python3 -m unittest discover -s tools/tests -v

# Rust SDK unit tests and ABI checks
cd sdk/rust && cargo test -p kube32-sdk --locked
for m in sdk/rust/target/wasm32-unknown-unknown/release/*.wasm; do
  python3 tools/kube32-cli check "$m"
done
```

### Integration suites

Start a single-node simulator that models a board without PSRAM, as CI does, then run `tests/integration/runner.py`. Use `--target sim|hw` to select a simulator or board cluster. Available `--suite` values are `api`, `validation`, `resources`, `workloads`, `cluster`, `federation`, and `all`. The `federation` suite requires `make -C federation build` first:

```bash
./build/kube32-sim --role standalone --workers "" --psram 0 &
python3 tests/integration/runner.py --target sim --server http://127.0.0.1:6443 --suite api
```

## Testing on boards

Flash one board at a time with no other process holding the serial port:

```bash
bash scripts/flash.sh <port> 921600
```

Prefer the native USB-Serial/JTAG port over a CH340 USB-UART bridge. CH340 bridges drop data above 115200 baud. Do not press G0 on the Cardputer during a flash.

If automatic reset does not enter the bootloader, send `K32_CMD:BOOTLOADER` on the serial console and flash without resetting:

```bash
KUBE32_FLASH_BEFORE=no-reset bash scripts/flash.sh <port> 921600
```

### Serial console

Connect at 115200 baud. Opening the serial port resets the board. Keep a single reader process attached and log to a file to avoid repeated resets.

Diagnostic commands:
- `K32_CMD:STATUS` prints node name, role, free heap, PSRAM, uptime, pod count, storage, auth, and TLS state.
- `K32_CMD:TASKS` prints FreeRTOS task states and stack high-water marks.
- `K32_CMD:ACCESSLOG:on` enables per-request HTTP logging, and `K32_CMD:ACCESSLOG:off` disables it.
- `K32_CMD:SD` prints microSD card state.
- `K32_CMD:KEY:<text>` injects keystrokes on the Cardputer.

See [docs/flashing-troubleshooting.md](docs/flashing-troubleshooting.md) for the full command list. When your change affects board behavior, include the target board profile, `generic` or `cardputer`, and console or `kubectl` output in the pull request description.

## Code style

- Match the formatting, naming, and structure of the surrounding code.
- Firmware and host simulator share `src/` and `include/`. Keep target-specific entry points in `src/main_esp32.cpp` and `src/main_host.cpp`.
- Write code comments in the present tense.
- Do not commit generated build outputs or `src/embedded/embedded_blobs_esp.cpp`. Only rebuilt pod and web assets under `images/` are committed alongside their source changes.
- Never commit Wi-Fi credentials, bearer tokens, MAC addresses, or local network addresses.

## Commit messages

Format commit messages using Conventional Commits with a scope:

```text
type(scope[,scope]): concise description in lowercase with no trailing period
```

- Allowed types are `feat`, `fix`, `perf`, `chore`, `docs`, `test`, and `style`.
- Common scopes are `api`, `kubelet`, `runtime`, `net`, `ipc`, `firmware`, `sd`, `tessera`, and `devex`.
- Explain the reason for the change in the commit body. Keep unrelated changes in separate commits.

## Pull request checklist

- [ ] Host simulator builds and all `ctest` programs pass.
- [ ] `make -C apps/tessera test`, `kube32-cli` tests, and `cargo test -p kube32-sdk` pass.
- [ ] `pio run -e esp32s3` succeeds when C++ or embedded image files change.
- [ ] Rebuilt binaries in `images/` are included if pod or web sources changed.
- [ ] PR description states whether the change was tested on the host simulator, ESP32-S3 boards, or both.
- [ ] No credentials, MAC addresses, or local paths appear in code, logs, or documentation.

## Licensing

Kube32 is licensed under the Apache License 2.0 in `LICENSE`. Contributions are licensed under the same terms. Third-party code must use a compatible license and retain its license notices.
