# Tessera HTTP API

Tessera is an issue tracker (board, list, search, analytics, comments) made of
four WebAssembly pods on a Kube32 cluster:

| Pod | Node | Role |
|---|---|---|
| `tessera-web` | control plane | Serves the web client from its image bundle; client routes get `index.html` (`web/README.md`). |
| `tessera-api` | control plane (PSRAM) | Serves every `/api/*` route below from its Wasm linear memory: the store and search index, loaded from `tessera-db` at startup (`api/README.md`). |
| `tessera-db` | Cardputer | The store of record: an append-only log on the microSD card (`db/README.md`). |
| `tessera-hud` | Cardputer | Live counters on the LCD and keyboard input (`hud/README.md`). |

The Ingress `tessera` (`examples/tessera.yaml`) routes `/` to `tessera-web`
and `/api` to `tessera-api`, and serves `/events` (SSE) and `/api/cluster`
itself.

All bodies are JSON. All timestamps are integer milliseconds in a synthetic
epoch: the boards have no clock, so `now` is `EPOCH_BASE_MS + uptime`, moved
past the newest stored record when the API starts. Every response that
carries timestamps also carries `"now"` so the client renders relative times
without trusting its own clock.

Error shape, any status >= 400:

```json
{"error":"not_found","message":"issue WEB-9999 does not exist"}
```

Changes (`POST`, `PATCH`, `DELETE` below, and keypad comments) are stored in
`tessera-db` before the API answers: a `2xx` means the change is on the card.
While the API loads its data, and while `tessera-db` cannot be reached, they
return `503` with `Retry-After: 2`; reads keep working:

```json
{"error":"unavailable","message":"loading data from tessera-db"}
```

## Auth

Session cookie `tsid` (HttpOnly, Path=/). 32 concurrent sessions, 24 h idle
expiry, kept in the API's memory. All users share password `demo`.

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
  "projects": [{"id":1,"key":"WEB","name":"Web App","color":"#5b8def"},
               {"id":2,"key":"IOS","name":"iOS App","color":"#3dbf8a"},
               {"id":3,"key":"API","name":"Platform API","color":"#f0a05a"}],
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
  "id": 42, "key": "WEB-42", "project": 1,
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

Activity object: `{"id":n,"ts":...,"actor":userId,"issue":42,"key":"WEB-42","op":"create|update|comment|delete","field":"status","from":"todo","to":"done"}`

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
{"items":[{"key":"WEB-12","title":"...","status":"todo","priority":2,"assignee":1,"updated":..,"score":17}],
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
{"issues":2400,"comments":4800,"users":8,"sessions":2,
 "loading":{"done":4032,"total":9001},
 "storage":{"state":"loading","reachable":true,"log":"9c96cb91","seq":4032,"writes":0,"write_errors":0,"conflicts":0,"load_ms":0},
 "index":{"terms":306,"postings":54843},
 "memory":{"used":1083120,"limit":1310720},
 "requests":{"total":n,"errors":n,"avg_us":n,"p99_us":n,"last_us":n},
 "uptime_ms":n,"now":n}
```

`loading` is present until every record of `tessera-db` is loaded (`done` and
`total` count records). `storage.state` is `connecting`, `loading`, `ready` or
`failed` (a damaged record stopped the load; `storage.error` names it).

## Live events (SSE)

`GET /events` served by the ingress on the control plane. `text/event-stream`, retry 2000.
Event types:

- `issue`: `{"op":"create|update|delete","key":"WEB-42","status":"todo","actor":3,"title":"..."}`
- `comment`: `{"key":"WEB-42","author":3,"id":7}`
- `hud`: `{"text":"typed on the keyboard so far"}` (from the keypad of `tessera-hud`)
- `cluster`: node telemetry every 2 s, same shape as `GET /api/cluster`
- `presence`: `{"viewers":n}`

Clients should refetch the affected view on `issue`/`comment` rather than
patching local state, except for the board which may move the card directly.

## Cluster telemetry (served by the ingress)

`GET /api/cluster` -> 200

```json
{"nodes":[{"name":"esp32-node-01","role":"control-plane","free_heap":n,"min_free_heap":n,"free_psram":n,"uptime_ms":n,"pods":["tessera-hud"],"ready":true}, {...}],
 "ingress":{"rps":n,"p50_us":n,"p99_us":n,"active_sse":n,"workers":4,"requests":n},
 "link":{"espnow_frames_rx":n,"espnow_frames_tx":n,"crc_errors":n,"reassembled":n},
 "now":n}
```

## Keyboard input path

`tessera-hud` emits `{"t":"key","c":"a"}` on channel `tessera.input` for every
key press. `tessera-api` receives it through `k32_event`, appends it to the
"live note" of the most recently viewed issue (or issue id 1), and emits a `hud`
event with the accumulated text. Enter stores the text as a comment by the
`keypad` user (id 0) and clears the note; a note that cannot be stored yet
(loading, `tessera-db` unreachable) is dropped. Backspace deletes one character.
