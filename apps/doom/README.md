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

Pixel rasterization is 981 K ops a frame (73%), 17.4 ops a pixel over 56.5 K pixels (37.4 K wall, 15.9 K floor and ceiling, 3.2 K sprite). The draw list is 497 wall columns, 174 spans and 6 sprites a frame (p95: 937, 427, 18), 7.2 KB packed (p95 14.8 KB, max 16.1 KB). A frame touches 185 distinct texture columns (21.5 KB; p95 361, 39.9 KB), 3 to 4 flats (16 KB; p95 7, 28 KB) and 4 sprite patches (6.8 KB; p95 11, 13.5 KB). Without texture lookups a 416 KiB zone plays E1M1.

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
