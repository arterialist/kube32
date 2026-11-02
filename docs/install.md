# Install

The `kube32` command flashes boards, runs the host simulator, and builds, checks and deploys WebAssembly pods. It is a Python package on PyPI, installed with [uv](https://docs.astral.sh/uv/).

## Install with uv

Install uv if it is missing:

```sh
curl -LsSf https://astral.sh/uv/install.sh | sh
```

Install `kube32` as a tool. The `kube32` command then runs from any directory:

```sh
uv tool install kube32
kube32 version
```

`uvx` runs a command without installing it first:

```sh
uvx kube32 sim
uvx kube32 flash
```

Upgrade to the latest release, or install a given one:

```sh
uv tool upgrade kube32
uv tool install kube32==<version>
```

`uv tool uninstall kube32` removes it. The package needs Python 3.9 or newer, which uv downloads when the system has none. It installs `pyserial` and `esptool` as its dependencies, and also installs the command under the name `kube32-cli`.

## What each command needs

| Command | Needs |
| --- | --- |
| `kube32 flash`, `kube32 console`, `kube32 gateway` | A board on USB. On Linux, membership in the group that owns the serial device, usually `dialout` |
| `kube32 sim` | macOS or Linux on arm64 or x86_64, or Docker with `--docker` |
| `kube32 new`, `kube32 build` (C) | `clang` with the wasm32 target, `make`, and `wasm-ld` from LLVM's lld, or `rust-lld` from a Rust toolchain |
| `kube32 new`, `kube32 build` (Rust) | Rust from [rustup](https://rustup.rs) and `rustup target add wasm32-unknown-unknown` |
| `kube32 build` (AssemblyScript) | `npm install --save-dev assemblyscript` in the project |
| `kube32 deploy`, `push`, `sd-push --node`, `firmware`, `devices`, `checkpoint`, `move`, `checkpoints` | [`kubectl`](https://kubernetes.io/docs/tasks/tools/) on `PATH` and a kubeconfig context for the cluster |
| `kube32 check`, `embed`, `sd-push --to` | Nothing else to install |

On Debian and Ubuntu, `sudo apt-get install clang lld make` installs the C toolchain. On macOS, `clang` from the Xcode Command Line Tools compiles for wasm32, and `brew install lld` adds `wasm-ld`.

The commands that talk to a cluster run `kubectl`, so its contexts, credentials, and `--context`, `--kubeconfig` and `--server` flags apply. Without `kubectl` on `PATH`, they stop with an error that names it.

## Releases and versions

Each release of kube32 has a version `X.Y.Z` and a Git tag `vX.Y.Z`, and its parts share that version:

| Part | Name |
| --- | --- |
| The `kube32` Python package | `kube32` `X.Y.Z` on PyPI |
| Firmware for the ESP32-S3, a merged image written at `0x0` | `kube32-firmware-esp32s3-X.Y.Z.bin` on the GitHub release |
| `kube32-sim` for macOS and Linux, arm64 and x86_64 | `kube32-sim-X.Y.Z-<macos\|linux>-<arm64\|x86_64>.tar.gz` on the GitHub release |
| `kube32-sim` container image | `ghcr.io/arterialist/kube32-sim:X.Y.Z` |
| Rust SDK | `kube32-sdk` `X.Y.Z` on crates.io |
| C SDK (the ABI header) | `kube32-c-sdk-X.Y.Z.tar.gz` on the GitHub release |

`kube32 flash` and `kube32 sim` download the firmware and the simulator of the release that matches the installed `kube32`, or of `--version X.Y.Z`. They check each download against the `.sha256` file published next to it and keep it in the cache directory: `~/Library/Caches/kube32` on macOS and `$XDG_CACHE_HOME/kube32` (default `~/.cache/kube32`) on Linux, or `KUBE32_CACHE_DIR`. `KUBE32_RELEASES_URL` replaces `https://github.com/arterialist/kube32/releases/download` for a mirror.

A `kube32` built from source between releases has a development version, such as `1.3.0.dev4+g1a2b3c4`. No release matches it, so `kube32 flash` takes `--version` or `--file`, and `kube32 sim` takes `--version` or a `kube32-sim` on `PATH`. `kube32 new --lang rust` makes a development build's projects depend on the SDK in the Git repository, or on a local checkout with `--sdk-path`.

## From source

A checkout of the repository runs the same commands as `tools/kube32-cli` without installing anything, and the serial gateway as `tools/kube32_serial_gateway.py`. To install the checkout's package as the `kube32` tool:

```sh
uv tool install --force ./python
```

`uv build python` builds the sdist and the wheel. See [CONTRIBUTING.md](https://github.com/arterialist/kube32/blob/main/CONTRIBUTING.md) for building the firmware and the simulator.
