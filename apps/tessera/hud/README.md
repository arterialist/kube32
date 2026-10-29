# tessera-hud

The office keypad on the Cardputer: live counters on the LCD (open issues, requests per second, viewers, events) and the keyboard as an input to Tessera. It is a tick/event pod: no `k32_handle` and no thread of its own, so it runs on boards whose pods share one interpreter stack.

| Entry point | Does |
|---|---|
| `k32_init` | draws the screen |
| `k32_event` | `tessera.events` (pod annotation `kube32.io/event-topics: tessera.events`): issue and comment events from `tessera-api`, the ingress's `stats` lines |
| `k32_tick` | reads the keypad, publishes `{"t":"key","c":"<char>"}` on `tessera.input` per key, redraws the counters once a second; returns 1 so the node ticks it again after about 10 ms |
| `k32_alloc` | the 232-byte buffer the host copies one event into |

## Environment

| Name | Default | Description |
|---|---|---|
| `HUD_TITLE` | `TESSERA` | Title on the top bar |
| `HUD_SOUND` | off | `1` plays tones on new issues, deletes, updates and key presses. The pod also needs `hardware.kube32.io/speaker: "1"` and the `SPEAKER` capability; without them `k32_tone` is refused. |

## Memory

Static data only: a 256-byte stack (deepest call chain: 96 bytes) and 488 bytes of data, 752 bytes in all under the 1 KiB cap (`MEM_CAP`, pod limit `1Ki`). The module is 3.6 KB (`-Oz`). Besides its linear memory, the pod's interpreter allocations on a 64-bit host come to 36 KB at deploy and 32 KB after events and ticks; a 32-bit board needs a quarter to half of that.

```sh
make -C apps/tessera/hud      # ../../../images/tessera-hud.wasm, layout check against MEM_CAP
```
