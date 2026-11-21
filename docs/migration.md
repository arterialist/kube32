# Pod migration

A running request pod can move to another node and continue with its state. The pod does not start again: its counters, buffers and clock carry over, and `k32_init` does not run a second time. A pod moves on request, when a node is drained, when its node runs short of memory, when a faster node joins or leaves, and to the node that has the hardware its current step needs. A pod with periodic checkpoints comes back on another node when its node is lost. The state of one pod can also be copied into another pod that runs the same module, which then continues from it ([Copies](#copies)).

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
| `grant <resource>` | Control plane, from the pod spec | A request or limit for a hardware resource: `hardware.kube32.io/*` other than `ipc`, or `esp32.io/*`. The on-board device grants of a [hardware-following pod](#hardware-following-pods) are not blockers |
| `grant capability <NAME>` | Control plane | `securityContext.capabilities.add` names `DISPLAY`, `KEYBOARD`, `SPEAKER`, `SD` or `CHIP_TEMP`. For a hardware-following pod, only `SD` |
| `grant kube32.io/i2c-device` | Control plane | The pod has an I2C device grant |
| `volume claim <claim> on node storage` | Control plane | A `persistentVolumeClaim` or `kube32.io/sd` volume |
| `annotation kube32.io/migration: Never` | Control plane | The pod opts out |
| `entry point _start runs on its own thread` | Control plane for embedded images, the node for the others | The module exports `_start`, `app_main` or `main` |
| `open file <path> (descriptor <n>)` | Node | The pod holds a `k32_fs` descriptor open between calls |
| `call k32_handle still running after <ms> ms` | Node | A call outlasted the freeze timeout (10 s). The pod keeps running |

Write pods to move: keep state in linear memory and globals, close files between calls, and export `k32_restored` to refresh anything that depends on the node. See [Writing app pods](app-development.md).

## Moves

Move a pod with `kube32`:

```
kube32 move echo --to devkit
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
| `Copying` | The target pod of a [Copy](#copies) is restoring the copied state |
| `Completed` | The replacement runs on the target |
| `Failed` | Refused or rolled back, see `status.message` and `status.blockers` |

The controller records each decision as an event on the pod:

| Event | When |
| --- | --- |
| `Checkpointed` | A Save checkpoint is stored |
| `CheckpointFailed` | A Save checkpoint failed |
| `Moving` | The replacement is bound to the target. For a move a [hardware need](#hardware-following-pods) asked for, the message names the need: `Moving from node devkit to node cardputer for its need hardware.kube32.io/tft-display,hardware.kube32.io/keyboard with checkpoint ...` |
| `Moved` | The replacement runs. The message gives the time from the freeze, and the need as `Moving` does |
| `MoveFailed` | A move was refused or rolled back, a restore failed, or no node has the devices a hardware-following pod needs |
| `Restored` | A pod restored outside a move runs: after a node loss or a rollback. The message gives the checkpoint's age |
| `Copying`, `Copied`, `CopyFailed` | On the target pod of a [Copy](#copies): restoring, running with the copied state (the message gives the time from the snapshot), or refused or failed |

`kube32 checkpoint <pod>` saves a pod's state without moving it. The pod keeps running.

## Copies

A Copy restores the state of a running pod into another pod that runs the same module. The source pod keeps running. The target pod continues from the source's memory, globals and clock, as if it had run the source's history. Replicas of a deterministic computation use it: a replica whose state diverged, that started over, or whose node is gone takes the state of a healthy one and agrees with it again, on any architecture.

```
kube32 copy replica-a --into replica-c
```

or create a Checkpoint:

```yaml
apiVersion: kube32.io/v1
kind: Checkpoint
metadata:
  name: replica-c-from-a
spec:
  podName: replica-a        # the pod whose state is copied
  action: Copy
  target:
    podName: replica-c      # the pod that takes it, required
    nodeName: devkit        # optional, or nodeSelector
```

The Checkpoint controller carries out the copy:

1. It checks both pods. The source can be checkpointed, and the target can move: neither has a [blocker](#what-keeps-a-pod-from-moving). The target runs the same module (the same `kube32.io/wasm-sha256` for an inline image, else the same image) and is not being deleted.
2. It picks the target pod's node. The target stays on its node while that node is Ready and `spec.target` names no node. Otherwise it goes to `spec.target.nodeName`, or to the node the scheduler picks with `spec.target.nodeSelector` added to the pod's own, never to its own node when that node is not Ready.
3. It asks the source's kubelet for a snapshot with the pod left running and stores the bytes as the Checkpoint's data.
4. It replaces the target pod in one write, as a move replaces a pod: the same name, spec, labels and owners, a new uid, `spec.nodeName` set to the node of step 2 and `kube32.io/restore-from` naming the Checkpoint. The target's kubelet stops the old instance and the node it is bound to restores the copy. `k32_restored` runs there.
5. Once the target runs, the copy is `Completed` and the annotation is removed. A `RestoreError`, a `Failed` pod or no `Running` within 60 s starts the target afresh where it is bound, and the copy is `Failed`.

The target keeps its own spec: environment, labels, limits, events and channels. Its memory holds the source's values, so `k32_restored` reads again what differs from pod to pod, such as a name or a role from the environment. No other checkpoint of the target starts while the copy restores, and none of the source while its snapshot is taken.

## Drain and evictions

`kubectl cordon`, `kubectl uncordon` and `kubectl drain` work as in Kubernetes:

```
kubectl drain cardputer --ignore-daemonsets
kubectl uncordon cardputer
```

`kubectl drain` evicts each pod through `POST /api/v1/namespaces/<ns>/pods/<name>/eviction`. A running pod that can move, by its spec and its `kube32.io/migration` policy, moves to another node. Its replacement has a new uid, which is what `kubectl drain` waits for. Any other pod is deleted, and its owner starts a new one elsewhere. Pods without an owner need `--force`, as in Kubernetes. They move with their names when they can.

When a move that an eviction asked for fails before the replacement, the pod is deleted as an eviction deletes it, so the drain completes.

A `kube32-sim` worker stopped with Ctrl-C or `SIGTERM` drains itself before it exits. It cordons its Node, marks it with the annotation `kube32.io/left: "true"`, evicts its pods other than DaemonSet pods and waits up to 20 s for them to leave. When every pod has left, the worker deletes its Node and its Lease after its kubelet stops. A Node that still has pods stays cordoned with the annotation, so the node lifecycle controller evicts those pods once the Node is NotReady. The worker makes its Node schedulable again when it starts.

## Policies

The pod annotation `kube32.io/migration` sets the policy:

| Policy | Behaviour |
| --- | --- |
| `OnDrain` (default) | Evictions move the pod when it can move |
| `Never` | The pod never moves and is never checkpointed. Evictions delete it |
| `Auto` | `OnDrain`, and the pod also moves on its own, as below |

An `Auto` pod moves on its own in three cases:

- Memory pressure. A kubelet reports the node condition `MemoryPressure` while its free heap is below 24 KiB. The controller then moves the `Auto` pod on that node that requests the most memory, at most one pod per node per minute.
- A faster node, with `kube32.io/placement: fastest`. Each node reports its interpreter speed at boot in the annotation `kube32.io/interp-mops`, measured on a fixed kernel. The pod moves to the fastest Ready node that takes it when that node is at least twice as fast as its current one. A move of such a pod without a target, such as a drain, also goes to the fastest node that fits. When the fast node leaves, the pod comes back: a drain or a stopped `kube32-sim` worker moves it, and a lost node restores it from its periodic checkpoint.
- Hardware, with `kube32.io/placement: hardware`. The pod moves to a node that has the devices its current step needs, see [Hardware-following pods](#hardware-following-pods).

A pod moves for memory pressure or a faster node at most once per 10 minutes. Moves that a pod's hardware need asks for come at most once per 3 seconds: the pod asks for each of them itself, between two steps of its own work.

## Hardware-following pods

A job whose steps use different devices can run each step on the node that has that step's device. The pod moves with its state from node to node, and on each node it uses the hardware there. A check-in station is such a job: it reads a scale, takes a label typed on a keyboard and plays a confirmation on a speaker, where each device is on another board.

A move costs seconds (see [Measured times](#measured-times)), so it pays for a step that uses its device many times or on a tight schedule: a loop of key reads and redraws, a tone sequence with fixed gaps. A step that reads a sensor once calls the sensor's [Device](devices.md) Service instead and stays where it is.

### The pod

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: checkin
  annotations:
    kube32.io/migration: Auto
    kube32.io/placement: hardware
    kube32.io/prefer-nodes: "cardputer, kube32.io/board=s31-coreboard"   # optional
spec:
  containers:
    - name: checkin
      image: wasm://inline/checkin
      resources:
        requests:
          memory: 8Ki
          hardware.kube32.io/tft-display: "1"   # every device of the job's life
          hardware.kube32.io/keyboard: "1"
          hardware.kube32.io/speaker: "1"
          hardware.kube32.io/led: "1"
```

The spec lists every on-board device the pod uses over its life: `hardware.kube32.io/tft-display`, `keyboard`, `touch`, `speaker`, `microphone`, `led` and `chip-temp`, as resources or as their capabilities in `securityContext.capabilities.add`. These grants follow the pod and do not keep it from moving. The other grants still do: pins and buses (`esp32.io/*`), I2C device grants and volumes name the wiring or the storage of one node.

### The need

The pod tells its node what its current step needs with `k32_need`, a comma-separated list of device names: resource names (`hardware.kube32.io/speaker`), the part after `hardware.kube32.io/` (`speaker`) or capabilities (`SPEAKER`). An empty list is a step without hardware.

```c
int32_t k32_need(const char* resources, uint32_t len);
```

| Result | Meaning |
| --- | --- |
| `K32_OK` (`0`) | The pod holds every named grant on this node |
| `K32_NEED_MOVING` (`1`) | The node lacks one: the pod moves to a node that has them. It runs on until it is frozen |
| `K32_ERR_PERM` (`-1`) | The spec does not list a named device |
| `K32_ERR_NODEV` (`-19`) | The node lacks one and the pod does not follow its hardware |
| `K32_ERR_INVAL` (`-22`) | An entry names no on-board device, or the list is longer than 256 bytes |
| `K32_ERR_FAULT` (`-14`) | The list is outside linear memory |

The call returns at once. The kubelet writes each change of the need to the Pod's annotation `kube32.io/needs`, as resource names (`hardware.kube32.io/speaker,hardware.kube32.io/led`), and writes a change it could not deliver again a second later. A pod calls `k32_need` on every tick of a step: after the move, `k32_restored` runs on the new node and the same call answers `K32_OK` there.

### Where the pod goes

- The scheduler binds a hardware-following pod only to a node that offers every device of `kube32.io/needs`: one whose allocatable `hardware.kube32.io/*` resources hold them. The other devices the spec lists do not filter nodes. A pod created without the annotation fits any node; a pod created with it starts on a node with those devices. A node that fits none fails with `Insufficient hardware.kube32.io/<device>`.
- `kube32.io/prefer-nodes` ranks the nodes that fit, best first: node names or `<label>=<value>`, comma-separated. A node that matches an earlier entry wins over the scheduler's other scores. It applies to any pod, at binding and at every move.
- The Checkpoint controller moves a running hardware-following pod when its node lacks a device of its need. It creates a Move Checkpoint with the label `kube32.io/trigger: hardware` to the node the scheduler picks among the other nodes: Ready and schedulable, matching the pod's node selector, with room for its memory and its pod count, offering the need, by `kube32.io/prefer-nodes`. The move then runs as any move does, and the `Moving` and `Moved` events name the need.
- A move without a target, such as a drain, also goes to a node that offers the need.
- A pod's need moves it at most once per 3 seconds. These moves do not count against the 10 minutes of the other `Auto` moves.
- When no node has the need, a `MoveFailed` event names it and the scheduler's reason, once for each need: `No node has hardware.kube32.io/speaker,hardware.kube32.io/led for the pod's need (0/4 nodes are available: ...): it runs on node cardputer and moves once a node has it`. The pod keeps running, `k32_need` keeps answering `K32_NEED_MOVING` and the annotation keeps the need. A Node that joins or changes its devices starts a pass of the controller, which moves the pod once a node fits.

### Grants on each node

On each node the capability firewall gives the pod the devices its spec lists that the node has. A call to a listed device the node lacks returns `K32_ERR_PERM`, as any call without its grant does, and the node logs the violation. A pod that does not follow its hardware holds every device it lists, whatever the node has.

The state of the hardware stays on its node. The display, keyboard, speaker, microphone, touch panel and LED are devices of the board, shared by the pods that hold their grants. A pod holds no handle of its own on them between two calls, and freezes only between calls. When the pod leaves, its grants go: the panel keeps what the pod drew, the LED keeps its color, keys typed after the pod left stay in the board's queue, and a sound plays to its end. So a pod turns the LED off and lets its sound end before it needs other hardware, and sets up the hardware of its next step on the node it arrives at: `k32_restored` runs there first, then the step.

### In the simulator

`kube32-sim --sim-devices <list>` gives a node stand-in devices: the Node advertises their grants and the calls print what the pod does. Workers with different lists model boards with different hardware ([CLI reference](cli-reference.md)):

```
kube32-sim --role worker --node-name desk --node-id 1 --join http://192.0.2.10:6443 --sim-devices tft-display,keyboard
kube32-sim --role worker --node-name bell --node-id 2 --join http://192.0.2.10:6443 --sim-devices speaker,led
```

### Example

`apps/checkin` with `examples/checkin.yaml` is a check-in job of four steps. The weight and the label are globals in its linear memory, so they travel with it:

| Step | Needs | Runs on | Why there |
| --- | --- | --- | --- |
| 1 weigh | nothing | where `kube32.io/prefer-nodes` puts it, a devkit | The scale's driver pod owns its I2C bus; one reading is one small request to the scale's Service |
| 2 label | display and keyboard | a Cardputer (Xtensa) | Typing is a loop of key reads and redraws: a function call each on the board with the keys |
| 3 confirm | speaker and LED | an ESP32-S31 CoreBoard (RISC-V) | The tones keep their spacing only on a local speaker; the LED flash is on the same board |
| 4 log | nothing | where step 3 ran | The record goes to the pod's log |

## Periodic checkpoints and node loss

The pod annotation `kube32.io/checkpoint-interval` saves the pod's state at that interval, `"60s"` for example, 10 s at the least. Each one is a Save Checkpoint with the label `kube32.io/trigger: periodic`. The pod freezes for the snapshot and resumes.

When a node stops renewing its Lease, the node lifecycle controller evicts its pods for rescheduling. A pod with a `Saved` periodic checkpoint gets `kube32.io/restore-from` naming the latest one, so the node the scheduler binds it to restores it. This holds for pods without an owner too. The `Restored` event names the checkpoint and its age. A restore that fails ends the pod `Failed`. The controller then replaces it without the annotation, and it starts afresh.

## Retention

- The latest `Saved` periodic checkpoint of each pod stays. The older ones go, and all of a pod's periodic checkpoints go with the pod.
- Other Checkpoints, `Failed` ones included, go 1 hour after they finished.
- A Checkpoint stays while a pod's `kube32.io/restore-from` names it.

The label `kube32.io/trigger` tells what created a Checkpoint: `periodic`, `eviction`, `memory-pressure`, `placement` or `hardware`. Checkpoints a user creates carry none.

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
| `spec.action` | `Save` (the default), `Move` or `Copy` |
| `spec.target` | For `Move`: `nodeName` or `nodeSelector`. For `Copy`: `podName`, the pod that takes the state (required), and optionally `nodeName` or `nodeSelector` |
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

Snapshot data lives in the control plane's memory, outside the object store, up to 256 KiB for all snapshots on the board and 16 MiB in the simulator. It is pod state, so it takes the control plane's pod pool block first, and the kubelet's admission and the node's `kube32.io/memory-free` annotation count it. When the block is full it takes PSRAM above the system reserve. Storing a snapshot that does not fit drops the oldest other snapshots first. A snapshot larger than the whole space gets `413`, and one the memory cannot hold gets `507`. Deleting a Checkpoint drops its data. A control plane restart loses the data and keeps the Checkpoint objects. A snapshot holds whatever the pod holds, environment values included, so the data has the same access rules as a Secret.

`kubectl get checkpoints` prints `NAME`, `POD`, `ACTION`, `PHASE`, `NODE`, `SIZE` and `AGE`. `-o wide` adds `TARGET`, `NEW POD` and `MESSAGE`.

The Checkpoint controller carries out `Save`, `Move` and `Copy` Checkpoints, see [Moves](#moves) and [Copies](#copies).

## Limits

- The control plane takes one snapshot at a time. Others wait for it.
- A move freezes the pod for the snapshot and the transfer. Requests in that time answer `503` with `Retry-After: 1`.
- A replacement that does not run within 60 s rolls back. When the source node is not Ready then, the pod waits for the scheduler and restores where it is bound.
- The cooldowns of the `Auto` policy live in memory: a control plane restart clears them. So does the record of the needs a `MoveFailed` event named, which the next pass names again.
- A hardware-following pod's step runs where its node has the step's devices. Devices the Node does not advertise (a part on a GPIO pin, an I2C part) are reached through their [Device](devices.md) Service, not followed.
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
