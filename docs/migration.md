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

| Blocker | Found by | Text |
| --- | --- | --- |
| A hardware grant: `hardware.kube32.io/*` other than `ipc`, `esp32.io/*` other than `psram` | Control plane, from the pod spec | `grant esp32.io/gpio-2` |
| An I2C device grant | Control plane | `grant kube32.io/i2c-device` |
| A device capability: anything other than `CHANNEL_IPC` and `SERVICE_CALL` | Control plane | `capability DISPLAY` |
| A volume on node storage (`persistentVolumeClaim`, `kube32.io/sd`) | Control plane | `volume data on node storage` |
| A DaemonSet owner | Control plane | `DaemonSet pod: it belongs to its node` |
| The annotation `kube32.io/migration: Never` | Control plane | `annotation kube32.io/migration: Never` |
| An entry point (`_start`, `app_main`, `main`) | Node | `entry point _start runs on its own thread` |
| An open `k32_fs` descriptor | Node | `open file notes.txt (descriptor 3)` |
| A call that does not return within the freeze timeout | Node | `call k32_handle still running after 10000 ms` |

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

A `kube32-sim` worker stopped with Ctrl-C drains itself before it exits.

## Policies

The pod annotation `kube32.io/migration` sets the policy:

| Policy | Behaviour |
| --- | --- |
| `OnDrain` (default) | Evictions move the pod when it can move |
| `Never` | The pod never moves and policies never checkpoint it. Evictions delete it. `kube32-cli checkpoint` still saves it |
| `Auto` | `OnDrain`, and the pod also moves on its own, as below |

An `Auto` pod moves on its own in two cases:

- Memory pressure. A kubelet reports the node condition `MemoryPressure` while its free heap is below 24 KiB. The controller then moves the `Auto` pod on that node that requests the most memory, at most one pod per node per minute.
- A faster node, with `kube32.io/placement: fastest`. Each node reports its interpreter speed at boot in the annotation `kube32.io/interp-mops`, measured on a fixed kernel. The pod moves to the fastest Ready node that takes it when that node is at least twice as fast as its current one. A move of such a pod without a target, such as a drain, also goes to the fastest node that fits. When the fast node leaves, the pod comes back: a drain or a stopped `kube32-sim` worker moves it, and a lost node restores it from its periodic checkpoint.

A pod moves on its own at most once per 10 minutes.

## Periodic checkpoints and node loss

The pod annotation `kube32.io/checkpoint-interval` saves the pod's state at that interval, `"60s"` for example, 10 s at the least. Each one is a Save Checkpoint with the label `kube32.io/trigger: periodic`. The pod freezes for the snapshot and resumes.

When a node stops renewing its Lease, the node lifecycle controller evicts its pods for rescheduling. A pod with a `Saved` periodic checkpoint gets `kube32.io/restore-from` naming the latest one, so the node the scheduler binds it to restores it. This holds for pods without an owner too. The `Restored` event names the checkpoint and its age. A restore that fails removes the annotation and the container starts afresh.

## Retention

- The latest `Saved` periodic checkpoint of each pod stays. The older ones go, and all of a pod's periodic checkpoints go with the pod.
- Other Checkpoints, `Failed` ones included, go 1 hour after they finished.
- A Checkpoint stays while a pod's `kube32.io/restore-from` names it.

The label `kube32.io/trigger` tells what created a Checkpoint: `periodic`, `eviction`, `memory-pressure` or `placement`. Checkpoints a user creates carry none.

## Limits

- The control plane takes one snapshot at a time. Others wait for it.
- A move freezes the pod for the snapshot and the transfer. Requests in that time answer `503` with `Retry-After: 1`.
- A replacement that does not run within 60 s rolls back. When the source node is not Ready then, the pod waits for the scheduler and restores where it is bound.
- The cooldowns of the `Auto` policy live in memory: a control plane restart clears them.
- A moved pod has a new uid and creation time, so `kubectl get pods` shows its age from the move.

<!-- PLACEHOLDER (node agent): section "Snapshot format" -->

<!-- PLACEHOLDER (node agent): section "Kubelet endpoints" -->

<!-- PLACEHOLDER (node agent): section "Checkpoint objects and data" -->

## Measured times

<!-- PLACEHOLDER (hardware run after the merge): freeze-to-Running time per hop (Cardputer to devkit, devkit to kube32-sim, back), snapshot sizes, periodic checkpoint cost -->
