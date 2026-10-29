# tessera-db

`tessera-db` is Tessera's store of record: an append-only log of records on the microSD card of the node it runs on, served over HTTP to `tessera-api`. It runs in 8 KiB of linear memory on the Cardputer (no PSRAM) and streams every file read and write through one 2 KiB buffer.

The pod mounts the volume claim `tessera-db` (`/sd/kube32/volumes/tessera-db/`) and requests `hardware.kube32.io/sd`. Without a card or the grant, `k32_init` fails and the pod restarts.

## HTTP API

| Method | Path | Response |
|---|---|---|
| `POST` | `/append?expect=<seq>&log=<id>` | Body: one or more records (format below) with `seq` 0, at most 2048 bytes. The store assigns sequence numbers, seals the CRCs and writes the batch with one `k32_fs_write`; the segment file is closed before the answer. `200 {"first":f,"last":l,"log":"<id>"}`. `409 {"error":"conflict",...,"next":n}` when `expect` is not the next sequence number or `log` names another log (omit either to skip its check), `400` for a malformed batch or parameter, `413` over 2048 bytes, `507` when the card or the log is full, `503` when the log is read-only. |
| `GET` | `/records?after=<seq>&max=<bytes>` | `200 application/octet-stream`: the records with `seq > after`, whole, at most `max` bytes (default and limit 16384, at least one record), from one segment. Padding is left out. An empty body means there is nothing after `after`. |
| `GET` | `/stats` | `{"state":"ok"\|"read-only","format":1,"log":"<id>","first":1,"last":n,"records":n,"bytes":b,"segments":s,"segment_bytes":32768,"recovered_bytes":r,"appends":a,"append_errors":e}` plus `"error"` when read-only. |

A single writer (`tessera-api`) is assumed; `expect` turns a second writer into a `409` instead of interleaved logs.

## On-card format

Segment files `00000.seg`, `00001.seg`, ... at the volume root, each at most 32,768 bytes: a 32-byte header, then whole records. Records never span segments, and a full segment is never written again.

Segment header (little-endian):

| Offset | Field | |
|---|---|---|
| 0 | `u32` magic | `TSDB` |
| 4 | `u16` version | `1` |
| 6 | `u16` header size | `32` |
| 8 | `u32` log id | random, the same in every segment of a log |
| 12 | `u32` index | matches the file name |
| 16 | `u32` first sequence number | |
| 20 | `u32` segment size | `32768` |
| 24 | `u32` reserved | `0` |
| 28 | `u32` CRC-32 | of bytes 0..27 |

Record (12-byte header, then the payload; `../common/tlog.h`):

| Offset | Field | |
|---|---|---|
| 0 | `u16` payload length | at most 1280 (the largest API record is 1,185 bytes) |
| 2 | `u8` type | `1` issue create, `2` issue update, `3` comment, `4` issue delete, `255` padding |
| 3 | `u8` flags | `0` |
| 4 | `u32` sequence number | 1, 2, 3, ... (`0` in padding) |
| 8 | `u32` CRC-32 | zlib polynomial, of bytes 0..7 and the payload |

The payloads are defined by `tessera-api` (`../common/tlog.h`); the store treats types 1..127 as opaque.

## Recovery

Opening the log reads every segment header and scans the last segment record by record: framing, CRC and consecutive sequence numbers. Bytes after the last good record come from an interrupted append: at most 2048 bytes, and no whole record after the first bad byte. The store overwrites them in place with padding records, so the file needs neither truncation nor rename (the ABI has neither). A segment cut off while its header was written holds no records and is removed; a new segment whose creation fails is removed at once. A longer bad tail, a good record after a bad one, a damaged header or segments of different logs leave the log read-only (`503` on appends, reads still served). An append that fails part way is repaired the same way before the next append.

## Growth

The log is not compacted: updates are small and the API's memory, not the log, bounds the number of issues and comments. Record sizes, header included: 35 bytes plus title and description for an issue create; 26 bytes plus 9 per changed field for an update (3 plus the text for a title or description); 31 bytes plus the body for a comment; 25 bytes for a delete. A day of 1,000 changes adds roughly 50 KB. The log holds up to 256 segments (8 MiB, about 150,000 changes); appends then return `507`. A full load reads each segment once plus at most one segment per 16 KiB page skipped from the segment start (the ABI has no seek): 1.6 times the log size for the default dataset.

## Sample data

`make -C apps/tessera dataset` writes the default dataset (2400 issues, 1801 status changes, 4800 comments: 9001 records, 878,525 bytes in 28 segments) to `apps/tessera/build/card/kube32/volumes/tessera-db/`. Copy `apps/tessera/build/card/kube32` to the root of the card. An empty volume is an empty tracker.

## Memory

| Region | Bytes |
|---|---|
| Stack (`-z stack-size`) | 768 (deepest call chain: 704) |
| Data and bss (log state with the 256-entry segment table, 2 KiB file buffer, strings) | 4,256 |
| Request block (`k32_alloc`, from `__heap_base` to `TDB_MEM_CAP`) | 3,168 (an append needs 2048 + request line and headers) |
| Linear memory cap (`MEM_CAP`, pod limit `8Ki`) | 8,192 |

The module is built with `-Oz` (10.5 KB): the interpreter's compiled code grows with the module and is most of a pod's cost. Besides its linear memory, the pod's interpreter allocations on a 64-bit host come to 87 KB at deploy and 77 KB once every route has run; a 32-bit board needs a quarter to half of that.

`make wasm` fails when the layout does not fit under `MEM_CAP` (`../common/wasm_inspect.py`).

## Build and test

```sh
cd apps/tessera/db
make              # ../../../images/tessera-db.wasm, layout check against MEM_CAP
make test         # native: codec, append/read round trip, paging across segments, torn-tail recovery, damaged logs
```

Build settings: `MEM_CAP` (`8192`), `STACK_SIZE` (`768`), `CLANG`, `RUST_LLD`.
