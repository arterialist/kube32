# Kubernetes API coverage

The API server (`src/api/`) listens on port 6443 with plain HTTP/1.1. It
speaks JSON only: a request body encoded as protobuf gets
`415 UnsupportedMediaType`, and kubectl then retries in JSON. When kubectl
asks for `as=Table;v=v1;g=meta.k8s.io`, list responses come back in
`meta.k8s.io/v1` Table form. Large responses are streamed with chunked
transfer encoding.

This page describes the code at the time of writing. Where a figure is a
default, the file that sets it is named.

!!! warning "No authentication"
    The server reads no credentials and serves no TLS. Every request is
    accepted, and `SelfSubjectAccessReview` and `SelfSubjectRulesReview` always
    answer "allowed". Anyone who can reach port 6443 controls the cluster.

## kubectl and k9s

On the boards, `kubectl get`, `describe`, `logs`, `top` and `get events`
have been used, and so has k9s. For this page the simulator was also driven
with `apply`, `create`, `delete`, `scale deployment`, `get -w` and `logs -f`;
all of them worked.

`kubectl apply` and `create` need no extra flags. This was verified on the
simulator. The server publishes a
minimal OpenAPI v3 index (`/openapi/v3`) with one document per group
version. Each document lists the `fieldValidation` parameter for the kinds
it stores, and there are no schemas. kubectl therefore leaves validation to
the server, which then checks only top-level and `metadata` fields:

```
$ kubectl apply -f typo.yaml        # metadata.lables instead of metadata.labels
Error from server (BadRequest): error when creating "typo.yaml": strict decoding error: unknown field "metadata.lables"
```

A misspelt field deeper in the object, for example
`spec.containers[0].imagePulPolicy`, is accepted and stored. `/openapi/v2`
returns 404.

## Discovery

| Path | Answer |
| --- | --- |
| `/version` | Fixed values, see [Known inaccuracies](#known-inaccuracies) |
| `/api`, `/api/v1` | core `v1` |
| `/apis` | `apps`, `batch`, `coordination.k8s.io`, `events.k8s.io`, `authorization.k8s.io`, `metrics.k8s.io` |
| `/apis/<group>/<version>` | resource list for each group above |
| `/openapi/v3`, `/openapi/v3/<group version>` | index and per-group documents (no schemas); unknown groups get 404 |

Discovery accepts only `GET` and `HEAD`.

## Resources

| Kind | Collection paths | Verbs | What acts on it |
| --- | --- | --- | --- |
| Pod | `/api/v1/pods`, `/api/v1/namespaces/{ns}/pods` | list, get, create, update, patch, delete; `status` and `log` subresources | scheduler binds it; the kubelet on that node runs it and reports status |
| Node | `/api/v1/nodes` | list, get, create, update, patch, delete; `status` | kubelets register and heartbeat; node lifecycle controller |
| Deployment | `/apis/apps/v1/deployments`, `.../namespaces/{ns}/deployments` | list, get, create, update, patch, delete; `status`, `scale` | Deployment controller creates and replaces pods directly (no ReplicaSets) |
| Event (core/v1 and events.k8s.io/v1) | `/api/v1/events`, `/apis/events.k8s.io/v1/...` | list, get, create, update, patch, delete | written by the scheduler, kubelets and node lifecycle controller; at most 64 kept |
| Lease | `/apis/coordination.k8s.io/v1/...` | list, get, create, update, patch, delete | node heartbeats |
| Namespace, ConfigMap, Secret | core `v1` | list, get, create, update, patch, delete | stored only |
| Service | core `v1` | list, get, create, delete | stored only: no ClusterIP, endpoints or proxy |
| DaemonSet | `apps/v1` | list, get, create, delete | no controller (see [Known inaccuracies](#known-inaccuracies)) |
| Job | `batch/v1` | list, get, create, delete | no controller (see [Known inaccuracies](#known-inaccuracies)) |

No other kinds are served. That includes ReplicaSets, StatefulSets,
Endpoints, ServiceAccounts, RBAC and CRDs. There is no `exec`, `attach`,
`portforward`, `eviction` or `binding` subresource, and `deletecollection`
answers 405.

Five namespaces exist from the start: `default`, `kube-system`, `kube-public`,
`kube-node-lease` and `kube32-cluster`. Objects can be created in a namespace
that does not exist, and deleting a namespace deletes nothing else.

### Writes

- **PATCH** applies a recursive JSON merge whatever the `Content-Type`: objects
  merge, arrays and scalars replace. A `null` is stored as null instead of
  deleting the key. There is no strategic list merge and no JSON Patch.
- **PUT** does not check `resourceVersion`.
- **Not supported:** server-side apply, `dryRun` (a dry-run request is
  executed) and `limit`/`continue` (lists always return everything).
- **Duplicate names:** creating a Pod, Node, Deployment, Namespace, ConfigMap
  or Secret that already exists returns 409. Creating a Service, DaemonSet,
  Job or Lease that exists overwrites it.
- **Pod deletion** is graceful when the pod is bound to a node (30 s by
  default, or `gracePeriodSeconds`).
- **Size limits:** a request body may be up to 2 MiB. Chunked request bodies
  get 411. Headers larger than the receive buffer get 431; the buffer is
  4 KiB on the boards.

### Lists and watches

- **Table output** is available for every list except DaemonSets, Jobs and
  metrics. Pods, Nodes and Deployments have their own columns, and a
  single-object `get` of those three kinds also returns a Table.
- **Selectors:** `fieldSelector` supports `=`, `==` and `!=` on any dotted
  path. `labelSelector` supports `=`, `==`, `!=`, `in`, `notin`, `key` and
  `!key`.
- **Watches:** `?watch=true` works on every stored kind. One hub thread owns
  the streams; its defaults are in `include/api/watch_hub.hpp`:

| Setting | Value |
| --- | --- |
| Concurrent streams (watches and log follows together) | 8 on a board with PSRAM, 4 without, 16 on the simulator; 429 past the limit |
| Queue per stream | 16 events; overflow sends `410 Expired` and closes the stream |
| Replay ring for `resourceVersion=N` | 32 events; older versions get `410 Expired` |
| Bookmarks | every 30 s with `allowWatchBookmarks=true` |
| `sendInitialEvents=true` | initial `ADDED` events, then a BOOKMARK annotated `k8s.io/initial-events-end` (the WatchList protocol k9s uses) |
| Stream lifetime | 1800 s, or `timeoutSeconds` |

A watch on an item path, such as `pods/{name}`, streams the whole collection.

### Pod logs

`GET .../pods/{name}/log` supports `follow`, `timestamps`, `tailLines`,
`sinceSeconds` and `limitBytes`; `previous` and `sinceTime` are ignored. The
log is a ring buffer per pod:

- 1 KiB on a board without PSRAM, 2 KiB on a board with PSRAM, 16 KiB on the
  simulator.
- Fed by the pod's `k32_log` calls and by the runtime's own lines (container
  started, trapped, entry point returned).
- The kubelet ships it to the API server.

### Metrics

`metrics.k8s.io/v1beta1` serves NodeMetrics and PodMetrics from samples that
the kubelet sends with its heartbeat:

| Metric | Source |
| --- | --- |
| Node CPU | FreeRTOS run-time statistics: non-idle task time over wall time |
| Node memory | heap in use |
| Pod memory | linear memory, bytecode and interpreter stack |
| Pod `esp32.io/psram` | the part of a pod that lives in PSRAM, as a separate key |
| Pod CPU | interpreter time |

Samples older than 30 s are dropped, and at most 32 pod samples are kept. On
the simulator, "node memory" is the heap of the whole simulator process
compared with a declared 240 KiB, so `kubectl top nodes` shows more than
100 %.

## Known inaccuracies

These are places where the server reports something it did not measure or do.
They are bugs, listed here so nobody relies on them:

- **`/version`** returns fixed strings: `v1.33.0-kube32`, a build date of
  `2026-10-26`, `go1.22.0` and `gcc-xtensa`. `/api` reports
  `serverAddressByClientCIDRs` as `127.0.0.1:6443`.
- **DaemonSet create** writes one pod per node immediately, with phase
  `Running` and pod IPs `192.168.4.1`/`192.168.4.2`, before any kubelet has
  run it. Pod names are `<ds>-01` or `<ds>-02`.
- **Job create** writes one pod already in phase `Succeeded` on
  `esp32-node-01`.
- **Node heartbeats.** When a board measures 0 bytes of free PSRAM (a board
  without PSRAM), its heartbeat advertises 3840Ki of allocatable PSRAM
  anyway. GPIO, I2C and ADC counts and `cpu: 2` are constants, and `Ready`
  is always `True`.
- **Missing node fields** are filled in by the node table with `ESP-IDF v5.2`,
  `FreeRTOS-v10.4` and `127.0.0.1`. The standalone simulator seeds its two
  nodes with those values.
- **Deployments** report `NewReplicaSetAvailable` and `ReplicaSetUpdated`
  condition reasons, though no ReplicaSets exist.
- **Pod reasons.** Pods that fail to start are given the reason
  `CrashLoopBackOff`, and `restartCount` goes up after a trap, but nothing is
  ever restarted. An out-of-bounds memory access is reported as `OOMKilled`.
- **Completion codes.** A pod whose entry point returns is reported as
  "exited with code 0", whatever value it returned.
- **Discovery** lists `update` and `patch` for Services, DaemonSets and Jobs,
  which answer 405.
- **Strict validation of events.k8s.io/v1.** The field list holds core/v1
  Event fields only, so `regarding` and `note` are rejected.
