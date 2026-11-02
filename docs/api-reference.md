# Kubernetes API coverage

The API server (`src/api/`) listens on port 6443. It serves TLS and plain HTTP/1.1 on the same port, detected from the first bytes of each connection. Large responses stream with chunked transfer encoding.

## Authentication and TLS

Bearer token authentication is optional and off by default. Set or disable a token, 16 to 128 characters, over the serial console with `K32_CMD:TOKEN:<token>` or `K32_CMD:TOKEN:off`.

- Unauthenticated requests receive `401 Unauthorized` with a Status body.
- `/healthz`, `/livez`, `/readyz` and `/version` do not require authentication.
- Worker kubelets read the same NVS `token` key and send it over plain HTTP on the LAN.
- `SelfSubjectAccessReview` and `SelfSubjectRulesReview` always return allowed for authenticated callers, or for all callers when authentication is off.

On first boot, the control plane generates an ECDSA P-256 key and self-signed certificate in NVS for `kube32-apiserver`, `localhost`, the node name and the node IP. Run `K32_CMD:CERT` on the console to print the PEM certificate.

`kubectl` only sends bearer tokens over HTTPS. Configure `kubeconfig` with TLS:

```
kubectl config set-cluster kube32 --server=https://<control-plane-ip>:6443 --tls-server-name=kube32-apiserver --certificate-authority=<pem file> --embed-certs
kubectl config set-credentials kube32 --token=<token>
```

Without authentication, use `http://<control-plane-ip>:6443` and no credentials.

## Discovery

Discovery endpoints accept `GET` and `HEAD`.

| Path | Answer |
| --- | --- |
| `/version` | `gitVersion` `v1.33.0-kube32`, git commit, tree state, UTC build time, compiler and platform: `darwin/arm64`, `linux/amd64` or `freertos/xtensa`. `goVersion` is empty |
| `/healthz`, `/livez`, `/readyz` | `200 OK` with body `ok` |
| `/api`, `/api/v1` | core `v1` |
| `/apis`, `/apis/<group>`, `/apis/<group>/<version>` | `apps/v1`, `batch/v1`, `coordination.k8s.io/v1`, `discovery.k8s.io/v1`, `events.k8s.io/v1`, `networking.k8s.io/v1`, `authorization.k8s.io/v1`, `metrics.k8s.io/v1beta1`, `kube32.io/v1` |
| `/openapi/v3`, `/openapi/v3/<group version>` | OpenAPI v3 index and per-group documents listing the `fieldValidation` parameter without schemas. `/openapi/v2` and unknown groups return 404 |

## Resources

| Group | Kind | Short | Verbs and subresources | Notes |
| --- | --- | --- | --- | --- |
| core `v1` | Pod | `po` | get, list, watch, create, update, patch, delete; `status`, `log`, `binding`, `eviction` | Graceful deletion when bound to a node, 30 s by default or `gracePeriodSeconds`. `DELETE` returns the `Pod` for bound pods and a `Status` for unbound pods. Create, update and non-status patch add 300 s `NoExecute` tolerations for `node.kubernetes.io/not-ready` and `node.kubernetes.io/unreachable` unless already tolerated. See [Eviction](#eviction) |
| core `v1` | Node | `no` | get, list, watch, create, update, patch, delete; `status`, `proxy` | Registered by kubelets. `/api/v1/nodes/<node>/proxy/storage/<path>` supports `GET`, `PUT` and `DELETE`; `PUT` accepts chunked or `Content-Length` bodies up to 8 MiB by default, writes `<path>.part` and renames it into place. `POST .../proxy/checkpoint/<ns>/<pod>/<container>` and `POST .../proxy/thaw/<ns>/<pod>/<container>` take and resume pod checkpoints ([Pod migration](migration.md#kubelet-endpoints)) |
| core `v1` | Namespace | `ns` | get, list, watch, create, update, patch, delete | Seeded at boot: `default`, `kube-system`, `kube-public`, `kube-node-lease`, `kube32-cluster` |
| core `v1` | Service | `svc` | get, list, watch, create, update, patch, delete | Selects Running pods for Ingress, `k32_http_call` and environment variables |
| core `v1` | Endpoints | `ep` | get, list | Read-only. Computed on read from Services and their pods |
| core `v1` | ConfigMap | `cm` | get, list, watch, create, update, patch, delete | Stored in the object store |
| core `v1` | Secret | | get, list, watch, create, update, patch, delete | Stored in the object store |
| core `v1`, `events.k8s.io/v1` | Event | `ev` | get, list, watch, create, update, patch, delete | Kept in RAM, up to 64 events, and projected to both API groups |
| `apps/v1` | Deployment | `deploy` | get, list, watch, create, update, patch, delete; `status`, `scale` | Manages pods directly using `pod-template-hash` |
| `apps/v1` | DaemonSet | `ds` | get, list, watch, create, update, patch, delete | Template `restartPolicy` must be `Always` or unset, 422 otherwise |
| `batch/v1` | Job | `job` | get, list, watch, create, update, patch, delete | Template `restartPolicy` must be `Never` or `OnFailure`, 422 otherwise |
| `coordination.k8s.io/v1` | Lease | | get, list, watch, create, update, patch, delete | Kept in RAM for node heartbeats with duration `max(15 s, 3 * heartbeat)`. `POST` and `PUT` upsert. When a lease expires, the node is marked `Ready=Unknown` (`NodeStatusUnknown`) with `node.kubernetes.io/unreachable:NoSchedule`; after the eviction timeout (30 s by default) `node.kubernetes.io/unreachable:NoExecute` is added and pods are unbound and rescheduled once their matching `tolerationSeconds` elapses |
| `networking.k8s.io/v1` | Ingress | `ing` | get, list, watch, create, update, patch, delete | Routes HTTP traffic on port 80 |
| `networking.k8s.io/v1` | NetworkPolicy | `netpol` | get, list, watch, create, update, patch, delete | Validated and defaulted as by kube-apiserver. Egress rules restrict `k32_http_call` on the calling pod's node ([NetworkPolicy](networking.md#networkpolicy)); ingress rules are stored and not enforced |
| `discovery.k8s.io/v1` | EndpointSlice | | get, list | Read-only. Computed on read from Services and their pods |
| `metrics.k8s.io/v1beta1` | NodeMetrics, PodMetrics | | get, list | Kubelet samples of every running pod, kept 30 s |
| `authorization.k8s.io/v1` | SelfSubjectAccessReview, SelfSubjectRulesReview | | create | Always returns allowed |
| `kube32.io/v1` | FirmwareRollout | `fwr` | get, list, watch, create, update, patch, delete | Cluster-scoped. `spec.sha256` names the staged firmware image. The rollout controller updates nodes one at a time and writes `status`. The API server serves the staged image at `/apis/kube32.io/v1/firmware`. See [Firmware updates](firmware-updates.md#api). |
| `kube32.io/v1` | Device | `dev` | get, list, watch, create, update, patch, delete; `status` | A part wired to a node. `spec.driver` must be a driver of the catalog. The API server checks I2C addresses, multiplexer channels and pins, and stores addresses as `"0x77"`. A write to the object keeps `status`. A write to `status` changes nothing else. The Device controller runs the driver pod and Service and writes `status`. See [Devices](devices.md). |
| `kube32.io/v1` | Checkpoint | `ckpt` | get, list, watch, create, update, patch, delete; `status`, `data` | A pod's snapshot. `spec.podName` is required and `spec.action` is `Save` or `Move`. A write to the object keeps `status`. `GET` and `PUT` of `checkpoints/<name>/data` read and store the snapshot (`application/octet-stream`), kept in memory outside the object store. See [Pod migration](migration.md#checkpoint-objects-and-data). |

## Eviction

`POST /api/v1/namespaces/<ns>/pods/<name>/eviction` takes a `policy/v1` `Eviction` and answers `201` with a `Status`. Discovery lists the subresource, so `kubectl drain` uses it.

- A running pod that can move gets a `Move` Checkpoint with the label `kube32.io/trigger: eviction`. The Checkpoint controller moves the pod to another node. The replacement keeps the pod's name and has a new uid. A second eviction of the same pod finds the move under way and answers `201` again.
- A pod that cannot move is deleted as `DELETE` deletes it, with `deleteOptions.gracePeriodSeconds` when the body gives one. A pod cannot move when its spec holds a blocker, when its policy is `Never`, or when it is not running.
- When a move the eviction asked for fails before the replacement, the pod is deleted.
- `404` for an unknown pod. There are no PodDisruptionBudgets, so the answer is never `429`.

See [Pod migration](migration.md#drain-and-evictions).

## Checkpoint status

The Checkpoint controller writes `status` of each `kube32.io/v1` Checkpoint. The API server adds `imageSha256`, `snapshotSha256`, `sizeBytes` and `podTimeMs` when it stores the data:

| Field | Meaning |
| --- | --- |
| `phase` | `Pending`, `Saving`, `Saved` (Save), `Moving`, `Completed` (Move) or `Failed` |
| `nodeName` | The node the pod ran on when the snapshot was taken |
| `targetNode` | Move: the node the replacement runs on |
| `newPodName` | Move: the replacement's name, the pod's own |
| `imageSha256` | SHA-256 of the pod's module, from the snapshot |
| `snapshotSha256` | The snapshot's trailer, the SHA-256 of its bytes before it |
| `sizeBytes` | Size of the checkpoint's data |
| `podTimeMs` | The pod's clock (`k32_time_ms`) at the snapshot |
| `takenAt`, `finishedAt` | Wall-clock times, when the control plane has a clock |
| `message` | What happened, or why the checkpoint failed |
| `blockers` | Why the pod cannot move, one line each |

See [Pod migration](migration.md) for the phases and the retention of Checkpoints.

## Annotations

| Annotation | On | Meaning |
| --- | --- | --- |
| `kube32.io/migration` | Pod | `OnDrain` (default), `Never` or `Auto`. See [Policies](migration.md#policies) |
| `kube32.io/placement` | Pod | `fastest`: with `Auto`, run on the fastest node, by `kube32.io/interp-mops` |
| `kube32.io/checkpoint-interval` | Pod | Save the pod's state at this interval (`"60s"`, `"5m"`), 10 s at the least |
| `kube32.io/restore-from` | Pod | The Checkpoint the kubelet restores the pod from. The Checkpoint controller and the node lifecycle controller set it and remove it once the pod runs |
| `kube32.io/interp-mops` | Node | Interpreter speed the node measured at boot, millions of wasm3 operations per second |
| `kube32.io/trigger` (label) | Checkpoint | What created it: `periodic`, `eviction`, `memory-pressure` or `placement` |

`POST /apis/kube32.io/v1/egress` is the egress gateway: it makes the `https://` calls of pods on workers, for nodes only. See [Networking](networking.md#egress-gateway).

`GET /apis/kube32.io/v1/images/<sha256>` returns a module that a Pod or pod template carried inline (`application/wasm`, with `Content-Length`). A Pod, Deployment, DaemonSet or Job written with the annotation `kube32.io/wasm-base64` is stored with `kube32.io/wasm-sha256` and `kube32.io/wasm-size` in its place, and the module is stored under the digest. See [Writing app pods](app-development.md#inline-image-kube32-cli-deploy).

## Node annotations

| Annotation | Set by | Value |
| --- | --- | --- |
| `kube32.io/interp-mops` | Kubelet, once at boot | The interpreter's speed: millions of wasm3 operations per second on a fixed integer kernel, timed on the stack and memory the node gives pods, e.g. `"6.88"` on a Cardputer |

## kubectl and k9s

`kubectl` and `k9s` work without extra flags. Supported commands include `get`, `describe`, `apply`, `create`, `delete`, `patch`, `scale`, `logs`, `top`, `events`, `cordon`, `uncordon`, `drain` and `auth can-i`.

The client can send `Accept: as=Table;v=v1;g=meta.k8s.io`. List and single-object `GET` requests then return `meta.k8s.io/v1` Table responses for Pod, Node, Deployment, Event, Namespace, Service, ConfigMap, Secret, Lease, Ingress, Endpoints, EndpointSlice, FirmwareRollout and Device. DaemonSets, Jobs and metrics return plain objects for client-side printing.

OpenAPI v3 documents advertise `fieldValidation`. When `kubectl` sends `fieldValidation=Strict`, the server rejects unknown top-level and `metadata` fields with `400 BadRequest`:

```
$ kubectl apply -f typo.yaml        # metadata.lables instead of metadata.labels
Error from server (BadRequest): error when creating "typo.yaml": strict decoding error: unknown field "metadata.lables"
```

Write requests honour `dryRun=All` in the query string or in a `DeleteOptions` request body. The server validates the request and checks preconditions without writing to the store or firing watch events.

`application/merge-patch+json` applies a recursive JSON merge where objects merge and arrays or scalars replace. `application/strategic-merge-patch+json` also merges the lists of the core types by their key, for example containers by `name` and conditions by `type`. `PUT` and `PATCH` preserve `uid`, `creationTimestamp` and `status`, and increment `metadata.generation` when `spec` changes.

`fieldSelector` supports `=`, `==` and `!=` on any dotted path. `labelSelector` supports `=`, `==`, `!=`, `in`, `notin`, `key` and `!key`. List endpoints honour `limit` and `continue` and set `remainingItemCount` in the list metadata. Pod list endpoints accept `?inlineImages=omit` (used by the kubelet) to omit `kube32.io/wasm-base64` annotation payloads.

`GET /api/v1/namespaces/{ns}/pods/{name}/log` supports `follow`, `previous`, `timestamps`, `tailLines`, `sinceSeconds`, `limitBytes` and `container`. Logs live in a 2 KiB RAM ring per pod on the board, 1 KiB for the previous instance and 16 KiB on the simulator. Nodes with storage also write `<root>/kube32/logs/<ns>_<pod>/<container>.log`, capped at 32 KiB and rotated to `.log.1` on restart.

## Watches

`?watch=true` works on every stored resource collection. One hub thread in `include/api/watch_hub.hpp` owns all watch and log-follow streams:

| Setting | Value |
| --- | --- |
| Concurrent streams | 8 on a board with PSRAM, 4 without, 16 on the simulator; `429 TooManyRequests` past the limit |
| Queue per stream | 16 events; overflow sends `410 Expired` and closes the stream |
| Replay ring for `resourceVersion=N` | 32 events, at most 64 KiB of objects; older versions get `410 Expired` |
| Bookmarks | every 30 s with `allowWatchBookmarks=true` |
| `sendInitialEvents=true` | initial `ADDED` events followed by a `BOOKMARK` annotated `k8s.io/initial-events-end` |
| Stream lifetime | 1800 s, or `timeoutSeconds` |

## Limitations

- Server-side apply and JSON Patch return `415 UnsupportedMediaType`; use `kubectl apply` without `--server-side`.
- Request bodies must be JSON, except `ConfigMap`, `Secret`, `Namespace`, `Service`, `Deployment`, `Job`, `SelfSubjectAccessReview` and `SelfSubjectRulesReview`, which also accept protobuf (`kubectl create configmap|secret|namespace|deployment|job|service` works); other kinds in protobuf return `415 UnsupportedMediaType` with the reason.
- A persisted create or update that exceeds the object store capacity returns `500 InternalError` (`object store is full: X of Y bytes in use, N more needed`), checked before writing; shrinking writes always pass, and a lost create race returns `409 AlreadyExists`.
- There is no RBAC; authenticated callers have full access to all resources.
- `Endpoints` and `EndpointSlice` are read-only and do not support `watch`.
- Deployments manage Pods directly without `ReplicaSet` objects.
- `deletecollection`, `exec`, `attach` and `portforward` are not supported.
- `fieldValidation=Strict` checks top-level and `metadata` fields only, so unknown fields nested inside `spec` are accepted.
- Paginated lists with `limit` and `continue` read from the current store state without a snapshot.
- Request bodies are capped at 2 MiB, chunked request bodies return `411` (except node proxy storage uploads), and headers larger than the 4 KiB board receive buffer return `431`.
- Creating an object in a non-existent namespace succeeds, and deleting a namespace does not delete the objects in it.
