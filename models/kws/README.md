# Keyword spotter

Recognizes six words of Google Speech Commands v2 (cat, dog, bird, house, tree, bed), plus `_unknown_` (any other word) and `_silence_`, from one second of 16 kHz audio. The front end uses integer operations only and the network the node's int8 products, so a pod on the Cardputer computes the reference's features and logits bit for bit.

| | |
| --- | ---: |
| Input | 16,000 samples, int16, 16 kHz |
| Features | 49 frames x 40 log-mel bands (30 ms windows every 20 ms, 512-point integer FFT) |
| Network | conv 10x4 stride (2, 4) -> 24 ch, conv 3x3 stride 2 -> 32, conv 3x3 -> 48, conv 1x1 -> 64, mean, linear -> 8 |
| Weights | 25,280 (int8) |
| Blob (`kws.bin`, with the front end tables) | 30,000 B |
| Multiply-adds per second of audio | 1.66 M, plus 49 FFTs |
| Maps of `kws_infer.c` | int8 input 1,960 B, maps of 5,760 B and 1,920 B (`-DKWS_MAP_MAX=5760 -DKWS_MAP_B_MAX=1920`) |

## Accuracy

Speech Commands' test split (`testing_list.txt`): every test clip of the six words, as many unknown words as an average keyword has clips, and as many silence clips (background noise at a random volume, a tenth of them all zero). Computed by `kws.py test` with the int8 reference, the bits a pod computes: **94.33 %** of 1,586 clips (validation: 95.7 %).

| true \ predicted | cat | dog | bird | house | tree | bed | unknown | silence |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| cat | **185** | 0 | 2 | 1 | 0 | 3 | 2 | 1 |
| dog | 0 | **207** | 1 | 2 | 0 | 2 | 6 | 2 |
| bird | 0 | 1 | **174** | 0 | 1 | 4 | 4 | 1 |
| house | 1 | 0 | 2 | **181** | 0 | 0 | 6 | 1 |
| tree | 4 | 1 | 1 | 1 | **180** | 0 | 6 | 0 |
| bed | 4 | 0 | 4 | 0 | 2 | **192** | 5 | 0 |
| unknown | 0 | 7 | 3 | 2 | 4 | 2 | **180** | 0 |
| silence | 0 | 0 | 0 | 1 | 0 | 0 | 0 | **197** |

Training: the six words' training clips, twice as many unknown words drawn anew for each of 4 augmented copies (shift up to 100 ms, background noise with probability 0.8 at volume up to 0.1), silence clips; 40 epochs with batch norm, time and frequency masks and label smoothing, AdamW one-cycle at 3e-3, 2.1 minutes on an M3 Pro GPU (the features of the copies take about 5 minutes on 2 CPU workers). The batch norms fold into the convolutions; each convolution's input scale is the largest value over 20,000 training clips. Quantization-aware training lowered validation accuracy here, so the model is the folded float model with these scales.

## Front end

`frontend.py` and `frontend.c` (the same integer steps; `frontend_test.c` checks them against each other):

1. frame f: samples 320 f .. 320 f + 479, times a Q15 Hann window, `(s * w + 16384) >> 15`;
2. a 512-point radix-2 FFT in int32 with Q15 twiddles and int64 products rounded `(p + 16384) >> 15`, no scaling between stages;
3. power `(re^2 + im^2) >> 8` of bins 0..256;
4. 40 triangular mel filters (60-7600 Hz, Q10 weights), int64 sums;
5. log2 in Q8: `256 n + frac`, n the position of the leading one and frac the next 8 bits.

`kws_frame()` computes one frame from 480 samples, so a pod can keep a ring of the last 49 frames and classify every few frames. It uses about 6 KB of stack.

## Network

The features minus `offset`, quantized with the first convolution's scale; each convolution is one `k32_matvec_i8` per output pixel over its patch, plus bias, ReLU, and quantization with the next convolution's fixed scale (round half away from zero, clamp to -127..127). The last convolution's output is averaged per channel (sum over pixels in row order, divided by their count), quantized with one scale for the vector, and multiplied by the classifier. `kws.py` documents the blob layout and every operation's order; `kws_infer.c` runs the last two convolutions pixel by pixel into the mean, so their maps are never stored.

## Files

- `kws.bin` - model and front end tables.
- `vectors/` - one test clip of each class: `kws-audio.bin` (8 x 16,000 int16), `kws-features.bin` (8 x 49 x 40 int32), `kws-logits.bin` (8 x 8 float), and `kws.json` with the same values, hashes of every layer's output and the classes.
- `test.json` - accuracy and confusion matrix.
- `kws_infer.c`, `frontend.c` - inference for a pod; `make -C models check` runs them on the vectors. `kws_frame_buf()` computes a frame with its FFT in a buffer the caller gives, such as the network's first map between classifications (`kws_scratch()`), and `kws_quantize()` with `kws_classify_q()` lets a pod keep the last second as int8 frames.

```sh
python models/kws/fetch.py                      # the six words, a quarter of the other speakers, noise
python models/kws/data.py                       # features of 4 augmented copies, validation and test
python models/kws/train.py --out DIR --epochs 40 --qat-epochs 0
python models/kws/kws.py export --ckpt DIR/ckpt.pt --out kws.bin
python models/kws/kws.py test --model kws.bin
python models/kws/kws.py wav --model kws.bin clip.wav
```
