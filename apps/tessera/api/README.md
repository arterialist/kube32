# tessera-api

`tessera-api` is the Tessera backend (board, list, full-text search, analytics, comments, activity, and sessions) running inside WebAssembly linear memory via the Kube32 app ABI (`include/runtime/k32_app_abi.h`). See `../API.md` for the HTTP contract.

The module uses freestanding C11 without libc, floating point, recursion, or variable-length arrays. It bounds-checks all accesses and returns one response per request.

## Storage

The in-memory store and search index are the serving copy of the records kept by `tessera-db` (see `../db/README.md`). The pod calls the Service through `k32_http_call` and needs the `SERVICE_CALL` capability.

- **Load.** `k32_init` starts with an empty store. `k32_tick` reads `GET /stats` from `tessera-db`, fetches the log in pages of up to 16 KiB (`GET /records?after=<seq>`), checks every record (framing, CRC-32, consecutive sequence numbers) and applies `LOAD_CHUNK` records per tick. Requests are answered from the loaded part; `GET /api/stats` reports `"loading":{"done":N,"total":M}` until the load finishes. An empty log is an empty tracker.
- **Writes.** Every change (create, update, comment, delete, keypad comment) is encoded as one record (`../common/tlog.h`) and appended with `POST /append?expect=<next seq>&log=<log id>`. The store applies the record only after `tessera-db` acknowledges it, through the same code that applies records during the load, so the store always equals a replay of the log. A `2xx` answer means the change is on the card.
- **Unavailable storage.** Until the load finishes, and while `tessera-db` cannot be reached, writes return `503` with `Retry-After: 2` and reads keep working from memory. The load retries every 2 s.
- **Conflicts.** When `tessera-db` reports a different next sequence number or another log (another writer, a replaced card) the write returns `503` and the next tick empties the store and loads again. Sessions are kept.
- **Unknown outcomes.** When an append gets no answer or fails after its bytes may have reached the card, the write returns `503` and the next tick compares `GET /stats` with the store: a log that moved ahead is loaded again, so the change shows up. The same check runs once after every load.
- **Damaged records.** The load applies the records before the first damaged one and stops; reads serve what loaded, writes return `503`, and `/api/stats` names the sequence number. Every 10 s the API checks the log id and loads again once the log is replaced.
- **Clock.** The boards have no wall clock: `now` is `EPOCH_BASE_MS` plus uptime, moved forward past the newest record loaded so that a restart never goes back in time.

## Build and test

```sh
cd apps/tessera/api
make              # ../../../images/tessera-api.wasm
make test         # native build + assertions against an in-process tessera-db (500 issues, 1000 comments)
make bench        # 10,000 mixed requests on the default dataset (writes include the append)
make size         # memory layout, dataset size and load cost at the default dataset + wasm facts
make profile      # comparator / dictionary / stopword / posting work per request
make clean
```

The host tools link the real `tessera-db` code and the dataset generator: `k32_http_call` goes to an in-process `tessera-db` and its volume is a temporary directory (`../common/host/shim.c`).

Build settings are `make` variables: `ARENA_BYTES` (`1163264`), `SCRATCH_BYTES` (`65536`), `INITIAL_MEMORY` (`1310720`), `STACK_SIZE` (`65536`), `CLANG`, and `RUST_LLD`. `ARENA_BYTES` must be a multiple of 16.

## Environment variables

| Name | Default | Description |
|---|---|---|
| `TESSERA_DB_URL` | `http://tessera-db` | Base URL of the `tessera-db` Service |
| `MAX_ISSUES` | `2900` | Issue slab capacity |
| `MAX_COMMENTS` | `5300` | Comment slab capacity |
| `MAX_TERMS` | `1024` | Term dictionary capacity |
| `EPOCH_BASE_MS` | `1793059200000` | Synthetic epoch (`now = EPOCH_BASE_MS + k32_time_ms()` until a record is newer) |
| `LOAD_CHUNK` | `32` | Records applied per `k32_tick` during the load |

## Memory sizing

- Linear memory: 20 pages (1,310,720 bytes), the `1280Ki` PSRAM request of the pod.
- Stack: `[0, 65536)` (64 KiB). The deepest call chain uses 18 KiB.
- Request scratch: 65,536 bytes (`SCRATCH_BYTES`), holding the request buffer (up to 16 KiB) and per-request allocations. `k32_alloc` resets on the first call after `k32_handle` or `k32_event`.
- Record page: 16,384 bytes for one page of records during the load.
- Response buffer: 4 KiB streaming buffer flushed via `k32_resp_write`.
- Arena: 1,163,264 bytes (`ARENA_BYTES`). With the default dataset (2400 issues, 4800 comments) loaded: 1,083,120 bytes used, 80,144 free for new issues and comments.
- Slab tables: 257,120 bytes at the default capacity (`issues` 2900 x 40 B, `comments` 5300 x 16 B, `terms` 1024 x 16 B + 2048 B order + 4096 B hash, `activity` 512 x 32 B, `sessions` 32 x 32 B, the record page and the scratch).
- Postings and strings: strings grow upward in the arena; posting blocks (`[payload][u16 term+1][u16 cap]`) grow downward from the top and compact in place when the two ends meet.
- Storage exhaustion (arena or slab full) returns HTTP `507` before anything is written to `tessera-db`.

## Event topics

- `tessera.events` (emitted via `k32_event_emit`): JSON payloads under 200 bytes (`K32_MAX_EVENT_BYTES`) with a leading `"type"` discriminator for SSE mapping, sent after a change is stored. During the load, one `issue` create event per applied chunk. Titles truncate at 96 escaped bytes and HUD text at 160 escaped bytes on UTF-8 boundaries:
  ```json
  {"type":"issue","op":"create","key":"WEB-42","status":"todo","actor":3,"title":"..."}
  {"type":"comment","key":"WEB-42","author":3,"id":7}
  {"type":"hud","text":"typed so far"}
  ```
- `tessera.input` (handled via `k32_event`): keyboard input payloads `{"t":"key","c":"<char>"}` append to a 200-byte live note on the last viewed issue (or issue id 1) and emit a `hud` event. Enter (`"\n"`, `"\r"`, `"Enter"`) stores a comment authored by `keypad` (user id 0) and clears the note, as the HUD clears its line; a note that cannot be stored (loading, `tessera-db` unreachable) is dropped. Backspace (`"\b"`, `"\x7f"`, `"Backspace"`) removes one UTF-8 character.

## Source layout

```
src/k32lib.h/.c   freestanding runtime, PRNG, bump arena, sorting, streaming JSON
src/store.h/.c    slab tables, activity/session/latency rings, string arena, clock
src/index.h/.c    inverted index, term hash, prefix order array, posting blocks
src/journal.h/.c  applies tessera-db records to the store (load and live changes)
src/storage.h/.c  tessera-db client: paged load in k32_tick, write-through appends, stats
src/router.h/.c   request parser, sessions, /api routes, events, top-k selection
src/app.h/.c      ABI entrypoints (k32_init, k32_alloc, k32_handle, k32_tick, k32_event)
host_test/        test suite, benchmark, size printer, profiler (native)
../common/        record format, host ABI bridge, native shims, wasm inspector
```

## Implementation notes

- Issue IDs are 1-based global integers assigned in log order. Keys follow `<project key>-<id>` (`WEB-1`, `IOS-2`), and key lookups validate the project prefix.
- Bootstrap returns 9 users (8 people plus `keypad` at id 0). `stats.users` reports 8, and `keypad` cannot log in.
- Created, updated, and resolved timestamps store whole seconds (`u32`) relative to `EPOCH_BASE_MS - 120 days` and emit as second-aligned milliseconds. Latency fields (`took_us`, `avg_us`, `p99_us`, `last_us`) multiply `k32_time_ms` by 1000.
- The activity feed is rebuilt from the log at load time: the 512 most recent records' entries.
- Sessions expire after 24 hours of inactivity via `k32_time_ms` and evict least-recently-used when all 32 slots are full. They live in memory only.
- Activity `from` and `to` fields are always strings: enum names for status/priority, usernames for assignee, decimal strings for estimate and label bitmask, comment id in `to` for `op:"comment"`, and empty strings for title/description edits.
- Search uses AND semantics across query tokens, treating the final token as a prefix (up to 64 terms) and ignoring stopwords except on the final prefix token. `GET /api/issues?q=` applies the search index before sorting.
- Request bodies accept `assignee` as `null`, user id, or username (`assignee=none` filters unassigned in queries), `labels` as ids or names, and `status` as name or index. `GET /api/board` without `project` returns all projects.
- `GET /api/stats` adds an `"arena"` object (`used`, `size`, `tables`, `strings`, `postings`, `postings_leaked`) and a `"storage"` object alongside the documented fields.

## Limitations

- When the term dictionary reaches `MAX_TERMS`, new words in user-created issues and comments are not indexed.
- The live keyboard note drops characters beyond 200 bytes until Backspace or Enter is pressed.
