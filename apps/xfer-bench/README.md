# xfer-bench

xfer-bench measures how fast a pod on one node fetches bodies of a few KB from a pod on another node through `k32_http_call`.

| Pod | Node | What it runs | Memory |
| --- | --- | --- | ---: |
| `xfer-server` | control plane | `GET /bytes?n=<bytes>`: an n-byte body, 8 to 16,384 bytes | 16 KiB |
| `xfer-client` | Cardputer (`kube32.io/board: cardputer`) | 200 back-to-back calls a size from `k32_tick`, every body checked | 24 KiB |

A body holds `n / 4 - 1` words of a fixed formula of `n` and ends in their FNV-1a checksum (`xfer.h`). The client checks each body's size, the checksum against the words it received and the checksum against the formula. For each size it logs one line:

```
xfer n=8192 calls=200 ok=200 wall_ms=67152 calls_per_s=2.97 call_ms p50=176 p95=1508 max=2913 min=84 mean=325 over_500ms=22 fail: rc_timeout=0 ... status=0 size=0 sum=0
```

`calls_per_s` divides the calls by the wall time, which includes the checks and the 10 ms between ticks. Failures are counted by kind: a negative `k32_http_call` result by error, a status other than 200, a wrong size, a wrong checksum. The first eight failures of a size are also logged one by one.

## Running it

```
make -C apps/xfer-bench            # modules and build/xfer-bench.yaml (both inline)
make -C apps/xfer-bench apply      # the Service and both pods
kubectl --context kube32 logs -f xfer-client
make -C apps/xfer-bench delete
```

The client reads `URL` (default `http://xfer-server/bytes`), `SIZES` (default `4096,8192,16384`), `CALLS` (at most 200) and `TIMEOUT_MS` (default 3000). Pointing the client's `nodeSelector` at another node measures that node's receive path against the same server.

## Results

Devkit server; three runs of the Cardputer client (two at 16,384 bytes), and one run from the host simulator worker on the same network as a reference receiver. The calls over 500 ms come from the last run of each. Timeouts are calls that had no complete body after 3 s.

| Receiver | Body | Calls/s | p50 ms | p95 ms | Max ms | Calls over 500 ms | Failures |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Cardputer | 4,096 | 3.20 to 3.30 | 68 to 80 | 1,394 to 1,601 | 1,560 to 3,171 | 30 of 200 | none |
| Cardputer | 8,192 | 1.1 to 3.0 | 123 to 344 | 1,508 to 2,944 | 2,913 to 6,758 | 22 of 200 | 0 to 3 timeouts |
| Cardputer | 16,384 | 1.1 to 1.5 | 338 to 339 | 2,142 to 2,511 | 3,005 to 3,153 | 59 of 200 | 2 to 4 timeouts |
| Host worker | 4,096 | 9.78 | 62 | 233 | 1,989 | 4 of 200 | none |
| Host worker | 8,192 | 7.52 | 94 | 278 | 771 | 3 of 200 | none |
| Host worker | 16,384 | 4.53 | 164 | 380 | 2,743 | 7 of 200 | none |

Every body that arrived whole had the right size and checksum. On the Cardputer 11 to 30 percent of the calls take over 500 ms, and the slowest 5 percent take 1.4 to 3 s, one or two of lwIP's 1.5 s initial retransmission timeouts (`CONFIG_LWIP_TCP_RTO_TIME`). With a 5,760-byte TCP window (four segments) a lost segment seldom draws the three duplicate ACKs of a fast retransmit, so it waits for the timeout. The server's node logs these calls as time spent writing the body (`[POD-HTTP] ... 2803 ms writing 8192 bytes`). The timed-out calls stop at 1,608 to 14,456 bytes received.
