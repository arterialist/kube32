# wordsketch

wordsketch turns a word spoken to the Cardputer into a doodle and a short story. The Cardputer hears the word with a keyword spotter, the control plane draws it with a diffusion model and writes a story with stories260K, and the Cardputer's screen shows the drawing forming from noise next to the story as it is written.

| Pod | Node | What it runs | Memory |
| --- | --- | --- | ---: |
| `wordsketch-listen` | Cardputer (`kube32.io/board: cardputer`) | microphone, keyword spotter, screen, chime | 20 KiB + 56 KiB overhead |
| `wordsketch-draw` | a node with PSRAM (`esp32.io/psram`), own thread | doodle diffusion, 8 DDIM steps | 64 KiB + 33 KiB overhead |
| `wordsketch-story` | a node with PSRAM | stories260K, five layers (`llm-shard`) | 384 KiB + 40 KiB overhead |
| `wordsketch-teller` | a node with PSRAM | tokenizer, generation loop (`llm-coordinator`) | 64 KiB + 58 KiB overhead |
| `wordsketch-store` | a node with PSRAM | the story's blobs over HTTP | 8 KiB + 6 KiB overhead |

The words are the six the two models share: cat, dog, bird, house, tree, bed.

## How it works

1. **Listening.** `k32_tick` reads the microphone every 10 ms. Every 20 ms of audio becomes a frame of 40 log-mel values with the keyword spotter's integer front end (`models/kws/frontend.c`, its 512-point FFT in `k32_fft_q15`), quantized at once to the network's int8 input; the last 49 frames are one second. A frame costs the interpreter about 30,000 operations, 5 ms at the Cardputer's 6.3 M operations a second. A frame's mean log energy against the room's floor marks where a word starts and ends. When the word sits in the middle of the window, or a quarter second after its end, the network (`models/kws/kws_infer.c`) classifies the window: 1.66 M multiply-adds in `k32_matvec_i8` and 711,000 interpreter operations, about 115 ms on the Cardputer.
2. **A keyword.** With at least `THRESHOLD` percent, the pod stops the microphone, publishes `W <id> <class> <seed> <steps>` on the topic `wordsketch.word`, posts `Once upon a time there was a <word>.` to the teller, and plays a two-note chime. The speaker and the microphone share the I2S clock pin (GPIO 43), so they take turns.
3. **The drawing.** `wordsketch-draw` is an entry-point pod: it runs on its own thread, beside the request pods of its node, and waits on the channel `wordsketch.word`. It samples the class with `models/doodle/doodle_infer.c` (`doodle-l.bin`, 27.1 M multiply-adds a pass, two passes a step for classifier-free guidance; 77 M interpreter operations for 8 steps). The starting noise and the image after every step go out on `wordsketch.frame` as four events of 7 rows, each pixel one of 64 levels as a character, 200 bytes an event.
4. **The story.** `wordsketch-teller` encodes the prompt and feeds it through `wordsketch-story`, which holds all five layers of stories260K, and publishes every token on `wordsketch.text` (llm-pipeline's coordinator and shard, configured by environment).
5. **The screen.** `wordsketch-listen` subscribes to both topics. It draws the picture at 4x (112 x 112 pixels) on the left, one rectangle per run of pixels whose level changed, and the story on the right, 20 columns wrapped at words, the prompt in grey. When the drawing and the story are done it logs the word's timing and listens again.

### Memory on the Cardputer

The Cardputer admits pods on its 80 KiB pool block. `wordsketch-listen` takes 20 KiB of linear memory and 56 KiB of overhead (wasm3's compiled code and structures, and the 12.3 KB module it holds), 76 KiB in all:

| Part | Bytes |
| --- | ---: |
| Network maps: the first convolution's output (also the FFT's buffer while listening), the second's, a patch | 8,192 |
| The last second of features, int8 | 1,960 |
| Front end tables, scales and biases of the model | 4,720 |
| Audio (480 samples), the picture as drawn (784 levels), buffers, data | 4,264 |
| Shadow stack | 1,024 |
| Linear memory | 20,160 (limit 20 KiB) |

The 25,280 weights of the spotter stay in the blob `wordsketch-kws` in flash, and `k32_matvec_i8` reads them in place. The draw pod does the same with the 293,760 weights of `doodle-l.bin`: its memory holds the maps, the scales and biases and the step tables.

The listen pod needs firmware with `k32_fft_q15`.

## Build

```sh
make -C apps/wordsketch
```

This builds the three modules with clang, llm-pipeline's shard and coordinator, the blobs in `build/blobs/` (the spotter and the doodle model from `models/`, the story model and tokenizer with `llm.py build --split 0-4 --bits 8`, which downloads stories260K once) and `build/wordsketch.yaml`, the manifest with the modules inline.

## Run

```sh
make -C apps/wordsketch upload SERVER=http://<control-plane-ip>:6443
kubectl --context kube32 apply -f apps/wordsketch/build/wordsketch.yaml
kubectl --context kube32 get pods -o wide -l app=wordsketch
```

`upload` puts `wordsketch-kws` on the Cardputer and the other three blobs on the control plane, through the API server's node proxy. Say one of the six words to the Cardputer. The listen pod logs every classification (`heard <class> <percent> <second class> <percent> word_ms <length> end_to_result_ms <ms> network_ms <ms>`) and, once the drawing and the story are done, the word's timing on the Cardputer's clock from the word's last loud frame: the result, the noise, the first step, the last step (with the frames that arrived), the story's first and last token and the teller's tokens/s:

```sh
kubectl --context kube32 logs deploy/wordsketch-listen
kubectl --context kube32 logs deploy/wordsketch-draw      # per drawing: steps, time, slowest step, image hash
```

The Ingress `wordsketch` routes `POST /wordsketch/word?w=<word>[&seed=<n>]` to the listen pod, which starts the drawing and the story as if it had heard the word, and serves the story's tokens as Server-Sent Events at `/wordsketch/events`. The draw pod logs each drawing's time and a hash of the image; with `seed` 1 to 6 for cat to bed the hash is the one of `models/doodle/vectors/doodle-l.json`'s samples.

| Listen variable | Meaning |
| --- | --- |
| `THRESHOLD` | Percent a keyword needs (60) |
| `STEPS` | DDIM steps of the drawing, 8 or 16 (8) |
| `STORY_STEPS` | Tokens of the story (80) |
| `VOLUME` | Chime volume, percent of the speaker's ceiling (30; 0 for none) |
| `VAD` | A word's mean log2 energy above the room's floor, Q8 (512) |
| `STORY_URL` | The teller (`http://wordsketch-teller/generate`) |

## Test on the simulator

`kube32-sim --sim-mic <file>` plays a 16 kHz mono recording to `k32_mic_read` in real time. Run a control plane and a Cardputer-like worker (`--psram 0` models the 80 KiB pool):

```sh
kube32-sim --role standalone --workers '' --port 16443 --pod-port 17000 --node-id 1 --peer cardputer:2:7402
kube32-sim --role worker --join http://127.0.0.1:16443 --node-name cardputer --board cardputer --psram 0 \
  --pod-port 17001 --node-id 2 --peer esp32-node-01:1:7401 --sim-mic words.wav
```
