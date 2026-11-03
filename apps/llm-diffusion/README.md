# llm-diffusion

llm-diffusion runs a masked diffusion language model on the control plane. A generation starts with a window of 64 tokens that is BOS, an optional prompt, and masks. Each step runs the whole window through the model once and reveals the masked positions it is most confident about, so the text forms from the masks in a fixed number of steps. A request chooses the number of steps; the pod reports the window's text after every step, with the time it took.

The model is `models/mdlm` ([model card](../../models/mdlm/README.md)): 4 layers, dimension 128, hidden size 320, 4 heads, vocabulary of 512 tokens and a mask, 820,480 weights, trained on TinyStories. Its int8 matrices stay in the node's flash.

## How it works

- **Weights in flash.** The pod opens the [blob](../../docs/storage.md#blobs) `mdlm` (846,532 bytes, 13 of the flash store's 64 KiB blocks). `k32_matmul_i8` reads each matrix from it in place ([linear algebra](../../docs/app-development.md#linear-algebra)). The pod's memory holds the activations, the norm vectors, the rotary table and the tokenizer: 256 KiB of linear memory, 341 KiB held by the pod on the board.
- **One read for 64 positions.** A matrix product takes the 64 positions of the window at once (`k32_matmul_i8` with n = 64), so a step reads each matrix once, 819,200 bytes of weights in all. The feed-forward block takes 32 positions a call and the classifier 40, which keeps the activations in 80 KiB.
- **Interpreter and native code.** The node computes the matrix products and the attention over the window (`k32_matvec_f32_i8`, `k32_softmax_f32`, `k32_vecmat_f32_i8`). The interpreter runs the norms, the quantization of every activation vector, the rotary embedding and SiLU.
- **Steps in slices.** `k32_tick` computes one part of a layer each call, at most about a second, so the node's other pods and the pod's own requests wait for a slice and not for a step.
- **Same bits as the reference.** The arithmetic is the order of operations of `models/mdlm/mdlm_infer.c`. The windows for the 18 test vectors of the model (8, 16 and 32 steps, greedy and temperature 0.8, with and without a prompt) are the reference's tokens on `kube32-sim`. On the board, the 8-step vectors with and without a prompt, with temperature 0.8, and the 16-step and 32-step greedy vectors are the reference's tokens.

## Build and deploy

```sh
make -C apps/llm-diffusion
kubectl --context kube32 replace --raw \
  "/api/v1/nodes/<node>/proxy/blobs/mdlm?size=$(wc -c < apps/llm-diffusion/build/mdlm-blob.bin)" \
  -f apps/llm-diffusion/build/mdlm-blob.bin
kubectl --context kube32 apply -f apps/llm-diffusion/build/llm-diffusion.yaml
```

`make` builds the module, `build/mdlm-blob.bin` (the model without its rotary table) and `build/llm-diffusion.yaml` (`examples/llm-diffusion.yaml` with the module inline). `tools/pack.py` writes the rotary table and the tokenizer into the module's data. The pod needs a node with PSRAM and about 13 free flash blocks. Upload the blob to the node's API server address when the request body exceeds the serial gateway's 64 KB.

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

## Measurements

ESP32-S3 with PSRAM at 240 MHz, one generation of 63 new tokens, greedy, no prompt. Times are the pod's own compute time; a generation takes longer on the clock when other pods use the node's interpreter (6% longer on an idle node).

| Steps | Per step | Window | Tokens/s | Weights read |
| ---: | ---: | ---: | ---: | ---: |
| 8 | 9.04 s | 72.4 s | 0.87 | 6.6 MB |
| 16 | 9.02 s | 144.3 s | 0.44 | 13.1 MB |
| 32 | 9.00 s | 287.8 s | 0.22 | 26.2 MB |

At 8 steps the text has misspellings, and at 32 steps it reads cleanly ([model card](../../models/mdlm/README.md#quality)).

A step of 9.0 s splits as follows (32-step generation):

| Part | Where | Per step | Share |
| --- | --- | ---: | ---: |
| Matrix products (`k32_matmul_i8`, weights from flash) | native | 1.93 s | 21% |
| SiLU and quantization of the hidden values | interpreter | 3.03 s | 34% |
| Norms, quantization, rotary embedding | interpreter | 2.46 s | 27% |
| Attention | native calls from the interpreter | 1.05 s | 12% |
| Residual sums | interpreter | 0.30 s | 3% |
| Classifier norms, softmax, selection | interpreter | 0.18 s | 2% |

A step does 52 M multiply-adds. The kernel runs 24 to 34 M multiply-adds a second on this board (`/bench`), and the flash delivers about 9 MB/s to a one-vector product, so the 819 KB of weights a step reads take under 0.1 s of the 9 s. The step is limited by the interpreter and the kernel's multiply-add rate, not by the flash.

An autoregressive model of the same size reads 819 KB per token. On this board a token of it costs about 0.2 s: 0.09 s to read and multiply a token's 819 KB through `k32_matvec_i8` (9 MB/s), 0.09 s of interpreter work per position (the per-position cost above), and 0.02 s of attention.

| | Weights read per token | Time per token | Tokens/s |
| --- | ---: | ---: | ---: |
| Diffusion, 8 steps | 104 KB | 1.15 s | 0.87 |
| Diffusion, 16 steps | 208 KB | 2.29 s | 0.44 |
| Diffusion, 32 steps | 416 KB | 4.57 s | 0.22 |
| Autoregressive, same size (computed from the costs above) | 819 KB | about 0.2 s | about 5 |
| stories260K, 3 layers per stage (`llm-pipeline`) | 196 KB | 0.06 to 0.07 s | 15 |

On this board the diffusion model reads fewer weight bytes per token than an autoregressive model and is slower, because its 69 multiply-adds per weight byte meet an interpreter and a kernel that compute slower than the flash reads.

## Limits

- A generation holds the node's interpreter in slices of up to about a second for minutes. Other pods answer between slices.
- One generation runs at a time.
- The blob takes 13 flash blocks, the free blocks of a board that also holds other models.
