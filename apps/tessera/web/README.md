# Tessera web client

Vanilla JavaScript (ES2020) frontend for the Tessera issue tracker. Three
files, no build step, no framework, no external requests. The firmware stores
the files gzip'd in flash and streams them with `Content-Encoding: gzip`.

## Run the mock server

```sh
cd apps/tessera/web
python3 mock_server.py            # http://127.0.0.1:8787/
python3 mock_server.py --issues 3000 --port 8080 -v
```

Standard library only. It implements `../API.md` in memory with a seeded
dataset (600 issues by default), `tsid` session cookies, SSE on `/events`
(issue/comment events on mutations, a `cluster` event every 2 s, a simulated
`hud` keyboard event every 15 s, `presence` on connect/disconnect) and
`/api/cluster`. Every seeded user has the password `demo`. The server also
serves `index.html`, `app.js` and `styles.css` from this directory, so edit
and reload. It doubles as the executable spec the Wasm core is compared to.

## Size budget

`python3 build.py` writes `dist/index.html.gz`, `dist/app.js.gz` and
`dist/styles.css.gz` (gzip -9, mtime zeroed for reproducible bytes), prints
raw and compressed sizes and exits 1 when over budget:

| | budget |
|---|---|
| raw, three files together | < 150 KB |
| gzip -9, three files together | < 42 KB |

Check a single file by hand with `gzip -9 -c app.js | wc -c`.

## Routes

Hash router; every route is a deep link.

| Route | View |
|---|---|
| `#/board` | Kanban board, 5 status columns, drag and drop (PATCH status, optimistic) |
| `#/issues` | Issue table with filters, sort, server pagination, search (`q`) |
| `#/issues?status=todo,review&assignee=3&label=0&priority=4&q=wifi&sort=priority&order=desc&page=2` | Filter state lives in the query |
| `#/issue/TES-42` | Issue page: editable title and description (PATCH on blur), properties, comments, activity |
| `#/analytics` | Hand-drawn SVG charts from `/api/analytics` (7/14/30 days) |
| `#/activity` | Global activity feed |
| `#/settings` | Account, keyboard shortcuts, sign out |
| `#/settings/infra` | Cluster telemetry, engine stats, in-browser load test |

The project switcher (sidebar, or top bar on phones) scopes the board, the
issue list and analytics; the choice is remembered in `localStorage`.

## Keyboard

`⌘K` / `Ctrl+K` / `/` command palette (search backed by `/api/search`),
`c` new issue, `j`/`k` move selection in the list, `Enter` open, `Esc` close,
`⌘Enter` send a comment or create an issue.

## Live updates

After login the client opens `EventSource('/events')`. `issue` and `comment`
events refetch the affected view (debounced; the board moves the card
directly), `hud` shows a toast with the text typed on the device keyboard,
`presence` updates the viewer count next to the live dot, `cluster` feeds the
Infrastructure page. The only polling is on the Infrastructure page
(`/api/stats` every 2 s, plus `/api/cluster` when the event stream is down).

## Conventions resolved against API.md

- Issue keys are sequential per project (`TES-1`, `HUD-1`), independent of the
  global `id`; `/api/issues/{key}` is case-insensitive.
- `/api/bootstrap`, `/api/auth/login` and `/api/cluster` are public; everything
  else, including `/events`, answers 401 without a valid `tsid`.
- Comments by the `hud` system user carry `author: 0`, which is not in
  `bootstrap.users`; the client renders it as "Device keyboard".
- `assignee=none` filters unassigned issues; `project` omitted means all.
- A PATCH that changes `project` re-keys the issue in the new project.
