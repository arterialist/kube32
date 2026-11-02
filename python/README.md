# kube32

Command-line tools for [kube32](https://github.com/arterialist/kube32), a Kubernetes-compatible control plane and WebAssembly pod runtime for ESP32-S3 microcontrollers.

```sh
uv tool install kube32      # or run any command with: uvx kube32 ...
```

| Command | Purpose |
| --- | --- |
| `kube32 flash` | Write the release firmware to an ESP32-S3 board over USB |
| `kube32 console` | Open a board's serial console |
| `kube32 sim` | Run the `kube32-sim` host simulator |
| `kube32 new <dir> --lang c\|rust` | Create an app project |
| `kube32 build [dir]` | Compile an app to `.wasm` and check it against the pod ABI |
| `kube32 deploy <file.wasm>` | Create a Pod that carries the module |
| `kube32 gateway` | Serve a board's API server and ingress on local ports over USB |
| `kube32 firmware`, `devices`, `checkpoint`, `move` | Roll out firmware, read Devices, move pods |
| `kube32 version` | Print this tool's version and the cluster's |

Commands that talk to a cluster run [`kubectl`](https://kubernetes.io/docs/tasks/tools/), so it must be on `PATH`. Building C apps needs `clang` with the wasm32 target and `wasm-ld` (or `rust-lld`), and Rust apps need `cargo` with the `wasm32-unknown-unknown` target.

Documentation: <https://arterialist.github.io/kube32/install/>. License: Apache-2.0.
