# Writing pods

A pod is a WebAssembly module that the node runs in its wasm3 interpreter. It communicates with the node through host imports in the `env` module and entry point exports declared in `include/runtime/k32_app_abi.h`.

---

## Two kinds of pod

| Kind | Exports | Execution model |
| --- | --- | --- |
| Request pod | `k32_alloc`, `k32_handle`; optionally `k32_init`, `k32_tick`, `k32_event` | HTTP workers call `k32_handle` once per request. A timer calls `k32_tick`. Channel events arrive in `k32_event`. |
| Entry-point pod | `_start`, `app_main`, or `main` | Runs on the pod's own thread (requires a board with PSRAM). Loops on its own and exits when the function returns. |

A module can export both. The runtime looks up entry points in the order `_start`, `app_main`, `main`. `k32_init` runs once when the container starts, and a non-zero return fails the pod with `StartError` (`k32_init returned <n>`). The simulator and boards call `k32_tick` once per second, or every 10 ms while `k32_tick` returns a positive value. On boards without PSRAM, all pod calls share one interpreter stack and entry-point pods cannot start; use `k32_handle`, `k32_tick`, and `k32_event` pods on those boards.

### Request block

Before each `k32_handle` call, the host allocates a guest buffer with `k32_alloc(size)` (`size` is the block length plus one), copies the request into it, and calls `k32_handle(ptr, len)`. The block format is:

```
METHOD SP PATH[?QUERY] LF
(Header-Name ":" SP value LF)*
LF
body bytes
```

The host forwards `Cookie`, `Content-Type`, `X-Request-Id`, and `X-Forwarded-For`. The guest writes the response with `k32_resp_begin`, optional `k32_resp_header` calls, zero or more `k32_resp_write` calls, and `k32_resp_end`. Headers set with `k32_resp_header` before `k32_resp_begin` or `k32_resp_asset` are kept and sent with the response; headers that repeat (such as `Set-Cookie`) accumulate, and passing an empty value before the response starts withdraws pending headers of that name (case-insensitive). The host streams the body with HTTP chunked transfer encoding. If `k32_handle` returns without starting a response, the client receives `500 {"error":"pod_failure","message":"workload produced no response"}`.

`k32_event(topic_ptr, topic_len, ptr, len)` receives its topic and payload through `k32_alloc` in the same way. It receives messages published on topics listed in the pod's `kube32.io/event-topics` annotation.

---

## Host ABI

All imports belong to the `env` module (`src/runtime/app_pod.cpp`, `src/runtime/host_functions.cpp`). `kube32-cli check` rejects unknown imports or signatures.

| Group | Imports | Signatures | Needs | Details |
| --- | --- | --- | --- | --- |
| HTTP response | `k32_resp_begin`, `k32_resp_header`, `k32_resp_write`, `k32_resp_end`, `k32_resp_asset` | `v(iii)`, `v(iiii)`, `i(ii)`, `v()`, `i(iii)` | nothing | [Networking](networking.md) |
| Service HTTP | `k32_http_call` | `i(iiiiiiiiiiii)` | `securityContext.capabilities.add: ["SERVICE_CALL"]` | [Networking](networking.md) |
| Events and channels | `k32_event_emit`, `kube32_channel_open/send/recv/close` | `i(iiii)`, `i(ii)`, `i(iii)`, `i(iiii)`, `i(i)` | `hardware.kube32.io/ipc` (`CHANNEL_IPC`) | [Networking](networking.md) |
| Node services | `k32_log`, `k32_time_ms`, `k32_random`, `k32_env_get`, `k32_mem_stats`, `esp32_delay_ms`, `esp32_get_time_us` | `v(ii)`, `I()`, `i()`, `i(iiii)`, `i(ii)`, `v(i)`, `I()` | nothing | `include/runtime/k32_app_abi.h` |
| Storage | `k32_fs_open/read/write/close/size/list/remove` | `i(iii)`, `i(iii)`, `i(iii)`, `i(i)`, `i(i)`, `i(iiii)`, `i(ii)` | `hardware.kube32.io/sd` (`SD`) and a volume claim | [Storage](storage.md), [storage](storage.md) |
| Cardputer UI | `k32_display_text`, `k32_display_rect`, `k32_display_bar`, `k32_key_read`, `k32_tone` | `i(iiiiii)`, `i(iiiii)`, `i(iiiiiii)`, `i()`, `i(iii)` | `hardware.kube32.io/tft-display` (`DISPLAY`), `keyboard` (`KEYBOARD`), `speaker` (`SPEAKER`) | [Peripherals](peripherals.md), [Capability firewall](architecture/capability-firewall.md) |
| ESP32 peripherals | `esp32_gpio_set_mode/write/read`, `esp32_adc_read`, `esp32_i2c_write/read`, `esp32_uart_write/read` | `i(ii)`, `i(ii)`, `i(i)`, `i(i)`, `i(iiii)`, `i(iiii)`, `i(iii)`, `i(iii)` | `esp32.io/*` resources | [Peripherals](peripherals.md), [GPIO and other peripherals](peripherals.md) |

Calls without the required resource or capability return `K32_ERR_PERM` (`-1`) and log a firewall violation.

`k32_resp_asset(path, len, status)` streams a file from the pod's image bundle (`images/web/<name>/` for embedded images, `kube32/images/<name>/` on the card for `wasm://sd/<name>` images) without copying it into linear memory (`status` `0` means `200`; `HEAD` requests are supported). It serves the `.gz` variant when the client accepts gzip, or replies `406` if only the `.gz` file exists and the client refuses gzip. When the file does not exist it returns `K32_ERR_NOENT` (`-2`) without sending anything and leaves earlier headers pending.

`k32_http_call(method, method_len, url, url_len, headers, headers_len, body, body_len, resp, resp_cap, *resp_len, timeout_ms)` sends a synchronous HTTP request to `http://<service>[.<ns>][:<port>]/path` (`timeout_ms` `0` defaults to `5000` ms, maximum `60` s). A target on the same node is called in-process; a target on another node is called over HTTP/1.1. The target receives `Content-Type`, `Cookie`, `X-Request-Id`, and `Accept-Encoding` from the caller's headers. It copies up to `resp_cap` response bytes into `resp`, stores the byte count in `*resp_len`, and returns the HTTP status or a negative error: `K32_ERR_PERM` (`-1`), `K32_ERR_NOENT` (`-2`), `K32_ERR_IO` (`-5`), `K32_ERR_TOOBIG` (`-7`, after copying the first `resp_cap` bytes), `K32_ERR_LOOP` (`-40`, self-call or cycle across pods detected via `X-Kube32-Call-Chain`), or `K32_ERR_TIMEOUT` (`-110`).

---

## Runtime limits and lifecycle

### Limits

| Limit | Value | Behaviour when exceeded |
| --- | --- | --- |
| Request block | 16 KiB (`K32_MAX_REQUEST_BYTES`) | HTTP `413` |
| Event payload | 200 bytes (`K32_MAX_EVENT_BYTES`), topic 1..64 bytes | `k32_event_emit` returns `K32_ERR_INVAL` (`-22`) |
| Response header | name 1..64 bytes, value up to 256 bytes (`K32_MAX_HEADER_BYTES`), up to 8 pending headers | Header is dropped |
| Content type in `k32_resp_begin` | under 128 bytes | Defaults to `application/json` |
| Log line | 512 bytes | Line is dropped |
| `k32_display_text` | 64 bytes | Returns `K32_ERR_FAULT` (`-14`) |
| `k32_tone` | 2000 ms, 100% volume | Clamped |
| Files | 96-byte paths (`K32_FS_MAX_PATH`), 4 open per pod (`K32_FS_MAX_FDS`) and per node, 4096 bytes per call (`K32_FS_MAX_IO`) | Returns negative `K32_ERR_*`; see [Storage](storage.md) |
| Execution budget per call | 4,000,000,000 wasm3 gas units (`kDefaultGasBudgetPerCall`) | Call traps with exit code `152` |

### Memory limits

A module declares its linear memory at link time (`--initial-memory` and `--max-memory`, in multiples of 64 KiB). The runtime sets the pod's linear memory limit to `limits.memory` (or `requests.memory`), raised to `esp32.io/psram` if larger.

A memory limit below `64Ki` (such as `2Ki`, `4Ki`, or `16Ki`) caps the backing allocation of the module's first linear-memory page to that byte limit. The module declares 1 page (64 KiB), only the limit is allocated, accesses past the limit trap, and `memory.grow` past the cap fails.

On boards with PSRAM, one 1284 KiB block is reserved at boot for one large pod, and one shared 64 KiB interpreter stack in PSRAM. On boards without PSRAM, an 80 KiB pool block holding one shared 28 KiB interpreter stack is reserved at boot, and `app_main` (entry-point) pods require a board with PSRAM. On both, all pod calls run serialized on the shared stack. Every pod allocation (wasm3 runtime, compiled code, linear memory) comes from the pool block first and falls back to the general heap when the block is full (while a pod whose linear memory needs `>= 48 KiB` loads, its runtime goes to the general heap), with 24 KiB of heap reserved for the system.

Pods with up to 64 KiB of linear memory default to a 4 KiB wasm3 value stack, and larger pods get 16 KiB. Set `metadata.annotations["kube32.io/wasm-stack"]` (`512` to `64Ki`) to override the per-pod wasm3 value stack size; exhausting it traps with exit code `139` and the message `the module's stack of <N> bytes is exhausted (raise it with the annotation kube32.io/wasm-stack)`. The shadow stack for C and Rust locals lives inside linear memory (`-z stack-size`, 16 KiB by default in `kube32-cli build`).

### Execution budget

Every host-initiated call (`k32_init`, `k32_alloc`, `k32_handle`, `k32_tick`, `k32_event`) gets a fresh wasm3 gas budget, defaulting to `4,000,000,000` units. Override it with the pod annotation `kube32.io/execution-budget: "<units>"` or disable metering with `"off"` (or `"0"`). Unmetered pods compile without gas checks and cannot be interrupted mid-call when stopped.

### Restart policy and exit codes

Pods support `restartPolicy: Always`, `OnFailure`, and `Never`. Failed or restarting containers back off starting at 10 s, doubling on each crash up to 5 minutes (`CrashLoopBackOff`), and reset the back-off after 10 minutes of running. Container status records `lastState`, `restartCount`, and `BackOff` events. Blocking host calls (sleep, channel receive, tone, HTTP call) abort early when the kubelet stops a container, and deleting a pod reclaims its memory.

| Exit code | Reason | Cause |
| --- | --- | --- |
| Return value of `app_main` / `main` (`0` for `_start`) | `Completed` (when `0`) or `Error` | Entry point returned |
| `128` | `StartError` | Container failed to deploy or start (bad Wasm, `k32_init` non-zero, no thread stack) |
| `132` | `Error` | Unsupported instruction (`SIGILL`) |
| `134` | `Error` | `unreachable` or abort (`SIGABRT`) |
| `135` | `Error` | Unaligned atomic operation (`SIGBUS`) |
| `136` | `Error` | Integer division by zero, integer overflow, or invalid conversion (`SIGFPE`) |
| `137` | `OOMKilled` | Runtime allocation failed, memory limit exceeded, or abort after `memory.grow` was refused at the pod limit |
| `137` | `Error` | Stopped by the kubelet (`SIGKILL`) |
| `139` | `Error` | Out-of-bounds memory or table access, stack overflow (`the module's stack of <N> bytes is exhausted (raise it with the annotation kube32.io/wasm-stack)`), null reference, or indirect call type mismatch (`SIGSEGV`) |
| `152` | `Error` | Call exhausted its `kube32.io/execution-budget` (`SIGXCPU`) |

---

## Rust

`sdk/rust` is a Cargo workspace containing the `#![no_std]` crate `kube32-sdk` and two example pods (`sdk/rust/examples/counter` and `sdk/rust/examples/notes`).

| Feature of `kube32-sdk` | Provides |
| --- | --- |
| `panic-handler` (default) | `#[panic_handler]` that logs through `k32_log` and traps |
| `hardware` | `hardware::{display_text, display_rect, display_bar, key_read, tone, rgb565}` |
| `fs` | `fs::{File, list, remove}` over `k32_fs_*` |

### Request pod

`kube32_app!` exports the request entry points (`alloc`, `init`, `handle`, `tick`, `event`, in that order) and reserves the static `k32_alloc` buffer (default `16 KiB + 512`). From `sdk/rust/examples/counter/src/lib.rs`:

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

### Entry-point pod

`kube32_main!` exports `app_main`:

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

### Building Rust pods

Install the target with `rustup target add wasm32-unknown-unknown` and build with `kube32-cli build`:

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

Pass `--memory 128Ki --stack 32Ki` to change the linear memory and shadow stack sizes. A crate outside the workspace needs `crate-type = ["cdylib"]` and a dependency on `kube32-sdk`.

---

## C

A C pod includes `runtime/k32_app_abi.h` and marks exports with `K32_EXPORT`:

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

`kube32-cli build` compiles `.c` files with `clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory` and links them with `rust-lld` or `wasm-ld`. If the directory contains a `Makefile`, `kube32-cli build` runs `make -C <dir>` (pass `--artifact <path>` when the output `.wasm` cannot be inferred from `make -n -B`). See `apps/echo`, `apps/sdnote`, `apps/i2cscan`, and `apps/tessera` for complete C pods.

---

## AssemblyScript

Install `assemblyscript` in the project directory (`npm install --save-dev assemblyscript`), add `asconfig.json` with `{"entries": ["assembly/index.ts"]}`, and declare host imports with `@external("env", ...)`:

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

`kube32-cli build` invokes `asc` with `--runtime stub --noAssert --use abort= -O3z` so `abort` compiles to a trap instead of importing `env.abort`.

---

## Checking a module

`kube32-cli check` validates a `.wasm` binary against the runtime:

* Imports must be functions from `env` with signatures the host links.
* Exports (`k32_init`, `k32_alloc`, `k32_handle`, `k32_tick`, `k32_event`, `_start`, `app_main`, `main`) must match the host's signatures, and `k32_handle` or `k32_event` requires `k32_alloc`.
* Linear memory 0 must be defined by the module (not imported), 32-bit, and within `--memory` when given.
* Instructions must belong to the opcode set enabled in this wasm3 build.

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

| Accepted features (beyond MVP + float) | Rejected features |
| --- | --- |
| sign extension, saturating float-to-int, bulk memory, reference types, tail calls, multi-value, extended constant expressions, wide arithmetic, atomics, multiple memories | SIMD (`0xFD`, `v128`), exception handling (`throw`, `throw_ref`, `try_table`, tag sections), typed function references (`call_ref`, `return_call_ref`, `br_on_null`), GC types, 64-bit memory 0, imported memories and tables |

Exit codes of `kube32-cli`: `0` ok, `1` input or toolchain error, `2` usage error, `3` ABI check failed, `4` `kubectl` failed, `5` pod already exists on `deploy`.

---

## Deploying pods

The kubelet resolves `spec.containers[0].image` in this order:

1. `wasm://sd/<name>[:<tag>]` reads `<card>/kube32/images/<name>[-<tag>].wasm` (up to 256 KiB without PSRAM, 2 MiB with PSRAM).
2. Any other image reference is looked up in the firmware's embedded registry by its key (the last path component before `:`).
3. Otherwise, the kubelet decodes `metadata.annotations["kube32.io/wasm-base64"]`.

### Inline image (`kube32-cli deploy`)

`kube32-cli deploy` checks the module, embeds it in `kube32.io/wasm-base64`, and creates the Pod with `kubectl create`:

```
$ tools/kube32-cli deploy sdk/rust/examples/counter/build/counter.wasm --node esp32-node-01 --env GREETING=hello
pod/counter created
deployed default/counter: 6,400 bytes inline, 64Ki linear memory, hardware: ipc
```

Key flags for `kube32-cli deploy`:

* `--replace`: deletes an existing pod of the same name (`kubectl delete pod <name> --wait=true`) before creating the new one. The kubelet does not restart a running pod when its manifest changes in place.
* `--memory <qty>` and `--psram <qty>`: set `requests.memory`, `limits.memory`, and `esp32.io/psram`.
* `--hardware <list>`: comma-separated list of `ipc,display,keyboard,speaker,sd,service-call` or `none` (default: inferred from the module's imports).
* `--sd-claim <claim>`: volume claim name when `sd` is used (default: pod name).
* `--image <wasm://...>`: references an SD or embedded image instead of attaching `kube32.io/wasm-base64`.
* `--dry-run`: prints the generated Pod YAML without calling `kubectl`.

### SD card image (`kube32-cli sd-push` / `push`)

Copy a module onto a mounted card with `sd-push`, or upload it over the network through the node proxy (`/api/v1/nodes/<node>/proxy/storage/<path>`) with `kube32-cli push`:

```
$ tools/kube32-cli sd-push sdk/rust/examples/notes/build/notes.wasm --to $SIM/sdcard
copied $SIM/sdcard/kube32/images/notes.wasm (4,500 bytes)
image: wasm://sd/notes
$ tools/kube32-cli deploy sdk/rust/examples/notes/build/notes.wasm --image wasm://sd/notes --node esp32-node-02
```

See [Storage and volumes](storage.md) for volume and image layout.

### Embedded image (`kube32-cli embed`)

Copy a checked module into `images/<name>.wasm` for the next firmware or simulator build:

```
$ tools/kube32-cli embed sdk/rust/examples/counter/build/counter.wasm
embedded <repo>/images/counter.wasm (6,400 bytes)
image: wasm://embedded/counter:v1
```

Rebuild the firmware with `pio run -e esp32s3`, or reconfigure and build the host simulator with `cmake -S . -B build && cmake --build build`.

---

## Manifest fields and annotations

### Pod spec fields

| Field | Meaning |
| --- | --- |
| `spec.containers[0].image` | `wasm://sd/<name>[:<tag>]`, `wasm://embedded/<name>`, or `wasm://inline/<name>-<digest>` |
| `spec.nodeName`, `spec.nodeSelector` | Pins or selects nodes (such as `node-role.kubernetes.io/control-plane` or `kube32.io/board: cardputer`) |
| `spec.restartPolicy` | `Always` (default), `OnFailure`, or `Never` |
| `spec.containers[0].env`, `envFrom` | Literal values, `valueFrom` (`configMapKeyRef`, `secretKeyRef`, `fieldRef`), `envFrom` with `prefix`, `$(VAR)` expansion, and `<SVC>_SERVICE_HOST` / `_PORT` links (unless `enableServiceLinks: false`). Missing refs hold the pod in `CreateContainerConfigError`. |
| `resources.requests.memory`, `limits.memory` | Scheduler heap accounting and Wasm linear memory limit (supports sub-page limits below `64Ki`) |
| `resources.requests["esp32.io/psram"]` | Places the Wasm sandbox in PSRAM and raises the sandbox size to `max(memory, psram)` |
| `resources.requests["hardware.kube32.io/*"]` | Grants `ipc`, `tft-display`, `keyboard`, `speaker`, `sd` |
| `resources.requests["esp32.io/*"]` | Claims `gpio-<N>`, `gpio`, `adc`, `i2c`, `i2c-bus-<N>`, `uart`, `uart-<N>` |
| `securityContext.capabilities.add` | Grants `CHANNEL_IPC`, `DISPLAY`, `KEYBOARD`, `SPEAKER`, `SD`, `SERVICE_CALL` |
| `spec.volumes[].persistentVolumeClaim.claimName` | Mounts `<root>/kube32/volumes/<claim>` on card or internal flash (`kube32.io/sd: {claim: ...}` requires a card) |
| `ports[].containerPort` | Target port named by Services (`7000` is the node's pod HTTP server port) |

### Annotations

| Annotation | Target | Meaning |
| --- | --- | --- |
| `kube32.io/wasm-base64` | Pod | Base64-encoded `.wasm` module used when the image is not on SD or embedded |
| `kube32.io/wasm-sha256`, `kube32.io/wasm-size` | Pod | Module digest and byte size recorded by `kube32-cli deploy` |
| `kube32.io/execution-budget` | Pod | Per-call wasm3 gas limit (`<units>` or `"off"`, default `4000000000`) |
| `kube32.io/wasm-stack` | Pod | Per-pod wasm3 value stack size (`512` to `64Ki`, default `4Ki` for pods up to `64Ki` of linear memory) |
| `kube32.io/event-topics` | Pod | Comma-separated channel topics delivered to the pod's `k32_event` export |
| `kube32.io/i2c-bus-<N>` | Pod | Bus pins and speed, e.g. `"sda=8,scl=9,hz=100000"` |
| `kube32.io/uart-<N>` | Pod | UART pins and baud rate, e.g. `"tx=17,rx=18,baud=115200"` |
| `kube32.io/event-stream` | Ingress | Serves SSE streams of channel topics: `"<path>=<topic>[,<topic>...][;<path>=...]"` |
| `kube32.io/cluster-telemetry` | Ingress | Serves cluster telemetry JSON at `<path>` |
| `kube32.io/stats-topic` | Ingress | Publishes `{"t":"stats","ingress":"<ns>/<name>","rps":N,"viewers":N}` every second on `<topic>` |

---

## Running on the host simulator

Build and start `kube32-sim` in standalone mode (API server, Ingress controller, and simulated nodes `esp32-node-01` and `esp32-node-02` in one process):

```
$ cmake --build build -j8
$ export SIM=/some/scratch/dir
$ KUBE32_SD_ROOT=$SIM/sdcard ./build/kube32-sim --role standalone --port 16443 --ingress-port 18080 --pod-port 17000 --ipc-port 17400 --data-dir $SIM/data
```

Point `kubectl` at `http://127.0.0.1:16443` and apply a Pod, Service, and Ingress (such as `examples/echo.yaml`), or deploy a module with `kube32-cli deploy`. Reach request pods in two ways:

* Directly through the pod HTTP server (`--pod-port 17000`) with the `X-Kube32-Pod: <namespace>/<name>` header:

```
$ curl -s -H 'X-Kube32-Pod: default/counter' 'http://127.0.0.1:17000/inc?by=5' -X POST
{"counter":5,"event":"sent"}
```

* Through the Ingress controller (`--ingress-port 18080`), which routes requests according to `networking.k8s.io/v1` Ingress rules and serves SSE streams configured with `kube32.io/event-stream` (see [Networking](networking.md)):

```
$ kubectl --server=http://127.0.0.1:16443 apply -f examples/echo.yaml
$ curl -s -X POST http://127.0.0.1:18080/echo/hello
$ curl -sN http://127.0.0.1:18080/echo/events
```
