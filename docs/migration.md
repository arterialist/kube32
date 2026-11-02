# Pod migration

A running request pod can move to another node and continue with its state. The pod does not start again: its counters, buffers and clock carry over, and `k32_init` does not run a second time. A pod moves on request, when a node is drained, when its node runs short of memory, and when a faster node joins or leaves. A pod with periodic checkpoints comes back on another node when its node is lost.

Nodes of any architecture take part. A pod saved on an ESP32-S3 restores on another board or on a `kube32-sim` worker on a laptop, and the other way round.

## What moves

A request pod moves: a module that exports `k32_handle`, `k32_tick` or `k32_event` and has no entry point. Kube32 freezes it between two host calls. No Wasm frame is live then, so the pod's state is its linear memory, its mutable globals and its tables. The node it moves to loads the same module and writes that state back.

The state carries:

- linear memory, mutable globals and tables
- the pod's clock: `k32_time_ms` continues from its value at the snapshot and never goes backward
- the channels the pod opened with `kube32_channel_open`, reopened by name with the same handles
- events that arrive while the pod is frozen, delivered after the restore

The environment, event topics, tick interval, assets and limits come from the pod spec, as at any start.

The state does not carry HTTP requests in progress. A frozen pod answers `503` with `Retry-After: 1` and is not Ready, so Services stop routing to it. Random number state and effects the pod already caused stay behind. A move has no gap between the snapshot and the restore. A restore from a periodic checkpoint repeats what the pod did after that checkpoint.

The replacement runs the same image. It keeps the image reference of the pod spec (`wasm://...` or `kube32.io/wasm-sha256`), and a restore refuses a snapshot taken from a different module.

## What keeps a pod from moving

A move that cannot happen is refused. The Checkpoint turns `Failed`, `status.blockers` names every reason, a `MoveFailed` event repeats them, and the pod keeps running where it is.

| Blocker | Found by | Cause |
| --- | --- | --- |
| `grant <resource>` | Control plane, from the pod spec | A request or limit for a hardware resource: `hardware.kube32.io/*` other than `ipc`, or `esp32.io/*` |
| `grant capability <NAME>` | Control plane | `securityContext.capabilities.add` names `DISPLAY`, `KEYBOARD`, `SPEAKER`, `SD` or `CHIP_TEMP` |
| `grant kube32.io/i2c-device` | Control plane | The pod has an I2C device grant |
| `volume claim <claim> on node storage` | Control plane | A `persistentVolumeClaim` or `kube32.io/sd` volume |
| `annotation kube32.io/migration: Never` | Control plane | The pod opts out |
| `entry point _start runs on its own thread` | Control plane for embedded images, the node for the others | The module exports `_start`, `app_main` or `main` |
| `open file <path> (descriptor <n>)` | Node | The pod holds a `k32_fs` descriptor open between calls |
| `call k32_handle still running after <ms> ms` | Node | A call outlasted the freeze timeout (10 s). The pod keeps running |

Write pods to move: keep state in linear memory and globals, close files between calls, and export `k32_restored` to refresh anything that depends on the node. See [Writing app pods](app-development.md).

## Moves

Move a pod with `kube32-cli`:

```
kube32-cli move echo --to devkit
```

or create a Checkpoint:

```yaml
apiVersion: kube32.io/v1
kind: Checkpoint
metadata:
  name: echo-move
spec:
  podName: echo
  action: Move
  target:
    nodeName: devkit        # or nodeSelector, or none: the scheduler picks
```

The Checkpoint controller on the control plane carries out the move:

1. It checks the pod spec for blockers and picks the target. The target is `spec.target.nodeName`, or the node the scheduler picks with `spec.target.nodeSelector` added to the pod's own. It is never the node the pod runs on, and never a node that is cordoned or not Ready. Without a target that fits, the move fails with the scheduler's reason.
2. It asks the pod's kubelet for a snapshot with the pod left frozen and stores the bytes as the Checkpoint's data.
3. It replaces the pod in one write: the same name, spec, labels and owners, a new uid, `spec.nodeName` set to the target and the annotation `kube32.io/restore-from` naming the Checkpoint. The source kubelet stops the frozen pod, which is no longer bound to it. The target kubelet restores the pod from the Checkpoint's data.
4. Once the replacement runs, the move is `Completed` and the annotation is removed. A later restart of the container starts it afresh.

A pod a Deployment owns moves the same way. The Deployment counts one pod at every step: no pod is added and none goes terminating, so it never starts an extra pod that would begin without the state.

When the replacement fails to restore (`RestoreError`), fails, or does not run within 60 s, the pod goes back to its source node and restores there from the same Checkpoint. The Checkpoint turns `Failed` with the reason. A failure before the replacement, such as data that could not be stored, thaws the pod where it runs.

| Phase | Meaning |
| --- | --- |
| `Pending` | Created, not started |
| `Saving` | The node freezes the pod and sends its state |
| `Saved` | A Save checkpoint is stored |
| `Moving` | The replacement is restoring on the target |
| `Completed` | The replacement runs on the target |
| `Failed` | Refused or rolled back, see `status.message` and `status.blockers` |

The controller records each decision as an event on the pod:

| Event | When |
| --- | --- |
| `Checkpointed` | A Save checkpoint is stored |
| `CheckpointFailed` | A Save checkpoint failed |
| `Moving` | The replacement is bound to the target |
| `Moved` | The replacement runs. The message gives the time from the freeze |
| `MoveFailed` | A move was refused or rolled back, or a restore failed |
| `Restored` | A pod restored outside a move runs: after a node loss or a rollback. The message gives the checkpoint's age |

`kube32-cli checkpoint <pod>` saves a pod's state without moving it. The pod keeps running.

## Drain and evictions

`kubectl cordon`, `kubectl uncordon` and `kubectl drain` work as in Kubernetes:

```
kubectl drain cardputer --ignore-daemonsets
kubectl uncordon cardputer
```

`kubectl drain` evicts each pod through `POST /api/v1/namespaces/<ns>/pods/<name>/eviction`. A running pod that can move, by its spec and its `kube32.io/migration` policy, moves to another node. Its replacement has a new uid, which is what `kubectl drain` waits for. Any other pod is deleted, and its owner starts a new one elsewhere. Pods without an owner need `--force`, as in Kubernetes. They move with their names when they can.

When a move that an eviction asked for fails before the replacement, the pod is deleted as an eviction deletes it, so the drain completes.

A `kube32-sim` worker stopped with Ctrl-C or `SIGTERM` drains itself before it exits. It cordons its Node, marks it with the annotation `kube32.io/left: "true"`, evicts its pods other than DaemonSet pods and waits up to 20 s for them to leave.

## Policies

The pod annotation `kube32.io/migration` sets the policy:

| Policy | Behaviour |
| --- | --- |
| `OnDrain` (default) | Evictions move the pod when it can move |
| `Never` | The pod never moves and is never checkpointed. Evictions delete it |
| `Auto` | `OnDrain`, and the pod also moves on its own, as below |

An `Auto` pod moves on its own in two cases:

- Memory pressure. A kubelet reports the node condition `MemoryPressure` while its free heap is below 24 KiB. The controller then moves the `Auto` pod on that node that requests the most memory, at most one pod per node per minute.
- A faster node, with `kube32.io/placement: fastest`. Each node reports its interpreter speed at boot in the annotation `kube32.io/interp-mops`, measured on a fixed kernel. The pod moves to the fastest Ready node that takes it when that node is at least twice as fast as its current one. A move of such a pod without a target, such as a drain, also goes to the fastest node that fits. When the fast node leaves, the pod comes back: a drain or a stopped `kube32-sim` worker moves it, and a lost node restores it from its periodic checkpoint.

A pod moves on its own at most once per 10 minutes.

## Periodic checkpoints and node loss

The pod annotation `kube32.io/checkpoint-interval` saves the pod's state at that interval, `"60s"` for example, 10 s at the least. Each one is a Save Checkpoint with the label `kube32.io/trigger: periodic`. The pod freezes for the snapshot and resumes.

When a node stops renewing its Lease, the node lifecycle controller evicts its pods for rescheduling. A pod with a `Saved` periodic checkpoint gets `kube32.io/restore-from` naming the latest one, so the node the scheduler binds it to restores it. This holds for pods without an owner too. The `Restored` event names the checkpoint and its age. A restore that fails ends the pod `Failed`. The controller then replaces it without the annotation, and it starts afresh.

## Retention

- The latest `Saved` periodic checkpoint of each pod stays. The older ones go, and all of a pod's periodic checkpoints go with the pod.
- Other Checkpoints, `Failed` ones included, go 1 hour after they finished.
- A Checkpoint stays while a pod's `kube32.io/restore-from` names it.

The label `kube32.io/trigger` tells what created a Checkpoint: `periodic`, `eviction`, `memory-pressure` or `placement`. Checkpoints a user creates carry none.

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

The node refuses a checkpoint of a pod that cannot move with the blockers of [What keeps a pod from moving](#what-keeps-a-pod-from-moving).

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

Snapshot data lives in the control plane's memory, outside the object store, up to 256 KiB for all snapshots on the board and 16 MiB in the simulator. It is pod state, so it takes the control plane's pod pool block first, and the node's allocatable memory counts it. When the block is full it takes PSRAM above the system reserve. Storing a snapshot that does not fit drops the oldest other snapshots first. A snapshot larger than the whole space gets `413`, and one the memory cannot hold gets `507`. Deleting a Checkpoint drops its data. A control plane restart loses the data and keeps the Checkpoint objects. A snapshot holds whatever the pod holds, environment values included, so the data has the same access rules as a Secret.

`kubectl get checkpoints` prints `NAME`, `POD`, `ACTION`, `PHASE`, `NODE`, `SIZE` and `AGE`. `-o wide` adds `TARGET`, `NEW POD` and `MESSAGE`.

The Checkpoint controller carries out `Save` and `Move` Checkpoints, see [Moves](#moves).

## Limits

- The control plane takes one snapshot at a time. Others wait for it.
- A move freezes the pod for the snapshot and the transfer. Requests in that time answer `503` with `Retry-After: 1`.
- A replacement that does not run within 60 s rolls back. When the source node is not Ready then, the pod waits for the scheduler and restores where it is bound.
- The cooldowns of the `Auto` policy live in memory: a control plane restart clears them.
- A moved pod has a new uid and creation time, so `kubectl get pods` shows its age from the move.
- Checkpoint data lives in the control plane's memory, 256 KiB on the board, and counts against the memory the control plane offers pods. The oldest data goes first when the space is full, and a control plane restart loses it. A restore from a Checkpoint without data fails, and the pod starts afresh.
- A pod moves only to a node with room for it. A pod with one 64 KiB page does not fit a Cardputer that runs three drivers.

## Measured times

Measured on an ESP32-S3 devkit (control plane, PSRAM) and an M5Stack Cardputer (worker, no PSRAM), both at 240 MHz:

| Step | Time |
| --- | --- |
| Snapshot of a pod with 32 KB of memory, on the Cardputer | 856 ms |
| Freeze to Running, Cardputer to devkit | 3.2 s |
| Freeze to Running, devkit to Cardputer | 7.5 s |

| Node | `kube32.io/interp-mops` |
| --- | --- |
| ESP32-S3 devkit | about 8.2 |
| Cardputer | 6.9 to 7.5 |
| `kube32-sim` on an Apple silicon Mac | 1400 to 1600 |

The `Moved` event of each move gives its time from the freeze to the replacement running.
