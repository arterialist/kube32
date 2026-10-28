# Writing app pods

An app pod is a WebAssembly module that the node runs in its wasm3
interpreter. It talks to the node through a small set of host functions
(imports) and a few entry points the host calls (exports), declared in
`include/runtime/k32_app_abi.h`. This page covers writing one in Rust, C or
AssemblyScript, checking it with `tools/kube32-cli`, and getting it onto a
node without reflashing.

!!! note "What was run for this page"
    Every command and output on this page comes from one macOS machine:
    builds and checks locally, deployments against the host simulator
    (`kube32-sim`, built from this repository). None of it was run on the
    ESP32-S3 boards. Where board behaviour differs it is stated from the
    source and marked as not measured.

---

## Two kinds of pod

| Kind | Exports | Who runs it |
| --- | --- | --- |
| Request pod | `k32_alloc`, `k32_handle`; optionally `k32_init`, `k32_tick`, `k32_event` | The node's HTTP workers call `k32_handle` once per request; a timer calls `k32_tick`; channel events arrive in `k32_event` |
| `app_main` pod | `app_main` (or `_start`, or `main`, looked up in that order) | The node starts it on the pod's own thread; it loops on its own (display, keyboard, channels) and the container ends when it returns |

A module may export both. `k32_init` runs once when the container is
created; a non-zero return fails the pod (`k32_init returned <n>`).

### Request block

Before every call to `k32_handle` the host asks the guest for a buffer with
`k32_alloc(size)` (`size` is the block length plus one), copies the request
into it and calls `k32_handle(ptr, len)`. The block is:

```
METHOD SP PATH[?QUERY] LF
(Header-Name ":" SP value LF)*
LF
body bytes
```

Only `Cookie`, `Content-Type`, `X-Request-Id` and `X-Forwarded-For` are
forwarded. The guest answers with `k32_resp_begin` (status, content type),
any `k32_resp_header` calls, any number of `k32_resp_write` calls and
`k32_resp_end`. The host streams the body with chunked encoding, so the guest
never holds the whole response. If `k32_handle` returns without calling
`k32_resp_begin`, the client gets
`500 {"error":"pod_failure","message":"workload produced no response"}`.

`k32_event(topic_ptr, topic_len, ptr, len)` gets its topic and payload the same
way, through `k32_alloc`.

### Imports

All imports live in the `env` module. The signatures are the ones the host
links (`src/runtime/app_pod.cpp`, `src/runtime/host_functions.cpp`);
`kube32-cli check` rejects anything else.

| Import | Signature | Needs |
| --- | --- | --- |
| `k32_resp_begin`, `k32_resp_header`, `k32_resp_write`, `k32_resp_end` | `v(iii)`, `v(iiii)`, `i(ii)`, `v()` | nothing |
| `k32_log`, `k32_time_ms`, `k32_random`, `k32_env_get`, `k32_mem_stats` | `v(ii)`, `I()`, `i()`, `i(iiii)`, `i(ii)` | nothing |
| `k32_event_emit` | `i(iiii)` | `hardware.kube32.io/ipc` |
| `kube32_channel_open/send/recv/close` | `i(ii)`, `i(iii)`, `i(iiii)`, `i(i)` | `hardware.kube32.io/ipc` |
| `k32_display_text`, `k32_display_rect`, `k32_display_bar` | `i(iiiiii)`, `i(iiiii)`, `i(iiiiiii)` | `hardware.kube32.io/tft-display` |
| `k32_key_read` | `i()` | `hardware.kube32.io/keyboard` |
| `k32_tone` | `i(iii)` | `hardware.kube32.io/speaker` |
| `k32_fs_open/read/write/close/size/list/remove` | `i(iii)`, `i(iii)`, `i(iii)`, `i(i)`, `i(i)`, `i(iiii)`, `i(ii)` | `hardware.kube32.io/sd` and a volume claim ([microSD card](sd-card.md)) |
| `esp32_delay_ms` | `v(i)` | nothing |
| `esp32_gpio_*`, `esp32_adc_read`, `esp32_i2c_*`, `esp32_uart_*` | see `host_functions.cpp` | `esp32.io/*` resources ([GPIO and other peripherals](workloads/gpio-blinker.md)) |

Every import is linked into every module; a module only lists the ones it
calls, and that list says which manifest resources it needs.

### Limits

| Limit | Value | What happens past it |
| --- | --- | --- |
| Request block (request line, forwarded headers, body) | 16 KiB (`K32_MAX_REQUEST_BYTES`) | `413`; a 16,200-byte body passed, a 17,000-byte body got `413` on the simulator |
| Event payload | 200 bytes (`K32_MAX_EVENT_BYTES`), topic 1..64 bytes | `k32_event_emit` returns `K32_ERR_INVAL` (-22) |
| Response header | name 1..64 bytes, value up to 256 | the header is dropped |
| Content type in `k32_resp_begin` | under 128 bytes | `application/json` is sent instead |
| Log line | 512 bytes | the line is dropped |
| `k32_display_text` | 64 bytes | `K32_ERR_FAULT` |
| `k32_tone` | 2000 ms, 100 % | clamped |
| Files | 96-byte paths, 4 open per pod and per node, 4096 bytes per call | see [microSD card](sd-card.md) |
| Work per call | 400 M wasm3 gas units (`set_app_gas_budget`) | the call traps |
| Linear memory | declared by the module (initial = maximum) | see below |

**Memory.** The module declares its linear memory when it is linked
(`--initial-memory` = `--max-memory`, a multiple of 64 KiB). The node gives
the pod a sandbox of `max(limits.memory, esp32.io/psram)`, at least 64 KiB; a
module that declares more does not load. `requests.memory` is also what the
scheduler counts against a node's free heap: each simulated node offers
240 KiB, so a fourth 64 KiB pod on one node stays `Pending` with
`FailedScheduling ... insufficient heap memory or peripheral resources`.
One-page pods get a 4 KiB wasm3 value stack (`wasm_runtime.cpp`), larger
ones 16 KiB; the shadow stack for C and Rust locals lives in linear memory
(`-z stack-size`, 16 KiB by default in `kube32-cli build`).

**Ticks.** The simulator calls `k32_tick` once a second. On the boards
(`main_esp32.cpp`) it is once a second, or every 10 ms while `k32_tick`
returns a positive value, and on a board without PSRAM the tick thread has a
4 KiB stack (the source says display boards host no ticking pods). Ticking on
a board was not measured for this page; export `k32_tick` only when the pod
needs it.

---

## Rust

`sdk/rust` is a Cargo workspace with the `kube32-sdk` crate (`#![no_std]`, no
allocator, no dependencies) and two example pods:

| Path | What it is |
| --- | --- |
| `sdk/rust/kube32-sdk` | ABI bindings: `Request`, `Response`, `emit`, `env`, `log`, `time_ms`, `Buf`, `Channel`; `hardware` and `fs` modules behind features |
| `sdk/rust/examples/counter` | Request pod: a counter, an event per increment, `k32_tick`, `k32_event`, env |
| `sdk/rust/examples/notes` | Request pod that keeps a file in its microSD volume (feature `fs`) |

Features of `kube32-sdk`:

| Feature | Adds |
| --- | --- |
| `panic-handler` (default) | a `#[panic_handler]` that logs through `k32_log` and traps |
| `hardware` | `hardware::{display_text, display_rect, display_bar, key_read, tone, rgb565}` |
| `fs` | `fs::{File, list, remove}` over the `k32_fs_*` imports |

### A request pod

`kube32_app!` exports the entry points; the app is a few functions. Abridged
from `sdk/rust/examples/counter/src/lib.rs`:

```rust
#![no_std]

use core::sync::atomic::{AtomicU64, Ordering::Relaxed};
use kube32_sdk::{emit, kube32_app, parse_u32, Buf, Error, Request, Response};

static COUNTER: AtomicU64 = AtomicU64::new(0);

fn handle(req: &Request) -> i32 {
    match (req.method, req.path) {
        ("POST", "/inc") => {
            let by = req.query_param("by").and_then(parse_u32).unwrap_or(1);
            let value = COUNTER.load(Relaxed) + by as u64;
            COUNTER.store(value, Relaxed);

            let mut ev = Buf::<64>::new(); // event payloads are at most 200 bytes
            ev.push_str("{\"type\":\"counter\",\"value\":").push_u64(value).push_str("}");
            let outcome = match emit("counter.events", ev.as_bytes()) {
                Ok(_) => "sent",
                Err(Error::Permission) => "denied", // no hardware.kube32.io/ipc grant
                Err(_) => "failed",
            };

            let mut resp = Response::json(200);
            resp.write_str("{\"counter\":").write_u64(value)
                .write_str(",\"event\":\"").write_str(outcome).write_str("\"}");
            resp.end()
        }
        _ => {
            let mut resp = Response::json(404);
            resp.write_str("{\"error\":\"not_found\"}");
            resp.end()
        }
    }
}

kube32_app! {
    handle: handle,
}
```

The full keys are `alloc` (buffer size, default 16 KiB + 512), `init`,
`handle`, `tick` and `event`, in that order. The macro reserves the
`k32_alloc` buffer as a static array and answers `400 bad request` itself
when the request line is not ASCII (`GET /caf\xc3\xa9` got that answer on the
simulator). `Request` borrows `method`, `path`,
`query`, `cookie`, `content_type`, `request_id`, `forwarded_for` and `body`
from the block, with `query_param`, `cookie(name)` and `percent_decode` for
the encoded parts. On wasm32 without the atomics feature the `Atomic*` statics
compile to plain loads and stores (the module uses no `0xFE` opcodes), which
keeps `static mut` out of the app.

### Building

The workspace's release profile is `opt-level = "z"`, `lto = true`,
`codegen-units = 1`, `panic = "abort"`, `strip = true`, and
`sdk/rust/.cargo/config.toml` passes the link flags (16 KiB shadow stack, one
64 KiB page). Plain cargo works:

```
$ cd sdk/rust && PATH=$HOME/.cargo/bin:$PATH cargo build --release --target wasm32-unknown-unknown
    Finished `release` profile [optimized] target(s) in 0.19s
```

The target was already installed on the machine used for this page
(`rustup target list --installed` lists `wasm32-unknown-unknown`); without it,
`rustup target add wasm32-unknown-unknown`. The cargo has to be rustup's: a
Homebrew cargo earlier in `PATH` has no wasm32 standard library and fails
with ``can't find crate for `core` ``. `kube32-cli build` puts
`~/.cargo/bin` first by itself.

`kube32-cli build` runs the same build with the memory and stack sizes on its
command line, copies the module to `<crate>/build/<name>.wasm` and checks it:

```
$ tools/kube32-cli build sdk/rust/examples/counter
building <repo>/sdk/rust/examples/counter (rust)
sdk/rust/examples/counter/build/counter.wasm: 6,400 bytes (code 5,289, data 691)
kind: request pod (k32_handle)
exports (8): k32_alloc, k32_event, k32_handle, k32_init, k32_tick, __data_end [global], __heap_base [global], memory [memory]
imports (8): k32_env_get, k32_event_emit, k32_log, k32_resp_begin, k32_resp_end, k32_resp_header, k32_resp_write, k32_time_ms
memory: 1 page = 65,536 bytes (64Ki), max 1 page
features beyond MVP: bulk-memory (memory.copy x1, memory.fill x6); sign-ext (i32.extend8_s x1)
hardware: ipc  (manifest resources: hardware.kube32.io/ipc)
ok
```

`--memory 128Ki --stack 32Ki` gives a two-page module; the flags go through
`RUSTFLAGS`, which replaces the ones in `.cargo/config.toml`.

What keeps the module small: no `core::fmt` (the `write_u64`/`push_u64`
helpers format numbers by hand), and a request parser that checks for ASCII
instead of validating UTF-8. In an unstripped build of the counter the
handler with everything inlined into it is 1,888 bytes of code, `env()` 675
(most of it UTF-8 validation of the value) and the JSON string escaper 416.

A crate outside the workspace needs `crate-type = ["cdylib"]`, a path
dependency on `kube32-sdk` and the same release profile; `kube32-cli build`
passes the link flags itself, so it needs no `.cargo/config.toml`.

### An `app_main` pod

`kube32_main!` exports `app_main`. This one was built from a crate outside the
workspace with `kube32-sdk = { path = ".../sdk/rust/kube32-sdk", features = ["hardware"] }`:

```rust
#![no_std]

use kube32_sdk::hardware::{display_rect, display_text, key_read, rgb565, tone};
use kube32_sdk::{delay_ms, kube32_main, log, Buf, Error};

fn main_loop() -> i32 {
    let white = rgb565(255, 255, 255);
    let black = rgb565(0, 0, 0);
    if let Err(Error::Permission) = display_rect(0, 0, 240, 135, black) {
        log("blink: no display grant");
        return 1; // returning from app_main ends the container
    }
    let _ = tone(880, 50, 30);
    for i in 0..5u64 {
        let mut line = Buf::<32>::new();
        line.push_str("tick ").push_u64(i);
        let _ = display_text(8, 8, line.as_str(), white, black);
        if let Ok(Some(k)) = key_read() {
            let mut msg = Buf::<16>::new();
            msg.push_str("key ").push_u64(k as u64);
            log(msg.as_str());
        }
        delay_ms(200);
    }
    log("blink: done");
    0
}

kube32_main!(main_loop);
```

`kube32-cli build` reported 1,679 bytes, `kind: app_main pod`, `hardware:
display, keyboard, speaker`. On the simulator the display and speaker hooks
print to the node log, and `key_read` always finds the queue empty:

```
[TONE] 880 Hz for 50 ms
[DISPLAY 8,8] tick 0
...
[DISPLAY 8,8] tick 4
```

`kubectl logs` then shows `blink: done` and
`[kube32] entrypoint returned, container completed`. Since commit `ff03c3b`
the kubelet then reports the pod `Succeeded`. A one-line C `app_main` that
returned 0 was shown by `kubectl get pod` as `0/1 Completed`, with a
`Completed` event saying `exited with code 0`. The return value is not read,
though: the same module returning 3 got the same status and the same event.
Completed pods are not restarted.

---

## C

A C pod includes the ABI header and exports the entry points with
`K32_EXPORT`:

```c
/* hello-c: the smallest useful request pod in C. */
#include "runtime/k32_app_abi.h"

#define LIT(s) (s), (uint32_t)(sizeof(s) - 1)

static unsigned char request_buf[K32_MAX_REQUEST_BYTES + 512];
static uint32_t hits;

/* The host copies each request block here before calling k32_handle. */
K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof request_buf ? (uint32_t)(uintptr_t)request_buf : 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* req = (const char*)(uintptr_t)ptr;   /* "GET /path?query\n..." */
    uint32_t line = 0;
    while (line < len && req[line] != '\n') line++;

    char num[10], tmp[10];
    uint32_t v = ++hits, n = 0, t = 0;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (t) num[n++] = tmp[--t];

    k32_resp_begin(200, LIT("text/plain"));
    k32_resp_write(LIT("hello from C, request "));
    k32_resp_write(num, n);
    k32_resp_write(LIT(": "));
    k32_resp_write(req, line);
    k32_resp_write(LIT("\n"));
    k32_resp_end();
    return 200;
}
```

A directory of `.c` files builds with the flags the repository's own apps use
(`-mcpu=mvp -msign-ext -mbulk-memory`, freestanding, no libc), linked by
`rust-lld` (found under `~/.rustup`) or `wasm-ld`. Run from the directory
that holds `hello-c/`, with `$KUBE32` the repository checkout (scratch paths
shortened to `$SCRATCH`):

```
$ $KUBE32/tools/kube32-cli build hello-c -v
building $SCRATCH/apps/hello-c (c)
clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra -I<repo>/include -c $SCRATCH/apps/hello-c/hello.c -o /var/folders/.../kube32-cli-cujr7ze3/hello.o
~/.rustup/toolchains/stable-aarch64-apple-darwin/lib/rustlib/aarch64-apple-darwin/bin/rust-lld -flavor wasm --no-entry --export-dynamic --allow-undefined -z stack-size=16384 --initial-memory=65536 --max-memory=65536 -o $SCRATCH/apps/hello-c/build/hello-c.wasm /var/folders/.../kube32-cli-cujr7ze3/hello.o
hello-c/build/hello-c.wasm: 985 bytes (code 522, data 47)
kind: request pod (k32_handle)
exports (3): k32_alloc, k32_handle, memory [memory]
imports (3): k32_resp_begin, k32_resp_end, k32_resp_write
memory: 1 page = 65,536 bytes (64Ki), max 1 page
features beyond MVP: none
hardware: none
ok
```

Object files go to a temporary directory; only `build/<name>.wasm` is
written next to the sources.
Deployed to the simulator it answered
`hello from C, request 1: GET /any/path?x=1`.

A directory with a `Makefile` is built with `make -C <dir>`; `kube32-cli`
finds the module from the `-o <file>.wasm` in a dry run (`make -n -B`), so a
Makefile may write it outside its own directory, as
`apps/tessera/hud/Makefile` and `apps/sdnote/Makefile` do (the tests cover
this with a Makefile writing to `../out/`); `--artifact` names the file when
the dry run cannot tell. Larger C pods to read: `apps/echo/echo.c` (request
pod with events), `apps/sdnote/sdnote.c` (file ABI),
`apps/tessera/hud/hud.c` (`app_main` pod with display, keyboard, speaker and
channels), `apps/tessera/core` (request pod with its own arena allocator and
host-side tests).

---

## AssemblyScript

This was run with AssemblyScript 0.28.20, installed into the project
directory (`npm install --save-dev assemblyscript@0.28.20`; nothing global).
The project is `package.json`, `asconfig.json` with
`{"entries": ["assembly/index.ts"]}`, and `assembly/index.ts`:

```ts
// hello-as: a request pod in AssemblyScript. Nothing here allocates: the
// stub runtime never frees, so a pod that allocated per request would run
// out of its 64 KiB. Text goes out through one static buffer.

@external("env", "k32_resp_begin") declare function k32_resp_begin(status: i32, ctype: usize, ctypeLen: u32): void;
@external("env", "k32_resp_write") declare function k32_resp_write(data: usize, len: u32): i32;
@external("env", "k32_resp_end") declare function k32_resp_end(): void;
@external("env", "k32_log") declare function k32_log(msg: usize, len: u32): void;

const REQUEST_BUF: usize = memory.data(16 * 1024 + 512); // K32_MAX_REQUEST_BYTES plus room for an event
const OUT: usize = memory.data(256);
let hits: u32 = 0;

// ASCII literals only: at most 256 bytes per call.
function ascii(s: string): u32 {
  return <u32>String.UTF8.encodeUnsafe(changetype<usize>(s), s.length, OUT);
}

function writeStr(s: string): void {
  k32_resp_write(OUT, ascii(s));
}

function writeU32(v: u32): void {
  let n: u32 = 0;
  let tmp: u32 = v;
  do { n++; tmp /= 10; } while (tmp != 0);
  for (let i: u32 = n; i > 0; i--) {
    store<u8>(OUT + i - 1, 48 + v % 10);
    v /= 10;
  }
  k32_resp_write(OUT, n);
}

export function k32_init(): i32 {
  k32_log(OUT, ascii("hello-as: init"));
  return 0;
}

export function k32_alloc(size: u32): u32 {
  return size <= 16 * 1024 + 512 ? <u32>REQUEST_BUF : 0;
}

export function k32_handle(ptr: u32, len: u32): i32 {
  // The request line ends at the first '\n': "GET /path?query"
  let line: u32 = 0;
  while (line < len && load<u8>(ptr + line) != 10) line++;
  hits++;
  k32_resp_begin(200, OUT, ascii("text/plain"));
  writeStr("hello from AssemblyScript, request ");
  writeU32(hits);
  writeStr(": ");
  k32_resp_write(ptr, line);
  writeStr("\n");
  k32_resp_end();
  return 200;
}
```

Imports need `@external("env", ...)`: a bare `declare function k32_log` in
`assembly/index.ts` became the import `index.k32_log`, which `check`
rejects because the host links into `env` only.
`kube32-cli build` detects the project from `asconfig.json` (or an
`assemblyscript` dependency in `package.json`) and runs the local `asc`:

```
$ $KUBE32/tools/kube32-cli build hello-as -v
building $SCRATCH/apps/hello-as (assemblyscript)
$SCRATCH/apps/hello-as/node_modules/.bin/asc assembly/index.ts -o $SCRATCH/apps/hello-as/build/hello-as.wasm --runtime stub --noAssert --use abort= -O3z --initialMemory 1 --maximumMemory 1
hello-as/build/hello-as.wasm: 999 bytes (code 562, data 235)
kind: request pod (k32_handle)
exports (4): k32_alloc, k32_handle, k32_init, memory [memory]
imports (4): k32_log, k32_resp_begin, k32_resp_end, k32_resp_write
memory: 1 page = 65,536 bytes (64Ki), max 1 page
features beyond MVP: none
hardware: none
ok
```

The flags: `--runtime stub` (no garbage collector), `--noAssert`, and
`--use abort=`, which turns `abort` into a trap. Without it a module that can
`throw` imports `env.abort`, which the host does not provide; a two-line test
module with a `throw` built with and without the flag showed
`imports (5): abort, ...` and `error: unknown import env.abort`, and no
`abort` import with it. Deployed to the simulator, the pod logged
`hello-as: init` and answered `hello from AssemblyScript, request 1: GET /as?q=1`.

Only this much AssemblyScript was run: imports, exports, static memory,
`load`/`store`, UTF-8 encoding of literals into a static buffer. Strings
built at run time, arrays and anything else that allocates were not tried;
with the stub runtime they leak on every request.

---

## Checking a module

`kube32-cli check` reads a `.wasm` and validates it against this runtime:

* imports: functions from `env` only, names and signatures the host links;
* exports: `k32_handle` needs `k32_alloc`, every `k32_*` export and the
  `app_main`/`_start`/`main` entry point need the host's signature;
* memory: defined by the module (not imported), 32-bit, within `--memory`
  when given;
* opcodes: every function body is decoded against the opcode set of this
  wasm3 build.

```
$ tools/kube32-cli check images/tessera-core.wasm --memory 64Ki
images/tessera-core.wasm: 76,228 bytes (code 64,133, data 9,313)
kind: request pod (k32_handle)
exports (6): k32_alloc, k32_event, k32_handle, k32_init, k32_tick, memory [memory]
imports (10): k32_env_get, k32_event_emit, k32_log, k32_mem_stats, k32_random, k32_resp_begin, k32_resp_end, k32_resp_header, k32_resp_write, k32_time_ms
memory: 20 pages = 1,310,720 bytes (1280Ki), max 20 pages
features beyond MVP: sign-ext (i32.extend16_s x1)
hardware: ipc  (manifest resources: hardware.kube32.io/ipc)
error: memory: the module declares 1280Ki (20 pages) of linear memory but the limit is 64Ki
FAILED (1 error)
```

The wasm3 in `third_party/wasm3` is built (`CMakeLists.txt`,
`platformio.ini`) with exception handling, typed function references and
stack switching compiled out. What `check` accepts and rejects:

| Accepted (reported under "features beyond MVP") | Rejected |
| --- | --- |
| sign extension, saturating float-to-int, bulk memory, reference types, tail calls, multi-value, extended constant expressions, wide arithmetic, atomics, multiple memories | SIMD (`0xFD`, `v128`), exception handling (`throw`, `throw_ref`, `try_table`, tag sections), typed function references (`call_ref`, `return_call_ref`, `br_on_null`, ...), GC types, a 64-bit memory 0, imported memories and tables |

Floating point is part of the MVP and compiled in. The table was checked
against the real interpreter: small modules using SIMD and `throw` failed
`m3_CompileModule` with the repository's `build/libwasm3.a`, and modules using
`return_call`, `i32.extend8_s`, `memory.copy`, an atomic load, `i64.add128`, a
block with two results, `ref.null`/`ref.is_null`, a global initialised with
`i32.add` and a load from a second memory compiled. Every module named on
this page compiled the same way. Bulk memory and sign extension are in the
Rust examples; they ran on the simulator, and tessera's notes record that
bulk-memory opcodes were never part of an image run on the boards.

Exit codes of `kube32-cli`: `0` ok, `1` input or toolchain problem, `2` bad
arguments, `3` the module failed the check, `4` kubectl failed, `5` deploy
found the pod already there.

---

## Deploying without reflashing

`kube32-cli deploy` creates a Pod whose module travels in the
`kube32.io/wasm-base64` annotation. The kubelet resolves a container image in
this order: `wasm://sd/<name>` from the node's microSD card; otherwise the
firmware's embedded registry; otherwise the annotation. The manifest it
creates (`--dry-run` prints it):

```
$ tools/kube32-cli deploy sdk/rust/examples/counter/build/counter.wasm --name counter --node esp32-node-01 --env GREETING=hello --dry-run
apiVersion: v1
kind: Pod
metadata:
  name: counter
  namespace: default
  labels:
    app: counter
  annotations:
    kube32.io/wasm-base64: "AGFzbQEAAAABUA5gBH9/f38Bf2AEf39/fwBgAAF+YAN/f38AYAJ/fwBgAn9/AX9gAABgBX9/f39/AGADf39/AX9gAX8B...(8,536 characters)"
    kube32.io/wasm-sha256: "4346db01e84f3402f674e7e2e8a13ec30e60adb40406250c10563efbf4a30374"
    kube32.io/wasm-size: "6400"
spec:
  nodeSelector:
    kubernetes.io/hostname: esp32-node-01
  restartPolicy: Always
  containers:
    - name: counter
      image: "wasm://inline/counter-4346db01e84f"
      ports:
        - containerPort: 7000
          name: http
      env:
        - name: GREETING
          value: "hello"
      resources:
        requests:
          memory: 64Ki
          hardware.kube32.io/ipc: "1"
        limits:
          memory: 64Ki
      securityContext:
        capabilities:
          add: ["CHANNEL_IPC"]
```

Without `--dry-run` it runs `kubectl create --validate=false -f -` against
the current kubectl context (`--context`, `--kubeconfig`, `--server`
override it):

```
$ tools/kube32-cli deploy sdk/rust/examples/counter/build/counter.wasm --node esp32-node-01 --env GREETING=hello
pod/counter created
deployed default/counter: 6,400 bytes inline, 64Ki linear memory, hardware: ipc
$ kubectl get events --field-selector involvedObject.name=counter
LAST SEEN   TYPE     REASON      OBJECT        MESSAGE
4s          Normal   Scheduled   pod/counter   Successfully assigned default/counter to esp32-node-01
4s          Normal   Pulled      pod/counter   Container image "wasm://inline/counter-4346db01e84f" already present on machine (6400 bytes, inline annotation)
4s          Normal   Created     pod/counter   Created container counter
4s          Normal   Started     pod/counter   Started container counter
```

Details that matter:

* **The image name carries the digest.** The registry key is the last path
  component of the image before any `:`. With `wasm://inline/<name>:<tag>`, a
  firmware that embeds an image called `<name>` would run that one instead of
  the annotation: on the simulator with `counter` embedded, a 985-byte C
  module deployed as `counter` ran the 6,400-byte embedded counter. With the
  digest in the name it ran the C module.
* **`create`, not `apply`.** `kubectl apply` copies the whole manifest,
  module included, into the `kubectl.kubernetes.io/last-applied-configuration`
  annotation: the stored pod for the counter was 19,755 bytes with `apply` and
  10,393 bytes with `create`.
* **`--validate=false`.** `kube32-cli` still passes it. It dates from when
  the server published no OpenAPI index. Since commit `f737ab6` the server
  publishes one, and plain `kubectl apply` and `kubectl create` work: the
  server rejects unknown top-level and `metadata` fields and does not check
  nested ones ([Kubernetes API coverage](api-reference.md#kubectl-and-k9s)).
  The flag is harmless and hand-written manifests do not need it.
* **A running pod is not updated in place.** The kubelet deploys a pod the
  first time it sees it and ignores later changes to its manifest
  (`Kubelet::sync_assigned_pods` only compares pod names), so `deploy` stops
  with exit code 5 when the pod exists:

    ```
    error: pod default/counter already exists (module sha256 4346db01e84f, this file 4346db01e84f); the kubelet does not restart a running pod when its manifest changes: pass --replace to delete and recreate it
    ```

    `--replace` runs `kubectl delete pod <name> --wait=true` first, then
    creates the pod again (about one second on the simulator).
* **Hardware is inferred from the imports** unless `--hardware` names it
  (`ipc,display,keyboard,speaker,sd`, or `none`). A module that imports
  something `--hardware` does not grant gets a warning; its calls fail with
  `K32_ERR_PERM`.
* **Memory.** `--memory` sets `requests.memory` and `limits.memory` (default
  64Ki); `--psram` adds `esp32.io/psram` and the sandbox becomes the larger
  of the two. The module's declared memory is checked against it before
  anything is sent.

The annotation path was run on the simulator only. On the boards the module
also travels through the API server's store and every kubelet's pod list;
how large a module that path handles on a board was not measured.

---

## From a microSD card

`kube32-cli sd-push` checks the module and copies it to
`kube32/images/<name>.wasm` (or `<name>-<tag>.wasm` with `--tag`) under the
card's mount point. On the simulator the card is the directory named by
`KUBE32_SD_ROOT`:

```
$ tools/kube32-cli sd-push sdk/rust/examples/notes/build/notes.wasm --to $SIM/sdcard
copied $SIM/sdcard/kube32/images/notes.wasm (4,500 bytes)
image: wasm://sd/notes
$ tools/kube32-cli deploy sdk/rust/examples/notes/build/notes.wasm --image wasm://sd/notes --node esp32-node-02
pod/notes created
deployed default/notes: image wasm://sd/notes, 64Ki linear memory, hardware: sd
$ kubectl get events --field-selector involvedObject.name=notes
...
4s          Normal   Pulling     pod/notes   Pulling image "wasm://sd/notes" from the sd card
4s          Normal   Pulled      pod/notes   Successfully pulled image "wasm://sd/notes" in 0ms (4500 bytes, sd card, $SIM/sdcard/kube32/images/notes.wasm)
```

With `--image` the annotation is left out; the file is still checked and its
imports still decide the hardware. The `notes` pod imports `k32_fs_*`, so the
manifest requests `hardware.kube32.io/sd` and mounts the volume claim
`notes` (`--sd-claim` picks another):

```
$ curl -s -X POST -H 'X-Kube32-Pod: default/notes' --data 'first note' http://127.0.0.1:17000/notes
{"size":11}
$ curl -s -X POST -H 'X-Kube32-Pod: default/notes' --data 'second note' http://127.0.0.1:17000/notes
{"size":23}
$ curl -s -H 'X-Kube32-Pod: default/notes' http://127.0.0.1:17000/notes
first note
second note
$ find $SIM/sdcard -type f
$SIM/sdcard/kube32/images/notes.wasm
$SIM/sdcard/kube32/volumes/notes/notes.txt
```

A 6,000-byte body, more than the 4096 bytes one `k32_fs_write` moves, was
written whole by `File::write_all`. The kubelet pulls at most 256 KiB from
the card on a board without PSRAM and 2 MiB with PSRAM; `sd-push` warns past
those sizes. Pushing to a physical card was not done for this page. Card
layout, volume rules and the file ABI are in [microSD card](sd-card.md).

---

## Embedding into the firmware

`kube32-cli embed` checks the module and copies it to `images/<name>.wasm`;
pods then use `wasm://embedded/<name>:v1`:

```
$ tools/kube32-cli embed sdk/rust/examples/counter/build/counter.wasm
embedded <repo>/images/counter.wasm (6,400 bytes)
image: wasm://embedded/counter:v1
next: firmware: pio run -e esp32s3 (the pre-build step regenerates the image table)
      host simulator: cmake -S . -B build && cmake --build build (configure picks up new files in images/)
```

For the host simulator the image list is a CMake glob evaluated at configure
time: after adding a file, `cmake --build build` alone did not embed it, while
`cmake -S . -B build && cmake --build build` did, and the simulator then
reported `Image registry: 4 embedded image(s)` and pulled
`wasm://embedded/counter:v1` from the `embedded registry`. After removing a
file, the generated table is not rebuilt either; deleting
`build/generated/embedded_blobs.cpp` before `cmake --build build` regenerates
it. The firmware build runs `tools/embed_assets.py --esp` before every
`pio run`; that build was not run for this page.

---

## Manifest fields

| Field | Meaning on Kube32 |
| --- | --- |
| `spec.containers[0].image` | `wasm://sd/<name>[:<tag>]` is read from the card; any other reference is looked up in the firmware's registry by its key (the last path component before `:`), and when that finds nothing the annotation is used |
| `metadata.annotations["kube32.io/wasm-base64"]` | the module, base64; used when the image resolves to nothing else |
| `spec.nodeSelector["kubernetes.io/hostname"]` | pins the pod to a node |
| `spec.containers[0].env` | read by the guest with `k32_env_get` |
| `resources.requests.memory`, `limits.memory` | scheduler accounting against the node's free heap; the Wasm sandbox is `limits.memory` (else `requests.memory`) |
| `resources.requests["esp32.io/psram"]` | sandbox in PSRAM on boards that have it; the sandbox is the larger of this and memory |
| `resources.requests["hardware.kube32.io/*"]` | capability grants, see below; the firewall reads `requests`, or `limits` when there are no requests |
| `securityContext.capabilities.add` | the same grants by name: `CHANNEL_IPC`, `DISPLAY`, `KEYBOARD`, `SPEAKER`, `SD` |
| `spec.volumes[].persistentVolumeClaim.claimName` (or `kube32.io/sd: {claim: ...}`) | the pod's directory on the card, required with `hardware.kube32.io/sd` |
| `ports[].containerPort` | informational: request pods answer on the node's pod HTTP server |

---

## Hardware resources and the capability firewall

| Resource (or capability) | Grants |
| --- | --- |
| `hardware.kube32.io/ipc: "1"` (`CHANNEL_IPC`) | `k32_event_emit`, `kube32_channel_*` |
| `hardware.kube32.io/tft-display: "1"` (`DISPLAY`) | `k32_display_*` |
| `hardware.kube32.io/keyboard: "1"` (`KEYBOARD`) | `k32_key_read` |
| `hardware.kube32.io/speaker: "1"` (`SPEAKER`) | `k32_tone` |
| `hardware.kube32.io/sd: "1"` (`SD`) plus a volume claim | `k32_fs_*` |

A call without the grant returns `K32_ERR_PERM` (-1) and the node logs it.
The counter deployed with `--hardware none`:

```
$ tools/kube32-cli deploy sdk/rust/examples/counter/build/counter.wasm --name counter-locked --node esp32-node-02 --hardware none
warning: the module imports ipc functions but --hardware does not grant 'ipc': those calls will return K32_ERR_PERM
pod/counter-locked created
deployed default/counter-locked: 6,400 bytes inline, 64Ki linear memory, hardware: none
$ curl -s -X POST -H 'X-Kube32-Pod: default/counter-locked' http://127.0.0.1:17000/inc
{"counter":1,"event":"denied"}
```

and the simulator printed `[FIREWALL] denied default/counter-locked: ipc 0 write`.
The `notes` pod without `sd` answered `403 {"error":-1}` and the `app_main`
example without `display` logged `blink: no display grant`. The GPIO, ADC,
I2C and UART imports have their own resources and pin claims, described in
[Capability firewall](architecture/capability-firewall.md).

---

## Running on the host simulator

Build the host and start a standalone simulator: an API server and two
simulated nodes (`esp32-node-01`, `esp32-node-02`) in one process. With
`--ingress-port`, the simulator also starts the pod HTTP server
(`--pod-port`) and an ingress for the app named by `--app`. The ports below
keep it apart from the defaults (API 6443, pods 7000, IPC 7400), and the card
is a scratch directory:

```
$ cmake --build build -j8
$ export SIM=/some/scratch/dir   # holds sdcard/, data/ and the kubeconfig
$ KUBE32_SD_ROOT=$SIM/sdcard ./build/kube32-sim --role standalone --port 16443 --ingress-port 18080 --pod-port 17000 --ipc-port 17400 --app counter --core-pod counter --data-dir $SIM/data
...
[SD] simulated card (directory) .../sdcard: 471482 MB total, 8103 MB free
[READY] Kube32 API Server listening on http://0.0.0.0:16443
[INIT] ESP32 Kubelet node agent running for esp32-node-01.
[INIT] ESP32 Kubelet node agent running for esp32-node-02.
[APP] pod HTTP server on :17000 (2 workers)
[APP] ingress on :18080 serving app 'counter'
```

A kubeconfig of its own keeps kubectl away from the real cluster's context:

```yaml
# $SIM/kubeconfig
apiVersion: v1
kind: Config
clusters:
  - name: kube32-sim
    cluster:
      server: http://127.0.0.1:16443
contexts:
  - name: kube32-sim
    context:
      cluster: kube32-sim
      user: kube32-sim
      namespace: default
current-context: kube32-sim
users:
  - name: kube32-sim
    user: {}
```

```
$ export KUBECONFIG=$SIM/kubeconfig
$ kubectl config current-context
kube32-sim
$ kubectl get nodes
NAME            STATUS   ROLES    AGE   VERSION
esp32-node-01   Ready    worker   5s    v1.33.0-kube32
esp32-node-02   Ready    worker   5s    v1.33.0-kube32
```

Request pods are reached two ways:

* the **pod HTTP server** (`--pod-port`) passes the request through as is;
  `X-Kube32-Pod: <namespace>/<name>` picks the pod, otherwise the request
  pod whose `namespace/name` sorts first answers (the standalone simulator
  runs both nodes' pods in one process);
* the **ingress** (`--ingress-port`) forwards `/api/*`, path included, to the
  pod named by `--core-pod`, except `/api/cluster`, which it answers itself,
  and serves `/events` as a server-sent event stream of everything published
  on `<app>.events`.

The counter accepts its routes with or without the `/api` prefix for this
reason:

```
$ curl -s -H 'X-Kube32-Pod: default/counter' http://127.0.0.1:17000/
{"counter":0,"events_seen":0,"ticks":12,"uptime_ms":17752}
$ curl -s -X POST -H 'X-Kube32-Pod: default/counter' 'http://127.0.0.1:17000/inc?by=5'
{"counter":5,"event":"sent"}
$ curl -s -X POST http://127.0.0.1:18080/api/inc
{"counter":6,"event":"sent"}
$ curl -s http://127.0.0.1:18080/api/env
{"greeting":"hello"}
$ curl -s http://127.0.0.1:18080/api/state
{"counter":6,"events_seen":2,"ticks":16,"uptime_ms":21796}
```

and a `curl -sN http://127.0.0.1:18080/events` running alongside received:

```
event: counter
data: {"type":"counter","value":5}

event: counter
data: {"type":"counter","value":6}
```

`events_seen: 2` is the counter's own events delivered back to its
`k32_event`: the node hands `<app>.events` and `<app>.input` (here
`counter.events` and `counter.input`, from `--app counter`) to every pod that
exports `k32_event`. `ticks` grows once a second. `kubectl logs counter` shows the
pod's `k32_log` lines.

---

## Troubleshooting

| Symptom | Cause |
| --- | --- |
| `kube32-cli check`: `unknown import env.<name>` | a misspelt import (C links with `--allow-undefined`, so typos become imports), or a runtime helper such as AssemblyScript's `abort` |
| `strict decoding error: unknown field "..."` from kubectl | a misspelt top-level or `metadata` field in the manifest |
| pod `Pending`, `FailedScheduling ... insufficient heap memory or peripheral resources` | the node's free heap is taken by other pods' `requests.memory` |
| `503 {"error":"unavailable","message":"no request-handling pod on this node"}` | the pod named in `X-Kube32-Pod` is not on this node or exports no `k32_handle` |
| `500 {"error":"pod_failure","message":"workload produced no response"}` | `k32_handle` returned (or trapped) before `k32_resp_begin` |
| `deploy` exits 5 | the pod exists; `--replace` |
| calls return -1 (`K32_ERR_PERM`) | the manifest does not grant the resource; the node log has a `[FIREWALL] denied` line |
