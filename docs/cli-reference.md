# Tools

The cluster itself is driven with `kubectl` (and k9s). The tools below cover
the parts kubectl does not: building and shipping Wasm pods, reaching a board
over USB, and measuring an app.

| Tool | Purpose |
| --- | --- |
| `tools/kube32-cli` | build, check, deploy, embed and `sd-push` Wasm pods |
| `tools/kube32_serial_gateway.py` | reach a board's HTTP servers over its USB serial port |
| `tools/tessera_loadtest.py` | latency percentiles against a running Tessera ingress |
| `tools/embed_assets.py`, `tools/pio_embed.py` | generate the firmware's embedded image and asset tables (run by the builds) |
| `scripts/flash.sh` | flash one board ([Flashing](flashing-troubleshooting.md)) |

## kube32-cli

Python 3, standard library only. [Writing app pods](app-development.md) walks
through every subcommand with real output; this is the summary from
`tools/kube32-cli --help`:

| Command | What it does |
| --- | --- |
| `build <dir>` | compile a C, Rust or AssemblyScript app directory to `.wasm` and check it |
| `check <file>` | validate a `.wasm` against the app ABI and the opcode set of this wasm3 build (`--memory` adds a size limit) |
| `deploy <file>` | create a Pod that carries the module in the `kube32.io/wasm-base64` annotation, through `kubectl create`; no reflash |
| `embed <file>` | copy a checked module into `images/` so the next firmware build embeds it |
| `sd-push <file> --to <mount point>` | copy a checked module onto a microSD card as `kube32/images/<name>.wasm` |

Exit codes:

| Code | Meaning |
| --- | --- |
| 0 | ok |
| 1 | input or toolchain problem |
| 2 | usage |
| 3 | the ABI check failed |
| 4 | kubectl failed |
| 5 | `deploy` found the pod already there; pass `--replace` |

`deploy` uses the current kubectl context unless `--context`, `--kubeconfig`
or `--server` is given. It still passes `--validate=false` to kubectl. Since
the server started publishing an OpenAPI index, that flag is no longer
needed, and it does no harm.

The test suite is `python3 -m unittest discover -s tools/tests -v`, 62 tests.

## kube32_serial_gateway.py

Needs `pyserial`. It listens on local ports and relays each HTTP request to a
board through the console's `K32_HTTP` framing:

```
python3 tools/kube32_serial_gateway.py [--port <serial port>] [--map <local>:<device>]...
```

- **Port.** Without `--port` it uses `KUBE32_SERIAL_PORT`, then the first
  native USB-Serial/JTAG port, then a CH340 port.
- **Mappings.** The defaults are `6443:6443` (API) and `8080:80` (ingress).
- **Through a worker.** A worker forwards relayed requests to the control
  plane, so any board can be the one on USB.

It relays one request at a time and returns a response only once the board
has closed the connection. Watches, `kubectl logs -f` and k9s do not work
through it. The protocol is described under
[HTTP over the console](flashing-troubleshooting.md#http-over-the-console).

## tessera_loadtest.py

Runs concurrent clients against a Tessera ingress for a fixed time. It then
prints latency percentiles, throughput and errors per endpoint, all from
real requests:

```
python3 tools/tessera_loadtest.py http://<control-plane-address> --clients 4 --seconds 60 [--no-keepalive]
```

The request mix is weighted towards lists and the board view. One of its
endpoints, `GET /api/issues/TES-42`, predates the current seed data: the
project keys are now `WEB`, `IOS` and `API`, so that request answers 404 and
is counted as an error. Keep that in mind when reading its error column.

## Embedding assets

`tools/embed_assets.py` turns `images/*.wasm` and `images/web/<app>/*.gz`
into tables the image registry and the ingress serve:

- **Host build.** CMake runs it with `--host-out`, which writes byte arrays
  into `build/generated/`.
- **Firmware build.** PlatformIO runs it through `tools/pio_embed.py` with
  `--esp`, which writes `src/embedded/embedded_blobs_esp.cpp` and the file
  list the ESP-IDF component embeds.
