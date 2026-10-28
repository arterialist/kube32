# Kube32

Kube32 is a small Kubernetes-compatible control plane and node agent for the
ESP32-S3, written in C++17. One firmware image runs on every board. The board
with PSRAM becomes the control plane: an HTTP API server that `kubectl` and
k9s talk to, a scheduler, a node lifecycle controller, a Deployment controller
and an in-memory object store. Every board runs a kubelet that executes pods
as WebAssembly modules in the wasm3 interpreter, with access to the display,
keypad, speaker and microSD card granted per pod.

The same sources build `kube32-sim`, a host simulator for macOS and Linux.
The repository includes Tessera, a demo issue tracker whose backend and
device front end run as Wasm pods on the cluster.

Kube32 is an experiment, not a production orchestrator. The API server has no
authentication, the boards keep cluster state in RAM, and pods are never
restarted. The README's
[What is not done](https://github.com/arterialist/kube32#what-is-not-done)
section has the full list.

## Where to start

| If you want to | Read |
| --- | --- |
| run it on your laptop, or on two boards | [Quick start](quickstart.md) |
| write a pod in Rust, C or AssemblyScript | [Writing app pods](app-development.md) |
| understand how it fits together | [Architecture overview](architecture/index.md) |
| know which kubectl features work | [Kubernetes API coverage](api-reference.md) |
| flash a board or use its console | [Flashing and the serial console](flashing-troubleshooting.md) |
| see the demo app | [Tessera demo](tessera.md) |

## What has been verified where

The [README](https://github.com/arterialist/kube32#what-runs-today) lists
what was measured on the two development boards: an ESP32-S3 devkit with
2 MB of PSRAM and an M5Stack Cardputer. Each page here says whether its
examples ran on a board or on the simulator. When a page says nothing, the
statement comes from reading the code and has not been measured.
