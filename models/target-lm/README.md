# Target LM

A llama2.c Transformer trained on TinyStories with stories260K's 512-token vocabulary (tok512), so stories260K drafts for it in speculative decoding. Its weights do not fit a board's pod memory: a pod streams them from flash or the card once per pass, and a pass checks several drafted tokens at once.

| | Target LM | stories260K (draft) |
| --- | ---: | ---: |
| Dimension, hidden, layers | 256, 640, 5 | 64, 172, 5 |
| Heads (KV heads), head size | 8 (4), 32 | 8 (4), 8 |
| Context | 256 | 512 |
| Parameters | 3,574,528 | 260 K |
| Model slice, int8 (`llm.py build`) | 3,797,056 B | 327,456 B |
| Model slice, int4 layers | 2,076,736 B | |
| Read per pass: layers + classifier, int8 / int4 | 3.63 MB / 1.91 MB | 0.27 MB |
| Multiply-adds per position | 3.57 M | 0.26 M |
| KV cache per position (int8, all layers) | 1,444 B | 484 B |

## Quality

Validation loss on TinyStories' validation shard (data00; neither model trained on it), 1,920 windows of 256 tokens, the same windows for both. int8 and int4 simulate the node's quantization (weight rows, activations before every product, the KV cache):

| | float | int8 | int4 |
| --- | ---: | ---: | ---: |
| Target LM | 0.874 | 0.875 | 0.917 |
| stories260K | 1.330 | 1.333 | 1.491 |

Training: 21,000 steps of 64 x 256 tokens (344 M tokens, data01-24), AdamW, learning rate 1.5e-3 with a cosine to 1.5e-4, bf16 on an M3 Pro GPU (MPS), 72 minutes. `samples.txt` holds greedy text of both models from the int8 slices, computed by `llm.py generate`, the tokens a pipeline produces:

> Lily and Tom went to the park with Mom. They saw a big slide and wanted to play on it. They climbed up the ladder and slid down the slide. They laughed and shouted. "Look, Tom, I can slide down the slide!" Lily said. "Wow, Lily, you are so fast!" Tom said.

## Speculative decoding

`apps/llm-pipeline/tools/spec.py measure`, int8 target and int8 draft, greedy, 50 prompts (`models/lm/prompts.txt`, the first ten words of validation stories) and 200 tokens each (9,997 tokens). The text equals plain greedy decoding of the target, token for token, in every run. Acceptance is accepted drafts over proposed drafts; a pass yields the accepted drafts and the target's own next token.

| k | stories260K: acceptance | tokens a pass | prompt lookup: acceptance | tokens a pass |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0.772 | 1.77 | 0.304 | 1.17 |
| 2 | 0.694 | 2.38 | 0.216 | 1.24 |
| 3 | 0.620 | 2.84 | 0.170 | 1.27 |
| 4 | 0.559 | 3.21 | 0.137 | 1.29 |
| 6 | 0.456 | 3.69 | 0.098 | 1.30 |
| 8 | 0.379 | 3.97 | 0.077 | 1.31 |

`acceptance.json` has the counts. With int4 target layers (`acceptance-int4.json`) the draft's acceptance is 0.674, 0.535 and 0.436 for k = 2, 4, 6 (2.34, 3.11 and 3.57 tokens a pass). Prompt lookup drafts the tokens that followed the latest earlier occurrence of the newest 3, 2 or 1 tokens; TinyStories repeats little within 200 tokens, so it proposes rarely and gains little.

At 10-40 MB/s from flash a plain pass reads 3.63 MB in 91-363 ms; with k = 4 a token costs 28-113 ms of reading (int4: 1.91 MB, 15-61 ms), and the pass computes 5 positions (17.9 M multiply-adds) instead of 1.

## Files

- `vectors/spec-k{2,4,6}.json` - prompt "Lily and Tom went to the park", 120 tokens: the prompt tokens, the greedy tokens, and for the draft model and prompt lookup every pass's draft, the target's tokens at each of its positions and the accepted count.
- `acceptance.json`, `acceptance-int4.json`, `loss-256.json`, `samples.txt`.

The model slices are not in the repository: the llama2.c float checkpoint (14.3 MB) rebuilds every one of them. `spec.py blobs` writes the whole-model slices that `apps/llm-speculative` reads from flash (`--target-bits 8` for the int8 slice of 3,797,056 B):

```sh
python models/lm/train.py --mode ar --out DIR --dim 256 --hidden 640 --layers 5 --heads 8 --kv-heads 4 \
  --seq-len 256 --batch 64 --lr 1.5e-3 --min-lr 1.5e-4 --warmup 500 --steps 21000 --minutes 85 --bf16 --compile
python models/lm/evaluate.py export --ckpt DIR/ckpt.pt --out target-lm.bin
python apps/llm-pipeline/tools/llm.py build --model target-lm.bin --tokenizer tok512.bin --split 0-4 --bits 8
python apps/llm-pipeline/tools/spec.py run --target target-lm.bin --k 4 --prompt "Once upon a time"
python apps/llm-pipeline/tools/spec.py blobs --target target-lm.bin --target-bits 4 --out build/blobs
```

## Running it on a board

[`apps/llm-speculative`](../../apps/llm-speculative/README.md) runs it on the control plane with the int4 layers, which fit the board's 3.25 MB of flash blob space (the int8 slice does not).

Each position of a pass is computed exactly as `llm-shard` computes one position (`apps/llm-pipeline/shard.c`): a pass over k + 1 positions reads each layer's matrices once and applies them to every position, and attention at a position covers the cache up to it, including the drafts before it in the same pass. Thus the target's token at each position is the one `spec.py` reports, and rejected positions are overwritten by the next pass.
