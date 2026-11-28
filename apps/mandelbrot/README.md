# mandelbrot

mandelbrot renders one Mandelbrot image while it moves between nodes. The pod draws a row at a time and keeps the image in its linear memory. Moved to another node with `kube32 move`, it goes on at the next row there ([Pod migration](../../docs/migration.md)): `k32_restored` runs in place of `k32_init`, and the rows already drawn stay as they are. Each row keeps the node that drew it, and the rows of each platform take their own hue, so the finished image shows the nodes it visited as stripes: orange for an ESP32-S3 (`freertos/xtensa`), green for an ESP32-S31 (`freertos/riscv32`), blue for `kube32-sim` on Linux on arm64, violet for `kube32-sim` on a Mac.

```sh
make -C apps/mandelbrot         # writes images/sd/mandelbrot.wasm and build/mandelbrot.yaml
kubectl --context kube32 apply -f apps/mandelbrot/build/mandelbrot.yaml
kube32 move mandelbrot --to <node>
```

`build/mandelbrot.yaml` is `examples/mandelbrot.yaml` with the module inline in place of `"@mandelbrot.wasm"`: a Pod, a Service and an Ingress on the path `/mandelbrot`. The module travels with the pod to every node it moves to.

## Watching it

Open `http://<control-plane-ip>/mandelbrot/` on a board control plane, whose Ingress controller listens on port 80. A `kube32-sim` control plane serves Ingress only with `--ingress-port <port>`: then the page is at `http://<host>:<port>/mandelbrot/`. The Service selects the pod by its label, so the Ingress reaches it on whichever node it runs.

The page shows the image, scaled up, and reloads it once a second when a row was added. Under it are the rows done, the node the pod runs on, its platform and the number of moves, and a row a node with its hue, platform, board and the rows it drew. While the pod moves, requests get `503` and the page keeps the last image.

| Path | Answer |
| --- | --- |
| `/` (or any other path) | The page |
| `/image.bmp` | The image: an 8-bit BMP with 256 palette entries, 16 shades for each platform. Rows still to come are dark grey. |
| `/status` | JSON: `width`, `height`, `rows_done`, `rows_total`, `done`, `elapsed_ms` (pod time from the first row), `moves`, `restores`, `renders` (rows drawn since the start), the node the pod runs on (`node`, `os`, `arch`, `board`), `nodes` (each with its `rows` and `color`) and `rows`, a character a row: the index of its node in `nodes` (`0`-`9`, `a`-`z`), `.` for a row still to come |
| `/restart` | Clears the image and starts again; answers `/status` |

The pod matches requests on the path's last segment, so it answers the same under the Ingress prefix (`/mandelbrot/status`) and on its node's pod server:

```sh
curl -s -H 'X-Kube32-Pod: default/mandelbrot' http://<node-ip>:7000/status
```

## Moving it

```sh
kube32 move mandelbrot --to <node>
kubectl --context kube32 get events --field-selector reason=Moved
```

`kube32 move` creates a Move Checkpoint; the `Moved` event gives the time from the freeze to the pod running on the target. The pod's clock stops while it is frozen, so the time a move takes adds no gap to the render. The annotation `kube32.io/migration: OnDrain` also moves the pod when its node is drained.

The pod asks for no hardware and holds no file open, so nothing keeps it from moving to any node with 75 KiB free (see [Memory](#memory)).

## Where it runs

`k32_init` and `k32_restored` ask the API server where the pod runs, two calls of at most 2 s each:

1. `GET /api/v1/namespaces/<POD_NAMESPACE>/pods/<POD_NAME>` as a Table (`Accept: application/json;as=Table;v=v1;g=meta.k8s.io`): the `NODE` cell.
2. `GET /api/v1/nodes/<node>`: the labels `kubernetes.io/os`, `kubernetes.io/arch` and `kube32.io/board`. They come first in the Node, so the head of the answer holds them.

The answer holds until the next restore. After a restore the rows wait for the lookup, so no row goes to the node the pod left. A lookup that fails is tried again from `k32_tick`, three times a second apart and then every 10 s; after the third failure the rows render as the node `unknown`, in grey. The log names the call and its result: `mandelbrot: no node: GET http://...: -110`.

A platform's hue is fixed for `freertos/xtensa`, `freertos/riscv32`, `linux/arm64`, `darwin/arm64` and `linux/amd64`, and comes from a hash of `<os>/<arch>` for any other, so a new architecture gets a colour of its own. The image has room for 15 platforms.

## Environment

| Variable | Meaning |
| --- | --- |
| `API_URL` | The API server. The manifest sets `http://$(KUBERNETES_SERVICE_HOST):$(KUBERNETES_SERVICE_PORT)`, from the variables the kubelet sets in every pod. |
| `API_TOKEN` | Bearer token for the API server, when it has one. |
| `POD_NAME` | The pod's name, from `fieldRef: metadata.name`. |
| `POD_NAMESPACE` | Its namespace (default `default`). |
| `WIDTH`, `HEIGHT` | Image size, `16` to `1024` each (default 256x192). The height shrinks to the rows the memory limit holds. |
| `ROWS_PER_TICK` | Rows a render step draws, `1` to `64` (default `1`). |
| `TICK_MS` | Pod time between render steps, `10` to `60000` ms (default `300`). The pod's `kube32.io/tick-interval` (`100ms` in the manifest) has to be at most this. |
| `MAX_ITER` | Iterations a pixel, `8` to `1024` (default `128`). |

The pod needs the `SERVICE_CALL` capability for the lookup.

## Timing

A render step draws `ROWS_PER_TICK` rows every `TICK_MS` of pod time, on every node alike: the defaults draw 192 rows in 58 s. With a move every 10 s the image takes six stops of about 32 rows each, and each stripe's height is the time the pod spent on its node.

The arithmetic is 32-bit fixed point (4.12), so every architecture computes the same pixels. A row of 256 pixels at 128 iterations runs 175,000 wasm3 operations on average, metering included, and 379,000 at most, through the main cardioid: 18 ms and 40 ms on an ESP32-S3 that runs 9.6 million operations a second. The whole image is 33.6 million operations, 3.5 s of the 58 s on such a board, so the slowest node keeps the pace. `GET /image.bmp` streams 50 KB in 490,000 operations, 51 ms on the same board.

## Memory

The module is 10 KB and links its stack first. The pod's `32Ki` memory limit holds the 1 KiB shadow stack, 6.2 KiB of data and buffers, and the image after them: a byte a row for its node and half a byte a pixel, 24.2 KiB at 256x192. `k32_init` sizes the image from `__heap_base` to the limit (`k32_mem_stats`), so a larger limit holds a larger image.

`kube32.io/wasm-stack: 1Ki` sets the operand stack, which the deepest call needs a few hundred bytes of. The API server gives the pod a Pod Overhead of `43Ki` for the inline module, so the scheduler counts 75 KiB. That fits an AtomS3 Lite's 80 KiB pod pool when no other pod runs there. `build/kube32-podsize images/sd/mandelbrot.wasm --memory 32 --stack 1024` prints the estimate from the module itself, 73 KiB.
