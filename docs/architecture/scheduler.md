# Scheduler and controllers

Three control loops run on the control plane next to the API server:

- the scheduler, `src/scheduler/scheduler.cpp`;
- the node lifecycle controller, `src/controller/node_lifecycle_controller.cpp`;
- the Deployment controller, `src/controller/deployment_controller.cpp`.

## Scheduler

A pod created without `spec.nodeName` is scheduled during the create
request. A reconcile loop then retries anything still unbound: every 3 s on
the boards, and every second on the simulator (`--reconcile-interval`). A pod
created with `nodeName` already set skips the scheduler.

### Filters

Each candidate node must pass every filter, in this order:

1. **Schedulable.** `spec.unschedulable` is not set, the node has a status,
   and its `Ready` condition is `True`.
2. **Memory.** `requests.memory`, or `limits.memory` when the pod has no
   requests, must fit the node's allocatable `memory`. A pod that declares
   none counts as 16 KiB.
3. **PSRAM**, only when the pod requests `esp32.io/psram` (or `esp.io/psram`).
4. **Peripheral counts:** `esp32.io/gpio`, `esp32.io/i2c` and `esp32.io/adc`.
5. **Node selector:** exact matches on the node's labels.
6. **Node name.**

"Allocatable" comes from the node's `status.allocatable`, or `capacity`.
Pods already bound to the node (`Running` or `Pending`) are subtracted from
it.

The scheduler does not look at:

- `hardware.kube32.io/*` resources;
- specific pin claims (`esp32.io/gpio-<N>`);
- UART;
- taints, affinity or topology.

Those are checked later by the kubelet's
[capability firewall](capability-firewall.md), which fails the pod at
admission if a grant cannot be given.

### Scoring

When several nodes pass, each gets a "least allocated" score. The score is
the mean of two percentages: free memory after this pod, and free GPIO count
after this pod. The highest score wins, and ties go to the
alphabetically first node name. PSRAM, I2C and ADC are not scored.

### When nothing fits

The pod stays `Pending`, its `PodScheduled` condition is `False` with reason
`Unschedulable`, and a `FailedScheduling` warning is recorded on every pass.
`examples/oversized-pod.yaml` requests 4000 KiB of memory on a node; on the
simulator:

```
$ kubectl apply -f examples/oversized-pod.yaml
pod/oversized-pod created
$ kubectl get pod oversized-pod
NAME            READY   STATUS    RESTARTS   AGE
oversized-pod   0/1     Pending   0          4s
$ kubectl get events --field-selector involvedObject.name=oversized-pod
LAST SEEN   TYPE      REASON             OBJECT              MESSAGE
1s          Warning   FailedScheduling   pod/oversized-pod   0/2 nodes are available: insufficient heap memory or peripheral resources
```

The message does not say which filter failed. The scheduler computes the
reason and then discards it.

### Known limits

- **Memory is counted twice.** On a board, a node's allocatable memory is the
  free heap it measured at its last heartbeat. That figure already excludes
  running pods, and the scheduler subtracts their requests again.
- **Quantity suffixes are not standard.** `m` is read as Mi, and `K`, `M` and
  `G` are read as binary units.
- **A missing field gets a default.** When a node's status lacks a key, the
  scheduler assumes 240Ki memory, 14 GPIO, 2 I2C, 2 ADC or 3840Ki PSRAM, and
  so does the kubelet's heartbeat (see
  [Known inaccuracies](../api-reference.md#known-inaccuracies)).

## Node lifecycle controller

Each kubelet sends a heartbeat every 5 s: a Lease update and a Node status
patch. The controller remembers when it last saw one for each node and checks
every second (every 500 ms on the simulator):

| Time without a heartbeat | Action |
| --- | --- |
| 15 s | Node `Ready` set to `False` (reason `NodeStatusUnknown`), `NodeNotReady` event |
| 30 s | `Unreachable` condition added; every pod on the node is evicted |

Eviction does not delete the pod. It clears `spec.nodeName`, sets phase
`Pending` with reason `Evicted`, adds a `DisruptionTarget` condition and
records an event. The scheduler can then bind the pod to another node. When
the old node returns, its kubelet stops its copy, because the pod is no longer
bound to it.

The thresholds are set in `src/main_esp32.cpp`; on the simulator they come
from `--not-ready-timeout` and `--eviction-timeout`. The code has unit tests
(`test_lease_controller`). Nobody has yet measured eviction on the two boards
by unplugging one.

Two caveats:

- **Only arrival times count.** The controller tracks when it received a
  heartbeat. It ignores the Lease's `renewTime` and `leaseDurationSeconds`.
- **A restarted controller trusts every node.** After a control-plane
  restart, every node starts with a fresh "last seen" time.

## Deployment controller

Once a second the controller compares each Deployment with the pods that
carry its owner reference:

- It creates pods from the template, labelled with a `pod-template-hash`.
- It deletes surplus pods.
- When the template changes, it replaces one stale pod per pass. It does not
  read `strategy`.
- It writes `status` from the real pod phases.

There are no ReplicaSet objects. Pods whose owner Deployment is gone are
deleted. `kubectl scale deployment` works through the `scale` subresource.
