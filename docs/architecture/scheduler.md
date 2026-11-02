# Scheduler and controllers

Five control loops run on the control plane next to the API server:

- `src/scheduler/scheduler.cpp`
- `src/controller/node_lifecycle_controller.cpp`
- `src/controller/deployment_controller.cpp`
- `src/controller/workload_controller.cpp`
- `src/controller/checkpoint_controller.cpp`

The Deployment, DaemonSet and Job, Device and Checkpoint controllers watch the object store, as Kubernetes controllers watch the API server. A controller runs a pass when an object that it reads changes, when a wait that it set ends, and every 30 s without a change, which repairs what a missed change would leave. Changes to Node objects count only when they touch a field that the controller reads, so node heartbeats cause no passes. Passes are at least 1 s apart (2 s for the Device controller), or `--reconcile-interval` on the simulator.

## Scheduler

The scheduler binds pods created without `spec.nodeName` during the create request. Pods created with `nodeName` set skip the scheduler. A loop binds the other unbound pods, and pods whose node was deleted, as kube-scheduler's queue does:

- It tries a pod that it has not tried yet, or one that changed since its last attempt, at its next pass.
- It tries a pod that did not fit again after a change that can make it fit: a pod created, changed or deleted, or a Node whose labels, spec, `Ready` condition or resources change. Node heartbeats that change nothing else are not such a change.
- After each attempt that fails, a pod waits for its back-off before the next one: 1 s, doubled per attempt up to 10 s.
- Without a change, it tries every pod that does not fit every 5 minutes.

Passes are at least 3 s apart on hardware and `--reconcile-interval` apart on the simulator. An attempt that ends with the same verdict leaves the pod unchanged and records a `FailedScheduling` event.

### Filters

Candidate nodes must pass each filter in order:

1. `spec.unschedulable` is unset, status is present, and `Ready` is `True`.
2. `nodeName` matches if specified.
3. `nodeSelector` labels match the node labels.
4. `requests.memory`, or `limits.memory` if requests are omitted, fits within allocatable `memory`. Pods without a memory declaration default to 16 KiB.
5. `esp32.io/psram` or `esp.io/psram` fits within allocatable PSRAM.
6. `esp32.io/gpio`, `esp32.io/i2c`, `esp32.io/uart`, `esp32.io/adc`, `esp32.io/rmt-tx`, and `esp32.io/rmt-rx` counts fit within allocatable limits. Named buses such as `esp32.io/i2c-bus-0` or `esp32.io/uart-1` each consume one bus from the node's count.

Quantities follow Kubernetes: `Ki`, `Mi`, `Gi` are binary, `k`, `M`, `G` decimal, and `m` is milli. Unrecognized suffixes count as 0.

Allocatable resources come from `status.allocatable` or `capacity`. The kubelet reports what pods can still be given, counted the way its [memory admission](wasm-runtime.md#memory-admission) counts it, and unclaimed pins, so running pods are already included in the reported figures:

| Node | Allocatable `memory` | Allocatable `esp32.io/psram` |
| --- | --- | --- |
| Board with PSRAM | PSRAM above the system reserve, as much as was free when the kubelet first reported, less what pods hold | `memory` plus the free part of the PSRAM pool block |
| Board without PSRAM | Free part of the 80 KiB pool block | not reported |

The figures change only when pods load or stop. Capacity `memory` is the board's internal RAM plus its PSRAM. The scheduler subtracts requests only for bound pods the report does not include yet: `Pending` pods, containers waiting to restart, and pods modified after the node's last status write.

The scheduler ignores `hardware.kube32.io/*` resources, `esp32.io/gpio-<N>` pin claims, taints, affinity, and topology. The kubelet [capability firewall](capability-firewall.md) validates hardware and pin claims at admission.

### Scoring

Passing nodes receive a score equal to the mean of two percentages: free memory after binding the pod and free GPIO count after binding the pod. The highest score wins. Ties break by alphabetically first node name.

### Unschedulable pods

When no node passes filtering, the pod remains `Pending` with `PodScheduled` set to `False`, reason `Unschedulable`, and a `FailedScheduling` event in kube-scheduler format:

```
$ kubectl apply -f examples/oversized-pod.yaml
pod/oversized-pod created
$ kubectl get pod oversized-pod
NAME            READY   STATUS    RESTARTS   AGE
oversized-pod   0/1     Pending   0          4s
$ kubectl get events --field-selector involvedObject.name=oversized-pod
LAST SEEN   TYPE      REASON             OBJECT              MESSAGE
0s          Warning   FailedScheduling   pod/oversized-pod   0/2 nodes are available: 1 Insufficient esp32.io/psram, 1 node(s) didn't match Pod's node affinity/selector.
```

## Node lifecycle controller

Each kubelet renews a Lease in `kube-node-lease` with `leaseDurationSeconds: 15` every 5 s. The controller checks renewals every 1 s on hardware and every 500 ms on the simulator:

| Time without renewal | Action |
| --- | --- |
| 15 s, or `leaseDurationSeconds` | Sets the conditions the kubelet posts (`Ready`, `MemoryPressure`) to `Unknown` with reason `NodeStatusUnknown`, adds a `node.kubernetes.io/unreachable:NoSchedule` taint, and emits `NodeNotReady` |
| +15 s, or 30 s total | Adds a `node.kubernetes.io/unreachable:NoExecute` taint with `timeAdded` |
| +300 s, or 330 s total (default) | Evicts pods whose `node.kubernetes.io/unreachable:NoExecute` toleration has expired |

Simulator timeouts are configurable with `--not-ready-timeout` and `--eviction-timeout`. A lease renewal before toleration expiry clears both taints and emits `NodeReady`; bound pods stay on the node and keep running. After a control-plane restart, a previously `Ready` node gets a full lease duration before timing out, and existing `NoExecute` tolerations continue counting from the taint's `timeAdded`.

### Node condition times

The kubelet posts `Ready`, `MemoryPressure` and, on nodes that update their firmware through the cluster, `FirmwareUpdate` with every heartbeat as a strategic merge patch, so conditions merge by `type` and an entry that another writer set stays. Each condition carries `lastHeartbeatTime`, the time of the heartbeat, and `lastTransitionTime`, the time its status last changed. The kubelet notes a change on its uptime clock and reports its wall-clock time once the clock is set. The API server keeps a condition's stored `lastTransitionTime` while its status stays the same, and when the status changes it keeps the writer's time only if that time is later than the stored one, else it records the time of the write. A kubelet that comes back after the node lifecycle controller set `Ready` to `Unknown` therefore shows the moment it returned. When neither the kubelet nor the API server has a wall clock, a time that cannot be known is left out, and `kubectl describe node` prints the zero time (`0001-01-01`) in its place.

Pod admission and the Deployment and Job controllers add default `node.kubernetes.io/not-ready:NoExecute` and `node.kubernetes.io/unreachable:NoExecute` tolerations (`operator: Exists`, `tolerationSeconds: 300`) unless the pod already tolerates those taints. A pod is evicted when it has no matching toleration or when `tolerationSeconds` has elapsed since `timeAdded` (`<= 0` evicts immediately; omitting `tolerationSeconds` tolerates indefinitely). DaemonSet pods receive both tolerations without `tolerationSeconds` and are never evicted for these taints.

Eviction clears `spec.nodeName`, sets phase to `Pending` with reason `Evicted`, adds a `DisruptionTarget` condition, and records an `Evicted` event so the scheduler can rebind the pod. The controller removes terminating pods from the store when their toleration expires, and leaves `Succeeded`, `Failed`, and DaemonSet pods on the node.

## Deployment controller

The controller reconciles Deployments against the pods that carry their owner reference, after a Deployment or one of those pods changes:

- Creates missing pods from the template with a `pod-template-hash` label.
- Deletes surplus pods, removing not-ready and stale pods first, and deletes pods whose owner Deployment is deleted.
- Replaces one stale pod per pass on template changes.
- Tracks spec changes in `metadata.generation`, updates `status` and `Available` / `Progressing` conditions with `maxUnavailable` defaulting to 25%, and serves the `scale` subresource for `kubectl scale deployment`.

kube32 does not create ReplicaSet objects.

## DaemonSet and Job controller

One loop in `src/controller/workload_controller.cpp` reconciles DaemonSets and Jobs after a DaemonSet, a Job, a pod that they own, or the labels of a Node change. Pods carry an owner reference to their DaemonSet or Job and are deleted when the owner is deleted.

### DaemonSet

- Places one pod per node matching the template's `nodeSelector` by setting a `kubernetes.io/hostname` node selector and `controller-revision-hash` label.
- Deletes pods on nodes that leave or no longer match. DaemonSet pods stay on unreachable nodes.
- Performs node-by-node rolling updates on template changes under `RollingUpdate`, with `maxUnavailable` defaulting to 1. The controller replaces not-ready pods first and waits for terminating pods to leave the node before creating replacements. `OnDelete` leaves updates to the user.
- Rejects DaemonSets whose pod template sets `restartPolicy` to anything other than `Always`.

### Job

- Starts pods from the template until `completions`, defaulting to 1, reach `Succeeded`, running at most `parallelism`, defaulting to 1, active pods at a time, then sets the `Complete` condition.
- Counts `Failed` pods against `backoffLimit`, which defaults to 6. When exceeded, the controller deletes active pods and sets the `Failed` condition with reason `BackoffLimitExceeded`.
- Rejects Jobs whose pod template `restartPolicy` is not `Never` or `OnFailure`. Containers restarted in place under `OnFailure` do not count as pod failures.

## Checkpoint controller

The controller carries out `kube32.io/v1` Checkpoints, applies the migration policies of pods and keeps the retention of Checkpoints ([Pod migration](../migration.md)). It runs a pass after a Checkpoint changes, after a pod with a `kube32.io/migration`, `kube32.io/checkpoint-interval` or `kube32.io/restore-from` annotation changes, and after a Node changes a condition, `spec.unschedulable` or `kube32.io/interp-mops`. Move deadlines, checkpoint intervals, cooldowns and expiries are waits it sets. A pass takes at most one snapshot, because the kubelet call blocks it. It asks the scheduler for a move's target without binding anything, and the node lifecycle controller asks it for the latest periodic checkpoint of each pod it evicts.

## Endpoints and EndpointSlices

The API server computes read-only `core/v1` Endpoints and `discovery.k8s.io/v1` EndpointSlices on every read from Services and their selected pods:

- Services with a non-empty `spec.selector` match non-terminating pods in the same namespace that have a `podIP`.
- Named `targetPort` values resolve against container `ports` on each pod. A pod's endpoint port is its node's pod port when the node reports one in `status.daemonEndpoints.kubeletEndpoint.Port`, and the resolved target port otherwise. The API server groups pods whose ports come out different into separate subsets or slices.
- Ready `Running` pods populate `addresses`, or `ready` and `serving` in EndpointSlices. Other non-terminating pods populate `notReadyAddresses`.

## Limitations

- Any write to a Node object advances its `resourceVersion`, so the scheduler stops subtracting a pod started right before a label or taint update until the kubelet's next status report.

