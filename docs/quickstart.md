# Quick start

Two ways to run Kube32: the host simulator, which needs nothing but a
compiler, and the two-board cluster it was built for.

## Host simulator

Requirements:

- CMake 3.16 or newer;
- a C++17 compiler (Apple Clang or GCC);
- Python 3, used by the build to embed the Wasm images;
- `kubectl`.

The commands and output below were run on macOS 27 with kubectl 1.33.

```
$ cmake -S . -B build
$ cmake --build build -j8
$ ctest --test-dir build
...
100% tests passed out of 23
```

On Linux, 5 of the 23 test programs currently fail or hang; see
[CONTRIBUTING.md](https://github.com/arterialist/kube32/blob/main/CONTRIBUTING.md#tests).

Start a standalone simulator: the API server on port 6443 and two simulated
nodes, `esp32-node-01` and `esp32-node-02`, in one process. `--ingress-port`
also serves the Tessera web app. The pod HTTP server is moved off its default
port 7000 because macOS's AirPlay Receiver listens there.

```
$ ./build/kube32-sim --ingress-port 8080 --pod-port 17000
...
[INIT] Seeded simulated cluster nodes (esp32-node-01, esp32-node-02).
[INIT] Node Lifecycle Controller active (NotReady: 15000ms, Evict: 30000ms).
[INIT] Autonomous scheduler started with LeastAllocated scoring.
[INIT] Deployment controller active.
[READY] Kube32 API Server listening on http://0.0.0.0:6443
...
[APP] pod HTTP server on :17000 (2 workers)
[APP] ingress on :8080 serving app 'tessera'
```

In a second terminal, give kubectl a kubeconfig of its own, so your other
contexts are not touched:

```
$ export KUBECONFIG=$PWD/build/kubeconfig
$ kubectl config set-cluster kube32-sim --server=http://127.0.0.1:6443
$ kubectl config set-context kube32-sim --cluster=kube32-sim
$ kubectl config use-context kube32-sim
$ kubectl get nodes
NAME            STATUS   ROLES    AGE   VERSION
esp32-node-01   Ready    worker   2s    v1.33.0-kube32
esp32-node-02   Ready    worker   2s    v1.33.0-kube32
$ kubectl apply -f examples/tessera.yaml
pod/tessera-core created
pod/tessera-hud created
service/tessera created
$ kubectl get pods
NAME           READY   STATUS    RESTARTS   AGE
tessera-core   1/1     Running   0          3s
tessera-hud    1/1     Running   0          3s
$ kubectl logs tessera-core
tessera: ready issues=10
tessera: seeding target=2400
tessera: seed chunk=10
[kube32] container started (linear memory 1280 KiB, limit 1280 KiB)
```

Open <http://127.0.0.1:8080/> and sign in as `mara`, password `demo`. Seeding
the 2,400 issues takes about four minutes on the simulator, because it ticks
pods once a second. The HUD pod's display and speaker calls go to the
simulator's log as `[DISPLAY x,y] ...` and `[TONE] ...` lines.

The simulator's node capacity, OS image and kernel version are fixed values
it declares, not measurements. Its "node memory" metric is the heap of the
whole simulator process.

Other simulator flags are listed in
[CONTRIBUTING.md](https://github.com/arterialist/kube32/blob/main/CONTRIBUTING.md#host-simulator-and-unit-tests).
To give the simulator a microSD card, set `KUBE32_SD_ROOT` to a directory
([microSD card](sd-card.md#host-simulator)).

## Two boards

The cluster this was developed on is an ESP32-S3 devkit with 2 MB of quad
PSRAM, which becomes the control plane, and an M5Stack Cardputer, which
becomes the worker ([Hardware](hardware-spec.md)).

Requirements: PlatformIO Core 6.1, Python 3 with `pyserial`, `kubectl`, and a
2.4 GHz Wi-Fi network both boards can join.

### 1. Build and flash

```
pio run -e esp32s3
bash scripts/flash.sh <serial port of the first board> 921600
bash scripts/flash.sh <serial port of the second board> 921600
```

Flash one board at a time, with nothing else holding its port. Over a native
USB port each flash took about 26 s. Read
[Flashing](flashing-troubleshooting.md#flashing) first if PlatformIO is not
installed under the path the script expects.

### 2. Provision over the serial console

Open each board's USB serial port at 115200 baud, for example with
`python3 -m serial.tools.miniterm --echo <port> 115200`. Opening the port
resets some boards; wait for the boot log to end. Every command below stores
a value in NVS and reboots the board.

On the Cardputer:

```
K32_CMD:BOARD:cardputer
K32_CMD:WIFI:<ssid>:<password>
```

On the devkit:

```
K32_CMD:WIFI:<ssid>:<password>
K32_CMD:AP:off
```

After the last reboot, the devkit's boot log has the line
`[CONTROL-PLANE] API server http://<address>:6443, scheduler + node lifecycle active`.
Give that address to the Cardputer:

```
K32_CMD:API:<address>
```

The Cardputer's screen then shows `ready`, its own address and the app URL.

### 3. Use the cluster

```
kubectl config set-cluster kube32 --server=http://<address>:6443
kubectl config set-context kube32 --cluster=kube32
kubectl config use-context kube32
kubectl get nodes
kubectl apply -f examples/tessera.yaml
kubectl get pods -o wide
```

Both nodes were Ready about 10 s after a reset. Open `http://<address>/` for
Tessera, and type on the Cardputer to see the keys arrive in the browser.

### Without an existing network

Without stored Wi-Fi credentials, the control plane starts an access point
called `Kube32-Cluster`; its password is set in `src/main_esp32.cpp`. A
worker without credentials joins that access point and looks for the API at
`192.168.4.1`. This mode exists in the code, but it is not the configuration
the measurements were made with: the Cardputer's connection to it was
unstable.
