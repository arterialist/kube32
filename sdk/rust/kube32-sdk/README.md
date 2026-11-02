# kube32-sdk

`no_std` Rust bindings for the [kube32](https://github.com/arterialist/kube32) WebAssembly pod ABI. kube32 runs pods as WebAssembly modules on ESP32-S3 boards and in the `kube32-sim` host simulator, managed with `kubectl`.

A request-handling pod is a function and one macro invocation:

```rust
#![no_std]
use kube32_sdk::{kube32_app, Request, Response};

fn handle(req: &Request) -> i32 {
    let mut resp = Response::json(200);
    resp.write_str("{\"path\":").write_json_str(req.path).write_str("}");
    resp.end()
}

kube32_app! { handle: handle, }
```

Add the SDK to a library crate whose `Cargo.toml` sets `crate-type = ["cdylib"]` under `[lib]`, then build it for `wasm32-unknown-unknown` with one 64 KiB page of linear memory and a 16 KiB shadow stack:

```sh
cargo add kube32-sdk
rustup target add wasm32-unknown-unknown
RUSTFLAGS="-C link-arg=-zstack-size=16384 -C link-arg=--initial-memory=65536 -C link-arg=--max-memory=65536" \
    cargo build --release --target wasm32-unknown-unknown
```

## Features

- `panic-handler` (default): a `#[panic_handler]` that logs through `k32_log` and traps.
- `hardware`: the display, keypad, speaker and infrared imports.
- `fs`: the `k32_fs_*` imports for pod volumes.
- `linalg`: native products for quantized neural networks (`k32_matvec_*`, `k32_vecmat_f32_i8`, `k32_softmax_f32`).

The [pod guide](https://arterialist.github.io/kube32/app-development/) covers the request format, limits, entry-point pods and manifests.

## License

Apache-2.0
