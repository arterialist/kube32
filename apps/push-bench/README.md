# push-bench

push-bench measures how many frames of a few KB a pod on one node can push to a pod on another node over a channel topic, at a fixed frame rate.

| Pod | Node | What it runs | Memory |
| --- | --- | --- | ---: |
| `push-pub` | control plane | publishes frames of each size in `SIZES` at each rate in `RATES` on `doom.draw`, `SECONDS` a run | 32 KiB |
| `push-sub` | Cardputer (`kube32.io/board: cardputer`) | reassembles the frames from `k32_event` and logs one line a run | 24 KiB |

With `CHUNK=0` (the manifest's setting) each frame goes out whole in one `k32_publish` message of up to 16 KiB and arrives as one event. With `CHUNK` from 17 to 200 the frame goes out with `k32_event_emit` (at most 200 bytes, `K32_MAX_EVENT_BYTES`) as `count` chunks of a 16-byte header and up to 184 frame bytes (`push.h`). The header holds the run, the frame number, the chunk's index and count, the frame's size and rate, and the publisher's wall clock when the frame went out. The subscriber checks every chunk's bytes and reassembles up to `WINDOW` frames at a time. A chunk of a newer frame when every slot is taken evicts the oldest frame as partial.

```
push sent run=1 size=4600 fps=6 chunk=200 count=25 frames=318 chunks=7950 skipped=42 emit_err=0 (first rc=0) frame_send_ms mean=168 max=1453
push recv run=1 size=4600 fps=6 count=25 window=2 frames=360 whole=238 partial=80 missing=42 whole_fps=3.96 chunk_loss_ppm=144666 ok=7698 ...
```

`skipped` counts frames the publisher let pass because the previous frame was still going out; the subscriber counts them as missing. `whole_fps` is whole frames over the run's length. `chunk_loss_ppm` compares the chunks received with every chunk of every frame number in the run, skipped frames included. `reasm_ms` runs from a frame's first chunk to its last on the subscriber's clock. `e2e_ms` runs from the publisher's wall clock to the subscriber's, so it includes the offset between the two clocks.

## Running it

```
make -C apps/push-bench            # modules and build/push-bench.yaml (both inline)
make -C apps/push-bench apply      # the subscriber, then the publisher (DELAY 10 s)
kubectl --context kube32 logs -f push-sub
make -C apps/push-bench delete
```

Publisher: `TOPIC` (default `doom.draw`), `SIZES` (default `4600,12600`), `RATES` (default `6,8,10`), `CHUNK` (default 200; 0 for whole frames), `PER_TICK` (chunks a 10 ms tick, 0 for the whole frame in one tick), `SECONDS` (60), `PAUSE` (5), `DELAY` (10). Subscriber: `WINDOW` (1 to 4), `CHECK` (0 skips the byte check).

## Results

Devkit publisher, Cardputer subscriber, wordsketch-listen stopped.

### Whole frames (`k32_publish`, `CHUNK=0`)

Firmware with large channel messages; messages go over ESP-NOW (the default route). Whole frames a second, then the publisher-to-subscriber latency (p50 / p95 ms, both nodes' wall clocks).

| Size | 6 fps | 8 fps | 10 fps | Latency p50 / p95 ms |
| ---: | ---: | ---: | ---: | --- |
| 4,600 | 5.91 | 7.95 | 9.51 | 36 to 40 / 93 to 133 |
| 12,600 | 5.02 | 6.55 | 7.26 | 86 to 89 / 122 to 136 |

Runs of 45 s. Every frame that arrived had the right bytes. 1 to 5% of the 4.6 KB frames and 16 to 27% of the 12.6 KB frames were lost. The Cardputer's Wi-Fi stops sending under a sustained stream of large messages (in this pass during the 4.6 KB run at 10 fps): the node turns NotReady while ESP-NOW frames keep arriving, and only a reboot brings it back.

### Frames in 200-byte events (`k32_event_emit`)

Runs of 60 s. "Delivered" is the share of chunks the publisher sent that arrived.

| Size | Rate | Chunk | Per tick | Window | Frames sent | Whole frames/s | Partial | Delivered | Reassembly p50 / p95 / max ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 4,600 | 6 | 200 | all | 1 | 277 | 0.88 | 224 | 77.7% | 229 / 586 / 819 |
| 4,600 | 8 | 200 | all | 1 | 392 | 0.95 | 333 | 73.8% | 140 / 651 / 847 |
| 4,600 | 10 | 200 | all | 1 | 518 | 0.60 | 480 | 70.3% | 124 / 370 / 547 |
| 12,600 | 6 | 200 | all | 1 | 234 | 0.01 | 233 | 55.9% | 599 (one frame) |
| 12,600 | 8 | 200 | all | 1 | 236 | 0 | 235 | 53.1% | |
| 12,600 | 10 | 200 | all | 1 | 230 | 0 | 229 | 56.4% | |
| 4,600 | 6 | 200 | all | 2 | 325 | 0.66 | 285 | 77.9% | 177 / 514 / 543 |
| 4,600 | 6 | 100 | all | 2 | 302 | 0.03 | 300 | 53.5% | 1,371 (two frames) |
| 4,600 | 6 | 200 | 3 | 2 | 229 | 3.55 | 16 | 99.3% | 211 / 339 / 2,187 |
| 4,600 | 6 | 200 | 6 | 2 | 318 | 3.96 | 80 | 96.8% | 144 / 275 / 1,000 |
| 4,600 | 8 | 200 | 6 | 2 | 331 | 4.01 | 90 | 96.0% | 141 / 297 / 610 |
| 12,600 | 6 | 200 | 6 | 2 | 134 | 1.03 | 72 | 95.6% | 404 / 566 / 636 |
| 12,600 | 8 | 200 | 6 | 2 | 129 | 0.85 | 78 | 96.1% | 400 / 610 / 1,296 |

No chunk arrived with wrong bytes or twice. Two limits show:

- **Bursts.** A frame published in one tick reaches the Cardputer as 25 to 69 back-to-back ESP-NOW frames, and a fifth to almost half of them are lost before they reach the pod. The publisher's node counts 4% of its channel sends as unacknowledged, so most of the loss happens on the Cardputer after its radio has acknowledged a frame: a node without PSRAM queues 8 received frames, 8 dispatched messages and 8 pod events. Smaller chunks mean more frames a burst and lose more; a second reassembly slot recovers nothing. Spreading a frame over ticks, 3 to 6 chunks at a time, brings delivery to 96 to 99%.
- **The publisher's rate.** One event takes the devkit about 4 ms to send (an acknowledged ESP-NOW frame to each ESP32 peer, a UDP datagram to each other peer), and a paced frame waits for further ticks, which come every 25 to 36 ms. The publisher sends 270 events a second at most in one tick, and 95 to 155 paced: 5.5 frames a second of 4.6 KB and 2.2 of 12.6 KB.
