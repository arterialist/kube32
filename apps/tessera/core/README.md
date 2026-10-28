# tessera-core

Issue tracker backend (board, list, full-text search, analytics, comments,
activity, sessions) that lives entirely inside WebAssembly linear memory and is
served through the Kube32 app ABI (`include/runtime/k32_app_abi.h`). The HTTP
contract is `apps/tessera/API.md`; interpretations are listed in `NOTES.md`.

Freestanding C11, no libc, no floating point, no recursion, no VLAs. Every
access is bounds-checked; every request produces exactly one response.

## Layout

```
src/k32lib.h/.c   runtime: memcpy/memset/memmove/memcmp/strlen (exported as plain
                  symbols), integer formatting/parsing, URL and query decoding,
                  lowercase tokenizer + stopwords, xorshift32 PRNG, bump arena,
                  heapsort/shellsort, streaming JSON writer with escaping,
                  in-place flat JSON reader (strings, ints, bools, null, small
                  arrays of ints/strings; nested values are skipped)
src/k32_host.h    ABI bridge: K32_IMPORT under __wasm32__, plain externs natively
src/store.h/.c    data model: users, projects, labels, issues (40 B), comments
                  (16 B), activity ring (512 x 32 B), sessions (32 x 32 B), term
                  dictionary (16 B), packed timestamps, string arena, latency ring
src/index.h/.c    inverted index: term dictionary with stable indices, an
                  open-addressing hash for exact lookups and a sorted order
                  array for the prefix range of the last query token;
                  postings arrays of (issue_id u16, weighted tf u8) kept in
                  id order inside blocks of a top-down posting region that
                  is compacted in place; per-issue add/remove, per-comment
                  merge
src/seed.h/.c     deterministic seed data from word lists (components, verbs,
                  symptoms, hardware/protocol nouns); 90-day timestamp spread,
                  ~40% done; resumable in chunks (seed_begin / seed_step)
src/router.h/.c   request parsing, cookie sessions, every /api route, events,
                  bounded top-k selection for list / board / search pages
src/app.h/.c      k32_init / k32_alloc / k32_handle / k32_tick / k32_event,
                  env configuration, chunked seeding, keyboard live note
host_test/shim.c  native implementations of all k32_* imports (env from the
                  process environment, monotonic clock, rand, captured
                  responses and events)
host_test/test.c  assertions (1242 checks)     -> make test
host_test/bench.c 10,000 mixed requests, p50/p99 -> make bench
host_test/size.c  memory layout printer        -> make size
host_test/profile.c work counters per request  -> make profile
host_test/wasm_inspect.py  lists imports/exports/memory of the .wasm (no
                  external tools)
Makefile          wasm build (clang + rust-lld) and the native targets
NOTES.md          contract interpretations and gaps
```

`host_test/serve.py` was not written: neither the `wasmtime` nor the `wasm3`
Python package is installed here (`python3 -c "import wasmtime"` fails), and
nothing was pip-installed.

## Commands

```sh
cd apps/tessera/core
make              # build/tessera-core.wasm
make test         # native build + assertions (runs with SEED_ISSUES=500 SEED_COMMENTS=1000)
make bench        # native build, 10,000 mixed requests at the default seed sizes
make size         # memory layout at the default seed sizes + wasm facts
make profile      # comparator / dictionary / stopword / posting work per request
make clean
python3 host_test/wasm_inspect.py build/tessera-core.wasm
xxd build/tessera-core.wasm | head -1      # 0000: 0061 736d 0100 0000 ...
```

Compile: `clang --target=wasm32 -O2 -nostdlib -ffreestanding -fno-exceptions
-mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra -c`.
Link: `rust-lld -flavor wasm --no-entry --export-dynamic --allow-undefined
-z stack-size=65536 --initial-memory=1572864 --max-memory=1572864`
(`RUST_LLD` is discovered with `ls $HOME/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1`;
there is no `wasm-ld` on this machine and rust-lld links wasm fine).

Overridable make variables: `ARENA_BYTES` (1425408), `SCRATCH_BYTES` (65536),
`INITIAL_MEMORY` (1572864), `STACK_SIZE` (65536), `CLANG`, `RUST_LLD`.

## Module facts

```
build/tessera-core.wasm: 76,149 bytes
imports (10, all from "env"): k32_resp_begin, k32_resp_header, k32_resp_write,
  k32_resp_end, k32_event_emit, k32_time_ms, k32_log, k32_random, k32_env_get,
  k32_mem_stats
exports: memory, k32_init, k32_alloc, k32_handle, k32_tick, k32_event
memory: 24 pages = 1,572,864 bytes (initial = max)
sections: code 64,054 B, data 9,313 B (one segment at 65536)
linear memory map: [0, 65536) stack, [65536, 74849) data, then .bss which is the
  1,425,408-byte arena plus ~1.5 KB of globals; ~70 KB of the 1.5 MiB is unused
no bulk-memory opcodes (memory.copy / memory.fill) are emitted; memcpy, memmove
  and memset are word-wide loops
```

The image deployed to the boards (`images/tessera-core.wasm`) is the same
source linked with `make INITIAL_MEMORY=1310720 ARENA_BYTES=1163264`: 68,336
bytes, 20 pages = 1,310,720 bytes, run with `SEED_ISSUES=2400 SEED_COMMENTS=4800`.

## Environment variables (read with `k32_env_get` in `k32_init`)

| name | default | meaning |
|---|---|---|
| `SEED_ISSUES` | 3000 | issues generated at init |
| `SEED_COMMENTS` | 6000 | comments generated at init |
| `MAX_ISSUES` | SEED_ISSUES + 500 | issue slab capacity (comments: SEED_COMMENTS + 500) |
| `EPOCH_BASE_MS` | 1793059200000 | synthetic epoch; `now = EPOCH_BASE_MS + k32_time_ms()` |
| `MAX_TERMS` | 1024 | term dictionary capacity |
| `SEED_CHUNK` | 10 | issues seeded per `k32_init` / `k32_tick` call |

Seeding is deterministic (xorshift32, fixed seed) and relative to
`EPOCH_BASE_MS`, so the data set is identical on every boot. It is also
incremental: `k32_init` seeds users, projects and the first `SEED_CHUNK`
issues (with their comments, indexed as they are created) and returns;
`k32_tick` seeds `SEED_CHUNK` more per call from the carried RNG state and
returns 1 while work remains, 0 when idle (the host calls it again after a
short yield while it returns 1). The call that seeds the last issue also
distributes the leftover comments, seeds the activity ring and packs the
posting region, exactly as the former single pass did. Requests work on the
partial set meanwhile; `GET /api/stats` carries
`"seeding":{"done":N,"total":M}` until the set is complete, and every chunk
ends with one `issue`/`create` event (the last seeded issue) so SSE clients
refetch and the HUD counter climbs. An issue created by a client mid-seed
takes the next id, so the issues seeded after it are shifted by one id; the
RNG sequence, texts and timestamps are unchanged.

## Memory budget (measured with `make size`, default seeds)

```
arena size              1425408 bytes (1392 KiB)
arena used              1302794 bytes (1272 KiB)  91%
arena free               122614 bytes (119 KiB)     <- runtime growth (new issues/comments/postings)
slab tables              283936 bytes
  issues      3500 x 40 B =   140000  (used 3000)
  comments    6500 x 16 B =   104000  (used 6000)
  terms       1024 x 16 B =    16384  (used 318)  + 2048 B order + 4096 B hash
  activity     512 x 32 B =    16384
  sessions      32 x 32 B =     1024
request scratch           65536 bytes   (request buffer <= 16 KiB + per-request work)
strings                  722114 bytes   (titles, descriptions, comments, term text)
postings                 229518 bytes   (72292 postings x 3 B + up to 1/16 slack per term, 0 leaked)
seeding                  299 ticks of 10 issues, 5 posting compactions
```

Total touched memory at the default seeds: 1,302,794 (arena) + 65,536 (stack)
+ 9,313 (data) = 1,377,643 bytes, under 1.4 MB. The pod limit is 1536Ki and the
module declares exactly that. At the board settings (`ARENA_BYTES=1163264`,
2400/4800) the arena ends at 1,073,492 bytes used, 89,772 free, postings
185,283 bytes, 239 ticks, 5 compactions.

Posting arrays live in a region that grows down from the end of the arena;
growing a term allocates a fresh block and abandons the old one, and when the
two ends of the arena meet the region is compacted in place (dead blocks
dropped, each term re-sized to `count + count/16 + 1`). `postings_leaked` in
`/api/stats` is therefore transient: after the 10,000-request bench (166 new
issues, 160 comments, many patches) the arena is at 1,339,091 bytes with 8,788
bytes awaiting the next compaction. Strings replaced by PATCH are rewritten in
place when the new text is not longer; otherwise the old text is abandoned
(bump arena, no free). Exhaustion answers `507`, never a trap.

Scaling down (same sources, relinked): `make ARENA_BYTES=180224
INITIAL_MEMORY=262144 SCRATCH_BYTES=32768` with `SEED_ISSUES=300
SEED_COMMENTS=300 MAX_ISSUES=350 MAX_TERMS=512` uses 167,038 of 180,224 arena
bytes inside a 256 KiB memory (the test suite's count assertions assume 500
issues, so run it with the default link).

## Native bench (`make bench`, Apple M-series, default 3000/6000 seeds)

```
10000 mixed requests in 212.1 ms (47150 req/s)
  category        n    p50_us    p99_us    max_us    avg_us
  list         3072      21.0     102.0     144.0      28.6   (filters, sorts, pages, q=)
  search       1969      12.0      30.0      75.0      12.9
  get          1988       2.0       4.0      11.0       2.2
  board        1011      65.0     140.0     331.0      67.6   (one scan, 5 bounded top-40 selections)
  analytics     460      37.0      96.0     100.0      40.3
  activity      511       8.0      13.0      16.0       8.3
  write         480       1.0       8.0      21.0       1.7   (create / patch / comment)
  stats         509       2.0      10.0      15.0       4.0
  all         10000      15.0      93.0     331.0      21.2
```

List, board and search pages are bounded top-k selections (`k32_topk`): every
candidate is compared once against the current k-th best and only inserted
(binary search + memmove in a u16 array of size k) when it beats it, with
k = page x limit for lists, 40 per board column and `limit` for search. The
issue comparator is a strict total order (id is the final tiebreaker), so a
page is exactly what a full sort would return. The scan rejects a
candidate against the current k-th best with one or two 32-bit compares
(`sort_key`: the same order as a (primary, secondary, id) triple, no call,
no i64 math) and only calls the comparator for actual inserts. `make profile` prints the work per request;
on the seeded 2400-issue set `GET /api/issues?limit=1` costs 0 comparator
calls instead of 45,285 (heapsort over every candidate), `limit=50` 461,
`page=10&limit=50` 4,491, the board 2,413 instead of 19,431. Under wasm3 a comparator call is an indirect call
through the interpreter (about 15 us measured on the board), which is why
this matters more there than natively. Search never scans issues: it walks
postings for each query term (prefix range capped at 64 terms) and only
touches matching ids.

The seeding/indexing work shrank the same way: the former single pass did
897,890 dictionary string compares and 12,747,984 stopword compares for
2400/4800 (the stopword list was scanned linearly for every token); the
hashed stopword table and the hashed dictionary bring that to 66,544 and
72,143 plus 44,674 posting binary-search steps, spread over 239 ticks whose
longest (the final chunk: leftover comments, activity ring, compaction) is
under 1 ms natively.

## Request handling notes

- `k32_alloc` bumps inside the 64 KiB scratch region; the bump pointer resets
  on the first `k32_alloc` after a `k32_handle`/`k32_event`, and `k32_handle`
  keeps the request bytes alive while it resets everything after them.
- Responses stream through a 4 KiB buffer to `k32_resp_write` (one HTTP chunk
  per flush on the host); nothing buffers a whole response.
- The JSON writer copies runs with the word-wide `memcpy`: literal keys are
  single `"key":` fragments (`k32_jw_keylit`), numbers are formatted with a
  two-digit table into a stack buffer and copied once, and titles,
  descriptions and comment bodies carry a flag computed at insert time that
  says whether any byte needs escaping (seeded text never does), so they are
  copied without a scan. `make profile` shows the board (49.8 KB) going from
  49,767 single-byte writes + 29,638 escape-scanned bytes to 4,703 + 1,344
  with 5,265 bulk copies and 13 flushes instead of 33; responses are
  byte-identical (verified against the previous build on 57 requests
  including escape-heavy user text).
- Request latency is sampled per request (`k32_time_ms` based) into a 256-entry
  ring for `p99_us`; `avg_us` is over all requests.
- `k32_tick` expires idle sessions and, while the data set is still being
  seeded, seeds one more chunk (returning 1 so the host calls it again soon).
  The scratch region is reset around that work; no request buffer is live
  between calls.
- Events: `{"type":"issue"|"comment"|"hud",...}` on `tessera.events`, each
  under 200 bytes (title cut at 96 escaped bytes, hud text at 160).
- Keyboard: `tessera.input` `{"t":"key","c":"x"}` appends to the live note of
  the last viewed issue (or issue 1); Enter commits it as a comment by user 0
  (`hud`), Backspace removes one UTF-8 character; every key emits a hud event.
