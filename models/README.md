# Models

Small neural networks for kube32 pods, trained on public datasets and exported in formats that pods read directly. Every model's inference is a sequence of kube32's [linear algebra imports](../docs/app-development.md#linear-algebra) (`k32_matvec_i8`, `k32_matvec_f32_i8`, `k32_vecmat_f32_i8`, `k32_softmax_f32`) and IEEE binary32 operations in a fixed order. Thus each model's Python reference computes the bits a pod computes, on an ESP32-S3 or on `kube32-sim`, and its test vectors check a pod value for value.

| Model | Directory | Task | Weights | Quality | Runs on |
| --- | --- | --- | ---: | --- | --- |
| Target LM | [target-lm](target-lm/) | TinyStories text, the target of speculative decoding with stories260K as draft | 3.57 M (3.8 MB int8, 2.1 MB int4) | validation loss 0.875 int8 (stories260K 1.333); 3.21 tokens a pass at k = 4 | control plane, weights streamed from flash |
| Masked diffusion LM | [mdlm](mdlm/) | TinyStories text, 64 tokens a window, 8-32 denoising steps | 0.82 M (855 KB int8) | bound 1.91 nats a token | control plane (PSRAM) |
| Doodle denoiser | [doodle](doodle/) | Quick, Draw! cat, dog, bird, house, tree, bed at 28x28, DDIM in 8 or 16 steps | 29 K (35.6 KB) / 294 K (313 KB) | class accuracy 73-79 % / 96-98 % | Cardputer / control plane |
| Keyword spotter | [kws](kws/) | Speech Commands v2: cat, dog, bird, house, tree, bed, unknown, silence | 25 K (30 KB blob) | 94.3 % on the test split | Cardputer |

## Layout

- `common/k32ref.py` - the node's arithmetic in numpy (quantization, the linear algebra imports, `k32_expf`, the xorshift32 generator).
- `common/k32_host.cpp` - the linear algebra imports for host builds of a model's C code, linked to the node's kernels (`src/runtime/pod_linalg.cpp`).
- `lm/` - the Transformer, training and evaluation for both language models; `mdlm.py` exports and samples the diffusion model. The autoregressive model is a llama2.c checkpoint, so `apps/llm-pipeline/tools/llm.py build --model` splits it into pipeline stages and `apps/llm-pipeline/tools/spec.py` runs speculative decoding.
- `doodle/`, `kws/` - training, export, reference, and C inference (`*_infer.c`) for pods.

## Checks

```sh
make -C models check
```

builds the C inference of the keyword spotter and the doodle denoiser with the node's kernels and compares it with the test vectors, value for value.

Training data downloads into `~/.cache/kube32-models` (`lm/tinystories.py`, `doodle/fetch.py`, `kws/fetch.py`); the scripts run with any Python that has torch, numpy and sentencepiece.
