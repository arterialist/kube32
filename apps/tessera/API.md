# Tessera HTTP API

Tessera is an issue tracker (board, list, search, analytics, comments) that runs
as two WebAssembly pods on a Kube32 cluster:

- `tessera-core` on `esp32-node-02` (PSRAM node). Owns all data and serves every
  `/api/*` route below from inside its Wasm linear memory.
- `tessera-hud` on `esp32-node-01` (display node). Renders live counters on the
  LCD, reads the physical keyboard, beeps on new issues.

The ingress on `esp32-node-01` (port 80) serves the static frontend from flash,
proxies `/api/*` to `tessera-core`, and serves `/events` (SSE) and
`/api/cluster` natively.

All bodies are JSON. All timestamps are integer milliseconds in a synthetic
epoch: the boards have no clock, so `now` is `EPOCH_BASE_MS + uptime`. Every
response that carries timestamps also carries `"now"` so the client renders
relative times without trusting its own clock.

Error shape, any status >= 400:

```json
{"error":"not_found","message":"issue TES-9999 does not exist"}
```

## Auth

Session cookie `tsid` (HttpOnly, Path=/). 32 concurrent sessions, 24 h idle
expiry. Seeded users all share password `demo`.

| Method | Path | Body | Response |
|---|---|---|---|
| POST | `/api/auth/login` | `{"username":"mara","password":"demo"}` | 200 `{"user":{...},"now":...}` + `Set-Cookie: tsid=...` |
| POST | `/api/auth/logout` | | 204 |
| GET | `/api/me` | | 200 `{"user":{...}}` or 401 |

User object: `{"id":1,"username":"mara","name":"Mara Okafor","initials":"MO","color":"#f0a05a"}`

## Bootstrap

`GET /api/bootstrap` -> 200

```json
{
  "now": 1793059200000,
  "users": [User...],
  "projects": [{"id":1,"key":"TES","name":"Tessera Core","color":"#5b8def"}],
  "labels": [{"id":0,"name":"bug","color":"#e5484d"}, ...],        // up to 16
  "statuses": ["backlog","todo","in_progress","review","done"],
  "priorities": ["none","low","medium","high","urgent"],
  "counts": {"open":1840,"total":3000,"by_status":{"backlog":..,"todo":..}}
}
```

## Issues

Issue object (full):

```json
{
  "id": 42, "key": "TES-42", "project": 1,
  "title": "...", "description": "...",
  "status": "todo", "priority": 3,
  "assignee": 4, "reporter": 2,
  "labels": [0, 3], "estimate": 5,
  "created": 1792900000000, "updated": 1793000000000,
  "comment_count": 3
}
```

Issue summary (lists, board, search) omits `description` and adds nothing else.

| Method | Path | Notes |
|---|---|---|
| GET | `/api/issues` | query: `project`, `status` (comma list), `assignee`, `label`, `priority`, `q`, `sort` = `updated`,`created`,`priority`,`key`; `order` = `asc`,`desc`; `page` (1-based), `limit` (max 50). Response `{"items":[summary...],"total":n,"page":p,"limit":l,"took_us":n,"now":..}` |
| GET | `/api/issues/{key}` | full issue + `"comments":[...]` + `"activity":[...]` (last 20) |
| POST | `/api/issues` | `{"title","description","project","status","priority","assignee","labels","estimate"}`; title required (1..120 chars), description <= 1000. 201 full issue. Emits event. |
| PATCH | `/api/issues/{key}` | any subset of `title, description, status, priority, assignee, labels, estimate`. 200 full issue. Emits event. |
| DELETE | `/api/issues/{key}` | 204. Emits event. |
| POST | `/api/issues/{key}/comments` | `{"body":"..."}` (1..280). 201 `{"id","issue","author","body","created"}`. Emits event. |

Comment object: `{"id":7,"issue":42,"author":3,"body":"...","created":...}`

Activity object: `{"id":n,"ts":...,"actor":userId,"issue":42,"key":"TES-42","op":"create|update|comment|delete","field":"status","from":"todo","to":"done"}`

## Board

`GET /api/board?project=1` -> 200

```json
{"now":..,"columns":{"backlog":[summary...],"todo":[...],"in_progress":[...],"review":[...],"done":[...]},
 "truncated":{"backlog":true}, "limit":40}
```

Each column holds at most `limit` issues ordered by priority desc, updated desc.

## Search

`GET /api/search?q=wifi+timeout&limit=20` -> 200

```json
{"items":[{"key":"TES-12","title":"...","status":"todo","priority":2,"assignee":1,"updated":..,"score":17}],
 "took_us":412,"scanned":38,"total":9,"now":..}
```

Full-text over title + description + comments. Tokens are lowercase
alphanumerics; the last token matches as a prefix. Ranking is term frequency
weighted by field (title x3) plus a recency boost.

## Analytics

`GET /api/analytics?project=1&days=14` -> 200

```json
{"now":..,
 "by_status":{"backlog":n,...}, "by_priority":[n0,n1,n2,n3,n4],
 "by_assignee":[{"user":1,"open":n,"done":n}],
 "created_per_day":[n...], "resolved_per_day":[n...],   // oldest first, length = days
 "cycle_time_hours":{"p50":n,"p90":n},
 "open_age_buckets":{"lt1d":n,"lt7d":n,"lt30d":n,"gte30d":n},
 "took_us":n}
```

## Activity feed

`GET /api/activity?limit=50` -> 200 `{"items":[Activity...],"now":..}`

## Engine stats

`GET /api/stats` -> 200

```json
{"issues":3000,"comments":6000,"users":8,"sessions":2,
 "index":{"terms":4120,"postings":61000},
 "memory":{"used":1310720,"limit":1572864},
 "requests":{"total":n,"errors":n,"avg_us":n,"p99_us":n,"last_us":n},
 "uptime_ms":n,"now":n}
```

## Live events (SSE)

`GET /events` served by the ingress on node 1. `text/event-stream`, retry 2000.
Event types:

- `issue`: `{"op":"create|update|delete","key":"TES-42","status":"todo","actor":3,"title":"..."}`
- `comment`: `{"key":"TES-42","author":3,"id":7}`
- `hud`: `{"text":"typed on the keyboard so far"}` (from the keyboard pod on node 1)
- `cluster`: node telemetry every 2 s, same shape as `GET /api/cluster`
- `presence`: `{"viewers":n}`

Clients should refetch the affected view on `issue`/`comment` rather than
patching local state, except for the board which may move the card directly.

## Cluster telemetry (native, node 1)

`GET /api/cluster` -> 200

```json
{"nodes":[{"name":"esp32-node-01","role":"control-plane","free_heap":n,"min_free_heap":n,"free_psram":n,"uptime_ms":n,"pods":["tessera-hud"],"ready":true}, {...}],
 "ingress":{"rps":n,"p50_us":n,"p99_us":n,"active_sse":n,"workers":4,"requests":n},
 "link":{"espnow_frames_rx":n,"espnow_frames_tx":n,"crc_errors":n,"reassembled":n},
 "now":n}
```

## Keyboard input path

`tessera-hud` emits `{"t":"key","c":"a"}` on channel `tessera.input` for every
key press. `tessera-core` receives it through `k32_event`, appends it to the
"live note" of the most recently viewed issue (or `TES-1`), and emits a `hud`
event with the accumulated text. Enter commits the text as a comment by the
`hud` system user (id 0). Backspace deletes one character. This is the
demonstration that physical input on one chip reaches state on the other chip
and every browser within a few milliseconds.
