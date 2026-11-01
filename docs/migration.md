# Pod migration

## Snapshot format

A snapshot holds the state of a request pod between two host calls. No Wasm frame is live at that point, so the state of the module is its linear memory, its mutable globals and its tables. The snapshot also holds what the node keeps for the pod: its clock, the channels it opened and the events that arrived while it was frozen. The compiled code is not part of it. The destination instantiates the same module again and writes the state back.

The format is `K32S`, version 1. All numbers are little-endian. No field has the size of a pointer, so the same bytes restore on the ESP32-S3, arm64 and x86_64.

| Part | Layout |
| --- | --- |
| Header, 68 bytes | `"K32S"`, `u16` version, `u16` flags (0), image SHA-256 (32 bytes), `u32` memory bytes, `u32` memory pages, `u64` pod time in ms, `u64` Unix time in ms (0 when the node clock is unset), `u32` section count |
| Section | `u16` type, `u32` length, then `length` bytes |
| Trailer, 32 bytes | SHA-256 of every byte before it |

| Type | Section | Content |
| --- | --- | --- |
| 1 | `MEMORY` | The linear memory, `memory bytes` long |
| 2 | `GLOBALS` | For each mutable global of the module: `u32` index, `u8` value type (`0x7F` i32, `0x7E` i64, `0x7D` f32, `0x7C` f64, `0x70` funcref), `u64` bits |
| 3 | `TABLE` | `u32` table index, `u32` size, then a `u32` function index for each entry (`0xFFFFFFFF` for null). One section for each table |
| 4 | `CHANNELS` | For each channel the pod opened with `kube32_channel_open`: `u32` handle, `u16` name length, name |
| 5 | `EVENTS` | For each held event: `u16` topic length, topic, `u32` payload length, payload |

The image SHA-256 is the digest of the module's bytes. Memory bytes and memory pages differ for a pod whose memory limit is below one 64 KiB page: such a pod has one page with only the limit backed. Pod time is the value `k32_time_ms` returned when the pod froze. A reader skips a section of a type it does not know.

A restore checks the snapshot as it streams in. It refuses the snapshot, with a message that names the cause, when:

* the magic is not `K32S` or the version is not one the node reads,
* the image SHA-256 differs from the digest of the module being deployed,
* the pod's memory limit gives fewer bytes than the snapshot holds,
* a global or a table does not match the module,
* the trailer does not match the bytes before it,
* the stream ends before the trailer.

Linear memory streams from the module to the socket on the source, and from the socket into the module on the destination. Neither side copies it into a second buffer. A restore needs memory for the module, its linear memory and a few hundred bytes for the small sections.

## Kubelet endpoints

The kubelet endpoint of every node takes checkpoints of its pods. The API server reaches it through the node proxy, also on the control plane's own node:

```text
POST /api/v1/nodes/<node>/proxy/checkpoint/<namespace>/<pod>/<container>?leave=running|frozen&timeoutMs=<ms>
POST /api/v1/nodes/<node>/proxy/thaw/<namespace>/<pod>/<container>
```

`POST /checkpoint` freezes the pod once its current call returns, then answers its snapshot.

| Parameter | Default | Meaning |
| --- | --- | --- |
| `leave` | `running` | `running` thaws the pod after the snapshot. `frozen` keeps it frozen for a move |
| `timeoutMs` | `10000` | How long the current call may take to return, 1 to 60000 ms |

| Status | Body |
| --- | --- |
| `200` | The snapshot, `application/octet-stream` with `Content-Length` and the headers below |
| `404` | `{"reason":"NotFound",...}`: no such pod or container on the node |
| `409` | `{"reason":"NotMovable","blockers":[...]}`: the pod cannot move. Every reason is listed and the pod keeps running |
| `504` | `{"reason":"Timeout","blockers":["call k32_handle still running after 10000 ms"]}`: a call outlasted `timeoutMs`. The pod keeps running |

| Header | Value |
| --- | --- |
| `X-Kube32-Image-Sha256` | SHA-256 of the pod's module |
| `X-Kube32-Snapshot-Sha256` | The snapshot's trailer, the SHA-256 of the bytes before it |
| `X-Kube32-Pod-Time-Ms` | The pod's clock at the snapshot |

A frozen pod gets no calls. Requests get `503` with `Retry-After: 1`. Ticks stop. Events for the pod are held, at most 8, and later ones are dropped and counted. The kubelet reports the pod `Ready=False` within 2 s, so Services stop sending it requests. The same frozen pod gives the same snapshot every time, so a failed transfer can be retried.

`POST /thaw` resumes a frozen pod: `200 {"thawed":true}`, or `404` when the pod is not frozen. Held events are delivered first.

The node refuses a checkpoint of a pod that cannot move:

| Blocker | Cause |
| --- | --- |
| `entry point _start runs on its own thread` | The module exports `_start`, `app_main` or `main` |
| `grant <resource>` | A request or limit for a hardware resource: `hardware.kube32.io/*` other than `ipc`, or `esp32.io/*` |
| `grant capability <NAME>` | `securityContext.capabilities.add` names `DISPLAY`, `KEYBOARD`, `SPEAKER`, `SD` or `CHIP_TEMP` |
| `grant kube32.io/i2c-device` | The pod has an I2C device grant |
| `volume claim <claim> on node storage` | A `persistentVolumeClaim` or `kube32.io/sd` volume |
| `annotation kube32.io/migration: Never` | The pod opts out |
| `open file <path> (descriptor <n>)` | The pod holds a `k32_fs` descriptor open between calls |

A pod with the annotation `kube32.io/restore-from: <checkpoint>` starts from that Checkpoint's snapshot. The kubelet streams `GET /apis/kube32.io/v1/namespaces/<namespace>/checkpoints/<checkpoint>/data` into the new instance. It then restores the pod's clock, reopens its channels under the same handles and calls `k32_restored` once instead of `k32_init`. The held events are delivered after that. A container that restarts later on the node starts fresh with `k32_init`. A restore that fails ends the pod `Failed`, with the container terminated with reason `RestoreError` and a message such as `cannot restore the snapshot: the snapshot's trailer does not match its contents (damaged in transfer or storage)`. The kubelet does not restart such a container.

## Checkpoint objects and data

`kube32.io/v1` `Checkpoint` (plural `checkpoints`, short name `ckpt`) is a namespaced object with a `status` subresource and a `data` subresource.

| Field | Meaning |
| --- | --- |
| `spec.podName` | The pod, required |
| `spec.containerName` | Its container. Default: the only container |
| `spec.action` | `Save` (the default) or `Move` |
| `spec.target` | For `Move`: `nodeName` or `nodeSelector` |
| `status.phase` | `Pending`, `Saving`, `Saved`, `Moving`, `Completed` or `Failed` |
| `status.nodeName`, `status.takenAt` | Where and when the snapshot was taken |
| `status.sizeBytes`, `status.snapshotSha256`, `status.imageSha256`, `status.podTimeMs` | Facts of the stored snapshot |
| `status.targetNode`, `status.newPodName`, `status.message`, `status.blockers` | The outcome of a move |

The API server keeps `status` out of creates and of writes to the object. Writes to `checkpoints/<name>/status` change only `status`.

The snapshot itself is the Checkpoint's data:

```text
GET /apis/kube32.io/v1/namespaces/<namespace>/checkpoints/<name>/data
PUT /apis/kube32.io/v1/namespaces/<namespace>/checkpoints/<name>/data
```

`PUT` takes the snapshot as `application/octet-stream` with a `Content-Length`. The API server checks its magic, version and trailer and answers `422` when they are wrong. It then sets `status.sizeBytes`, `status.snapshotSha256`, `status.imageSha256` and `status.podTimeMs` from the snapshot and answers the Checkpoint. `GET` answers the snapshot, or `404` while the Checkpoint has none.

Snapshot data lives in the control plane's memory, outside the object store: PSRAM on the board, with 256 KiB for all snapshots, and 16 MiB in the simulator. Storing a snapshot that does not fit drops the oldest other snapshots first. A snapshot larger than the whole space gets `413`. Deleting a Checkpoint drops its data. A control plane restart loses the data and keeps the Checkpoint objects. A snapshot holds whatever the pod holds, environment values included, so the data has the same access rules as a Secret.

`kubectl get checkpoints` prints `NAME`, `POD`, `ACTION`, `PHASE`, `NODE`, `SIZE` and `AGE`. `-o wide` adds `TARGET`, `NEW POD` and `MESSAGE`.

A pod moves by hand with these steps:

```sh
API=https://<control plane>:6443
AUTH="Authorization: Bearer $TOKEN"
kubectl apply -f - <<'EOF'
apiVersion: kube32.io/v1
kind: Checkpoint
metadata: {name: echo-move}
spec: {podName: echo, action: Move}
EOF
curl -k -X POST -H "$AUTH" -o echo.k32s "$API/api/v1/nodes/<source>/proxy/checkpoint/default/echo/echo?leave=frozen"
curl -k -X PUT -H "$AUTH" -H "Content-Type: application/octet-stream" --data-binary @echo.k32s \
  "$API/apis/kube32.io/v1/namespaces/default/checkpoints/echo-move/data"
kubectl delete pod echo
kubectl apply -f echo-restored.yaml   # spec.nodeName: <destination>, annotation kube32.io/restore-from: echo-move
```
