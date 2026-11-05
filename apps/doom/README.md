# DOOM

DOOM ([doomgeneric](https://github.com/ozkl/doomgeneric), a portable Chocolate Doom) as a kube32 request pod. `k32_tick` runs the game at 35 tics a second and draws a frame; the pod serves the frame and takes keys over HTTP, and reads the IWAD from a blob, lump by lump, into DOOM's purgeable zone cache.

| Request | Answer |
| --- | --- |
| `GET /` | A page that shows the game and sends the keyboard |
| `GET /frame` | 768 bytes of palette (RGB), then 320 x 200 palette indices; `X-Doom-Frame` counts the frames drawn |
| `POST /key` | Lines `d <code>` (pressed) or `u <code>` (released), DOOM key codes (`doomgeneric/doomkeys.h`) |
| `GET /stats` | Zone, heap, blob and frame counters |
| `GET /bench?tics=<n>&frames=<n>` | Runs `n` tics without drawing, then draws `n` frames; the microseconds of each part |

A path matches by its last segment, so the pod also answers under an Ingress prefix (`/doom/frame`).

With `DOOM_PLAYERS=2` the pod plays player 1 of a two-player game and a browser joins as player 2 ([Two players](#two-players)).

## Build

```sh
make                       # build/doom.wasm, a 512 KiB zone (ZONE_KB)
make bench                 # build/doombench: tic and frame costs on the host's wasm3
```

`make bench` needs the repository's wasm3 libraries: `cmake --build build --target wasm3 wasm3_opframes`.

### Where a frame's cost goes

`make stats` builds `build/doom-stats.wasm`, the same engine with counting rasterizers (`port/drawlist_stats.c`). `doombench --stats-mode 1` replaces the column and span rasterizers with counters, so a frame costs the BSP and visibility pass and the texture lookups alone; `--stats-mode 2` also answers texture and flat lookups with dummy data. `--view-only` draws the 3D view without the status bar, `--spin` turns the player the whole time so the view sweeps the room. The bench then prints, per frame, what a split renderer would ship (wall columns, spans, sprites, their packed bytes) and the texture working set the frame touches.

Shareware E1M1, 700 tics, `--view-only --spin`, after the first second (wasm3 operations a frame, mean / p95):

| Mode | Ops a frame | What it measures |
| --- | --- | --- |
| 0, real rasterizers | 1,337 K / 1,762 K | the whole 320 x 168 view |
| 1, counting rasterizers | 356 K / 704 K | BSP, visibility, sprite sorting, texture and flat lookups |
| 2, no texture lookups | 315 K / 629 K | what a node that only emits a draw list pays |

### The sim-only pod

`make sim` builds `build/doom-sim.wasm`: the same engine with no rasterizers and no screen buffers. A frame is a draw list (`port/drawlist_emit.c` documents the format): wall strips with an 18-byte header a seg part and 3 bytes a column, planes with 13 bytes a span, sprites with their per-column clip, two-sided middle texture columns. `GET /drawlist?keys=d173,u173` answers the latest list (16 KiB at most, `X-Doom-Frame` counts frames) and takes key events in the query. A rasterizer draws the frame from the list and the render pack alone.

```sh
python3 wad.py pack build/wad/doom1-e1m1-min.wad build/wad/doom1-e1m1.pack       # what a rasterizer reads
python3 wad.py simpack build/wad/doom1-e1m1-min.wad build/wad/doom1-e1m1-sim.wad  # what the sim reads
```

The render pack holds the textures composed as the engine composes them (single-patch textures stay patches, for their posts), the flats, the sprite patches, the palette and the colormaps, with tables by texture, flat and sprite lump number. The sim pack is the IWAD with patches and sprites cut to their 8-byte headers and flats to their names, so lump numbers stay the same.

Shareware E1M1, 700 tics, `--spin`, after the first second, on the sim pack:

| | mean | p95 | max |
| --- | --- | --- | --- |
| Draw list, bytes | 4,644 | 9,660 | 12,595 |
| Draw list, entries | 35 | 89 | 117 |
| Frame, wasm3 ops | 333 K | 657 K | 706 K |
| Tic, wasm3 ops | 15.4 K | 43 K | 92 K |

No frame reaches the 16 KiB cap. The sim build leaves out the engine's screen-side files (`SIM_EXCLUDE` in the Makefile: menu, automap, intermission, finale, wipe, HUD, status bar, savegames, rasterizers, screen, sound, joystick, configuration; `port/sim_stubs.c` answers what the rest still calls): its code section is 124,887 B against the full engine's 185,551 B, its module 266,525 B, and its static data, zero-initialised statics and stack end at 410 KB (444 KB at 320 wide) against 533 KB. With that, a 256 KiB zone plays E1M1 with 80 KB to spare and no purges (224 KiB plays with purges; the engine's own `-kb` is the knob): **704 KiB of linear memory at 240 wide**, 736 KiB at 320. `kube32-podsize` (the API server's estimate, `tools/podsize.cpp`) puts the sim's overhead at 457 KiB read from flash and 717 KiB held in RAM, so the pod takes 1,161 KiB of the devkit's 1,280 KiB with the module from flash, 1,421 KiB with it held. The sim pack is 273,714 B (1,004,720 B cut from the 1,278,434 B map cut); the render pack is 1,323,116 B: textures 734,680 (71; 614,080 composed, 120,600 kept as patches), flats 98,304 (24), sprites 477,032 (296), tables 3,628, palette and colormaps 9,472. The sim reads 158 KB from its blob over the 700 tics, with no zone purges at 416 KiB.

### The view pod

`make view` builds `build/doom-view.wasm` (`view/view.c`, 7.4 KB): the Cardputer side. The sim pod publishes each frame's draw list whole on the channel topic `doom.draw` (`k32_publish`, at `DOOM_FPS` frames a second, capped at 12 KiB at 240 wide). `k32_publish` sends no faster than the rate the view's node announces and waits for the last message to leave before it sends the next, so the sim draws and publishes a frame only once the last list has left at `DOOM_DRAW_RATE` bytes a second (default 49152, the Cardputer's), and assumes a quarter less after a publish that still waited (over 60 ms; a publish itself takes about 9 ms on the devkit), down to half of it, and an eighth more after each that did not: the pod, and the POSTs of a second player, do not wait on the radio. `GET /stats` reports how long a tick and a publish take (`tick_ms_mean`, `tick_ms_max`, `publish_ms_mean`, `publish_ms_max`), the ticks a frame waited for the last to leave (`frame_waits`) and the rate assumed (`draw_rate_assumed`); the view pod receives it in `k32_event`, decodes it (`view/drawlist.c`) into records for the [`k32_raster_columns` and `k32_raster_spans`](../../docs/app-development.md#raster-kernels) kernels, which read the textures in place from the render pack blob (`DOOM_PACK_BLOB`, a flash blob), and puts each band of 27 rows on the screen with `k32_display_blit_pal`. Every tick it sends the keys its keyboard produced as an event on `doom.keys` (`"d173,u173,..."`, DOOM key codes; the Cardputer reports presses only, so a key is held for one tick). It needs `hardware.kube32.io/tft-display`, `keyboard` and `ipc`, the `DISPLAY`, `KEYBOARD` and `CHANNEL_IPC` capabilities, and `kube32.io/event-topics: doom.draw`. The textures never enter its memory: 12 KiB of draw list (after the topic), a 6.5 KiB band, the pack's 3 KiB of tables and 4 KiB of stack, a 32 KiB memory limit; `kube32-podsize build/doom-view.wasm --memory 32` puts it at 62 KiB of the Cardputer's 80 KiB pool block with the module held in RAM, 55 KiB as a blob image, which leaves the block room for the node's held messages. A frame that does not arrive whole is dropped by the node; the next one draws normally. Both pods log their flow every 100 frames (`kubectl logs`).

`make W=240` and `make W=240 sim` build the engines at 240 x 135, the Cardputer's screen (`DOOM_SCREENBLOCKS=11` draws the whole screen; the status bar is not drawn below 320 wide). The 240-wide sim costs 267 K ops a frame (p95 531 K) and its draw list is 3.9 KB (p95 8.1 KB, max 10.8 KB), on the same course.

### The check against the engine

`make check` builds `build/drawlist_check` (`bench/drawlist_check.cpp`): the full engine and the sim run in lockstep on the host's wasm3, same IWAD, keys and clock; every frame the sim's draw list is drawn with the native kernels (`src/runtime/pod_raster.cpp`) through the view pod's decoder and the render pack, and compared pixel by pixel with the frame the full engine drew.

```sh
./build/drawlist_check build/doom.wasm build/doom-sim.wasm build/wad/doom1-e1m1-min.wad build/wad/doom1-e1m1.pack --tics 350 --spin
./build/drawlist_check build/doom-240.wasm build/doom-sim-240.wasm build/wad/doom1-e1m1-min.wad build/wad/doom1-e1m1.pack --tics 350 --spin --screenblocks 11
```

At 320 x 168 and at 240 x 135, 350 frames of the turning course match the engine pixel for pixel (0 of 18,816,000 and 0 of 11,340,000). A column reads up to 128 texels past its start whatever lies there (vanilla's masked index), so the port zeroes 128 bytes past every cached lump and composite (`DG_LUMP_PAD`) and the pack keeps 128 zero bytes past every patch, composite and sprite: both read zeros. On the host the kernels take 130 us a frame at 320 wide and 65 us at 240 (4,339 and 2,426 column records, 1,302 and 749 span records, in 5 bands), the decoder 32 and 18 us.

`--state` also compares the two engines' game states every tic (`port/state_digest.c`: CRC-32 digests of the players, map objects, sectors, `P_Random`'s index and the other thinkers; `M_Random`'s index is the screen side's and kept apart) and the renderers' frames (drawsegs with their clip arrays, vissprites, visplanes, openings), and names the first part that differs; `--sim-args` and `--sim-wad` give the sim its own zone and IWAD; `--locate x,y,tic` names the entry behind a pixel. `make verify` runs the three comparisons the host suite guards: 2,000 tics on both courses at 320 wide and on the turning course at 240 wide, the full engine with a 640 KiB zone and the sim on the sim pack with 224 or 256 KiB; `make verify TICS=10000` is the long run. The game states and every pixel agree over 10,000 tics on both courses, and the result does not depend on the sim's zone (224, 256, 480 and 640 KiB tried). The one divergence found this way was a wall column the engine clips away (`dc_yh < dc_yl`, which `R_DrawColumn` draws nothing of) that the emitter recorded with its bottom row wrapped to 255; the game states had never differed.

Pixel rasterization is 981 K ops a frame (73%), 17.4 ops a pixel over 56.5 K pixels (37.4 K wall, 15.9 K floor and ceiling, 3.2 K sprite). The draw list is 497 wall columns, 174 spans and 6 sprites a frame (p95: 937, 427, 18), 7.2 KB packed (p95 14.8 KB, max 16.1 KB). A frame touches 185 distinct texture columns (21.5 KB; p95 361, 39.9 KB), 3 to 4 flats (16 KB; p95 7, 28 KB) and 4 sprite patches (6.8 KB; p95 11, 13.5 KB). Without texture lookups a 416 KiB zone plays E1M1.

## On the boards

The two pods run from blob images ([Blob image](../../docs/app-development.md#blob-image-wasmblobname)) with their data as blobs on the nodes, and `examples/doom.yaml` carries the wordsketch pods at 0 replicas (`examples/wordsketch.yaml` the DOOM pods at 0), so one `kubectl apply` switches the boards either way:

```sh
make -C apps/doom pods                                  # the modules and the packs as blobs (build/blobs/)
make -C apps/doom manifest                              # build/doom.yaml (build/doom-only.yaml without wordsketch)
make -C apps/doom upload SERVER=http://<control-plane-ip>:6443
kubectl --context kube32 apply -f apps/doom/build/doom.yaml
curl -s http://<control-plane-ip>/doom/stats            # frames published, keys received
curl -s http://<control-plane-ip>/doom-view/stats       # frames drawn on the Cardputer
kubectl --context kube32 apply -f apps/wordsketch/build/wordsketch.yaml   # back to wordsketch
```

| Blob | Node | Bytes | 64 KiB extents | What |
| --- | --- | --- | --- | --- |
| `doom-sim` | control plane | 279,897 | 5 | the sim pod's module (`make W=240 sim`) |
| `doom-wad` | control plane | 94,034 | 2 | the sim pack (`wad.py simpack`: the map, the texture and sprite headers, the palette) |
| `doom-web` | control plane | 648,562 | 10 | player 2's page, engine and IWAD, gzipped (`wad.py web`; served with `Content-Encoding: gzip`) |
| `doom-view` | Cardputer | 7,351 | 1 | the view pod's module |
| `doom-pack` | Cardputer | 1,437,292 | 22 | the render pack (`wad.py pack`) |

The flash store places blobs on 64 KiB boundaries and keeps a replaced blob's old bytes until its pod lets go, so the largest free gap can be well under the free bytes: DOOM's three devkit blobs take 17 extents, which fit beside the wordsketch and llm-diffusion blobs and the module blobs (`llm-diffusion`, `llm-shard`, `llm-coordinator`); upload `doom-web` first, and delete it before replacing it. The draw lists travel as channel messages of up to 12 KiB; the Cardputer takes them on the UDP route (the default), which held 10 minutes at 8 fps where ESP-NOW stalled its Wi-Fi within minutes. `wordsketch-listen` must be at 0 while DOOM runs: the Cardputer's pool block holds one of them.

## Two players

Player 1 plays on the boards: the sim pod on the control plane runs the game, and the Cardputer's view pod draws it and sends the keys, as above. Player 2 plays in a browser at `http://<cluster>/doom/`: the page runs the same engine (`build/doom.wasm`) and draws its own view. It is a vanilla netgame of two players (`-lockstep 2`, `doomgeneric/d_loop.c`) whose ticcmds travel through kube32 (`port/net.c`):

- The pod orders the ticcmds. It runs tic T once player 2's ticcmd for T is in, builds player 1's at that moment, and publishes both on a channel topic, two tics a message. The Ingress serves the topic to the page as Server-Sent Events. The pod runs the tics due by its clock whenever the node ticks it and whenever a POST arrives, and catches up to a second of tics when the node ticks it late, so a busy node keeps the game at 35 tics a second.
- The page runs the tics from that stream, in order, and nothing else. It builds player 2's ticcmds by the clock, 4 tics (the input delay) past the pod's tic, which it estimates from the newest confirmed tic, the time since it arrived and half the round trip; a ticcmd then has the input delay for the trip one way. It sends them with `POST /doom/tics`, two at a time. Every POST carries all of the page's ticcmds the pod has not confirmed, and every answer carries the confirmed tics the page lacks, so a lost POST, a lost event or a dropped event stream costs time and never the game state. The page runs its tics as the messages arrive rather than on a timer, so a hidden tab, whose timers the browser slows to once a second, keeps the game at full speed.
- A late ticcmd holds the pod's next tic, and its clock with it, until it arrives; the tic runs as the POST lands. A page that sends nothing for 5 s leaves the game ("Player 2 left the game") and player 1 plays on alone.
- A page that joins starts a new session: the pod restarts the map, and the page's engine starts it in the same state at the same tic. Reloading the page joins again.
- Both sides digest their game state every 35 tics (`port/state_digest.c`) and compare: the page reports its digest in each POST, the pod sends its own with the tics. `GET /doom/netstats` and the page's status line count the checks. The vanilla consistency check (`consistancy` in the ticcmd) runs as well.

| Request | Answer |
| --- | --- |
| `GET /doom/net` | The game: players, session, tic, and the IWAD name and arguments a client's engine starts with |
| `POST /doom/join` | Takes player 2's place: `{"t":"join","g":<session>,"s":<first tic>,"delay":4,"digest":35}` |
| `POST /doom/tics` | `g=<session>&a=<first tic lacking>&s=<first tic>&c=<ticcmds, hex>[&d=<tic>&h=<digest>]`; answers the confirmed tics from `a` |
| `GET /doom/netstats` | Tics run, tics that waited and for how long, how early ticcmds arrive, digest checks; how often the node ticks the pod (`tick_gap_mean_ms`, `tick_gap_max_ms`), the tics run as POSTs land (`post_tics`) and the tics of time dropped after a second behind (`dropped_tics`) |
| `GET /doom/`, `/doom/doom-net.js`, `/doom/doom.wasm`, `/doom/doom.wad` | The page, its host, the engine and the IWAD, from the `DOOM_WEB_BLOB` blob |

Confirmed tics, on the topic and in each answer: `{"t":"tics","g":<session>,"s":<first tic>,"m":<players in the game, bits>,"c":"<hex>"[,"d":<tic>,"h":"<digest>"]}`. A ticcmd is 7 bytes (forwardmove, sidemove, angleturn, chatchar, buttons, consistancy), so a tic of two players is 28 hex digits and a message of two tics is about 100 bytes; a channel message holds at most 4 tics.

| Setting (pod env) | Meaning |
| --- | --- |
| `DOOM_PLAYERS` | `2`: the two-player game |
| `DOOM_NET_TOPIC` | The channel topic of the confirmed tics (default `doom.tics`); the Ingress's `kube32.io/event-stream` serves it |
| `DOOM_NET_DELAY` | The input delay a page plays with, in tics (default 4; the page's `?delay=` overrides it). It covers the trip one way and its jitter: 5 for a round trip near 170 ms |
| `DOOM_NET_BATCH` | Tics a message (default 2, at most 4) |
| `DOOM_NET_TIMEOUT_MS` | How long a silent player 2 stays in the game (default 5000) |
| `DOOM_NET_DIGEST` | Tics between state digests (default 35, 0 for none) |
| `DOOM_WEB_BLOB` | The blob holding the page, the engine and the IWAD (default `doom-web`) |

```sh
make W=240 sim && make                      # the pod's engine and the page's
python3 wad.py simpack build/wad/doom1-e1m1-min.wad build/wad/doom1-e1m1-sim.wad
python3 wad.py web build/wad/doom1-e1m1-min.wad build/doom.wasm build/wad/doom-web.wad
for b in doom-sim:build/doom-sim-240.wasm doom-sim-wad:build/wad/doom1-e1m1-sim.wad doom-web:build/wad/doom-web.wad; do
  curl -X PUT --data-binary @${b#*:} http://<control-plane>:6443/api/v1/nodes/<control-plane node>/proxy/blobs/${b%%:*}
done
kubectl --context kube32 apply -f doom-2p.yaml   # the pod, its Service, and the Ingress with the event stream
```

The three blobs take 2.2 MB of the control plane's 3.2 MB blob region, so replacing `doom-web` takes a `DELETE` of the old one first. The page is 1.66 MB to load: the engine (366 KiB) and the IWAD (1.2 MiB). At 240 wide the pod's linear memory stays at 704 KiB; the netgame adds 8,875 B to the module (278,313 B), and `kube32-podsize` puts the overhead at 491 KiB read from flash (467 KiB for the single-player pod), so the pod takes 1,195 KiB of the devkit's 1,280 KiB.

`make verify-net` plays both sides on the host's wasm3: the sim pod as the authority, driven as a node drives it (`k32_tick` every 10 ms, the requests through `k32_handle`, its channel messages captured), and the full engine as the page drives it (a step on every message and every 4 ms, a frame every 16 ms), each player on a scripted course. Every message crosses a relay that delays it 5 to 45 ms, so messages overtake each other, and loses 2% of them; the network then goes dark for 1.5 s, the page reloads, and later goes silent past the timeout and joins again. The two game states are compared at every tic both ran (`bench/net_check.cpp`), with the sim at 320 and at 240 wide against the page's 320, and once more on a slow network (60 to 110 ms one way) with a node that ticks the pod only every 350 ms (`--pod-tick-ms 350`). Each run plays 7,200 tics, and all 7,147 to 7,155 compared tics agree. The input delay sets how often a tic waits for player 2; over the 2,000 tics before the outage (57.1 s at 35 tics a second), with the page posting 18 times a second:

| Input delay | 5 to 45 ms one way: tics that wait | Time waited | 60 to 110 ms one way, pod ticked every 350 ms: tics that wait | Time waited |
| --- | --- | --- | --- | --- |
| 3 tics (86 ms) | 1.3% | 479 ms in 57.5 s | | |
| 4 tics (114 ms) | 0.4% | 119 ms in 57.2 s | 2.15% | 775 ms in 57.8 s |
| 5 tics (143 ms) | 0.1% | 21 ms in 57.1 s | 0.35% | 89 ms in 57.3 s |
| 6 tics (171 ms) | the first tic only | 6 ms in 57.1 s | the first tic only | 15 ms in 57.1 s |
| 8 tics (229 ms) | | | none | 0 ms in 57.1 s |

The outage holds one tic for 1.5 s, and the game goes on from there; the silent page holds one tic for the 5 s timeout, then player 2 leaves the game. `--client-args` gives the page's engine other arguments: with `-nomonsters` the check stops at tic 0 and names the parts that differ.

## IWAD

```sh
python3 wad.py fetch                                    # Freedoom phase 1 (BSD), checked by SHA-256
python3 wad.py cut build/freedoom1.wad build/e1m1.wad   # one map and the lumps it needs
python3 wad.py cut doom1.wad build/doom1-e1m1.wad --no-intermission
```

The shareware `doom1.wad` works too: set `DOOM_IWAD=doom1.wad`. Upload the cut IWAD as a blob (`docs/storage.md#blobs`) and name it in the pod:

| Setting | Meaning |
| --- | --- |
| annotation `kube32.io/blobs` | the blob holding the IWAD |
| env `DOOM_WAD_BLOB` | that blob's name (default `doom-wad`) |
| env `DOOM_IWAD` | the IWAD file name DOOM identifies the game by (default `freedoom1.wad`) |
| env `DOOM_ARGS` | DOOM's command line, such as `-warp 1 1 -skill 3 -kb 480` (`-kb`: the zone in KiB) |

A cut IWAD has no music or sound effects, and no other maps: leaving the map stops the game.

## License

The engine in `doomgeneric/` is GPL-2.0-or-later (`doomgeneric/COPYING`); `port/`, `bench/`, `wad.py` and the changes to the engine are under the same license. A module built from this directory is therefore distributed under the GPL. The kube32 runtime that runs it is a separate program under Apache-2.0, as the rest of this repository is. Freedoom is BSD-3-Clause. The shareware `doom1.wad` is not part of this repository.
