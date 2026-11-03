# llm-speculative

llm-speculative runs greedy speculative decoding in one pod. A small draft model proposes k tokens, and the target model checks them in one pass over its weights. The target's weights stay in the node's flash: the native kernels read them in place, so a model larger than the pod's memory runs on a board.

| | Target | Draft |
| --- | --- | --- |
| Model | [`models/target-lm`](../../models/target-lm/README.md): llama2.c, dim 256, 5 layers, 3.57 M parameters | Andrej Karpathy's [stories260K](https://huggingface.co/karpathy/tinyllamas/tree/main/stories260K) (MIT license): dim 64, 5 layers |
| Tokenizer | tok512 | tok512 |
| Weights | int4 layers, int8 embedding and classifier | int8 |
| Blob | `target-lm`, 2,076,736 B | `stories260k`, 327,456 B |
| Weights read from flash | 1.90 MB a pass | 0.27 MB a token |

Both blobs and the tokenizer (`tok512`, 6,227 B) fit the 3,342,336 B of a board's [flash blob region](../../docs/storage.md#blobs). The pod's 448 KiB of memory hold the activations of 8 positions and both KV caches (160 positions).

## How it works

- **Weights in place.** The pod opens its blobs (`kube32.io/blobs`) and passes `K32_BLOB_ADDR` addresses of the weight rows, their scales, the norm vectors and the rotary table to the [linear algebra](../../docs/app-development.md#linear-algebra) imports. It reads one embedding row per position with `k32_blob_read`.
- **Native steps.** The norms, the int8 quantization of activations and of the keys and values, the rotary embedding, the SwiGLU gate, the residual additions and the pick of the next token are element-wise imports, each one call for all positions of a pass. The interpreter runs the control flow, the embedding lookup and the attention loop over the heads.
- **A pass.** stories260K catches up on the text and drafts k tokens, one position at a time. The target then computes the newest token and the k drafts as k + 1 positions at once: `k32_matmul_i4` (`k32_matmul_i8` for the classifier) reads each weight row once and applies it to every position. The positions write their keys and values into the cache before any of them attends, and each attends up to itself. The pass keeps the drafts up to the first one the target disagrees with, plus the target's own token after them.
- **Same tokens.** Each position is computed operation for operation as `llm-shard` computes it ([llm-pipeline](../llm-pipeline/README.md)), so the text is the target's greedy text, token for token, at every k. `tools/spec.py` gives the tokens and every pass's drafts and accepted count.
- **Ticks.** `k32_tick` runs the prompt (8 positions a tick) and then one pass a tick, so the node serves requests between passes. The pod is unmetered (`kube32.io/execution-budget: "off"`): a pass over 8 positions costs more than a call's default budget.

## Speed

Measured on the control plane, an ESP32-S3 devkit at 240 MHz with its flash in DIO mode at 80 MHz: 2 prompts ("Lily and Tom went to the park", "Once upon a time, there was a little girl named"), 64 tokens each, int4 target, int8 draft. Every run gives the reference's tokens, and every pass the reference's accepted count. The tokens/s count the pod's own calls; the node ran other pods between them, and the tokens/s by the wall clock are in parentheses.

| Run | tokens/s | acceptance | tokens a pass | verify pass | draft a token | target weights read a token |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| target alone | 3.56 (3.09) | | 1.00 | 281 ms | | 1.90 MB |
| speculative, k = 2 | 4.00 (3.67) | 0.599 | 2.17 | 398 ms | 74 ms | 0.88 MB |
| speculative, k = 4 | 3.60 (3.29) | 0.442 | 2.72 | 510 ms | 63 ms | 0.70 MB |
| speculative, k = 6 | 2.96 (2.77) | 0.333 | 2.91 | 610 ms | 65 ms | 0.65 MB |
| stories260K alone | 15.0 (10.9) | | | | 67 ms | |

With two drafts a pass the pod is 12% faster than the target alone; at k = 4 the two are even, and at k = 6 the drafts that the target rejects cost more than the reads they save. A verify pass costs about 225 ms plus 55 ms a position:

- The fixed part is mostly the read of the target's 1.9 MB of int4 weights, which stream from flash at about 12 MB/s.
- A position adds its multiply-adds, which run on the core's vector instructions, its attention over the cache, and the element-wise steps, which run natively over all positions of the pass in one call each.
- A drafted token takes 63 to 74 ms, about 22 ms of it the read of stories260K's 0.27 MB.

Acceptance decides the best k: the second prompt accepts 71% of two drafts and runs 4.24 tokens/s at k = 2 and 4.32 at k = 4, against 3.45 for the target alone.

## Build

```sh
make -C apps/llm-speculative TARGET_CKPT=<target-lm.bin>
```

This builds the module, writes the three blobs to `build/blobs` and renders `build/llm-speculative.yaml` with the module inline. `TARGET_CKPT` is the target's llama2.c checkpoint ([models/target-lm](../../models/target-lm/README.md) trains and exports it); stories260K and its tokenizer are downloaded once. The tools need [uv](https://docs.astral.sh/uv/), or a Python with numpy (`make SPEC="python3 ../llm-pipeline/tools/spec.py" LLM="python3 ../llm-pipeline/tools/llm.py"`).

## Run

Upload the blobs to the control plane's flash, then apply the manifest. Both go to the API server's network address: the serial gateway accepts bodies up to 64 KB.

```sh
python3 apps/llm-pipeline/tools/spec.py upload --dir apps/llm-speculative/build/blobs \
  --server http://<control-plane-ip>:6443 --node <control-plane-node>
kubectl --context kube32 --server http://<control-plane-ip>:6443 apply -f apps/llm-speculative/build/llm-speculative.yaml
curl -s -X POST "http://<control-plane-ip>/spec/generate?k=4&steps=64" -d "Lily and Tom went to the park"
curl -s http://<control-plane-ip>/spec
```

| Request | Answer |
| --- | --- |
| `POST /spec/generate?mode=M&k=K&steps=N` | Starts a generation of the prompt in the body. `mode`: `speculative` (default), `target` (the target alone) or `draft` (stories260K alone); `k`: drafts a pass, 0 to 7; `batch=0` runs the target's products one position at a time |
| `GET /spec` | The state, the settings, prefill and decode time, tokens/s, passes, acceptance, tokens a pass, verify time a pass, draft time a token, the time in the matrix products, the bytes of target weights read, the tokens and the text |
| `GET /spec/text` | The text so far |

| Variable | Meaning |
| --- | --- |
| `TARGET`, `DRAFT`, `TOKENIZER` | Blob names (`target-lm`, `stories260k`, `tok512`) |
| `K` | Drafts a pass when a request names none (4) |
| `STEPS` | Tokens to generate when a request names none (96) |
| `CONTEXT` | Positions at most (what the memory limit holds) |

`tools/spec.py bench --target <target-lm.bin> --target-bits 4 --url http://<control-plane-ip>/spec --prompts FILE` runs each prompt with the target alone, with drafts at each k and with the draft alone, checks every run's tokens and every pass's accepted count against the reference, and prints the means.
