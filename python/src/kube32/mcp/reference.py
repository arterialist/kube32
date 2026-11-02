"""What the MCP server tells an agent: its instructions, each Device's reference document, the guide
to writing a pod that uses a Device, and the explore-then-codify prompt."""
from . import limits as lim

INSTRUCTIONS = """\
kube32 runs a Kubernetes-compatible cluster on ESP32-S3 microcontrollers. Parts wired to the boards \
(sensors, scales, readers, switches) are Devices. A driver pod on the part's node operates each one, \
started on demand. These tools discover and read Devices, send them actions within the limits their \
owner declared, and deploy WebAssembly pods that the agent writes in C or Rust.

Work in two steps. Explore: list_devices, describe_device (the Device's reference: what it reads, its \
actions, its owner's notes and limits), then read_device with samples to learn how the part behaves. \
Codify: write a pod that does the task deterministically, deploy it with deploy_code bound to the \
Device, and check its output with workload_status. The resource kube32://guides/device-pod shows how \
to write such a pod; the prompt explore-then-codify walks through the steps.

Readings are values of the physical world: report them with their units. write_device moves real \
hardware: send an action only when the user asked for it."""

GUIDE_URI = "kube32://guides/device-pod"
ABI_URI = "kube32://sdk/k32_app_abi.h"
REFERENCE_URI = "kube32://reference"
DEVICE_URI = "kube32://devices/%s/%s"

# The pod of the guide and of docs/mcp.md. tools/tests run it on kube32-sim.
EXAMPLE_POD = r'''/* humidity-watch: raises an event when the relative humidity that a BME688
 * reports changes faster than THRESHOLD %RH per minute.
 *
 * Each reading of the Device arrives in k32_event (the pod subscribes to
 * device.<name>) with the driver's GET / body. Past the threshold the pod
 * logs a line and publishes it on ALERT_TOPIC:
 *   {"device":"<name>","humidity_pct":63.40,"rate_pct_per_min":24.60}
 *
 * Environment: DEVICE_NAME (deploy_code sets it), THRESHOLD in %RH per
 * minute (default 10), ALERT_TOPIC (default humidity.fast). */
#include "runtime/k32_app_abi.h"

#define LIT(s) (s), (uint32_t)(sizeof(s) - 1)

static char in[256];                 /* each event is copied here (k32_alloc) */
static char out[160];                /* the line being built */
static uint32_t out_len;
static char device[64], topic[48] = "humidity.fast";
static uint32_t device_len, topic_len = 13;
static int32_t threshold = 1000;     /* hundredths of %RH per minute */
static int32_t last = -1;            /* the previous humidity, hundredths of %RH */
static int64_t last_ms;

static void put(const char* s, uint32_t n) { while (n-- && out_len < sizeof out) out[out_len++] = *s++; }
static void put_str(const char* s) { uint32_t n = 0; while (s[n]) n++; put(s, n); }

/* A value in hundredths as a decimal number: 6340 -> 63.40 */
static void put_centi(int32_t v) {
    char t[14];
    int n = 0;
    if (v < 0) { put("-", 1); v = -v; }
    for (int i = 0; i < 3 || v; i++, v /= 10) {
        t[n++] = (char)('0' + v % 10);
        if (i == 1) t[n++] = '.';
    }
    while (n) put(&t[--n], 1);
}

/* The number after "<key>": in [s, s + n), in hundredths. 0 when the key is
 * missing or its value is not a number (null). */
static int number(const char* s, uint32_t n, const char* key, int32_t* value) {
    uint32_t k = 0;
    while (key[k]) k++;
    for (uint32_t i = 0; i + k + 3 < n; i++) {
        if (s[i] != '"') continue;
        uint32_t j = 0;
        while (j < k && s[i + 1 + j] == key[j]) j++;
        if (j < k || s[i + 1 + k] != '"' || s[i + 2 + k] != ':') continue;
        const char* p = s + i + k + 3;
        const char* end = s + n;
        int neg = p < end && *p == '-';
        if (neg) p++;
        if (p >= end || *p < '0' || *p > '9') return 0;
        int32_t v = 0, decimals = -1;          /* digits after the point; -1 before it */
        for (; p < end; p++) {
            if (*p == '.' && decimals < 0) { decimals = 0; continue; }
            if (*p < '0' || *p > '9') break;
            if (decimals < 2) v = v * 10 + (*p - '0');
            if (decimals >= 0 && decimals < 2) decimals++;
        }
        if (decimals < 0) decimals = 0;
        while (decimals++ < 2) v *= 10;
        *value = neg ? -v : v;
        return 1;
    }
    return 0;
}

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof in ? (uint32_t)(uintptr_t)in : 0;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    char v[12];
    int32_t n = k32_env_get(LIT("DEVICE_NAME"), device, sizeof device);
    device_len = n > 0 ? (uint32_t)n : 0;
    n = k32_env_get(LIT("ALERT_TOPIC"), topic, sizeof topic);
    if (n > 0) topic_len = (uint32_t)n;
    n = k32_env_get(LIT("THRESHOLD"), v, sizeof v);
    if (n > 0) {                     /* a whole number of %RH per minute */
        threshold = 0;
        for (int32_t i = 0; i < n && v[i] >= '0' && v[i] <= '9'; i++) threshold = threshold * 10 + (v[i] - '0');
        threshold *= 100;
    }
    out_len = 0;
    put_str("humidity-watch: watching "); put(device, device_len);
    put_str(", alert above "); put_centi(threshold); put_str(" %RH/min on "); put(topic, topic_len);
    k32_log(out, out_len);
    return 0;
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
'''

GUIDE = """\
# Writing a pod that uses a Device

A pod is a WebAssembly module that the node runs in its wasm3 interpreter. It talks to the node
through the imports that `runtime/k32_app_abi.h` declares (resource %(abi)s). C needs no standard
library: build with `deploy_code` and include only that header.

## Reaching the Device

- **Events.** `deploy_code` with `device` subscribes the pod to `device.<name>`: each reading
  arrives in `k32_event(topic_ptr, topic_len, ptr, len)` with the driver's `GET /` body,
  `{"ok":true,"readings":{...}}`, at most 200 bytes (`uptime_ms`, `node`, `device` and `driver`
  can be left out). The subscription keeps an OnDemand driver running while the pod exists.
- **Calls.** `k32_http_call` to `DEVICE_URL` (`http://<device>/`) reads with `GET /` and sends
  actions with `POST /` and `{"action":...}`. A module that imports `k32_http_call` deploys only
  when the server allows writes, because its calls bypass the Device's limits.

`deploy_code` sets `DEVICE_NAME`, `DEVICE_NAMESPACE`, `DEVICE_TOPIC` and `DEVICE_URL`.

## Exports

| Export | Called |
| --- | --- |
| `k32_init()` | once at start; non-zero fails the pod |
| `k32_alloc(size)` | before each event or request: return a buffer of `size` bytes in linear memory, or 0 |
| `k32_event(topic, topic_len, ptr, len)` | for each message on a subscribed topic |
| `k32_tick(uptime_ms)` | every second (or `kube32.io/tick-interval`); return > 0 to run again in 10 ms |
| `k32_handle(ptr, len)` | for each HTTP request: answer with `k32_resp_begin`, `k32_resp_write`, `k32_resp_end` |

Useful imports: `k32_log(text, len)` (one line of the pod's log), `k32_event_emit(topic, topic_len,
data, len)` (publish, 200 bytes at most), `k32_env_get(key, key_len, out, cap)`, `k32_time_ms()`.

## Rules of the runtime

- No libc and no heap: use static buffers, and write the few string and number helpers you need.
  Floating point works; fixed point (hundredths in an `int32_t`) keeps the code small.
- Keep functions short (`-O2` inlines helpers; on boards without PSRAM each instruction of a call
  holds native stack). Mark long computations `__attribute__((noinline))`.
- `deploy_code` sizes the pod's memory from the module: static data plus a 4 KiB stack, a few KiB.
  A board without PSRAM has an 80 KiB pool for all its pods, drivers included.
- A pod gets no bus, pin, display, speaker or card: it reaches hardware through Device drivers.

## Example: alert on fast humidity changes

Deploy with `deploy_code(name="humidity-watch", source=<this file>, device=<the BME688 Device>)`.
The first log line names the Device; the second shows its first reading.

```c
%(example)s```
""" % {"abi": ABI_URI, "example": EXAMPLE_POD}

PROMPT_NAME = "explore-then-codify"
PROMPT = {
    "name": PROMPT_NAME,
    "title": "Explore a Device, then codify what you learned",
    "description": "Learn how a Device behaves by reading it, then write and deploy a pod that does the task without an agent in the loop.",
    "arguments": [
        {"name": "device", "description": "The Device to work with (name or namespace/name); list_devices lists them", "required": True},
        {"name": "goal", "description": "What the deployed code must do, e.g. raise an event when humidity changes fast", "required": True},
    ],
}

PROMPT_TEXT = """\
Goal: {goal}
Device: {device}

Work in two phases with the kube32 tools.

Explore
1. describe_device {device}: what it reads (units), its actions, its owner's notes and limits, and how its driver behaves.
2. read_device {device} with samples and interval_seconds to watch it over time. When the goal depends on a change \
(a press, a breath, a load), tell me what to do with the part and when, then read while I do it.
3. State what you learned with numbers: resting values and noise, how large and how fast a real change is, and the \
reading period. Pick thresholds from these numbers, not from guesses.

Codify
4. Read {guide} and write a pod in C that does the goal without you: it reacts to the Device's readings, logs what \
it decides, and publishes an event for each thing it detects.
5. deploy_code with device={device}. Fix build or check errors and deploy again.
6. workload_status until the log shows the pod working, then trigger the behaviour once more and confirm the log \
or event.
7. Report: what you measured, the rule the pod applies, the pod's name and node, and how to remove it \
(delete_workload).

Do not send actions with write_device unless the goal asks for them; the Device's limits apply to every action."""


def device_ref(device):
    meta = device.get("metadata", {})
    return "%s/%s" % (meta.get("namespace", "default"), meta.get("name", ""))


def address(spec):
    """The part's address as `kubectl get devices` shows it: i2c0/0x70:2/0x77, gpio pin=4, or internal."""
    i2c = spec.get("i2c")
    if i2c:
        mux = i2c.get("mux")
        via = "%s:%s/" % (mux.get("address", "0x70"), mux.get("channel")) if mux else ""
        return "i2c%s/%s%s" % (i2c.get("bus", 0), via, i2c.get("address", "default"))
    if spec.get("pins"):
        return "gpio " + ", ".join("%s=%s" % kv for kv in sorted(spec["pins"].items()))
    return "internal"


def render_device(device, info, rules, answer=None, warnings=()):
    """A Device's reference document in Markdown. info: the driver's catalog entry merged with its
    /describe; rules: its parsed limits; answer: the driver's GET / body, when read."""
    meta, spec, st = device.get("metadata", {}), device.get("spec", {}), device.get("status", {}) or {}
    ns, name = meta.get("namespace", "default"), meta.get("name", "")
    notes = (meta.get("annotations") or {}).get(lim.NOTES_ANNOTATION, "").strip()
    readings = (answer or {}).get("readings") or {}
    out = ["# Device %s" % name, ""]
    out.append("- Part: %s (driver `%s`%s)" % (info.get("part", "unknown"), spec.get("driver", "?"),
                                               ", chip " + st["chip"] if st.get("chip") else ""))
    out.append("- Node: %s, %s" % (st.get("nodeName") or spec.get("nodeName") or "not placed", address(spec)))
    phase = st.get("phase", "Pending")
    out.append("- Phase: %s%s" % (phase, " (%s)" % st["message"] if st.get("message") and phase != "Running" else ""))
    activation = spec.get("activation", "OnDemand")
    out.append("- Activation: %s%s" % (activation, ", the driver stops %s s after the last use" % spec.get("idleSeconds", 300)
                                         if activation == "OnDemand" else ""))
    out.append("- Reading period: %s ms" % (spec.get("intervalMs") or info.get("period_ms", "?")))
    out.append("- From pods: Service `http://%s%s/`, channel topic `device.%s`" % (name, "" if ns == "default" else "." + ns, name))
    labels = {k: v for k, v in (meta.get("labels") or {}).items() if not k.startswith("kube32.io/")}
    if labels:
        out.append("- Labels: " + ", ".join("%s=%s" % kv for kv in sorted(labels.items())))
    if spec.get("config"):
        out.append("- Config: " + ", ".join("%s=%s" % kv for kv in sorted(spec["config"].items())))
    out += ["", "## Readings", "", "| Key | Unit |%s" % (" Latest |" if answer else ""), "| --- | --- |%s" % (" --- |" if answer else "")]
    for key, unit in info.get("readings", {}).items():
        out.append("| `%s` | %s |%s" % (key, unit, " %s |" % _json(readings.get(key)) if answer else ""))
    if answer and not answer.get("ok", False):
        out += ["", "The driver answers: %s" % answer.get("error", "no reading")]
    out += ["", "## Actions", ""]
    if info.get("actions"):
        for entry in info["actions"]:
            args = {k: v for k, v in entry.items() if k != "action"}
            out.append("- `%s`%s" % (entry["action"], " " + ", ".join("`%s`: %s" % kv for kv in args.items()) if args else ""))
    else:
        out.append("None: the driver only reads.")
    out += ["", "## Limits", ""]
    if rules.rules or rules.errors:
        out.append("Declared by the owner in `%s`. The server refuses actions outside them." % lim.ANNOTATION)
        out.append("")
        out += ["- %s" % r for r in rules.rules]
        out += ["- error: %s" % e for e in rules.errors]
    else:
        out.append("None declared (`%s`). The driver checks its own argument ranges." % lim.ANNOTATION)
    if warnings:
        out += [""] + ["- warning: %s" % w for w in warnings]
    out += ["", "## Notes", "", notes or "None (`%s`)." % lim.NOTES_ANNOTATION]
    if info.get("behaviour"):
        out += ["", "## Driver behaviour", ""] + ["- %s" % b for b in info["behaviour"]]
    return "\n".join(out) + "\n"


def _json(v):
    return "null" if v is None else "true" if v is True else "false" if v is False else str(v)
