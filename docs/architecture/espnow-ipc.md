# Events between nodes

Pods on different boards exchange small messages through a publish and
subscribe layer in `src/ipc/channel.cpp`. On the boards it runs over two
transports at once: ESP-NOW and UDP on port 7400. Each frame is sent on the
transport through which the peer was discovered. This is the path the
Tessera keypad uses: a key pressed on the Cardputer travels as an event to
the core pod on the control plane, and from there to browsers as a
server-sent event.

## What a pod uses

| Call | Effect | Needs |
| --- | --- | --- |
| `k32_event_emit(topic, payload)` | publishes to local subscribers and to every known peer | `hardware.kube32.io/ipc` |
| `kube32_channel_open(name, flags)` | opens a receive queue for frames published under `name` | `hardware.kube32.io/ipc` |
| `kube32_channel_recv(handle, buf, cap, timeout_ms)` | reads one frame from that queue | an open handle |
| `kube32_channel_send(handle, data, len)` | delivers to the queue on the same node only | an open handle |
| `k32_event` export | the node calls it for each frame on `<app>.events` and `<app>.input` | exporting `k32_event` and `k32_alloc` |

`kube32_channel_send` does not cross nodes. The code has a remote-route
table, but nothing fills it. To reach another board, use `k32_event_emit`.

On the boards the app name is `tessera`, so the topics delivered to
`k32_event` are `tessera.events` and `tessera.input`. The Tessera HUD pod:

- publishes key presses with `k32_event_emit("tessera.input", ...)`;
- reads `tessera.events` through `kube32_channel_open` and
  `kube32_channel_recv`.

The core pod publishes on `tessera.events`, and the ingress turns those
events into SSE for browsers.

## Limits

| Item | Limit |
| --- | --- |
| `k32_event_emit` payload | 200 bytes; topic 1 to 64 bytes |
| Published message | 1024 bytes, split into frames of up to 224 bytes of payload |
| Reassembly | up to 32 fragments, 500 ms timeout, 8 messages in flight |
| Receive queue per channel | 8 slots of 512 bytes on a board; 16 slots of 1024 bytes on the simulator |
| Delivery | best effort: no acknowledgement, no retransmission |

## Peer discovery

Every node sends a HELLO frame every 3 s (every 2 s on the simulator). The
frame carries the node's ID and name, and goes out:

- to the ESP-NOW broadcast address (boards only);
- to every known peer;
- to UDP seed addresses.

Workers seed the control plane's address. Every 10 s the control plane seeds
the `InternalIP` of every registered node. Seeding is what makes events flow
when the two boards sit on different access points of the same network: they
may then be on different Wi-Fi channels, and ESP-NOW frames never arrive. On
such a network the keypad events used to stop at the worker; that is why the
UDP transport exists (commit `464c402`). Peers never expire.

ESP-NOW runs unencrypted at a fixed 24 Mbps rate. The simulator uses UDP
only, on port 7400 plus the node ID.

## Frame format

Little-endian, packed, defined in `include/ipc/channel.hpp`:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic, the bytes `K32C` (`0x4332334B` read as a little-endian `u32`) |
| 4 | 1 | version, `1` |
| 5 | 1 | flags: `0x01` fragment, `0x02` first, `0x04` last, `0x10` HELLO; `0x08` (ack requested) is defined but unused |
| 6 | 2 | sequence number |
| 8 | 4 | FNV-1a hash of the channel or topic name |
| 12 | 4 | FNV-1a hash of the sending pod |
| 16 | 4 | FNV-1a hash of the target pod (ignored on receive) |
| 20 | 1 | fragment index |
| 21 | 1 | fragment count |
| 22 | 2 | payload length, 0 to 224 |
| 24 | n | payload |
| 24 + n | 2 | CRC-16/CCITT-FALSE (polynomial `0x1021`, initial `0xFFFF`) over header and payload |

A frame is at most 250 bytes, the ESP-NOW maximum. Frames with a bad magic,
version, length or CRC are dropped.

Latency and throughput of this path have not been measured in isolation. The
end-to-end keypad path (key, HUD, transport, core pod, browser) works on the
boards; no timing for it has been recorded.
