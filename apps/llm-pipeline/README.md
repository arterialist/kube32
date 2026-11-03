# llm-pipeline

llm-pipeline runs one language model as pipeline stages on several nodes. Each stage is a pod that holds a run of the model's layers. A position goes through the stages one after the other, and the scheduler places each stage on a node that can hold it.

The model is Andrej Karpathy's [stories260K](https://huggingface.co/karpathy/tinyllamas/tree/main/stories260K) (llama2.c, MIT license): 5 layers, dimension 64, 8 heads with 4 KV heads, a vocabulary of 512, trained on TinyStories. `examples/llm-pipeline.yaml` splits it across three nodes:

| Stage | Node | Layers | Weights | Slice | KV cache |
| --- | --- | --- | --- | ---: | ---: |
| `llm-shard-0` | control plane (PSRAM) | embedding, 0-2 | int8 | 195,936 B | 292 B per position |
| `llm-shard-1` | Cardputer (80 KiB pod pool) | 3 | int4 | 25,632 B | 100 B per position |
| `llm-shard-2` | the fastest node, a `kube32-sim` worker | 4, final norm, classifier | int8 | 83,360 B | 100 B per position |

`llm-coordinator` runs the generation on the control plane, and `llm-store` serves the model's files from the `kube32-sim` node's storage.

## How it works

- **Weights.** Each matrix row is int8 or int4 (-7..7, two values a byte) with one float scale. Before each matrix product a stage quantizes the activation vector to int8 with one scale, so the product is a sum of integers. The KV cache is int8 with one scale per head and position. The rotary table goes with the first stage, which sends each position's coefficients along with it.
- **Native products.** The node computes the matrix products (`k32_matvec_i8`, `k32_matvec_i4`) and the attention over the KV cache (`k32_matvec_f32_i8`, `k32_softmax_f32`, `k32_vecmat_f32_i8`) natively ([linear algebra](../../docs/app-development.md#linear-algebra)), and so are the norms, the quantization, the rotary embedding, the SwiGLU gate, the residual additions and the pick of the next token (`k32_rmsnorm_f32`, `k32_quantize_i8`, `k32_rope_f32`, `k32_swiglu_f32`, `k32_add_f32`, `k32_argmax_f32`).
- **Stages.** At start a stage fetches its slice (`MODEL_URL`) into its linear memory and gives the rest of its memory limit to the KV cache. `POST /x` queues a position. `k32_tick` computes the oldest one and posts it to `NEXT_URL`. Thus a stage that waits for the network leaves its node's interpreter to the other pods, and stages on different nodes compute at the same time while the coordinator feeds the prompt. The last stage picks the most likely token and posts the position to the coordinator.
- **Coordinator.** It encodes prompts with the model's tokenizer, keeps up to 4 positions in flight, and feeds each chosen token back to the first stage. It publishes each token on the channel topic `llm.text`. A stage that lost its cache answers `409`, or no position comes back for 10 s. The coordinator then replays the session, and greedy decoding picks the same tokens again.
- **Screen.** A stage with `SCREEN=1` and the `DISPLAY` capability draws the text of the positions that pass through it, the prompt in grey, under a title row with the speed.
- **Same bits everywhere.** Every float operation runs in a fixed order as one IEEE binary32 operation, with no fused multiply-add and exp from its own polynomial (`k32_expf`), in the pod and in the node's native products alike. Thus a stage computes the same bits on an ESP32-S3 and on `kube32-sim`, and `tools/llm.py generate` gives the tokens the pipeline must produce.
- **Moves.** `llm-shard-2` has `kube32.io/migration: Auto` and `kube32.io/placement: fastest`. It runs on the fastest node. When that node leaves, the stage moves to a board with its KV cache and the generation goes on.

### Memory on the Cardputer

A board without PSRAM admits pods on its 80 KiB pool block, counting each pod's linear memory and its Pod Overhead (wasm3's structures, compiled code, value stack and the module's bytes). `llm-shard-1` takes 40 KiB of linear memory and 40 KiB of overhead, 80 KiB in all:

| Part | Bytes |
| --- | ---: |
| Shadow stack and static data | 3,472 |
| Layer 3: 7 int4 matrices, 600 row scales, 2 norm vectors | 25,632 |
| Activations, the int8 buffer, 3 position slots of 524 B | 4,464 |
| KV cache: 73 positions at 100 B (unused: 92 B) | 7,392 |
| Linear memory | 40,960 (40 KiB) |
| Pod Overhead (`spec.overhead`) | 40 KiB |

At int8 the layer is 48,288 B, and with its overhead it does not fit the pool. Other pods on the Cardputer, such as an active device driver, leave the stage `Pending`.

## Build

```sh
make -C apps/llm-pipeline
```

This builds the three modules with clang, downloads the model and tokenizer once into `build/model` (checked by SHA-256), and writes the slices and the tokenizer to `build/card/kube32/images/llm-store/` next to `llm-store.wasm`. It also writes `build/llm-pipeline.yaml`, the example with the modules inline. `tools/llm.py` needs [uv](https://docs.astral.sh/uv/), or a Python with numpy (`make LLM="python3 tools/llm.py"`). `make SPLIT=0-1,2,3-4 BITS=8,4,8` places other layers on each stage.

## Run

Start a `kube32-sim` worker that serves the model's files, on an address the boards reach:

```sh
KUBE32_SD_ROOT=$PWD/apps/llm-pipeline/build/card ./build/kube32-sim --role worker \
  --join http://<control-plane-ip>:6443 --node-ip <host-ip> --node-name model-host --pod-port 17000
kubectl apply -f apps/llm-pipeline/build/llm-pipeline.yaml
kubectl get pods -o wide
```

The coordinator generates its `PROMPT` once the stages have loaded, and the Cardputer shows the text. Start more generations and read the speed and the time of each stage and hop:

```sh
uv run --script apps/llm-pipeline/tools/llm.py run --url http://<control-plane-ip>/llm \
  --prompt "Lily went to the park" --steps 100 --check
```

`--check` compares the tokens with the reference. The coordinator's API is routed by the Ingress `llm`:

| Request | Answer |
| --- | --- |
| `POST /llm/generate` | Starts a generation: `{"prompt":"...","steps":N}`, or the prompt as plain text |
| `GET /llm` | The state, the tokens and text, prefill time, tokens/s, the token round trip, and per stage its node, compute time, queue wait and the time of its call to the next stage |
| `GET /llm/text` | The text so far |
| `GET /llm/events` | Each token as a Server-Sent Event |

| Coordinator variable | Meaning |
| --- | --- |
| `PROMPT` | Generated once at start (empty: none) |
| `STEPS` | Tokens to generate when a request names none (96) |
| `CONTEXT` | Positions at most (128). The smallest stage cache can make it fewer |
| `STALL_MS` | Replay after this long without a position back (10000) |

`tools/llm.py generate --prompt "..." --steps N --context C` prints the reference generation. `kube32 move <llm-shard-2 pod> --to <node>`, `kubectl drain`, or stopping the `kube32-sim` worker moves the last stage while a generation runs.

## Test

```sh
python3 tests/integration/runner.py --suite llm
```

The suite runs the pipeline on three `kube32-sim` nodes. It compares the tokens with the reference and checks the Cardputer's memory and its screen. It also stops the model host during a generation.
