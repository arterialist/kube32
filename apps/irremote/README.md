# irremote

Receives and sends infrared remote codes through the node's RMT channels. It
decodes NEC (including repeat codes and 16-bit extended addresses), logs any
other frame as raw durations, publishes the codes it decodes, and transmits
codes it is asked to send. It is a tick, event and request pod: it has no
thread of its own and never blocks waiting for a frame, so it runs on boards
whose pods share one interpreter (the Cardputer).

```sh
make -C apps/irremote        # writes images/irremote.wasm (embedded as wasm://embedded/irremote)
kubectl apply -f examples/irremote.yaml
kubectl logs -f irremote
```

| Claim | Pin | Used for |
| --- | --- | --- |
| `esp32.io/rmt-rx` | `kube32.io/rmt-rx: "pin=1"` | Receiving (M5Stack Unit IR receiver on the Cardputer's Grove port) |
| `esp32.io/rmt-tx` | board default (GPIO 44 on the Cardputer) or `kube32.io/rmt-tx: "pin=2"` | Sending |
| `hardware.kube32.io/ipc` | none | Publishing on `ir.received` |

Without `esp32.io/rmt-rx` the first receive returns `K32_ERR_PERM`; the pod
logs that once and only sends (`examples/irremote-tx.yaml`).

## Codes

Messages on `ir.send`, `POST /send` bodies and `IR_SEND_CODE` are one line:

| Text | Frame |
| --- | --- |
| `nec <addr> <cmd>` | NEC; `addr` 0 to `0xff` is sent with its complement, `0x100` to `0xffff` as an extended address; `cmd` 0 to `0xff`; decimal or `0x` hex |
| `nec repeat` | The NEC repeat code |
| `raw <d1>,<d2>,...` | Durations in microseconds, mark first, at most 512 |

Codes published on `ir.received` use the same form (`nec 0x04 0x08`, `nec
repeat`), so a message taken from it can be sent again unchanged.

## Log lines

```
irremote: receiving
irremote: nec addr=0x04 cmd=0x08
irremote: nec repeat
irremote: raw n=23 2400,600,1200,...
irremote: sent nec addr=0x04 cmd=0x08
```

## HTTP

* `GET /`: `{"received":N,"sent":N,"receiving":true,"codes":[{"t_ms":N,"code":"nec 0x04 0x08"},...]}`, the last 8 codes, newest first.
* `POST /send`: transmits the body; `{"sent":<durations>}`, or `400` for a body it cannot parse, `403` without the `esp32.io/rmt-tx` claim.

## Environment

| Variable | Meaning |
| --- | --- |
| `IR_SEND_EVERY_S` | Send `IR_SEND_CODE` every that many seconds; unset or `0` sends only on request. |
| `IR_SEND_CODE` | The code sent periodically (default `nec 0x04 0x08`). |

The module uses 8.3 KiB of linear memory (a 1 KiB stack, a 512-duration
receive buffer and send buffer, 4 KiB for request blocks); the manifests cap
it at `12Ki`.
