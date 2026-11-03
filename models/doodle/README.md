# Doodle denoiser

Class-conditional diffusion on Google Quick, Draw! 28x28 bitmaps of six words that Speech Commands also has: cat, dog, bird, house, tree, bed. A DDIM sampler draws one doodle in 8 or 16 steps from a seed. Two sizes: `doodle-s.bin` for the Cardputer, `doodle-l.bin` for a board with PSRAM.

| | `doodle-s.bin` | `doodle-l.bin` |
| --- | ---: | ---: |
| Channels at 14x14, 7x7; 7x7 blocks | 20, 32; 1 | 48, 96; 2 |
| Weights (int8) | 29,376 | 293,760 |
| Blob (weights, scales, step and class tables for 8 and 16 steps) | 35,596 B | 313,108 B |
| Multiply-adds per network pass | 3.56 M | 27.1 M |
| Per image, 16 steps with guidance (2 passes a step) | 114 M | 868 M |
| Maps of `doodle_infer.c` (image, two guidance outputs included) | 21.2 KB | 38.4 KB |

## Quality

A classifier trained on the real drawings (91.8 % on held-out real drawings) labels 32 samples of each class from the int8 reference; class accuracy is the share labeled as the class they were drawn for (`judge-16.json`, `judge-8.json`):

| | 16 steps | 8 steps | weakest class |
| --- | ---: | ---: | --- |
| `doodle-s.bin` | 72.9 % | 79.2 % | dog (0.38) |
| `doodle-l.bin` | 96.4 % | 97.9 % | dog (0.91) |

Sample grids from the int8 reference, each column one seed (guidance 2): `samples/doodle-l-16steps.png`, `samples/doodle-l-8steps.png`, `samples/doodle-s-16steps.png`, `samples/doodle-s-8steps.png`.

Training: 60,000 drawings a class (the first ones of each Quick, Draw! file; the last 2,000 held out), horizontal flips, v-prediction with alpha = cos(pi t / 2), sigma = sin(pi t / 2), the class dropped 10 % of the time for classifier-free guidance, AdamW with an EMA of the weights (0.999), then the input scale of each convolution from the 99.99th percentile over sampler runs and 4,000 steps of quantization-aware training. On an M3 Pro GPU: small 16 minutes (40,000 steps of 256), large 27 minutes (40,000 steps).

## Network and sampler

The image goes in as a 14x14 map of 4 channels (each 2x2 block), through seven or eight 3x3 convolutions (`in`, `e1`, stride-2 `down`, `mid` blocks at 7x7, `up` from the 7x7 map repeated 2x2 plus `e1`'s output, `d1` plus `in`'s output, `out`), and comes back as v at 28x28. Each convolution is one `k32_matvec_i8` per output pixel over its int8 patch at a fixed input scale, then the bias, a per-channel bias for the step and the class (int8 tables), the skip, and SiLU with `k32_expf`. The step's alpha and sigma are tables in the blob, the starting noise twelve xorshift32 uniforms per pixel. `doodle.py` documents the blob layout and every operation's order.

## Files

- `doodle-s.bin`, `doodle-l.bin` - the models.
- `vectors/doodle-{s,l}.json` - one sample per class (16 steps, guidance 2, seeds 1-6): the final image as float bits and u8 pixels, a hash of the image after every step, and one network pass from the first noise with its input and output. `vectors/doodle-{s,l}-samples.bin` hold the same images for `doodle_test.c`.
- `doodle_infer.c` - the sampler for a pod (build the small model with `-DDOODLE_C0_MAX=20 -DDOODLE_C1_MAX=32`); `make -C models check` compares its images with the reference's. `doodle_on_step`, when set, receives the starting noise and the image after every step.

```sh
python models/doodle/fetch.py
python models/doodle/train.py --out DIR --c0 20 --c1 32 --mids 1 --steps 40000 --qat-steps 4000   # large: --c0 48 --c1 96 --mids 2
python models/doodle/doodle.py export --ckpt DIR/ckpt.pt --out doodle-s.bin
python models/doodle/doodle.py sample --model doodle-s.bin --class house --steps 8 --seed 7
python models/doodle/judge.py --model doodle-s.bin --steps 16
```
