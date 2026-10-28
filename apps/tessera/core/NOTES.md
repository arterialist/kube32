# tessera-core: contract notes

Places where `API.md` or `k32_app_abi.h` left room for interpretation, and
what this implementation does. None of these required changing the contract
files.

## Events on `tessera.events`

The channel carries three SSE event types (`issue`, `comment`, `hud`) but the
ABI gives `k32_event_emit` only a topic and a payload. Every payload therefore
carries a `"type"` discriminator as its first key so the ingress can map it to
the SSE event name:

    {"type":"issue","op":"create","key":"TES-42","status":"todo","actor":3,"title":"..."}
    {"type":"comment","key":"TES-42","author":3,"id":7}
    {"type":"hud","text":"typed so far"}

Titles are cut to 96 escaped bytes and hud text to 160 escaped bytes (never
splitting a UTF-8 sequence) so every payload stays under `K32_MAX_EVENT_BYTES`.

## Microsecond fields with a millisecond clock

The only clock in the ABI is `k32_time_ms`. `took_us`, `avg_us`, `p99_us` and
`last_us` are `ms * 1000`; on hardware they will read as multiples of 1000.
`esp32_get_time_us` is mentioned in a header comment but not declared, so it
is not used.

## Issue keys and ids

Issue ids are global and 1-based; the key is `<project key>-<id>`, so ids are
not contiguous within a project (`TES-1`, `HUD-2`, `TES-3`...). Lookups by key
check that the project prefix matches the issue's project, so `HUD-1` is a 404
when issue 1 belongs to TES. The keyboard fallback "or `TES-1`" is implemented
as "issue id 1, or the first live issue if 1 was deleted".

## Users

Bootstrap lists 9 users: the 8 seeded humans plus `hud` (id 0) so clients can
render comments authored by the keyboard. `stats.users` reports 8, matching
the API example; `hud` cannot log in.

## Stats

`memory.used/limit` come from `k32_mem_stats`; when the host returns an error
(the native shim does) the arena numbers are used instead. An extra `"arena"`
object (`used, size, tables, strings, postings, postings_leaked`) is added
beside the documented keys; `used` counts both ends of the arena. While the
data set is still being seeded a `"seeding":{"done":N,"total":M}` object
follows `sessions`.

## Activity `from` / `to`

Both are always strings. Status and priority changes use the enum names,
assignee changes use usernames (empty when unassigned), estimate and labels use
decimal numbers (labels as the bitmask), title/description changes use empty
strings. For `op:"comment"`, `to` holds the comment id.

## Keyboard input

`tessera.input` payloads are `{"t":"key","c":"<char>"}`. Enter is accepted as
`"\n"`, `"\r"`, `"Enter"`; Backspace as `"\b"`, `"\x7f"`, `"Backspace"`. Other
control characters are ignored. The live note is capped at 200 bytes (comment
limit is 280, but the hud event has to stay under 200 bytes); further keys are
dropped until Backspace or Enter.

## Sessions and ticks

Idle expiry (24 h) is measured with `k32_time_ms`, not the 32-bit `uptime_ms`
argument of `k32_tick` (which wraps after 49 days). When all 32 slots are in
use, a login evicts the least recently used session. Expired sessions are also
dropped lazily when presented in a cookie.

## Incremental seeding

`k32_init` used to seed everything and build the index in two passes, which
took over two minutes on the board while the pod could not serve anything (the
host serializes init/handle/tick per pod). Now `k32_init` seeds the first
`SEED_CHUNK` (default 10) issues and returns; `k32_tick` seeds the next chunk
on every call and returns 1 while issues remain (the host then calls it again
after a ~10 ms yield), 0 when idle. The RNG state, the comment budget and the
synthetic `now` live in `g.seed`, so the sequence of draws is the one
`seed_run` made: same texts, ids and timestamps. Each issue is indexed right
after its comments are attached (`index_add_issue`), the leftover-comment
distribution adds its tokens with `index_add_comment` (tf merged in place,
equal to re-indexing the issue), and the activity ring plus the final posting
compaction run in the same call as the last issue, so from the outside
`seeding.done == total` is never observed with work pending.

Mid-seed requests see the partial set: lists, board, search and stats work on
what exists. A client create mid-seed takes the next free id, which shifts
the ids of the issues seeded after it by one (the leftover comments and the
activity ring still address `1..SEED_ISSUES`). `GET /api/stats` carries
`"seeding":{"done":N,"total":M}` only while seeding, so the finished response
is unchanged. The chunk-end event is `events_issue("create")` for the last
seeded issue: the HUD counts creates and web views refetch on `issue` events;
the ingress `stats` line does not carry issue counts, and `hud` events are
ignored by the HUD.

If seeding fails (arena full), the error is logged, the seeder stops, and
the partial set is served; `seeding.done` then stays below `total`.

## Posting region and compaction

Per-issue indexing with the old doubling `term_grow` on a bump arena left
~1.4x the final postings live plus every superseded array leaked, which
exhausted the board arena (measured: +97 KB and still incomplete). Posting
arrays therefore live in a region that grows down from the end of the arena
(`k32_arena_alloc_top`), as contiguous blocks `[payload][u16 term+1][u16 cap]`
with `term+1 == 0` marking a dead block. When either end of the arena cannot
grow, `index_compact` walks the blocks from the top, slides live arrays up in
place (always `dst >= src`, overlapping `memmove`) and gives each term
`count + count/16 + 1` slack when the space allows, which is what
`index_build` allocated. The final chunk compacts once more, so the resting
budget matches the old one (185,283 vs 185,367 bytes at 2400/4800; the small
difference is slack clamped by available space). Runtime regrowth still leaks
until the next compaction (`postings_leaked` is transient now). `ARENA_BYTES`
must be a multiple of 16 (static assert) so the block walk starts aligned.

Postings inside a block are kept in ascending issue-id order by every writer
(append for a new highest id, binary search + memmove otherwise, memmove on
removal), so `index_add_comment` and `term_remove_posting` find an issue's
posting with a binary search instead of a scan. Posting order is not
observable: search accumulates per id and lists hits in id order.

## Dictionary and stopwords

Terms keep their index for life (insertion order); exact lookups use an
open-addressing hash of `2 x MAX_TERMS` u16 slots (FNV-1a, linear probing,
load <= 0.5) and prefix walks use `term_order`, the indices sorted by string.
The profile showed the stopword list, not the dictionary, was the per-token
cost: 91 literals compared for every token (12.7 M compares for the 2400/4800
seed). It is now a 256-slot hash built on first use. `make profile` prints
both counters per request.

## Bounded selection

`k32_topk` keeps a sorted u16 array of size k: one comparator call against
the k-th best, and binary search + memmove only when the candidate beats it.
`h_issues_list` scans newest-first for descending orders so arrivals tend to
follow the sort order (for `sort=key&order=desc` that is 2,449 compares
instead of 14,344 with an ascending scan); the result does not depend on the
scan order because `cmp_issue` is strict for every key (id tiebreaker, flipped
with the rest under `desc`). `analytics` keeps its full sort (it needs the
median).

## Response writer

On the board a response cost ~25 us per output byte on top of the fixed
work: every byte went through `jw_putc` (call, capacity check, two
increments), keys paid `strlen` plus the escape switch per character and
numbers an i64 division per digit. Now `jw_write` memcpy's runs into the
4 KiB buffer, `k32_jw_escaped` scans with a 256-entry class table and copies
the runs between escapes, literal keys are complete `"key":` fragments
written with one copy (`k32_jw_keylit`; the four dynamic keys keep
`k32_jw_key`), `k32_fmt_u32/i64` use a two-digit table with u32 arithmetic
(one i64 division splits a 13-digit timestamp), and `issue_t.flags` /
`comment_t.flags` carry `*_ESC` bits set at insert time (and on PATCH) that
say whether the text contains a byte JSON must escape; clean text is copied
without a scan. Escaped copies are not stored: that would double user text
and the tokenizer must see the raw bytes.

## Memory primitives

`memcpy`, `memmove` and `memset` copy four bytes per iteration (unaligned
i32 loads/stores are legal in wasm and declared as such for the native build).
Bulk-memory opcodes are not used: wasm3 compiles `memory.copy`, but the
shipped image never contained one, so they are unproven on the boards.

## Timestamps

Internally created/updated/resolved are stored as whole seconds (u32) relative
to `EPOCH_BASE_MS - 120 days` to keep the issue record at 40 bytes and the
comment record at 16. Emitted values are milliseconds as the API requires, but
they are second-aligned. `now` keeps full ms precision.

## Memory scaling

The module declares its linear memory at link time (`--initial-memory`), so
the environment variables scale the data set, not the memory footprint. For a
256 KiB node relink with `make ARENA_BYTES=180224 INITIAL_MEMORY=262144
SCRATCH_BYTES=32768` and run with `SEED_ISSUES=300 SEED_COMMENTS=400
MAX_ISSUES=400 MAX_TERMS=512`; the sources are identical.

## Small extensions

- `assignee=none` filters unassigned issues; `assignee` in bodies accepts
  `null`, an id or a username.
- `labels` in bodies accept ids or label names; `status` accepts the name or
  its index.
- `MAX_TERMS` (default 1024) bounds the term dictionary; when it is full, new
  words from user-created issues are silently not indexed.
- Storage exhaustion (issue/comment table or arena full) answers `507`.
- Unknown routes are `404`, wrong methods `405`, malformed request framing
  `400`; in every case exactly one response is produced.
- `GET /api/board` without `project` covers all projects.
- `GET /api/issues?q=` applies the search index as a filter (AND semantics on
  all tokens, last token as prefix) and then sorts by the requested key.
- Search is AND over tokens; stopwords (short function words) are not indexed
  and are ignored in queries, except that the final prefix token is always
  honoured.

## Not built

`host_test/serve.py` was skipped as instructed: neither the `wasmtime` nor the
`wasm3` Python package is installed on this machine.
