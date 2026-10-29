# Scheduler and controllers

Four control loops run on the control plane next to the API server:

- `src/scheduler/scheduler.cpp`
- `src/controller/node_lifecycle_controller.cpp`
- `src/controller/deployment_controller.cpp`
- `src/controller/workload_controller.cpp`

## Scheduler

The scheduler binds pods created without `spec.nodeName` during the create request. A reconcile loop retries unbound pods every 3 s on hardware and every 1 s on the simulator via `--reconcile-interval`. Pods created with `nodeName` set skip the scheduler.

### Filters

Candidate nodes must pass each filter in order:

1. `spec.unschedulable` is unset, status is present, and `Ready` is `True`.
2. `nodeName` matches if specified.
3. `nodeSelector` labels match the node labels.
4. `requests.memory`, or `limits.memory` if requests are omitted, fits within allocatable `memory`. Pods without a memory declaration default to 16 KiB.
5. `esp32.io/psram` or `esp.io/psram` fits within allocatable PSRAM.
6. `esp32.io/gpio`, `esp32.io/i2c`, `esp32.io/uart`, and `esp32.io/adc` counts fit within allocatable limits. Named buses such as `esp32.io/i2c-bus-0` or `esp32.io/uart-1` each consume one bus from the node's count.

Quantities follow Kubernetes: `Ki`, `Mi`, `Gi` are binary, `k`, `M`, `G` decimal, and `m` is milli. Unrecognized suffixes count as 0.

Allocatable resources come from `status.allocatable` or `capacity`. The kubelet measures free heap, free PSRAM, and unclaimed pins, so running pods are already included in the reported figures. The scheduler subtracts requests only for bound pods the report does not include yet: `Pending` pods, containers waiting to restart, and pods modified after the node's last status write.

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
| 15 s, or `leaseDurationSeconds` | Sets Node `Ready` to `Unknown` with reason `NodeStatusUnknown`, adds a `node.kubernetes.io/unreachable:NoSchedule` taint, and emits `NodeNotReady` |
| +15 s, or 30 s total | Adds a `node.kubernetes.io/unreachable:NoExecute` taint with `timeAdded` |
| +300 s, or 330 s total (default) | Evicts pods whose `node.kubernetes.io/unreachable:NoExecute` toleration has expired |

Simulator timeouts are configurable with `--not-ready-timeout` and `--eviction-timeout`. A lease renewal before toleration expiry clears both taints and emits `NodeReady`; bound pods stay on the node and keep running. After a control-plane restart, a previously `Ready` node gets a full lease duration before timing out, and existing `NoExecute` tolerations continue counting from the taint's `timeAdded`.

Pod admission and the Deployment and Job controllers add default `node.kubernetes.io/not-ready:NoExecute` and `node.kubernetes.io/unreachable:NoExecute` tolerations (`operator: Exists`, `tolerationSeconds: 300`) unless the pod already tolerates those taints. A pod is evicted when it has no matching toleration or when `tolerationSeconds` has elapsed since `timeAdded` (`<= 0` evicts immediately; omitting `tolerationSeconds` tolerates indefinitely). DaemonSet pods receive both tolerations without `tolerationSeconds` and are never evicted for these taints.

Eviction clears `spec.nodeName`, sets phase to `Pending` with reason `Evicted`, adds a `DisruptionTarget` condition, and records an `Evicted` event so the scheduler can rebind the pod. The controller removes terminating pods from the store when their toleration expires, and leaves `Succeeded`, `Failed`, and DaemonSet pods on the node.

## Deployment controller

The controller reconciles each Deployment every 1 s against pods carrying its owner reference:

- Creates missing pods from the template with a `pod-template-hash` label.
- Deletes surplus pods, removing not-ready and stale pods first, and deletes pods whose owner Deployment is deleted.
- Replaces one stale pod per pass on template changes.
- Tracks spec changes in `metadata.generation`, updates `status` and `Available` / `Progressing` conditions with `maxUnavailable` defaulting to 25%, and serves the `scale` subresource for `kubectl scale deployment`.

kube32 does not create ReplicaSet objects.

## DaemonSet and Job controller

One loop in `src/controller/workload_controller.cpp` reconciles DaemonSets and Jobs every 1 s. Pods carry an owner reference to their DaemonSet or Job and are deleted when the owner is deleted.

### DaemonSet

- Places one pod per node matching the template's `nodeSelector` by setting a `kubernetes.io/hostname` node selector and `controller-revision-hash` label.
- Deletes pods on nodes that leave or no longer match. DaemonSet pods stay on unreachable nodes.
- Performs node-by-node rolling updates on template changes under `RollingUpdate`, with `maxUnavailable` defaulting to 1. The controller replaces not-ready pods first and waits for terminating pods to leave the node before creating replacements. `OnDelete` leaves updates to the user.
- Rejects DaemonSets whose pod template sets `restartPolicy` to anything other than `Always`.

### Job

- Starts pods from the template until `completions`, defaulting to 1, reach `Succeeded`, running at most `parallelism`, defaulting to 1, active pods at a time, then sets the `Complete` condition.
- Counts `Failed` pods against `backoffLimit`, which defaults to 6. When exceeded, the controller deletes active pods and sets the `Failed` condition with reason `BackoffLimitExceeded`.
- Rejects Jobs whose pod template `restartPolicy` is not `Never` or `OnFailure`. Containers restarted in place under `OnFailure` do not count as pod failures.

## Endpoints and EndpointSlices

The API server computes read-only `core/v1` Endpoints and `discovery.k8s.io/v1` EndpointSlices on every read from Services and their selected pods:

- Services with a non-empty `spec.selector` match non-terminating pods in the same namespace that have a `podIP`.
- Named `targetPort` values resolve against container `ports` on each pod. The API server groups pods whose ports resolve to different numbers into separate subsets or slices.
- Ready `Running` pods populate `addresses`, or `ready` and `serving` in EndpointSlices. Other non-terminating pods populate `notReadyAddresses`.

## Limitations

- Any write to a Node object advances its `resourceVersion`, so the scheduler stops subtracting a pod started right before a label or taint update until the kubelet's next status report.

