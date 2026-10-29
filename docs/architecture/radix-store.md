# Object store

The control plane stores API objects as JSON strings in a single-process, in-memory radix tree in `src/storage/radix_tree.cpp` and `include/storage/store.hpp`. A single recursive mutex guards the tree and watch list. The API server copies data out of the store and releases the lock before it serializes and streams responses. Watch callbacks also run after the lock is released.

## Keys

`resource_info` in `src/api/route_handler.cpp` defines the etcd-style key paths for stored resources:

| Kind | Key |
| --- | --- |
| Node | `/registry/nodes/<name>` |
| Namespace | `/registry/namespaces/<name>` |
| Pod | `/registry/pods/<namespace>/<name>` |
| Service | `/registry/services/<namespace>/<name>` |
| ConfigMap | `/registry/configmaps/<namespace>/<name>` |
| Secret | `/registry/secrets/<namespace>/<name>` |
| Event | `/registry/events/<namespace>/<name>` |
| Deployment | `/registry/deployments/<namespace>/<name>` |
| DaemonSet | `/registry/daemonsets/<namespace>/<name>` |
| Job | `/registry/jobs/<namespace>/<name>` |
| Lease | `/registry/leases/<namespace>/<name>` |
| Ingress | `/registry/ingresses/<namespace>/<name>` |

List requests run a prefix scan on the matching branch of the tree, such as `/registry/pods/default/`. `Endpoints` and `EndpointSlice` objects are computed from Services and Pods on read and are not stored in the tree.

## Versions and watches

Each write or delete increments a store-wide revision counter and injects the value into `metadata.resourceVersion`. `put_extended` accepts an `expected_version` for compare-and-swap writes, but the HTTP PUT and PATCH handlers omit it, making concurrent API updates last-writer-wins.

`WatchHub` in `include/api/watch_hub.hpp` subscribes to `/registry/` and manages up to 8 concurrent watch or log streams on PSRAM boards, 4 without PSRAM, and 16 on the simulator:

- Watches with `resourceVersion=0` or unset stream `ADDED` events for current objects, followed by a `k8s.io/initial-events-end: "true"` bookmark when `sendInitialEvents=true`.
- Watches with `resourceVersion=N` replay events from a 32-event ring buffer. If `N` was evicted or exceeds the current revision, the server sends an `ERROR` frame with code `410` and reason `Expired`, then closes the stream.
- Each stream buffers up to 16 pending events and sends `BOOKMARK` frames every 30 seconds when `allowWatchBookmarks=true`.

## Persistence journal

When node storage is mounted, or when you pass `--enable-flash` to the simulator, `FlashLog` in `include/storage/flash_log.hpp` persists objects to `<root>/kube32/store/journal.img`:

- The journal is a preallocated ring of 4 KiB sectors with `K32L` magic and a sequence number. Each `Put`, `Delete`, or `RevisionMark` record is 4-byte aligned and carries a CRC32 over the sector sequence number, header, key, and value. The journal reclaims space from the oldest sector by re-appending live records to the head.
- Leases and Events stay in RAM, as defined in `include/storage/persist_policy.hpp`. Node heartbeat status flushes at most every 10 minutes, while Node labels, annotations, `spec`, or `Ready` status flush within 1 second. All other objects flush within 1 second. Pending updates to the same key coalesce, and the journal skips writes when the content outside `resourceVersion` is unchanged.
- The journal file is 1 MiB on internal flash at `/data`, with ~480 KB of live object capacity, and 4 MiB on a microSD card at `/sd` or in the simulator, with ~1.9 MB of live capacity. The maximum value size is 64 KiB. When live bytes reach capacity, the store refuses new object creations with HTTP 500 `StorageFull`, while updates and deletes continue to work.
- At boot, `recover` scans the consecutive sector run, skips torn records with invalid CRCs, and replays the latest record per key. The store reserves revisions on flash in blocks of 10,000 via `RevisionMark` records so `resourceVersion` never goes backwards across restarts. Watches opened before a restart receive `410` and relist. Pods already in `Succeeded` or `Failed` phase are not restarted after boot.
- Send `K32_CMD:STORE` on the serial console to print live keys, byte usage, sector usage, and revision counters.

