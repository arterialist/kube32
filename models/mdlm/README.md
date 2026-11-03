# Masked diffusion LM

A masked diffusion language model (MDLM / LLaDA style) on TinyStories with the tok512 vocabulary. It writes a window of 64 tokens: the window starts as BOS, an optional prompt, and MASK everywhere else, and each step reveals the masked positions the model is most confident about. Every step reads the weights once for all 64 positions, so the model trades multiply-adds for memory reads, which these chips are short of.

| | |
| --- | ---: |
| Dimension, hidden, layers | 128, 320, 4 |
| Heads (KV heads), head size | 4 (4), 32 |
| Window | 64 tokens (BOS included) |
| Vocabulary | 512 + MASK (id 512) |
| Parameters | 820,480 |
| Blob, int8 (`mdlm.bin`) | 854,724 B |
| Activations of `mdlm_infer.c` (`-DMDLM_MAX_DIM=128 -DMDLM_MAX_HIDDEN=320`) | about 97 KB |

## Quality

Validation shard (data00), 2,560 windows of 64 tokens at random offsets. The masked diffusion number is its bound on the negative log-likelihood (nats a token); the autoregressive numbers are their loss with at most 64 tokens of context:

| | float | int8 |
| --- | ---: | ---: |
| Masked diffusion (this model, 0.82 M) | 1.908 | 1.913 |
| stories260K (autoregressive, 0.26 M) | 1.481 | 1.484 |
| Target LM (autoregressive, 3.57 M) | 1.076 | 1.077 |

Training: 58,263 steps of 256 windows of 64 tokens (955 M tokens; half the windows start at a story), MDLM's bound with a linear schedule (mask each token with probability t, weight 1/t), no time input, AdamW at 2e-3 with a cosine to 2e-4, bf16 on an M3 Pro GPU, 55 minutes.

Samples from `mdlm.bin` (int8 reference, `samples.txt` has all 18 of the test vectors):

> 32 steps, greedy: Once upon a time, there was a little girl named Lily. She loved to play with her toys and explore the world. One day, she found a big buttty flower in her garden. I was

> 32 steps, temperature 0.8, prompt "Lily went to the park": Lily went to the park. She saw a pretty crack on the ground. She wanted to toatch it, but she didn't know what to do. So, she found a wre

> 8 steps, greedy: Once upon a time, there was a little girl named Lily. She loved to play with hers a the the the toysss. One day, shessked her to toyy of she wantould too to to at the heros..

At 8 steps each step fills 8 positions independently, and neighbors disagree (misspelled words); 32 steps read cleanly.

## Cost per generated token

63 tokens a window; the autoregressive model of the same size reads its weights for each token and attends to 32 positions on average. Computed by `mdlm.py cost`:

| | Passes a token | Weight bytes a token | Multiply-adds a token | Multiply-adds per weight byte |
| --- | ---: | ---: | ---: | ---: |
| Masked diffusion, 8 steps | 0.127 | 104 KB | 7.19 M | 69 |
| Masked diffusion, 16 steps | 0.254 | 208 KB | 14.4 M | 69 |
| Masked diffusion, 32 steps | 0.508 | 416 KB | 28.8 M | 69 |
| Autoregressive, same size | 1 | 819 KB | 0.85 M | 1.04 |

The multiply-adds count every position's classifier; `mdlm_infer.c` computes logits only at masked positions.

## Format and sampler

`mdlm.bin` is llm-pipeline's model slice (`apps/llm-pipeline/llm.h`) with flags EMBED, HEAD, TIED (the classifier is the embedding, stored once) and BIDIR: vocabulary 513, rotary table of 64 positions. A step computes every layer as `llm-shard` does, except that attention at each position covers all 64 positions: q, k and v of every position first (K and V int8 with a scale per head), then each position's attention, output projection and feed-forward block. The sampler (`models/lm/mdlm.py`, `mdlm_infer.c`):

- step s of S reveals floor(M (s + 1) / S) - floor(M s / S) of the M positions masked at the start;
- temperature 0: a masked position's candidate is its first largest logit of 0..511, its confidence that token's probability under `k32_softmax_f32`;
- temperature T: probabilities `k32_softmax_f32(logits * (1.0f / T))`, one xorshift32 uniform per masked position in position order (`models/common/k32ref.py`), the first token whose running sum exceeds it;
- the step reveals the most confident positions, the lower position first among equal ones.

## Files

- `mdlm.bin` - the model.
- `vectors/mdlm.json` - 18 samples (no prompt and "Lily went to the park"; 8, 16, 32 steps; greedy and temperature 0.8 with seeds 1 and 2): the window, the text, each step's revealed positions, tokens and confidences, and the first step's logits at position 1. `vectors/mdlm-samples.bin` holds the same windows for `mdlm_test.c`.
- `mdlm_infer.c` - the sampler in C for a pod; `make -C models check` compares its 18 windows with the reference's.
- `tok512.bin` - the tokenizer (llama2.c format) the pod encodes prompts with.
- `loss-64.json`, `samples.txt`.

`apps/llm-diffusion` runs the model as a pod ([README](../../apps/llm-diffusion/README.md)).

```sh
python models/lm/train.py --mode mdlm --out DIR --dim 128 --hidden 320 --layers 4 --heads 4 --kv-heads 4 \
  --seq-len 64 --batch 256 --lr 2e-3 --min-lr 2e-4 --warmup 500 --steps 100000 --minutes 55 --bf16 --compile
python models/lm/mdlm.py export --ckpt DIR/ckpt.pt --out mdlm.bin
python models/lm/mdlm.py sample --model mdlm.bin --steps 16 --temperature 0.8 --seed 3 --prompt "Tom had a red ball"
```
