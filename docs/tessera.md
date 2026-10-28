# Tessera demo

Tessera is an issue tracker built to exercise Kube32 with a real application.
It has a board view, a filterable issue list, full-text search, issue pages
with comments, an activity feed and analytics. The seeded data reads like
the tracker of a small product team. Everything that holds or computes data
runs as WebAssembly pods on the cluster; the browser gets static files and
JSON.

## Parts

| Part | Source | Runs as | Size |
| --- | --- | --- | --- |
| Core | `apps/tessera/core` (freestanding C, no libc) | request pod `tessera-core` on the control plane | `images/tessera-core.wasm`, 76,228 bytes; 1,280 KiB of linear memory |
| HUD | `apps/tessera/hud/hud.c` | `app_main` pod `tessera-hud` on the Cardputer | `images/tessera-hud.wasm`, 5,338 bytes; one 64 KiB page |
| Web client | `apps/tessera/web` (plain JavaScript, no build step) | static files served by the ingress | `images/web/tessera/*.gz`, 30,027 bytes gzipped |

- **The core** keeps users, projects, issues, comments, a term index and
  sessions inside its linear memory, and answers the HTTP API in
  `apps/tessera/API.md`. It seeds its data set in chunks from `k32_tick`, so
  it answers requests within milliseconds of starting, while the rest of the
  data arrives. Its native test suite has 1,244 checks
  (`make -C apps/tessera/core test`).
- **The HUD** draws open issue counts and request rates on the Cardputer's
  display, beeps on new issues, and sends each key press to the core as an
  event. Typed text shows up live in browsers. Enter adds it as a comment on
  the issue last viewed.
- **The web client** keeps a server-sent event stream open and refetches
  when issues or comments change.

`apps/tessera/API.md` still names an earlier placement, with the core on
`esp32-node-02` and the HUD on `esp32-node-01`, and an older project list. The
placement in `examples/tessera.yaml`, described above, is the one that runs.

## Running it

On the boards, after the [quick start](quickstart.md):

```
kubectl apply -f examples/tessera.yaml
```

Then open `http://<control plane address>/`. Every seeded user's password is
`demo`; `mara` is one of them.

On the simulator, start `kube32-sim` with `--ingress-port 8080 --pod-port
17000`, apply the same manifest and open `http://127.0.0.1:8080/`. The
simulator ticks pods once a second instead of every 10 ms while there is
work, so seeding 2,400 issues takes about four minutes there instead of
about 40 s.

`apps/tessera/web/mock_server.py` serves the web client against a Python
implementation of the same API. It is for working on the front end without
a cluster and is not part of what runs on the boards.

## Measured on the boards

Measured during development in October 2026, control plane with PSRAM at
80 MHz:

| What | Time |
| --- | --- |
| Core pod: load and compile | about 0.5 s |
| Core pod: `k32_init` | 0.26 s |
| Seeding 2,400 issues in the background | about 40 s |
| `GET /api/issues?limit=25` | about 0.2 s |
| Board view (about 50 KB of JSON) | about 0.6 s |

For comparison, the same C code compiled natively for an Apple M-series
laptop handles 47,150 requests per second in `make bench` (commit `6b29a7c`).
Running under wasm3 on an ESP32-S3 is a different regime. On the board, an
indirect call through the interpreter (the issue comparator) was measured at
about 15 µs. That cost is why the list and board scans avoid calling the
comparator for most candidates.

To measure a running instance yourself:

```
python3 tools/tessera_loadtest.py http://<control plane address> --clients 4 --seconds 60
```

See [Tools](cli-reference.md#tessera_loadtestpy) for a caveat about one of its
endpoints.
