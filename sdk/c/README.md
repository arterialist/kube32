# kube32 C SDK

Build kube32 pods from C. A pod is a WebAssembly module that includes
`runtime/k32_app_abi.h`, exports `k32_alloc` and `k32_handle`, and answers
requests through the `k32_resp_*` host functions.

## Requirements

- `clang` with the `wasm32` target (on macOS: `brew install llvm`, then pass `CLANG=$(brew --prefix llvm)/bin/clang`)
- A WebAssembly linker: `wasm-ld` from LLVM's `lld`, or `rust-lld` from a Rust toolchain

## Build

```sh
make
```

This compiles every `.c` file in the directory into `build/hello.wasm`. `make APP=<name>` names the module, and `MEMORY` and `STACK` set the linear memory and shadow stack sizes.

## Deploy

Check the module and deploy it with `kube32-cli`, or inline it in a manifest as `kube32.io/wasm-base64`:

```sh
kube32-cli check build/hello.wasm
kube32-cli deploy build/hello.wasm --name hello
```

`include/runtime/k32_app_abi.h` documents every host function. The [pod guide](https://arterialist.github.io/kube32/app-development/) covers the request format, limits, and manifests.
