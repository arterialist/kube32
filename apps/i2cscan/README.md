# i2cscan

Scans an I2C bus and logs every 7-bit address (0x08 to 0x77) where a device
acknowledges. It is a C program with an `app_main` entry point, built with
clang and rust-lld:

```sh
make -C apps/i2cscan        # writes images/i2cscan.wasm (embedded as wasm://embedded/i2cscan)
kubectl apply -f examples/i2cscan.yaml
kubectl logs i2cscan
```

| Variable | Meaning |
| --- | --- |
| `I2C_BUS` | Bus to scan, `0` or `1` (default `0`). The pod must claim it with `esp32.io/i2c-bus-<N>`. |
| `SCAN_EVERY_MS` | Repeat the scan at this interval; unset or `0` scans once and exits. |

Each probe is `esp32_i2c_write(bus, addr, 0, 0)`: `0` when a device answers,
`K32_ERR_NODEV` when none does. The program exits with 1 on any other error,
such as `K32_ERR_PERM` when the bus is not claimed.
