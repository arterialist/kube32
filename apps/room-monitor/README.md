# room-monitor

One view of a room's sensors. The app knows Service names, not hardware: each request calls the [device drivers](../drivers/README.md) behind `ENV_URL` and `SCALE_URL` (`GET /`) with `k32_http_call` and answers with what they said. It runs on any node, including a `kube32-sim` worker without GPIO, wherever the parts are wired.

```sh
make -C apps/room-monitor        # writes images/room-monitor.wasm (embedded as wasm://embedded/room-monitor)
kubectl apply -f examples/devices/room-monitor.yaml
curl -s http://<control plane>/kitchen
```

| Request | Answer |
| --- | --- |
| `GET /` | `{"room":"kitchen","env":<the env driver's GET / body>,"scale":<the scale's>}`. A device the app could not reach appears as `{"ok":false,"error":"..."}`. |
| `GET /view` | The same as an HTML page that reloads every 5 seconds, with a button that tares the scale. |
| `POST /tare` | Sends `{"action":"tare"}` to the scale and answers with its reply. A form post from the page redirects back to `view`. |

Paths match on their end, so the app works behind an Ingress prefix such as `/kitchen/view`.

| Variable | Meaning |
| --- | --- |
| `ROOM` | The room's name (default `room`). |
| `ENV_URL` | The environment sensor's Service, such as `http://kitchen-env`. |
| `SCALE_URL` | The scale's Service, such as `http://kitchen-scale`. |
| `TIMEOUT_MS` | Timeout per call, `100` to `30000` (default `3000`). |

Either URL may be left out. The pod needs the `SERVICE_CALL` capability. `examples/devices/room-monitor.yaml` defines the Services as label selectors over driver pods (`kube32.io/driver: bme688` and `location: kitchen`): a Device labeled `location=kitchen` is the one the app reads, on whichever node it is wired to. Linear memory is capped at 4 KiB (a 512-byte stack and 3.3 KiB of data, checked after linking).
