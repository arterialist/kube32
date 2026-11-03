# Example: neural networks in pods

`models/` holds small neural networks trained on public datasets and exported in the formats pods read. Each model's inference is a fixed sequence of the [linear algebra imports](../app-development.md#linear-algebra) and binary32 operations, so a numpy reference computes the bits a pod computes, on an ESP32-S3 or on `kube32-sim`.

| Model | Task | Weights | Quality |
| --- | --- | ---: | --- |
| [Target LM](https://github.com/arterialist/kube32/tree/main/models/target-lm) | TinyStories text; the target of speculative decoding | 3.57 M | validation loss 0.875 (stories260K: 1.333) |
| [Masked diffusion LM](https://github.com/arterialist/kube32/tree/main/models/mdlm) | TinyStories text, 64 tokens a window in 8 to 32 steps | 0.82 M (855 KB) | 1.91 nats a token |
| [Doodle denoiser](https://github.com/arterialist/kube32/tree/main/models/doodle) | Quick, Draw! drawings at 28x28 for six classes | 29 K and 294 K | class accuracy 73 to 79 % and 96 to 98 % |
| [Keyword spotter](https://github.com/arterialist/kube32/tree/main/models/kws) | Speech Commands v2, eight classes | 25 K | 94.3 % on the test split |

The checkpoints of the keyword spotter, the doodle denoisers and the diffusion model are in the repository. The target LM's weights come from `models/lm/train.py` and `apps/llm-pipeline/tools/spec.py blobs`. `make -C models check` builds the C inference of the first three with the node's kernels and compares it with their test vectors, and `models/playground.py` runs all of them on the host.

## Pods

| Example | What it runs |
| --- | --- |
| [`llm-pipeline`](https://github.com/arterialist/kube32/tree/main/apps/llm-pipeline) | stories260K split into three pipeline stages across nodes, with a move of a stage and its KV cache while it generates |
| [`llm-speculative`](https://github.com/arterialist/kube32/tree/main/apps/llm-speculative) | greedy speculative decoding: stories260K drafts, the target LM checks k tokens in one pass over int4 weights read in place from flash |
| [`llm-diffusion`](https://github.com/arterialist/kube32/tree/main/apps/llm-diffusion) | the masked diffusion LM, with the weights in a flash blob and the text of every denoising step in the response |

The weights of the language models are [blobs](../storage.md#blobs): the node maps them from flash and `k32_matvec_i8`, `k32_matvec_i4`, `k32_matmul_i8` and `k32_matmul_i4` read them in place, so a model larger than the pod's memory runs on a board. `llm-speculative` and `llm-diffusion` read fewer weight bytes a token than plain autoregressive decoding, and on an ESP32-S3 they generate fewer tokens a second: the interpreter and the multiply-add rate of the kernels limit a step, not the flash. Each app's README has the measurements.
