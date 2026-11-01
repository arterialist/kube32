# room-monitor

room-monitor shows the sensors of one room. The app knows Service names, not hardware. For each request, it calls the [device drivers](../drivers/README.md) behind `ENV_URL` and `SCALE_URL` (`GET /`) with `k32_http_call`, and it answers with their replies. The app runs on any node, including a `kube32-sim` worker without GPIO, on whichever nodes the parts are wired to.

```sh
make -C apps/room-monitor        # writes images/room-monitor.wasm (embedded as wasm://embedded/room-monitor)
kubectl apply -f examples/devices/room-monitor.yaml
curl -s http://<control plane>/kitchen
```

| Request | Answer |
| --- | --- |
| `GET /` | `{"room":"kitchen","env":<the env driver's GET / body>,"scale":<the scale's>}`. A device that the app cannot reach appears as `{"ok":false,"error":"..."}`. |
| `GET /view` | The same data as an HTML page that reloads every 5 seconds. The page has a button that tares the scale. |
| `POST /tare` | Sends `{"action":"tare"}` to the scale and answers with its reply. A form post from the page redirects back to `view`. |

The app matches each path on its end. Thus it works behind an Ingress prefix, for example `/kitchen/view`.

| Variable | Meaning |
| --- | --- |
| `ROOM` | The room's name (default `room`). |
| `ENV_URL` | The environment sensor's Service, for example `http://kitchen-env`. |
| `SCALE_URL` | The scale's Service, for example `http://kitchen-scale`. |
| `TIMEOUT_MS` | Timeout for each call, `100` to `30000` (default `3000`). |

You can omit either URL. The pod needs the `SERVICE_CALL` capability.

`examples/devices/room-monitor.yaml` defines the Services as label selectors over driver pods (`kube32.io/driver: bme688` and `location: kitchen`). Thus the app reads the Device with the label `location=kitchen`, on any node that has the part.

The linear memory has a limit of 4 KiB: a 512-byte stack and 3.3 KiB of data. The build checks this size after the link step.
