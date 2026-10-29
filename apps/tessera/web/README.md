# tessera-web

The Tessera web client (ES2020 JavaScript, `index.html`, `app.js` and `styles.css`, no external dependencies) and `tessera-web`, the pod that serves it.

## The pod

`server.c` maps request paths to files of the pod's image bundle and sends them with `k32_resp_asset`; the host streams them from flash and picks the `.gz` variant when the client accepts gzip.

| Request | Response | `Cache-Control` |
|---|---|---|
| `GET /`, `GET /index.html` | `index.html` | `no-cache` |
| `GET /<name>.<ext>?v=<hash>` | the bundle file | `public, max-age=31536000, immutable` |
| `GET /<name>.<ext>` | the bundle file | `public, max-age=3600` |
| `GET /<path without an extension>` | `index.html` (client routes) | `no-cache` |
| a file the bundle lacks, `..`, encoded or other unsafe characters, `/api/...`, `/events` | `404` | `no-store` |
| other methods than `GET` and `HEAD` | `405` | `no-store` |

Linear memory: 512-byte stack (unused by the code), 237 bytes of data, request blocks in the rest of the 4 KiB cap (`MEM_CAP`, pod limit `4Ki`). `make wasm` fails when the layout does not fit.

## Build

```sh
cd apps/tessera/web
make              # bundle + ../../../images/tessera-web.wasm
make bundle       # python3 build.py: ../../../images/web/tessera-web/*.gz
make test         # native tests of the path mapping against the bundle
```

`build.py` writes `index.html.gz`, `app.js.gz` and `styles.css.gz` (`gzip -9`, zeroed modification times) into `images/web/tessera-web/`, the bundle of the embedded image `tessera-web`. It rewrites the links in `index.html` to `/app.js?v=<hash>` and `/styles.css?v=<hash>` (first 10 hex digits of the SHA-256 of the file), so browsers keep those files for a year and fetch new ones after a change. The script prints raw and compressed sizes and exits 1 over the size budget:

| Metric | Budget |
|---|---|
| Raw total | < 150 KB |
| Gzip -9 total | < 42 KB |

## Run the mock server

Start the in-memory mock server for UI development:

```sh
cd apps/tessera/web
python3 mock_server.py            # http://127.0.0.1:8787/
python3 mock_server.py --issues 3000 --port 8080 -v
```

The server uses the Python standard library to implement `../API.md` with 600 generated issues, `tsid` session cookies, `/api/cluster`, and Server-Sent Events on `/events`. Every user has the password `demo`. It serves `index.html`, `app.js`, and `styles.css` from this directory. Reload the browser to see file edits.

## Routes

The client uses a hash router. The project switcher scopes the board, issue list, and analytics, and stores the selection in `localStorage`.

| Route | View |
|---|---|
| `#/board` | Kanban board, 5 status columns, drag and drop |
| `#/issues` | Issue table with filters, sort, server pagination, search |
| `#/issues?status=todo,review&assignee=3&label=0&priority=4&q=wifi&sort=priority&order=desc&page=2` | Filtered issue table |
| `#/issue/WEB-42` | Issue details, editable title and description, properties, comments, activity |
| `#/analytics` | SVG charts from `/api/analytics` for 7, 14, or 30 days |
| `#/activity` | Global activity feed |
| `#/settings` | Account, keyboard shortcuts, sign out |
| `#/settings/infra` | Cluster telemetry, engine stats (with the load progress and the storage state), in-browser load test |
