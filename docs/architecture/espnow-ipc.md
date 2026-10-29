# Events between nodes

Pods exchange messages across `src/ipc/channel.cpp` over ESP-NOW and UDP port 7400. Each frame uses the transport that discovered the peer. The simulator uses UDP on port 7400 plus the node ID.

## Host API

| Call | Effect | Requirement |
| --- | --- | --- |
| `k32_event_emit(topic, payload)` | Publishes to local subscribers and known peers | `hardware.kube32.io/ipc` |
| `kube32_channel_open(name, flags)` | Opens a receive queue for `name` | `hardware.kube32.io/ipc` |
| `kube32_channel_recv(handle, buf, cap, timeout_ms)` | Reads one frame from the queue | Open handle |
| `kube32_channel_send(handle, data, len)` | Delivers to the local queue only | Open handle |
| `k32_event(topic_ptr, topic_len, ptr, len)` | Receives topics listed in `kube32.io/event-topics` | `k32_event` and `k32_alloc` exports |

Set the pod annotation `kube32.io/event-topics: a,b` to deliver channel topics from any node to the pod's `k32_event` export. Messages queue up to 16 entries and run on the `k32_events` thread, which uses a 16 KiB internal RAM stack or a 32 KiB PSRAM stack. In the `tessera` app, `tessera-hud` publishes keystrokes with `k32_event_emit("tessera.input", ...)` and reads `tessera.events` with `kube32_channel_open` and `kube32_channel_recv`. `tessera-api` subscribes to `tessera.input` via `kube32.io/event-topics` and publishes updates on `tessera.events`, which the Ingress streams as Server-Sent Events via `kube32.io/event-stream`.

## Limits

| Item | Limit |
| --- | --- |
| `k32_event_emit` payload | 200 bytes; topic 1 to 64 bytes |
| `k32_events` queue | 16 messages; stack 16 KiB internal RAM or 32 KiB PSRAM, 256 KiB on the simulator |
| Published message | 1024 bytes, split into 224-byte payload frames |
| Reassembly | 32 fragments, 500 ms timeout, 8 messages in flight |
| Receive queue per channel | 8 slots of 512 bytes on hardware; 16 slots of 1024 bytes on the simulator |
| Maximum frame size | 250 bytes |
| Delivery | Best effort, no acknowledgement or retransmission |

## Peer discovery

Nodes send a HELLO frame carrying the node ID and name on topic `k32.hello` every 3 s on hardware, or every 2 s on the simulator, to:

- The ESP-NOW broadcast address on hardware.
- Every known peer.
- UDP seed addresses on port 7400.

Workers seed the control plane address at boot. Every 10 s, the control plane reads node objects and seeds the `InternalIP` of each registered node on UDP port 7400. UDP seeding routes traffic when boards sit on different access points or Wi-Fi channels and cannot receive ESP-NOW broadcast frames. On the simulator, pass `--peer name:id:port` to add static UDP peers. Peers never expire. ESP-NOW runs unencrypted at 24 Mbps.

## Frame format

Defined as a packed little-endian struct in `include/ipc/channel.hpp`:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Magic `K32C` (`0x4332334B` as little-endian `u32`) |
| 4 | 1 | Version `1` |
| 5 | 1 | Flags: `0x01` fragment, `0x02` first, `0x04` last, `0x08` ack unused, `0x10` HELLO |
| 6 | 2 | Sequence number |
| 8 | 4 | FNV-1a hash of channel or topic name |
| 12 | 4 | FNV-1a hash of sending pod |
| 16 | 4 | FNV-1a hash of target pod, ignored on receive |
| 20 | 1 | Fragment index |
| 21 | 1 | Fragment count |
| 22 | 2 | Payload length, 0 to 224 |
| 24 | n | Payload |
| 24 + n | 2 | CRC-16/CCITT-FALSE, polynomial `0x1021`, initial `0xFFFF`, over header and payload |

Frames with an invalid magic, version, length, or CRC are dropped.

## Limitations

`kube32_channel_send` delivers only to the local node; use `k32_event_emit` to reach other boards.
