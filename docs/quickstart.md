# Quick start

Run Kube32 on the host simulator or on a two-board ESP32-S3 cluster. Both use the `kube32` command, installed with `uv tool install kube32` ([Install](install.md)), and `kubectl`.

## Host simulator

The container image runs the latest release with no build. It serves the API on port 6443 and the Ingress on port 8080; configure `kubectl` as below:

```sh
docker run --rm -p 6443:6443 -p 8080:8080 ghcr.io/arterialist/kube32-sim
```

To build the simulator from source instead, you need:

- `kube32`
- `kubectl`
- macOS or Linux on arm64 or x86_64, or Docker

Start a standalone simulator with the API server on port 6443, the Ingress controller on port 8080, the pod HTTP server on port 17000, and two simulated nodes, `esp32-node-01` as a `generic` control plane and `esp32-node-02` as a `cardputer` worker:

```sh
mkdir -p sdcard
KUBE32_SD_ROOT=$PWD/sdcard kube32 sim --ingress-port 8080 --pod-port 17000
```

`kube32 sim` runs `kube32-sim` from `PATH`, or else downloads the release's `kube32-sim` for this machine once. `kube32 sim --docker` runs the release's container image instead. Arguments that `kube32` does not take go to `kube32-sim`:

| Flag | Default | Description |
| --- | --- | --- |
| `--role` | `standalone` | Process role: `standalone`, `control-plane`, or `worker` |
| `--port` | `6443` | API server port |
| `--ingress-port` | `0` | Ingress controller HTTP port, `0` disables |
| `--pod-port` | `7000` | Pod HTTP server port for request pods and node storage |
| `--kubelet-name` | `esp32-node-01` | Control-plane node name in `standalone` mode |
| `--workers` | `esp32-node-02` | Comma-separated in-process worker node names |
| `--board` | `generic` | `kube32.io/board` label for the primary kubelet |
| `--worker-board` | `cardputer` | `kube32.io/board` label for in-process workers |
| `--psram` | `1284Ki` | Host memory reserved as `esp32.io/psram`, `0` on `worker` |
| `--data-dir` | `/tmp/kube32_<node>_data` | Node storage directory when `KUBE32_SD_ROOT` is unset |
| `--enable-flash` | off | Persist the object store journal to `<storage>/kube32/store/journal.img` |
| `--token` | `$KUBE32_API_TOKEN` | API bearer token |

In a second terminal, create a `kubectl` context and inspect the nodes:

```sh
kubectl config set-cluster kube32-sim --server=http://127.0.0.1:6443
kubectl config set-context kube32-sim --cluster=kube32-sim
kubectl config use-context kube32-sim
kubectl get nodes
```

```text
NAME            STATUS   ROLES           AGE   VERSION
esp32-node-02   Ready    worker          8s    v1.33.0-kube32
esp32-node-01   Ready    control-plane   8s    v1.33.0-kube32
```

Create a C app, build it, deploy it, and call it through the pod HTTP server ([Writing pods](app-development.md)):

```sh
kube32 new hello --lang c
kube32 build hello
kube32 deploy hello/build/hello.wasm
curl -H 'X-Kube32-Pod: default/hello' http://127.0.0.1:17000/
```

```text
hello: request 1, GET /
```

In a checkout of the repository, generate the sample Tessera dataset, push it to the worker node storage, and deploy `examples/tessera.yaml`:

```sh
make -C apps/tessera dataset
kube32 push apps/tessera/build/card/kube32/volumes/tessera-db --node esp32-node-02 --path volumes/tessera-db
kubectl apply -f examples/tessera.yaml
kubectl get pods
```

Open <http://127.0.0.1:8080/> and sign in as `mara` with password `demo`. Display and speaker calls from `tessera-hud` print to the simulator log as `[DISPLAY x,y]` and `[TONE]` lines.

### Simulator from source

Building the simulator needs CMake 3.16 or newer and a C++17 compiler, Apple Clang or GCC. Build it, run the unit test suite, and start it:

```sh
cmake -S . -B build
cmake --build build -j8
ctest --test-dir build --output-on-failure
KUBE32_SD_ROOT=$PWD/build/sdcard ./build/kube32-sim --ingress-port 8080 --pod-port 17000
```

With `build/` on `PATH`, `kube32 sim` runs this build.

## Two boards

An ESP32-S3 devkit with 2 MB quad PSRAM runs the control plane, and an M5Stack Cardputer with a FAT32 microSD card runs as the worker. See [Hardware](hardware-spec.md).

Requirements:

- `kube32`
- `kubectl`
- A 2.4 GHz Wi-Fi network

### 1. Flash

Connect one board at a time, with no other program holding its serial port, and flash it:

```sh
kube32 flash
```

`kube32 flash` downloads the firmware of the release that matches the installed `kube32`, checks it against its SHA-256, and writes it over USB with esptool. It finds the board by its USB vendor ID; with several boards connected, pass `--port <serial-port>`. See [Flashing and the serial console](flashing-troubleshooting.md#flashing) for ports, drivers, and flashing a build from source.

The [browser installer](flash.md) writes the latest release instead, with no tools, and sets up each board's Wi-Fi.

### 2. Provision over the serial console

Open each board's serial console. `kube32 flash` offers to open it when it finishes:

```sh
kube32 console --port <serial-port>
```

Wait for the boot log to finish before sending commands. Each provisioning command writes to NVS and reboots the board.

On the control plane devkit, configure Wi-Fi and disable the fallback softAP:

```text
K32_CMD:WIFI:<ssid>:<password>
K32_CMD:AP:off
```

After reboot, read `<control-plane-ip>` from the `[CONTROL-PLANE] API server http://<control-plane-ip>:6443` line in the boot log, or run `K32_CMD:STATUS` on the console.

On the Cardputer worker, set the board profile, Wi-Fi credentials, and control plane address:

```text
K32_CMD:BOARD:cardputer
K32_CMD:WIFI:<ssid>:<password>
K32_CMD:API:<control-plane-ip>
```

After reboot, the Cardputer LCD displays `ready`, its node IP, and `http://<control-plane-ip>/`.

The control plane announces its API server address to workers on the same network over ESP-NOW. A worker whose control plane address gets no answer for 30 seconds while the control plane announces a different one stores the announced address and restarts. After a network change or a new DHCP lease, workers find the control plane again without `K32_CMD:API`.

Without stored Wi-Fi credentials, the control plane starts a WPA2 softAP named `Kube32-Cluster` at `192.168.4.1`. The control plane generates a password on first boot; run `K32_CMD:APPASS` on its console to print it. Send `K32_CMD:APPASS:<password>` to each worker so workers without other Wi-Fi credentials can join the access point.

To run the cluster without a local network, keep both boards on that access point. Send `K32_CMD:WIFI:` to each board to erase stored credentials, and send `K32_CMD:API:auto` to the worker. Connect the control plane to the host computer over USB and run `kube32 gateway`. The gateway serves the API server at `http://127.0.0.1:6443` and the ingress at `http://127.0.0.1:8080`. Use `http://127.0.0.1:6443` as the server in step 3. If a worker on a connected link receives no response to pod syncs for 60 seconds, it disconnects from the access point and reconnects. If the control plane does not answer a minute later, the worker restarts its Wi-Fi driver (at most once a minute). This clears the 802.11w keys of a restarted access point and recovers the connection when the control plane restarts.

### 3. Configure kubectl and deploy Tessera

Configure a `kubectl` context using plain HTTP:

```sh
kubectl config set-cluster kube32 --server=http://<control-plane-ip>:6443
kubectl config set-context kube32 --cluster=kube32
kubectl config use-context kube32
kubectl get nodes
```

If bearer token authentication is enabled with `K32_CMD:TOKEN:<token>`, export the TLS certificate with `K32_CMD:CERT` and configure HTTPS:

```sh
kubectl config set-cluster kube32 --server=https://<control-plane-ip>:6443 --tls-server-name=kube32-apiserver --certificate-authority=<ca.crt> --embed-certs
kubectl config set-credentials kube32 --token=<token>
kubectl config set-context kube32 --cluster=kube32 --user=kube32
kubectl config use-context kube32
```

See [Cluster access](cluster-access.md) for authentication, TLS, and serial gateway options.

In a checkout of the repository, generate the Tessera dataset, push it to the Cardputer's microSD card, and apply the manifest:

```sh
make -C apps/tessera dataset
kube32 push apps/tessera/build/card/kube32/volumes/tessera-db --node <worker-node> --path volumes/tessera-db
kubectl apply -f examples/tessera.yaml
kubectl get pods -o wide
```

Open `http://<control-plane-ip>/` and sign in as `mara` with password `demo`. Keystrokes typed on the Cardputer appear live in the web interface.

### Firmware from source

Building the firmware needs PlatformIO Core 6.1. Build it and flash one board at a time:

```sh
pio run -e esp32s3
bash scripts/flash.sh <serial-port> 921600
```
