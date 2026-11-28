# voter

voter runs triple modular redundancy across CPU architectures. Three [lockstep](../lockstep/README.md) lanes integrate one double pendulum on three nodes, an ESP32-S3, an ESP32-S31 and an arm64 Linux board, and report the bits of their state every 100 steps. The voter compares the lanes at each of these checkpoints, bit for bit. Two lanes that agree outvote the third, and the vote goes on with the two. A repair copies the state of a healthy lane into the outvoted one, which then agrees again.

```sh
make -C apps/lockstep && make -C apps/voter
kubectl --context kube32 apply -f apps/voter/build/voter.yaml
kubectl --context kube32 apply -f apps/lockstep/build/lockstep.yaml
kubectl --context kube32 apply -f apps/lockstep/build/lockstep-injector.yaml   # optional: the button on the ESP32-S31
kubectl --context kube32 logs -f voter
```

`build/voter.yaml` is `examples/voter.yaml` with the module inline: a Pod on the control plane node (`node-role.kubernetes.io/control-plane`), the Service `voter` that the lanes report to, and an Ingress on the path `/voter`.

## Watching it

The page is at `http://<control-plane-ip>/voter/`, or on port 8088 of a Linux board whose node serves its Ingress there (`http://<control-plane-ip>:8088/voter/`). It shows:

- the pendulum of the voted state, white, with each lane's arms under it in the lane's colour at the same step. Lanes that agree draw over each other as one arm; an outvoted lane's arms peel off. A trail follows the lower bob.
- a row a lane: status, node, architecture and OS (from the node's `kubernetes.io/arch` and `kubernetes.io/os` labels), the lane's step, the hash of its state at the voted step (red when it differs from the vote), its time a step, and buttons to inject a fault and to repair it.
- for each outvoted lane, the injected bit and step, the checkpoint that outvoted it and the latency in steps and milliseconds; for each repair, the time until the lane agreed again.
- the voter's last log lines.
- the native panel ([Native runs](#native-runs)).

On a node with a screen, the voter draws the same at the screen's size (`k32_display_info`): the pendulum on the left, and on the right `VOTE 3/3` (green), `VOTE 2/3` (amber) or `NO MAJORITY` (red), the voted step and its pendulum time, the target and the rate, then a block a lane: its colour, name, status and time a step; its node; its architecture, step and hash at the voted step. The last detection and the last repair are at the bottom. It draws into an 8-bit frame and sends it with `k32_display_blit_pal` every 100 ms; the arms move from one voted checkpoint to the next over the time the vote takes to advance.

A flipped bit is outvoted at the next checkpoint whatever its weight. How soon an outvoted lane's arms part from the others depends on the bit: the example flips bit 48 of theta1, a change of about 0.1 rad that shows at once; bit 40 is a change of a few ten-thousandths of a radian, which the motion takes seconds of pendulum time to grow into a visible gap.

Keys, on a node with a keyboard: a lane's name, or `1` to `4` for the lanes in order, injects a fault into that lane (`FAULT_BIT`); `r` repairs every outvoted or lagging lane; Esc adds one to `leave` in `/state`, a request to give the screen back for whatever runs the voter to act on, such as a launcher entry that redeploys it without the screen.

## The vote

The voter keeps the last 64 checkpoints of each lane and the last 128 voted states. A lane is in the vote while it reports (a report in the last `LOST_MS`), is not outvoted, and its checkpoints reach the last voted step. At the next checkpoint step the voter waits until every lane in the vote has reported it, then groups the lanes by the bits of their state:

| Result | When | Then |
| --- | --- | --- |
| Majority | One group holds more than half of the lanes that voted, and at least two | Its state is the voted state. A lane outside it is outvoted at this step. |
| No majority | Two lanes that differ, or three that all differ | The step has no voted state and the lanes show `split`. Detection without a majority: no lane is outvoted. |

The first vote is at the latest first checkpoint of the lanes. Before it, the voter waits up to `START_MS` for every lane in `LANES`.

An outvoted lane stays outvoted until it starts a new incarnation. The log names the lane, its node and architecture, the step, its hash and the majority's, and the cause:

```
voter: lane b on node s31 (riscv32) outvoted at step 4600: hash 5e21c0d4, majority 9a8f3c12; bit 40 flipped at step 4521, detected 79 steps and 412 ms later
voter: lane b on node s31 (riscv32) outvoted at step 13600: hash 0c44e1b7, majority 77d2a90e; no fault injected, 1100 steps after it began at step 12500
```

The second form is a lane that diverged with no fault injected: the step it began at says whether it started at step 0 or restored from a copy, and how far it ran before it differed.

A checkpoint is 100 steps, 0.1 s of pendulum time. A bit flipped at step s is outvoted at the next checkpoint step, at most 100 steps later. The latency in milliseconds runs from the flip, as the lane reports its age, to the vote at that checkpoint.

### Lockstep

The voter answers every report with the target step and, after the first vote, the last voted step. Once voting starts, the target grows at `RATE` steps a second but stays within `LEAD` steps of the slowest lane in the vote; it holds while no lane is in the vote. A lane slower than `RATE` slows the vote down rather than falling behind it. At the defaults, 250 steps a second and a lead of 400 steps, the lanes run at a quarter of the pendulum's own time, a checkpoint every 0.4 s, within 1.6 s of each other.

### Lost lanes

A lane that has not reported for `LOST_MS` (8 s) shows `lost` and leaves the vote; the other two vote on, two of three, and the target follows the slower of them. A move or a Wi-Fi stall of a few seconds keeps the vote waiting, not the lane out of it.

A lane that reports again, or a new incarnation of it, is `behind` while its checkpoints are older than the voted step. It learns so from the voted step in the answer and runs at the full speed of its ticks until it reaches its target, which leads the lanes in the vote by up to `LEAD` steps. Each checkpoint the vote has passed is compared with the voted state of its step, while the voter still holds it: a match counts as agreement, a difference outvotes the lane. Once it reaches the voted step, it is in the vote.

A lane whose pod starts again begins at step 0 and catches up at the speed of its node, since the computation repeats exactly. A repair brings it to the vote at once.

## Repair

A repair is a [Copy checkpoint](../../docs/migration.md#copies): the state of a lane in the majority, snapshot without stopping it, restores into the outvoted or lagging lane's pod on that pod's own node. `k32_restored` gives the lane a new incarnation from the copied step, and the voter compares it from there. The lane goes on from the healthy lane's state, catches up to the target and agrees again at its next checkpoint.

```sh
curl -s 'http://<control-plane-ip>:8088/voter/repair?lane=b'                 # from the lane of the majority furthest ahead
curl -s 'http://<control-plane-ip>:8088/voter/repair?lane=b&from=a&to=<node>' # from lane a, restored on <node>
kube32 copy lockstep-a --into lockstep-b                                      # the same, by hand
```

The voter creates the Checkpoint through the API server with `k32_http_call`, at `API_URL`: a `POST` of `kube32.io/v1` Checkpoint `<pod>-<5 hex digits>` with the label `app=voter`. On a cluster whose API server has a token, set `API_TOKEN`. `to` must be a node that the lane's `nodeSelector` admits.

`AUTO_REPAIR_S` repairs on its own a lane that has been outvoted or behind for that many seconds, at most once every 15 s while repairs fail. The example sets 20 s: long enough to see the fault, and a lane whose node comes back after a loss joins the vote without a hand on it.

The voter reports a repair's time from the request to the lane's first agreeing checkpoint. On `kube32-sim` workers on one host, that is under a second (`tests/integration/cluster/test_lockstep_vote.py` prints it).

A lane that comes back after its node was lost is repaired by a copy, not by a periodic checkpoint of its own. A copy restores the current state of a healthy lane in one snapshot, on any architecture, and the lane is at the vote's step at once. A periodic checkpoint would cost a snapshot of every lane every period, and restore a state as old as the period, which the lane must still compute up to the target.

## Endpoints

| Path | Answer |
| --- | --- |
| `POST /report` | A lane's report ([Lockstep](../lockstep/README.md#lockstep)) |
| `/state` | JSON: `step` (the last voted step), `voted`, `bits` and `hash` of the voted state, `target`, `rate`, `every`, `lead`, `started`, `votes`, `unanimous` (votes with every lane identical), `leave` (Esc presses), `majority` and `voters` of the last vote, `lanes`, `log` |
| `/lane?name=<lane>` | One word: `agree`, `outvoted`, `joining`, `behind`, `split`, `lost` or `absent` |
| `/fault?lane=<lane>[&bit=<n>]` | Fault injection: the lane flips the bit at its next report. `202` |
| `/repair?lane=<lane>[&from=<lane>][&to=<node>]` | A Copy checkpoint into the lane; `201` with `from` and `checkpoint`, or the error. Without `lane`, every outvoted or lagging lane |
| `/native.js` | The recorded native runs |
| any other | The page |

Each lane of `/state` has `name`, `pod`, `node`, `arch`, `os`, `status`, `inc`, `from`, `step`, `latest` (its last checkpoint), `us` (microseconds a step), `age_ms` (since its last report), `at` and `hash` (its state at the voted step), `out_step`, `det` (`injected`, `step`, `from`, and for an injected fault `fault_step`, `bit`, `steps`, `ms`), `repairing`, `repairs`, `repair_ms` and `repair_from`.

## Environment

| Variable | Meaning |
| --- | --- |
| `LANES` | The lanes, comma-separated (default `a,b,c`). Others that report join while there is room for four. |
| `CHECK_EVERY` | The lanes' `CHECK_EVERY` (default `100`) |
| `RATE` | Target steps a second, `1` to `100000` (default `250`) |
| `LEAD` | Steps the target runs ahead of the slowest lane in the vote (default 4 x `CHECK_EVERY`) |
| `LOST_MS` | Silence after which a lane leaves the vote (default `8000`) |
| `START_MS` | The longest wait for every lane before the first vote (default `10000`) |
| `AUTO_REPAIR_S` | Repair a lane outvoted or behind this long; `0`, the default, never |
| `FAULT_BIT` | The bit keys and `/fault` flip without `bit` (default `40`) |
| `API_URL`, `API_TOKEN` | The API server for repairs and node labels. The manifest sets `http://$(KUBERNETES_SERVICE_HOST):$(KUBERNETES_SERVICE_PORT)`. |
| `POD_NAMESPACE` | The namespace of the lanes and the Checkpoints (default `default`) |
| `SCREEN`, `KEYS` | `off`: no drawing, no key reads |

The pod requests `hardware.kube32.io/tft-display` and `hardware.kube32.io/keyboard` with the capabilities `DISPLAY`, `KEYBOARD` and `SERVICE_CALL`. On a node without a screen, drop the two requests and set `SCREEN` and `KEYS` to `off`.

## Native runs

The native panel replays recorded runs of `native/chaos.c`: the same equations, step and start as the lanes, in C with the C library's `sin` and `cos`, built natively with four toolchains:

| Run | Toolchain and C library |
| --- | --- |
| 1 | Apple clang 21 on macOS, arm64 |
| 2 | gcc 14 with musl, Linux arm64 |
| 3 | gcc 14 with musl, Linux x86_64 |
| 4 | gcc 13 with glibc 2.39, Linux arm64 |

The runs start from the same state. The bits of theta1 differ between them from step 2000; their angles differ by more than 0.01 rad from step 18,240 and by more than 0.1 rad from step 19,560, 19.6 s of pendulum time. The panel lists the bits of theta1 every 2000 steps with the digits that differ from run 1 in red, and draws the four pendulums at the step of the vote, each with its trail.

`native/runs.sh` builds and runs `chaos.c` with each toolchain (clang on an arm64 Mac, Docker with arm64 and amd64 emulation for the Linux ones) and `native/pack.py` packs the output into `native_runs.h`: theta1 and theta2 every 40 steps as 16-bit fractions of a turn, base64 per run, and the bits of theta1 every 2000 steps. These runs are data, recorded once; nothing in the voter computes them.

## Memory

The module is 58 KB: the page, the native runs (24 KB) and the code. The pod's memory limit is 192 KiB: an 8 KiB stack and 141 KiB of data, mostly the 75 KiB screen frame (up to 320 x 240 pixels), the lanes' checkpoints and the voted history. The build checks the layout after linking. The voter holds the display and keyboard grants, so it stays on its node.
