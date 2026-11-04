# llm-diffusion

llm-diffusion runs a masked diffusion language model on the control plane. A generation starts with a window of 64 tokens that is BOS, an optional prompt, and masks. Each step runs the whole window through the model once and reveals the masked positions it is most confident about, so the text forms from the masks in a fixed number of steps. A request chooses the number of steps; the pod reports the window's text after every step, with the time it took.

The model is `models/mdlm` ([model card](../../models/mdlm/README.md)): 4 layers, dimension 128, hidden size 320, 4 heads, vocabulary of 512 tokens and a mask, 820,480 weights, trained on TinyStories. Its int8 matrices stay in the node's flash.

## How it works

- **Weights in flash.** The pod opens the [blob](../../docs/storage.md#blobs) `mdlm` (846,532 bytes, 13 of the flash store's 64 KiB blocks). `k32_matmul_i8` reads each matrix from it in place ([linear algebra](../../docs/app-development.md#linear-algebra)). The pod's memory holds the activations, the norm vectors, the rotary table and the tokenizer: 256 KiB of linear memory, 341 KiB held by the pod on the board.
- **One read for 64 positions.** A matrix product takes the 64 positions of the window at once (`k32_matmul_i8` with n = 64), so a step reads each matrix once, 819,200 bytes of weights in all. The feed-forward block takes 32 positions a call and the classifier 40, which keeps the activations in 80 KiB.
- **Native code.** The node computes the matrix products, the attention of every position over the whole window (`k32_attention_full_f32_i8`, all heads and positions of a layer in one call) and the element-wise steps: the norms, the quantization of every activation vector and key and value row, the rotary embedding, the SwiGLU gate, the residual sums and the embedding rows (`k32_rmsnorm_f32`, `k32_quantize_i8`, `k32_rope_f32`, `k32_swiglu_f32`, `k32_add_f32`, `k32_add_f32_i8`), each over all positions of a part in one call. The token table is read from the blob once a step.
- **Steps in slices.** `k32_tick` computes one part of a layer each call, at most about a second, so the node's other pods and the pod's own requests wait for a slice and not for a step.
- **Same bits as the reference.** The arithmetic is the order of operations of `models/mdlm/mdlm_infer.c`. The windows for the 18 test vectors of the model (8, 16 and 32 steps, greedy and temperature 0.8, with and without a prompt) are the reference's tokens on `kube32-sim`. On the board, all 18 windows are the reference's tokens.

## Build and deploy

```sh
make -C apps/llm-diffusion
kubectl --context kube32 replace --raw \
  "/api/v1/nodes/<node>/proxy/blobs/llm-diffusion?size=$(wc -c < apps/llm-diffusion/build/llm-diffusion.wasm)" \
  -f apps/llm-diffusion/build/llm-diffusion.wasm
kubectl --context kube32 replace --raw \
  "/api/v1/nodes/<node>/proxy/blobs/mdlm?size=$(wc -c < apps/llm-diffusion/build/mdlm-blob.bin)" \
  -f apps/llm-diffusion/build/mdlm-blob.bin
kubectl --context kube32 apply -f apps/llm-diffusion/build/llm-diffusion.yaml
```

`make` builds the module, `build/mdlm-blob.bin` (the model without its rotary table), `build/llm-diffusion.yaml` (a copy of `examples/llm-diffusion.yaml`: the Deployment names the module blob, `wasm://blob/llm-diffusion`, so the object stays under the serial gateway's body limit) and the [viewer](#viewer-on-the-cardputer)'s `build/llm-diffusion-viewer.yaml`. `tools/pack.py` writes the rotary table and the tokenizer into the module's data. The pod needs a node with PSRAM and about 14 free flash blocks (13 for the weights, 1 for the module). Upload the blob to the node's API server address when the request body exceeds the serial gateway's 64 KB.

## API

The Ingress routes `/diffusion` to the pod.

| Request | Answer |
| --- | --- |
| `GET /diffusion/generate?steps=16&seed=1&temp=0&prompt=Once%20upon` | Starts a generation: 1 to 64 steps (default 16), temperature `temp / 100` (default 0, greedy), an optional URL-encoded prompt. `409` while one runs |
| `GET /diffusion/state?from=<step>` | The state (`idle`, `running`, `done`), and the steps from `from` on: `text` (masked positions as `_`), `revealed`, `us` and `matmul_us`. When done: `tokens`, `generated`, `compute_us`, `matmul_us`, `parts_us`, `wall_us`, `us_per_step`, `tokens_per_s_x100` and `weight_bytes_read` |
| `GET /diffusion/` | The model and the blob |
| `GET /diffusion/bench?n=<vectors>&reps=<r>` | The first feed-forward matrix applied to `n` vectors `r` times: the rate of the node's kernel |

```sh
curl -s "http://<control-plane>/diffusion/generate?steps=8&prompt=Lily%20went%20to%20the%20park"
curl -s "http://<control-plane>/diffusion/state?from=0"
```

`tools/check.py --url http://<control-plane>/diffusion` runs the model's test vectors against the pod and compares tokens, text and the tokens each step reveals.

Eight steps with no prompt, the text after steps 1, 4 and 8:

```
Once upon a time, there_ a_______________________________________________________
Once upon a time, there was a little girl named Lily. She loved to play with hers a the the the_ysss_______________________________
Once upon a time, there was a little girl named Lily. She loved to play with hers a the the the toysss. One day, shessked her to toyy of she wantould too to to at the heros..
```

## Viewer on the Cardputer

`llm-diffusion-viewer` (`viewer.c`) shows the text forming on the Cardputer's 240 x 135 screen. It starts a generation through the Service `llm-diffusion` with `k32_http_call`, reads `/state?from=<step>` every 400 ms while it runs, and draws the window after every step in 40 columns: revealed tokens in white, each masked position as a grey block, and a title line with the step and its time. A finished generation shows the time a step and the tokens a second, and stays on the screen for `PAUSE` ms before the next one starts with a fresh seed.

```sh
make -C apps/llm-diffusion
kubectl --context kube32 apply -f apps/llm-diffusion/build/llm-diffusion-viewer.yaml
kubectl --context kube32 logs llm-diffusion-viewer     # a line per step drawn, a line per generation
```

| Key | Action, when no generation runs |
| --- | --- |
| `1`, `2`, `3` | Starts 8, 16 or 32 steps |
| any other | Starts the next generation of `STEPS` |

| Variable | Meaning |
| --- | --- |
| `URL` | The diffusion pod's Service (`http://llm-diffusion`) |
| `STEPS` | Steps of each generation, in turn (`8,16,32`) |
| `TEMP` | Temperature x 100 (80; 0 is greedy) |
| `PAUSE` | Milliseconds the finished text stays (10000) |
| `PROMPT` | The start of the text (none) |

The pod holds 20 KiB of linear memory, its 6.6 KB module and the display, keyboard and `SERVICE_CALL` capabilities. The screen serves one pod at a time: on a board that runs wordsketch, scale `wordsketch-listen` to 0 first.

## Measurements

ESP32-S3 devkit with PSRAM at 240 MHz, flash in [quad I/O mode](../../docs/flashing-troubleshooting.md#quad-io-flash) at 80 MHz, one generation of 63 new tokens, greedy, no prompt. Times are the pod's own compute time; a generation takes 1.4 to 1.9 times longer on the clock while other pods share the node's interpreter.

| Steps | Per step | Window | Tokens/s | Weights read |
| ---: | ---: | ---: | ---: | ---: |
| 8 | 1.63 s | 13.1 s | 4.81 | 6.6 MB |
| 16 | 1.44 s | 23.1 s | 2.73 | 13.1 MB |
| 32 | 1.44 s | 46.2 s | 1.36 | 26.2 MB |

Across all 18 test vectors a step takes 1.41 to 1.63 s on average and the fastest steps 1.17 s. At 8 steps the text has misspellings, and at 32 steps it reads cleanly ([model card](../../models/mdlm/README.md#quality)):

```
8 steps:  Once upon a time, there was a little girl named Lily. She loved to play with hers a the the the toysss. One day, shessked her to toyy of she wantould too to to at the heros..
16 steps: Once upon a time, there was a little girl named Lily. She loved to play with hers a the the the park. One day, she saw a big antchen on the ground.. The sun told herim aboutas
32 steps: Once upon a time, there was a little girl named Lily. She loved to play with her toys and explore the world. One day, she found a big buttty flower in her garden. I was
32 steps, prompt "Lily went to the park": Lily went to the park with her mom. They saw a big tree with many treats werees. Lily wanted to playe the tree, but she didn't know what to do. She want liked the
```

A step of 1.42 s splits as follows (8 steps, greedy, no prompt):

| Part | Per step | Share |
| --- | ---: | ---: |
| Attention (`k32_attention_full_f32_i8`, one call a layer) | 570 ms | 40% |
| Matrix products (`k32_matmul_i8`, weights from flash) | 431 ms | 30% |
| Norms, quantization, rotary embedding | 153 ms | 11% |
| SiLU gate and quantization of the hidden values | 129 ms | 9% |
| Classifier norms, softmax, selection | 62 ms | 4% |
| Embedding and residual sums | 56 ms | 4% |

A step does 52 M int8 multiply-adds, 120 M a second in the products. The 819 KB of weights a step reads take about 45 ms of that at the flash's 18 MB/s; the rest is the vector unit over the 64 positions. The attention's 4.2 M float multiply-adds over the window, and the element-wise steps over the pod's memory in PSRAM, take the larger part of a step.

| | Weights read per token | Time per token | Tokens/s |
| --- | ---: | ---: | ---: |
| Diffusion, 8 steps | 104 KB | 0.21 s | 4.81 |
| Diffusion, 16 steps | 208 KB | 0.37 s | 2.73 |
| Diffusion, 32 steps | 416 KB | 0.73 s | 1.36 |
| stories260K, autoregressive ([llm-speculative](../llm-speculative/README.md), draft alone, by the host's clock) | 270 KB | 0.10 s | 10.5 |

## Limits

- A generation holds the node's interpreter in slices of up to about a second, for about 15 s at 8 steps to about 50 s at 32 steps on the clock. Other pods answer between slices.
- One generation runs at a time.
- The blob takes 13 flash blocks, the free blocks of a board that also holds other models.
