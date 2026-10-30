# webhook

Forwards channel events to an HTTP endpoint as JSON. Each event on a forwarded topic is POSTed to `WEBHOOK_URL`. It is a tick and event pod written in C: `k32_event` queues events, and `k32_tick` sends the oldest one, one POST per tick. It needs no thread of its own and runs on boards whose pods share one interpreter (the Cardputer). Pods on workers send their `https://` calls through the control plane's [egress gateway](../../docs/networking.md#egress-gateway).

```sh
make -C apps/webhook        # writes images/webhook.wasm (embedded as wasm://embedded/webhook)
kubectl create secret generic webhook --from-literal=url=https://<endpoint>
kubectl apply -f examples/webhook.yaml
kubectl logs -f webhook
```

## Request body

```json
{"topic":"rfid.tag","payload":"04a1b2c3d4e580","node":"esp32-node-02","time":"2026-10-30T12:34:56.789Z"}
```

| Field | Value |
| --- | --- |
| `topic` | The channel topic. |
| `payload` | The message as a string. Bytes outside printable ASCII are escaped as `\u00XX`. |
| `node` | `NODE_NAME`, omitted when unset. |
| `time` | The time the event arrived (UTC), omitted while the node's clock is not set. Only the control plane sets its clock (SNTP over its uplink), so events forwarded from a worker carry no `time`. |

With `WEBHOOK_TEXT_FIELD` set, the body also carries the event as one line of text under that name, for chat webhooks: `text` for Slack, `content` for Discord.

```json
{"topic":"rfid.tag","payload":"04a1b2c3d4e580","node":"esp32-node-02","text":"rfid.tag: 04a1b2c3d4e580 on esp32-node-02"}
```

Requests carry `Content-Type: application/json` and `User-Agent: kube32-webhook`.

## Delivery

| Answer | Action |
| --- | --- |
| `2xx` | Delivered. The next queued event follows on the next tick. |
| `408`, `429`, `5xx`, or no answer (timeout, refused, TLS failure, `K32_ERR_BUSY`, NetworkPolicy denial) | The event stays first in the queue. Retries wait 1 s, then 2, 4, and so on up to 60 s. |
| Any other status, `K32_ERR_INVAL` or `K32_ERR_NOTSUP` | The event is dropped. |

The queue holds 6 events. When the queue is full, a new event pushes out the oldest one. Each POST blocks for at most `WEBHOOK_TIMEOUT_MS`. Pods on a node share one interpreter, so the node's other pods wait for the POST to finish. On the Cardputer, a POST to an https endpoint takes 3 to 4 s, mostly for the TLS handshake, and other pods' calls wait that long. Run the pod on the control plane when a worker reads tags. The pod drops the response body: a large body ends the read, and `k32_http_status` returns the status.

## Log lines

```
webhook: forwarding rfid.tag,ir.received to https://hooks.example.com
webhook: rfid.tag delivered (200, 840 ms)
webhook: rfid.tag not delivered (certificate rejected, -129), retry in 4 s
webhook: rfid.tag rejected (404), dropped
webhook: queue full, dropping the oldest events (rfid.tag first)
webhook: 3 event(s) dropped while the queue was full
```

The log shows only the scheme and host of the URL.

## Environment

| Variable | Meaning |
| --- | --- |
| `WEBHOOK_URL` | Endpoint, `https://` or `http://`, up to 383 bytes (required; `k32_init` fails without it). |
| `WEBHOOK_TOPICS` | Topics to forward, comma separated (default `rfid.tag,ir.received`). The pod annotation `kube32.io/event-topics` delivers them; the example sets this variable from the annotation. |
| `NODE_NAME` | Sent as `node`; the example sets it from `spec.nodeName`. |
| `WEBHOOK_TEXT_FIELD` | Name of a one-line text field to add (letters, digits and `_`, up to 24); unset adds none. |
| `WEBHOOK_TIMEOUT_MS` | Timeout per POST, `500` to `15000` (default `8000`). |

The pod needs the `SERVICE_CALL` capability. `GET /` returns `{"queued":N,"delivered":N,"rejected":N,"dropped":N,"retrying":false,"last_result":N}`. The module uses 6.7 KiB of linear memory (a 1 KiB stack, the queue, and the request and response buffers); the manifest caps it at `8Ki`.
