# rfid

Reads ISO 14443-A tags through an MFRC522-compatible I2C reader, such as the M5Stack RFID 2 unit (address `0x28`). Written in C with `k32_init`, `k32_tick`, and `k32_handle` exports, and built with clang and rust-lld:

```sh
make -C apps/rfid           # writes images/rfid.wasm (embedded as wasm://embedded/rfid)
kubectl apply -f examples/rfid.yaml
kubectl logs -l app=rfid -f
```

| Variable | Meaning |
| --- | --- |
| `I2C_BUS` | Reader bus number, `0` or `1` (default `0`). The pod must claim the bus with `esp32.io/i2c-bus-<N>`. |
| `I2C_ADDR` | Reader address in hex (default `28`). |
| `ACTION_TOPIC` | Channel topic for tag action messages (default `ir.send`). |
| `TAG_<UID>` | Message published when the tag with this UID (uppercase hex) enters the field. |
| `TAG_DEFAULT` | Message published for any tag without a matching `TAG_<UID>` variable. |

Each tag that enters the field is also published to `rfid.tag` as a lowercase hex UID. `GET` returns `{"reader","address","version","present","seen","polls","empty","errors","bus_errors","last_bus_error","recent"}`. Linear memory is capped at 4 KiB (a 2 KiB stack and 1.7 KiB of data, checked after linking).
