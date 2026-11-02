# MCP server for agents

`kube32 mcp` is a [Model Context Protocol](https://modelcontextprotocol.io) server. An AI agent such as Claude Code uses it to find the [Devices](devices.md) of a cluster, read them, send them actions within the limits that their owner declares, and turn what it learns into a WebAssembly pod that runs on the board without the agent.

The design follows the idea of Anthropic's [Model Hardware Standard](https://www.anthropic.com/news/model-hardware-standard-research-preview) research preview:

- Every Device has the same primitives: its driver reads the part (`GET /`) and takes actions (`POST /`).
- Discovery finds the parts on each node's I2C buses, and the cluster lists them as Devices.
- The owner describes each Device and its safe limits in plain words, in two annotations. The server compiles them, with the driver catalog and live readings, into a reference document for the agent.
- The agent works through MCP tools, the `kube32` command, or code that it deploys to the cluster.
- The agent explores a part, then packages what it learned into a deterministic pod.

## Add it to Claude Code

```sh
claude mcp add kube32 -- uvx kube32 mcp
```

The server runs `kubectl`, so the current kubeconfig context and its credentials apply. Name another context, and opt in to writes and deploys, after `mcp`:

```sh
claude mcp add kube32 -- uvx kube32 mcp --context kube32 --allow-writes --allow-deploy
```

`--scope project` writes the server into the project's `.mcp.json`, to share it with the people who work in the project. Another MCP client starts the same command:

```json
{
  "mcpServers": {
    "kube32": { "command": "uvx", "args": ["kube32", "mcp", "--context", "kube32"] }
  }
}
```

The machine that runs the server needs:

- `kubectl` on `PATH`.
- A route to the nodes' addresses. The server reads each driver on its node's pod server, as [`kube32 devices`](cli-reference.md#app-commands) does. With the API server behind [`kube32 gateway`](cli-reference.md#kube32-gateway), the machine must also be on the boards' network.
- For `deploy_code` from C: `clang` with the wasm32 target and `wasm-ld` or `rust-lld`. From Rust: `cargo` with the `wasm32-unknown-unknown` target.

### Streamable HTTP

`--http` serves the protocol at `http://<host>:<port>/mcp` instead of stdio:

```sh
kube32 mcp --http 8765 --context kube32
claude mcp add --transport http kube32 http://127.0.0.1:8765/mcp
```

The server listens on `127.0.0.1` unless `--http` names a host. On any other host it needs `--token` (or `KUBE32_MCP_TOKEN`), and clients send `Authorization: Bearer <token>`. It refuses requests whose `Origin` header names another site. It answers each POST with JSON and offers no event stream.

### Flags

| Flag | Default | Meaning |
| --- | --- | --- |
| `--allow-writes` | off | `write_device` sends actions to Devices, within their limits. A module that imports `k32_http_call` deploys. |
| `--allow-deploy` | off | `deploy_code` builds and deploys pods, and `delete_workload` deletes them. |
| `-n`, `--namespace` | `default` | The namespace of the pods that the server deploys and reports. Devices come from every namespace. |
| `--http [HOST:]PORT` | stdio | Serve streamable HTTP. |
| `--token <token>` | `KUBE32_MCP_TOKEN` | With `--http`: the bearer token that requests must carry. |
| `--context`, `--kubeconfig`, `--server`, `--kubectl` | | As for the other cluster commands. |

## Tools

| Tool | Does | Needs |
| --- | --- | --- |
| `list_devices` | The nodes (role, board, memory, the I2C parts each one found) and the Devices: driver, part, node, address, phase, readings with units, actions, notes and limits. Starts no driver. | |
| `read_device` | The latest readings of a Device, with units. Starts the driver of an `Idle` Device and waits for its first reading. With `samples` and `interval_seconds`, a table of readings over time with the minimum, maximum and change per minute of each. Flags readings outside the declared limits. | |
| `describe_device` | The Device's reference document in Markdown: part, node and address, readings with units and latest values, actions, limits, notes and how the driver behaves. `live: false` leaves the driver stopped. | |
| `write_device` | Sends an action to the Device's driver, such as `tare` or `led` on a scale or `set` on a switch, after the checks under [Safety](#safety). Returns the driver's answer. | `--allow-writes` |
| `deploy_code` | Builds C source, a `.c` file, a C or Rust project directory, or takes a `.wasm` file; checks it against the pod ABI; deploys it as a Pod or Deployment; waits for its first log lines. With `device`, binds the pod to the Device (see below). | `--allow-deploy` |
| `workload_status` | Phase, node, restarts, container state and exit reason, recent Events and the last log lines of a pod or of each pod of a Deployment. | |
| `delete_workload` | Deletes a pod or Deployment that `deploy_code` created. | `--allow-deploy` |

`deploy_code` builds with [`kube32 build`](cli-reference.md#app-commands) in the cache directory (`~/.cache/kube32/mcp/<name>/`, or `~/Library/Caches/kube32/mcp/<name>/` on macOS) and carries the module inline (`kube32.io/wasm-base64`). With `device`, the pod:

- receives each reading of the Device in `k32_event`: it subscribes to `device.<name>` (`kube32.io/event-topics`), which keeps an `OnDemand` driver running while the pod exists,
- gets `DEVICE_NAME`, `DEVICE_NAMESPACE`, `DEVICE_TOPIC` and `DEVICE_URL` (`http://<device>/`),
- runs on the Device's node with `near_device: true`, or on the node `node` names; otherwise the scheduler picks.

The pod's memory limit is what the module's static data and stack use (the `__heap_base` that `kube32 build` exports), rounded up to 1 KiB, unless `memory` sets it. A module without `__heap_base` gets its declared linear memory. The guide pod below runs in 5 KiB.

### Resources and prompt

| Resource | Content |
| --- | --- |
| `kube32://devices/<namespace>/<name>` | One Device's reference document, without live readings |
| `kube32://reference` | Every Device's reference document |
| `kube32://guides/device-pod` | How to write a pod that uses a Device, with the example below |
| `kube32://sdk/k32_app_abi.h` | The pod ABI header for C |

The prompt `explore-then-codify` takes `device` and `goal` and walks the agent through the loop below. Claude Code lists it as `/mcp__kube32__explore-then-codify`.

## Notes and limits

The owner of a Device writes two annotations. The server reads them on each call.

```sh
kubectl annotate device miniscale-cardputer-b0-m70c3-26 \
  kube32.io/notes='Kitchen scale. A 1 kg calibration weight sits on it between uses.' \
  kube32.io/limits='deny tare: the calibration weight must stay the zero point
led.r <= 64: the LED faces a bedroom
led.g <= 64
led.b <= 64
led every 5s: the unit is on a shared bus
weight_g <= 4500: the load cell is rated for 5 kg'
```

`kube32.io/notes` is free text: where the part is, what it measures in practice, what is near it. It appears in `list_devices` and in the reference document.

`kube32.io/limits` holds one rule a line. A reason in plain words may follow a colon; the server quotes it when it refuses an action. Lines that start with `#` are comments.

| Rule | Meaning |
| --- | --- |
| `deny <action>` | Refuse the action. `deny *` refuses every action. |
| `<action>.<argument> <op> <value>` | Refuse the action unless the argument satisfies the comparison. An action without the argument is refused. |
| `<action> every <duration>` | At most one such action per duration (`ms`, `s`, `m`, `h`). `* every 1m` counts every action. |
| `<reading> <op> <value>` | The reading's safe range: `read_device` and `describe_device` flag a reading outside it. |

`<op>` is `<`, `<=`, `>`, `>=`, `==` (or `=`) or `!=`. `<value>` is a number, `true`, `false` or a word; `<` and `>` compare numbers only. A value of another type than the rule's never satisfies it.

The rules fail closed. A line that does not parse, or a rule that names an action, argument or reading that the driver does not have, makes the server refuse every action on the Device until the annotation is fixed. `list_devices` shows these errors as `limit_errors`.

## Safety

- **Read-only by default.** Without flags, the server reads, describes and reports. Reading starts an `Idle` driver, as any use does ([Activation](devices.md#activation)).
- **Writes are opt-in.** With `--allow-writes`, `write_device` checks each action, in this order:
    1. the driver has the action, and each argument is one it takes, in its range (the driver's own `GET /describe`),
    2. the Device's `kube32.io/limits`.

    A refusal names the rule, its line and its reason. Rate rules count the actions that this server process sent.
- **Every action leaves a trace.** The server writes an Event with reason `Action` on the Device (`kubectl describe device`) and a line on its standard error.
- **Deploys are opt-in.** With `--allow-deploy`, `deploy_code` creates pods with the label `app.kubernetes.io/managed-by: kube32-mcp`. It replaces and `delete_workload` deletes only pods and Deployments that carry it.
- **Deployed pods reach hardware through drivers.** `deploy_code` grants channel events (`ipc`) and Service calls only. It refuses a module that imports GPIO, ADC, I2C, UART or RMT functions, or that uses the display, keyboard, speaker, SD card or chip temperature: deploy those with `kube32 deploy`.
- **Service calls count as writes.** A pod can send actions to drivers with `k32_http_call`, and these calls do not pass through the server. `deploy_code` deploys a module that imports `k32_http_call` only with `--allow-writes`. Without it, a deployed pod reads Devices through events.

The limits bind the actions that this server sends. `kubectl`, `curl` to a driver and pods that other tools deploy reach drivers directly, and the API server has no RBAC ([Kubernetes API coverage](api-reference.md#limitations)). The limits guard against an agent's mistakes, not against a user with access to the cluster.

Claude Code asks before each tool call unless the user allows the tool. The read tools carry `readOnlyHint`; `write_device` and `delete_workload` carry `destructiveHint`.

## Explore, then deploy: a BME688 example

An M5Stack ENV Pro (BME688) sits on channel 2 of a PaHub on the Cardputer. Discovery created its Device. The owner adds a note:

```sh
kubectl annotate device bme688-cardputer-b0-m70c2-77 \
  kube32.io/notes='On the kitchen shelf, 1 m above the kettle.'
```

In Claude Code, with the server added with `--allow-deploy`:

```text
/mcp__kube32__explore-then-codify bme688-cardputer-b0-m70c2-77 "raise an event when the humidity changes fast"
```

### Explore

The agent calls `describe_device` and learns the units (`humidity_pct` in `%RH`), the reading period (5 s), and that the driver takes no actions. It then asks the user to breathe on the sensor and samples it for a minute:

```json
{"device": "bme688-cardputer-b0-m70c2-77", "samples": 12, "interval_seconds": 5}
```

`read_device` answers with a table and a summary for each reading, for example:

```json
"summary": {
  "humidity_pct": {"min": 40.9, "max": 63.4, "first": 41.0, "last": 47.2,
                   "change_per_min": 6.2, "fastest_change_per_min": 270.0}
}
```

From the table the agent states what it learned, for example: the humidity rests near 41 %RH and moves by less than 0.2 %RH between readings, a breath raises it by about 20 %RH within one reading, and it falls back over about a minute. A threshold of 10 %RH per minute separates a breath from the resting noise by a wide margin.

### Codify

The agent reads `kube32://guides/device-pod` and writes the pod. Each reading arrives in `k32_event`; the pod compares it with the previous one and publishes on `humidity.fast` when the rate passes the threshold:

```c
/* humidity-watch: raises an event when the relative humidity that a BME688
 * reports changes faster than THRESHOLD %RH per minute. */
#include "runtime/k32_app_abi.h"

#define LIT(s) (s), (uint32_t)(sizeof(s) - 1)

static char in[256];                 /* each event is copied here (k32_alloc) */
static char out[160];
static uint32_t out_len;
static char device[64], topic[48] = "humidity.fast";
static uint32_t device_len, topic_len = 13;
static int32_t threshold = 1000;     /* hundredths of %RH per minute */
static int32_t last = -1;            /* the previous humidity, hundredths of %RH */
static int64_t last_ms;

/* In the guide: k32_init(), which reads DEVICE_NAME, THRESHOLD and
 * ALERT_TOPIC, and the helpers put(), put_str(), put_centi() and number() */

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof in ? (uint32_t)(uintptr_t)in : 0;
}

K32_EXPORT("k32_event") int32_t k32_event(uint32_t topic_ptr, uint32_t topic_n, uint32_t ptr, uint32_t len) {
    (void)topic_ptr; (void)topic_n;
    int32_t h;
    if (!number((const char*)(uintptr_t)ptr, len, "humidity_pct", &h)) return 0;
    int64_t now = k32_time_ms();
    out_len = 0;
    if (last < 0) {
        put_str("humidity-watch: first reading "); put_centi(h); put_str(" %RH");
        k32_log(out, out_len);
    } else if (now > last_ms) {
        int32_t rate = (int32_t)((int64_t)(h - last) * 60000 / (now - last_ms));
        if (rate >= threshold || -rate >= threshold) {
            put_str("{\"device\":\""); put(device, device_len);
            put_str("\",\"humidity_pct\":"); put_centi(h);
            put_str(",\"rate_pct_per_min\":"); put_centi(rate); put_str("}");
            k32_event_emit(topic, topic_len, out, out_len);
            k32_log(out, out_len);
        }
    }
    last = h;
    last_ms = now;
    return 0;
}
```

It deploys the pod next to the sensor:

```json
{"name": "humidity-watch", "source": "<the file>", "device": "bme688-cardputer-b0-m70c2-77",
 "near_device": true, "env": {"THRESHOLD": "10"}}
```

`deploy_code` builds the module (5 KB), checks it, sizes its memory (5 KiB) and waits one reading period:

```json
{
 "deployed": "Pod default/humidity-watch",
 "kind": "event",
 "memory": "5Ki",
 "hardware": ["ipc"],
 "node": "cardputer",
 "status": {"pod": "humidity-watch", "phase": "Running", "node": "cardputer", "restarts": 0, "state": "running"},
 "log": "humidity-watch: watching bme688-cardputer-b0-m70c2-77, alert above 10.00 %RH/min on humidity.fast\n[kube32] container started (linear memory 5 KiB, limit 5 KiB)\nhumidity-watch: first reading 41.02 %RH"
}
```

The agent asks for one more breath and confirms the alert with `workload_status`:

```text
{"device":"bme688-cardputer-b0-m70c2-77","humidity_pct":58.31,"rate_pct_per_min":207.48}
```

The pod now runs without the agent. Other pods receive its events with `kube32.io/event-topics: humidity.fast`, and an Ingress streams them to a browser with `kube32.io/event-stream: /humidity=humidity.fast` ([Networking](networking.md#ingress-and-pod-event-annotations)). `delete_workload humidity-watch` removes it.

## Limitations

- The server reads drivers at their nodes' addresses: the machine that runs it must reach them.
- Rate rules count the actions of one server process, and a restart clears them.
- Over HTTP the server sends no progress notifications; over stdio it sends them while it samples, builds and waits.
