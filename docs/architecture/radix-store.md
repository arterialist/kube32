# Object store

The control plane keeps every API object in an in-memory radix tree
(`src/storage/radix_tree.cpp`, behind the `Store` interface in
`include/storage/store.hpp`). It plays the role etcd plays in Kubernetes, at
a much smaller scale: one process, no replication, no consensus.

## Keys

Objects are stored as JSON strings under etcd-style keys. The kinds that have
their own handlers use these keys:

| Kind | Key |
| --- | --- |
| Node | `/registry/nodes/<name>` |
| Pod | `/registry/pods/<namespace>/<name>` |
| Deployment | `/registry/deployments/<namespace>/<name>` |
| Lease | `/registry/leases/<namespace>/<name>` |

Namespaces, Services, ConfigMaps, Secrets, DaemonSets, Jobs and Events
follow the same pattern; `resource_info` in `src/api/route_handler.cpp` lists
them. A list request is a prefix scan, for example `/registry/pods/default/`.
Prefix scans walk only the matching branch of the tree.

## Versions

Each write increments a store-wide revision. The object's `resourceVersion`
carries it, and watches use it to replay recent events. `put` can take an
expected version and fail on a mismatch. The API server's PUT path does not
pass one, so concurrent updates through the API are last-writer-wins.

## Persistence

| Target | Behaviour |
| --- | --- |
| Boards | Not persisted. `src/main_esp32.cpp` sets `enable_flash_persistence = false`. Nothing mounts the `storage` partition in `partitions.csv`. |
| Simulator | Persisted with `--enable-flash --data-dir <dir>`: one `<key>.json` file per object, written to a temporary file, fsynced and renamed. `recover()` reloads the directory at start. Leases and events are not written. |

The class that writes those files is called `LittleFSJournal`, but it uses
plain POSIX file calls. It is not LittleFS, and there is no checksum,
snapshot or append-only log.

What a control-plane reboot on a board does, as the code stands:

1. Every object is lost, including the Tessera pods.
2. The workers fail two heartbeats and register again.
3. On their next sync they see no pods bound to them, and they stop the pods
   they were running.
4. Nothing recreates the workloads; apply the manifests again.

## Concurrency

One mutex guards the tree. The API server copies what it needs out of the
store and releases the lock before it serializes and streams a response, so
a slow client does not hold up the scheduler or the controllers.
