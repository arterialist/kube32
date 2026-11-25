# lockstep

lockstep is one lane of a computation that several pods run in lockstep on different CPUs, and that the [voter](../voter/README.md) compares bit for bit. Each lane integrates the same double pendulum and reports the bits of its state every `CHECK_EVERY` steps. Lanes on an ESP32-S3 (Xtensa), an ESP32-S31 (RISC-V) and `kube32-sim` on arm64 or x86_64 compute the same bits at every step, so the voter compares them for equality, with no tolerance, and a lane that differs in one bit is outvoted.

```sh
make -C apps/lockstep           # writes images/sd/lockstep.wasm, images/sd/lockstep-injector.wasm and the manifests in build/
kubectl --context kube32 apply -f apps/voter/build/voter.yaml
kubectl --context kube32 apply -f apps/lockstep/build/lockstep.yaml
```

`build/lockstep.yaml` is `examples/lockstep.yaml` with the module inline in place of `"@lockstep.wasm"`: three Pods, `lockstep-a`, `lockstep-b` and `lockstep-c`, each with a Service of its name. Each lane selects its node by `kubernetes.io/arch`: `xtensa`, `riscv32` and `arm64`. Change the selectors to place the lanes on other nodes.

## The computation

Two unit masses on unit rods under g = 9.81, from theta1 = 2.0 and theta2 = 2.5 at rest, integrated with fourth-order Runge-Kutta at h = 0.001 s. A step is 1 ms of pendulum time. The motion is chaotic: a difference in the last bit of an angle grows until the trajectories part.

`pendulum.h` holds the whole computation. Every value is an IEEE 754 binary64 and every operation is one of `+ - * /`, a negation, a comparison or a conversion between `double` and `int`. IEEE 754 rounds each of these operations to one result, and WebAssembly runs each one alone: it has no fused multiply-add outside SIMD, and the interpreter executes the module's operations in the module's order. The C library's `sin` and `cos` differ from one library to the next, so the lane has its own: the fdlibm polynomial kernels on [-pi/4, pi/4] after a Cody-Waite reduction by pi/2 in three parts.

The build checks the module: `float_ops.py` lists the float instructions of each function and fails on any outside `f64` add, sub, mul, div, neg, abs, the comparisons, constants, loads, stores and the `i32` and `i64` conversions, on SIMD and on saturating conversions. The module is built with `-mcpu=mvp`.

On an ESP32-S3, an ESP32-S31, `kube32-sim` on Linux on arm64 and `kube32-sim` on an arm64 Mac, the state is the same at each of 35 checkpoints over 35,000 steps. The same equations compiled natively, with the C library's `sin` and `cos`, differ from step 2000 between toolchains ([Native runs](../voter/README.md#native-runs)).

## Lockstep

The lane advances by step number, not by time. Each tick (`kube32.io/tick-interval: 100ms`) runs up to `STEPS_PER_TICK` steps and never passes the target step that the voter answered last. A lane far behind its target (more than four ticks of steps) ticks again after 10 ms until it is close. A lane the vote has passed, one whose last checkpoint the voter holds is older than the voted step, ticks again after 10 ms until it reaches its target: the lanes in the vote trail the target by up to the voter's `LEAD`, so a lane that ran at their pace from four ticks short of the target would stay behind them.

Every `CHECK_EVERY` steps it records a checkpoint: the step and the four `f64` values of the state (theta1, theta2, omega1, omega2) as their bits. It keeps the last 32 and posts those the voter does not hold yet to `VOTER_URL`, eight at most a report, every `REPORT_MS` while it has new checkpoints or waits for its target, and every 2 s otherwise:

```
lane b
pod lockstep-b
node esp32-s31-node
inc 5c0e93a1
from 0
step 12345
us 820
fault 12301 40 3120
c 12300 c00760526fcca8bb 4008c3093858df0f 3ff9b5c3c2d15a0e bfe1d0b8f7f5c3a1
```

`inc` names the lane's incarnation and `from` the step it began at; `us` is the time a step takes on this node in microseconds, averaged; `fault` is the step, the bit and the age in milliseconds of the last fault injection, for 120 s after it. The voter answers `target <step>`, `ack <step>` (the last checkpoint it holds of this incarnation), `vote <step>` (the last voted step, after the first vote) and `fault <bit>` when someone asked it for one. A report that fails is sent again with the next one; the lane logs the first failure and every tenth after it.

## Fault injection

`GET /fault?bit=<n>` flips bit `n` (0 to 255) of the state's 32 bytes as Wasm stores them: bits 0 to 63 are theta1 (63 its sign, 52 to 62 its exponent), 64 to 127 theta2, 128 to 255 the angular velocities. Without `?bit=` it flips `FAULT_BIT`. The lane logs the flip and answers its status with `hash_before`. The voter injects through the same path when it answers a report with `fault <bit>`.

A flip that makes the state huge or NaN does not trap: the angle reduction takes such an angle as 0, and the lane runs on and is outvoted.

## Copies and restarts

The state lives in linear memory and globals, so the lane moves, and takes the state of another lane, like any request pod ([Copies](../../docs/migration.md#copies)). A Copy checkpoint brings another lane's memory, so `k32_restored` reads the lane's name, pod and node from the environment again and starts a new incarnation from the step it restored at. The voter compares it from there. A lane whose pod starts again begins at step 0 with a new incarnation, and runs to its target as fast as its ticks allow.

## Endpoints

| Path | Answer |
| --- | --- |
| `/fault[?bit=<n>]` | Flips the bit; the status with `hash_before` |
| any other | JSON: `lane`, `pod`, `node`, `step`, `target`, `hash` (FNV-1a of the state's bytes), `inc`, `from`, `us_per_step`, `reports`, `report_errors`, `last_rc`, `fault_step` and `fault_bit` after an injection, `theta1_bits` |

```sh
curl -s -H 'X-Kube32-Pod: default/lockstep-b' http://<node-ip>:7000/status
```

## Environment

| Variable | Meaning |
| --- | --- |
| `LANE` | The lane's name, as the voter lists it (`a`, `b`, `c`) |
| `POD_NAME`, `NODE_NAME` | From `fieldRef: metadata.name` and `spec.nodeName` |
| `VOTER_URL` | Where reports go, default `http://voter/report` |
| `STEPS_PER_TICK` | Steps a tick runs at most, `1` to `1000` (default `50`) |
| `CHECK_EVERY` | Steps between two checkpoints, `10` to `10000` (default `100`, 0.1 s of pendulum time); the voter's must match |
| `REPORT_MS` | Pod time between two reports, `50` to `10000` (default `400`) |
| `FAULT_BIT` | The bit `/fault` flips without `?bit=` (default `40`) |

## Speed and memory

A step is 2,204 wasm3 operations (counted with the interpreter built as the boards build it), about 600 of them `f64` arithmetic. The ESP32-S3 and ESP32-S31 have no double-precision FPU, so these run in software there. At the voter's default of 250 steps a second, a lane runs 25 steps a tick; `STEPS_PER_TICK` of 50 leaves it room to catch up. The lane reports its time a step, and the voter shows it for each lane.

The module is 7.3 KB. The pod's memory limit is 16 KiB: a 2 KiB stack, 5 KiB of data, mostly the 32 checkpoints and the report buffer. The build checks the layout after linking. It needs only `SERVICE_CALL`, no hardware, so it moves to any node and fits the pod memory of a board without PSRAM.

## lockstep-injector

`injector.c` builds `images/sd/lockstep-injector.wasm`, a fault button for one lane. `examples/lockstep-injector.yaml` runs it on an ESP32-S31-Function-CoreBoard-1 (the `s31-coreboard` profile) beside lane b:

```sh
kubectl --context kube32 apply -f apps/lockstep/build/lockstep-injector.yaml
```

A press of the boot button (GPIO 61, low when pressed, read every 50 ms and taken on two reads in a row) calls `FAULT_URL`, the lane's `GET /fault`. Every `POLL_MS` the pod asks the voter `GET /lane?name=<LANE>` and sets the board's RGB LED:

| LED | The voter's answer |
| --- | --- |
| Green | `agree`: the lane is in the majority |
| Red | `outvoted` |
| Amber | any other: the lane joins, catches up, has no majority to compare with, or is lost |
| Blue | no answer from the voter |

`GET /press` on the pod presses the button from the network, and `GET /status` answers `lane`, `status`, `presses`, `last_rc` and `button`.

| Variable | Meaning |
| --- | --- |
| `LANE` | The lane (default `b`) |
| `FAULT_URL` | Default `http://lockstep-<LANE>/fault` |
| `STATUS_URL` | Default `http://voter/lane?name=<LANE>` |
| `FAULT_BIT` | The bit to flip; without it the lane's own `FAULT_BIT` |
| `BUTTON_PIN` | The button's GPIO (default `61`), `none` for no button |
| `ACTIVE` | `low` (default) or `high`, the level of a pressed button |
| `LED` | `on` (default) or `off`: without the LED the pod needs no LED grant and asks the voter nothing |
| `POLL_MS` | Time between two status reads, `200` to `10000` (default `500`) |

It requests `esp32.io/gpio-input-61` and `hardware.kube32.io/led` and runs in 8 KiB. On a board whose button is on another pin, set `BUTTON_PIN` and request `esp32.io/gpio-input-<pin>`.
