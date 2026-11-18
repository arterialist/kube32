# clusterview

clusterview shows the cluster on a board's screen: the nodes and whether they are Ready, the hardware each one offers, the node of each selected pod, readings from selected Services, and the last messages on selected channel topics. It reads everything from the API server and the Services with `k32_http_call`, so it runs on any node with a screen. It sizes its layout from `k32_display_info`: the Cardputer Zero's 320x170 panel and the Cardputer's 240x135 one both work.

```sh
make -C apps/clusterview        # writes images/sd/clusterview.wasm and build/clusterview.yaml
kubectl --context kube32 apply -f apps/clusterview/build/clusterview.yaml
kubectl --context kube32 logs -f clusterview
```

`build/clusterview.yaml` is `examples/clusterview.yaml` with the module inline in place of `"@clusterview.wasm"`. The module can also go on a card as `wasm://sd/clusterview` (`kube32 sd-push`).

## The screen

| Part | Shows |
| --- | --- |
| Header | `TITLE`, and `4/4 ready`, or `1 not ready` and `api <reason>` in the accent colour |
| Nodes | One row a node, sorted by name: the name, twice the font's size while the rows take at most 3/5 of the screen, and tags from the Node's capacity. A node that is not Ready turns its row to the accent colour and its tags read `not ready ...`. Names turn dim while the API server does not answer. |
| Pods | The pods that `POD_SELECTOR` selects: the name, the node (`on devkit`), and the status when it is not `Running`. A pod that changes nodes keeps an accent row with the node it left (`atom > devkit`) for 8 s. |
| Readings | For each `READING_<n>`, its label and the JSON field of the Service's answer, three times the font's size (twice on a narrower screen), with its unit. A call that fails shows its reason (`no service`, `HTTP 503`, `no answer`) in the accent colour. |
| Messages | The last messages on the pod's `kube32.io/event-topics`, newest first, with the topic and the age (`now`, `42s`, `7m`, `3h`). JSON payloads read without their punctuation: `{"type":"echo","n":1}` reads `type echo n 1`. |

| Tag | Capacity |
| --- | --- |
| `disp` | `hardware.kube32.io/tft-display` |
| `touch` | `hardware.kube32.io/touch` |
| `keys` | `hardware.kube32.io/keyboard` |
| `spk`, `mic` | `hardware.kube32.io/speaker`, `hardware.kube32.io/microphone` |
| `sd` | `hardware.kube32.io/sd` |
| `i2c` | `esp32.io/i2c` |
| `ir`, `ir-tx`, `ir-rx` | `esp32.io/rmt-tx` and `esp32.io/rmt-rx`, or one of them |
| `gpio<N>` | `esp32.io/gpio` |

## Environment

| Variable | Meaning |
| --- | --- |
| `API_URL` | The API server. Default `http://$KUBERNETES_SERVICE_HOST:$KUBERNETES_SERVICE_PORT`, which the kubelet sets in every pod. |
| `API_TOKEN` | Bearer token for the API server, when it has one. |
| `POD_SELECTOR` | Label selector of the pods to show, e.g. `app in (irremote,room-monitor)`. Without it the screen has no pod column. |
| `POD_NAMESPACE` | Their namespace (default `default`; `all` for every namespace). |
| `READING_1` .. `READING_3` | `<label> <url> <field> [<unit>]`, e.g. `scale http://kitchen-scale/ weight_g g` for a [device driver](../drivers/README.md)'s `GET /`. |
| `TOPICS` | The topics it listens to, shown until the first message arrives. |
| `TITLE` | The header's title (default `cluster`). |
| `REFRESH_MS` | Time between refreshes, `500` to `60000` (default `2000`). |
| `TIMEOUT_MS` | Timeout of each call, `100` to `30000` (default `2000`). |
| `ACCENT` | The accent colour as `RRGGBB` (default `ffb000`). |

The pod needs `hardware.kube32.io/tft-display` (`DISPLAY`) and the `SERVICE_CALL` capability. Messages need no grant: list the topics in `kube32.io/event-topics`. A message whose topic and payload take 320 bytes or more together is skipped.

## How it reads the cluster

A refresh is a series of calls, one a `k32_tick`. `k32_tick` returns `1` while calls remain, so the node ticks the pod again 10 ms later, and no call holds the interpreter for longer than one HTTP call. Thus the pod runs on a board without PSRAM, whose pods share one interpreter.

1. `GET /api/v1/nodes?limit=4` with `Accept: application/json;as=Table;v=v1;g=meta.k8s.io`, then the next pages with `continue`. A Table row holds the node's name and `STATUS` in about 600 bytes, so a page fits the 4 KiB answer buffer. A page that does not fit is asked again with fewer rows.
2. `GET /api/v1/namespaces/<ns>/pods?labelSelector=<POD_SELECTOR>&limit=4` as a Table: the pod's name, `STATUS` and `NODE`.
3. `GET /api/v1/nodes/<name>` for the capacity of each node not read yet, and of one more node every refresh once a minute has passed since its last read.
4. `GET <url>` of each reading.

The pod scans the answers in place for the fields it needs. It keeps up to 8 nodes, 8 pods and 4 messages. A pod moved with `kube32 move` keeps its name, so a pod on another node than at the last refresh has moved. A pod of a Deployment or Job that is replaced gets a new name with the same prefix (the name before its last `-` and five characters); a new pod with a node whose prefix matches a pod that is gone, or terminating, has moved too.

## Memory

The pod's memory limit is 16 KiB: a 1.5 KiB stack and 13 KiB of data, mostly the 4 KiB answer buffer and the node and pod tables. The build checks this size after the link step. On a board without PSRAM the pod takes about 44 KB of the 80 KiB pod pool with its 19 KB module inline. A full redraw costs about 9.3 million gas units; large text is drawn again only when it changes.
