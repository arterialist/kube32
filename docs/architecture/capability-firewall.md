# Capability firewall

A pod reaches hardware and other nodes only through host functions, and the
capability firewall (`src/hal/capability_firewall.cpp`) decides which of
those calls a pod may make. It does two things:

- **At admission.** When the kubelet admits a pod, the firewall turns the
  manifest's resource requests into a grant. It claims any specific pins,
  buses or channels the grant names. If one is reserved or already owned by
  another pod, the pod fails with `CapabilityAdmissionFailed`.
- **On every call.** Each guarded host function checks the calling pod's
  grant. A call without the grant returns `K32_ERR_PERM` (-1), and the node
  logs a `[FIREWALL] denied <pod>: ...` line.

When the pod is removed, its claims are released.

## What a manifest can ask for

The firewall reads `resources.requests`, or `limits` when the container has
no `requests` block.

| Resource | Grants |
| --- | --- |
| `hardware.kube32.io/ipc: "1"` | `k32_event_emit`, `kube32_channel_open` |
| `hardware.kube32.io/tft-display: "1"` | `k32_display_text`, `k32_display_rect`, `k32_display_bar` |
| `hardware.kube32.io/keyboard: "1"` | `k32_key_read` |
| `hardware.kube32.io/speaker: "1"` | `k32_tone` |
| `hardware.kube32.io/sd: "1"` plus a volume claim | `k32_fs_*` in the pod's directory on the card ([microSD card](../sd-card.md)) |
| `esp32.io/gpio-<N>` | pin N, read and write; the value is ignored |
| `esp32.io/gpio: "<count>"` | that many free pins, lowest numbers first |
| `esp32.io/adc1-channel-<0..9>`, or `esp32.io/adc: "<count>"` | ADC1 channels |
| `esp32.io/i2c-bus-<0\|1>`, or `esp32.io/i2c: "<count>"` | I2C buses |
| `esp32.io/uart-<1\|2>` | a UART port |

Every `esp32.io/` key is also accepted with the `esp.io/` prefix.

`securityContext.capabilities.add` can grant the first five by name:

- `CHANNEL_IPC` (or `IPC`);
- `DISPLAY` (or `TFT_DISPLAY`);
- `KEYBOARD`;
- `SPEAKER` (or `AUDIO`);
- `SD` (or `SD_CARD`).

Any other capability name is ignored without a warning. In particular,
`GPIO_WRITE`, `GPIO_READ` and `ADC_READ`, which older versions of these
docs showed, grant nothing.

The Tessera HUD pod, for example (from `examples/tessera.yaml`):

```yaml
resources:
  requests:
    memory: 64Ki
    hardware.kube32.io/tft-display: "1"
    hardware.kube32.io/keyboard: "1"
    hardware.kube32.io/speaker: "1"
    hardware.kube32.io/ipc: "1"
```

## Reserved pins

These pins can never be claimed: 0, 19, 20, 26 to 32, 43 and 44
(`KUBE32_RESERVED_GPIO_MASK` in `include/hal/capability_firewall.hpp`). That
covers the boot strapping pin, native USB, the SPI flash and PSRAM bus, and
UART0. Every other number up to 48 is treated as free.

The list is incomplete for the boards Kube32 runs on:

- **Not reserved:** the other strapping pins (3, 45, 46) and the extra pins an
  octal PSRAM uses (33 to 37).
- **On the Cardputer**, a pod can claim pins that the firmware itself drives:
  - the LCD (33 to 38);
  - I2S (41 to 43; 43 is reserved only because it is also UART0 TX);
  - the key matrix (3 to 9, 11, 13, 15);
  - the microSD bus (12, 14, 39, 40).

No example uses the GPIO, ADC, I2C or UART functions; see
[GPIO and other peripherals](../workloads/gpio-blinker.md) for their state.
