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

Speech Commands' test split (`testing_list.txt`): every test clip of the six words, as many unknown words as an average keyword has clips, and as many silence clips (background noise at a random volume, a tenth of them all zero). Computed by `kws.py test` with the int8 reference, the bits a pod computes: **93.88 %** of 1,586 clips.

| true \ predicted | cat | dog | bird | house | tree | bed | unknown | silence |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| cat | **184** | 0 | 0 | 1 | 0 | 4 | 3 | 2 |
| dog | 0 | **202** | 0 | 0 | 0 | 0 | 16 | 2 |
| bird | 1 | 0 | **174** | 0 | 1 | 4 | 4 | 1 |
| house | 4 | 0 | 0 | **183** | 0 | 0 | 3 | 1 |
| tree | 1 | 1 | 0 | 0 | **179** | 1 | 10 | 1 |
| bed | 1 | 0 | 4 | 0 | 0 | **187** | 14 | 1 |
| unknown | 1 | 3 | 3 | 0 | 3 | 4 | **184** | 0 |
| silence | 0 | 0 | 0 | 0 | 1 | 0 | 1 | **196** |

### Far field

People talk to a node's microphone from 0.3-2 m, so the signal is quiet, reverberant and band-limited, with room noise and other people talking. The listen pod scales the detected word to a peak of 16,000. `farfield.py score --model kws.bin` runs these tests with fixed seeds (`farfield.py` documents the chain):

| Test | Accuracy |
| --- | ---: |
| Test split clips through the chain, peak 300-900 before the pod's gain, noise 20 dB below the speech | 89.8 % |
| the same at 10 dB | 84.5 % |
| the same at 5 dB | 77.0 % |
| Held-out synthesized voices (Daniel, Karen, Samantha; 3 rates, the keywords and 32 other words), as spoken | 84.5 % (keywords 90.7 %) |
| the same through the chain, 20 dB / 10 dB | 79.2 % / 76.6 % (keywords 90.7 % / 77.8 %) |

| False triggers (a keyword is predicted) | Rate |
| --- | ---: |
| 1 talker saying non-keyword words (test split speakers), far field | 3.6 % |
| 3-5 talkers | 2.4 % |
| The same with synthesized voices (words such as bad, bat, hat, dig and fog included) | 11.2 % / 8.6 % |
| Noise alone (pink, white, brown, recorded) | 0.3 % |

### Training

Four augmented copies of the training split, 71,748 clips: the six words' training clips, twice as many unknown words drawn anew for each copy, silence clips, and clips of other people talking (runs of non-keyword words from 1-5 talkers) as `_unknown_`. 35 % of the word clips get a shift up to 100 ms and background noise (probability 0.8, volume up to 0.1) and, for half of them, the pod's gain; 65 % go through the far-field chain: room reverb (RT60 0.12-0.7 s), a speaker and microphone response (high-pass 120-450 Hz, low-pass 3.2-7.2 kHz, random EQ, a resonance), a level with a peak of 150-9,000, noise or babble at 2-30 dB SNR, int16, and the pod's gain (85 % of the clips). About 2 % of the clips are synthesized words of voices other than the three held out. 100 epochs with batch norm, time and frequency masks and label smoothing, AdamW one-cycle at 3e-3, 6.6 minutes on an M3 Pro GPU. The batch norms fold into the convolutions; each convolution's input scale is the largest value over 20,000 training clips. Quantization-aware training lowered validation accuracy here, so the model is the folded float model with these scales.

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
- `farfield.py` - the far-field chain, the training copies and the tests above. `tts.py` - spoken words of the host's voices (macOS `say`).
- `kws_infer.c`, `frontend.c` - inference for a pod; `make -C models check` runs them on the vectors. `kws_frame_buf()` computes a frame with its FFT in a buffer the caller gives, such as the network's first map between classifications (`kws_scratch()`), and `kws_quantize()` with `kws_classify_q()` lets a pod keep the last second as int8 frames.

```sh
python models/kws/fetch.py                      # the six words, a quarter of the other speakers, noise
python models/kws/data.py                       # features of the plain split (validation, test)
python models/kws/tts.py                        # spoken words of the host's voices
python models/kws/farfield.py build             # features of 4 far-field training copies
python models/kws/train.py --far --out DIR --epochs 100 --qat-epochs 0
python models/kws/kws.py export --ckpt DIR/ckpt.pt --out kws.bin
python models/kws/kws.py test --model kws.bin
python models/kws/farfield.py score --model kws.bin
python models/kws/kws.py wav --model kws.bin clip.wav
```
