# Quick start

Run Kube32 on the host simulator or on a two-board ESP32-S3 cluster.

## Host simulator

Requirements:

- CMake 3.16 or newer
- C++17 compiler, Apple Clang or GCC
- Python 3
- `kubectl`

Build the simulator and run the unit test suite:

```sh
cmake -S . -B build
cmake --build build -j8
ctest --test-dir build --output-on-failure
```

Start a standalone simulator with the API server on port 6443, the Ingress controller on port 8080, the pod HTTP server on port 17000, and two simulated nodes, `esp32-node-01` as a `generic` control plane and `esp32-node-02` as a `cardputer` worker:

```sh
mkdir -p build/sdcard
KUBE32_SD_ROOT=$PWD/build/sdcard ./build/kube32-sim --ingress-port 8080 --pod-port 17000
```

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

In a second terminal, create a kubeconfig and inspect the nodes:

```sh
export KUBECONFIG=$PWD/build/kubeconfig
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

Generate the sample Tessera dataset, push it to the worker node storage, and deploy `examples/tessera.yaml`:

```sh
make -C apps/tessera dataset
tools/kube32-cli push apps/tessera/build/card/kube32/volumes/tessera-db --node esp32-node-02 --path volumes/tessera-db
kubectl apply -f examples/tessera.yaml
kubectl get pods
```

Open <http://127.0.0.1:8080/> and sign in as `mara` with password `demo`. Display and speaker calls from `tessera-hud` print to the simulator log as `[DISPLAY x,y]` and `[TONE]` lines.

## Two boards

An ESP32-S3 devkit with 2 MB quad PSRAM runs the control plane, and an M5Stack Cardputer with a FAT32 microSD card runs as the worker. See [Hardware](hardware-spec.md).

Requirements:

- PlatformIO Core 6.1
- Python 3 with `pyserial`
- `kubectl`
- A 2.4 GHz Wi-Fi network

### 1. Build and flash

Build the firmware and flash one board at a time with no other process holding the serial port:

```sh
pio run -e esp32s3
bash scripts/flash.sh <control-plane-port> 921600
bash scripts/flash.sh <worker-port> 921600
```

See [Flashing and the serial console](flashing-troubleshooting.md#flashing) for port and driver details.

### 2. Provision over the serial console

Open each board's USB serial port at 115200 baud:

```sh
python3 -m serial.tools.miniterm --echo <serial-port> 115200
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

Without stored Wi-Fi credentials, the control plane starts a WPA2 softAP named `Kube32-Cluster` at `192.168.4.1`. The control plane generates a password on first boot; run `K32_CMD:APPASS` on its console to print it. Send `K32_CMD:APPASS:<password>` to each worker so workers without other Wi-Fi credentials can join the access point.

To run the cluster without a local network, keep both boards on that access point. Send `K32_CMD:WIFI:` to each board to erase stored credentials, and send `K32_CMD:API:auto` to the worker. Connect the control plane to the host computer over USB and run `tools/kube32_serial_gateway.py`. The gateway serves the API server at `http://127.0.0.1:6443` and the ingress at `http://127.0.0.1:8080`. Use `http://127.0.0.1:6443` as the server in step 3. If a worker on a connected link receives no response to pod syncs for 60 seconds, it disconnects from the access point and reconnects. If the control plane does not answer a minute later, the worker restarts its Wi-Fi driver (at most once a minute). This clears the 802.11w keys of a restarted access point and recovers the connection when the control plane restarts.

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

Generate the Tessera dataset, push it to the Cardputer's microSD card, and apply the manifest:

```sh
make -C apps/tessera dataset
tools/kube32-cli push apps/tessera/build/card/kube32/volumes/tessera-db --node <worker-node> --path volumes/tessera-db
kubectl apply -f examples/tessera.yaml
kubectl get pods -o wide
```

Open `http://<control-plane-ip>/` and sign in as `mara` with password `demo`. Keystrokes typed on the Cardputer appear live in the web interface.

