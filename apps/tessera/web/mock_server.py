#!/usr/bin/env python3
"""Tessera mock API server (standard library only).

Implements apps/tessera/API.md in memory with a seeded dataset, session cookies,
SSE on /events and synthetic cluster telemetry. Serves index.html, app.js and
styles.css from this directory on http://127.0.0.1:8787/.

    python3 mock_server.py [--port 8787] [--issues 600] [--seed 7]
"""
import argparse
import json
import os
import queue
import random
import re
import secrets
import sys
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
EPOCH_BASE_MS = 1793059200000  # synthetic epoch (2026-10-27T00:00:00Z)
BOOT = time.monotonic()
STATUSES = ["backlog", "todo", "in_progress", "review", "done"]
PRIORITIES = ["none", "low", "medium", "high", "urgent"]
SORTS = ("updated", "created", "priority", "key")
SESSION_LIMIT = 32
SESSION_IDLE_MS = 24 * 3600 * 1000
BOARD_LIMIT = 40
MEM_LIMIT = 1572864

USERS = [
    {"id": 1, "username": "mara", "name": "Mara Okafor", "initials": "MO", "color": "#f0a05a"},
    {"id": 2, "username": "teo", "name": "Teodor Lindqvist", "initials": "TL", "color": "#5b8def"},
    {"id": 3, "username": "ines", "name": "Ines Carvalho", "initials": "IC", "color": "#4cc38a"},
    {"id": 4, "username": "kenji", "name": "Kenji Watanabe", "initials": "KW", "color": "#e5484d"},
    {"id": 5, "username": "ruth", "name": "Ruth Adeyemi", "initials": "RA", "color": "#b36ae2"},
    {"id": 6, "username": "pavel", "name": "Pavel Horak", "initials": "PH", "color": "#3fb8c9"},
    {"id": 7, "username": "aya", "name": "Aya Haddad", "initials": "AH", "color": "#e59f3a"},
    {"id": 8, "username": "noor", "name": "Noor Siddiqui", "initials": "NS", "color": "#d35fa0"},
]
HUD_USER = {"id": 0, "username": "hud", "name": "Device keyboard", "initials": "HK", "color": "#8b949e"}
PROJECTS = [
    {"id": 1, "key": "TES", "name": "Tessera Core", "color": "#5b8def"},
    {"id": 2, "key": "HUD", "name": "Display & Input", "color": "#e59f3a"},
    {"id": 3, "key": "ING", "name": "Ingress", "color": "#4cc38a"},
]
LABELS = [
    {"id": 0, "name": "bug", "color": "#e5484d"},
    {"id": 1, "name": "feature", "color": "#5b8def"},
    {"id": 2, "name": "perf", "color": "#e59f3a"},
    {"id": 3, "name": "wifi", "color": "#3fb8c9"},
    {"id": 4, "name": "espnow", "color": "#b36ae2"},
    {"id": 5, "name": "wasm", "color": "#4cc38a"},
    {"id": 6, "name": "ui", "color": "#d35fa0"},
    {"id": 7, "name": "docs", "color": "#8b949e"},
    {"id": 8, "name": "memory", "color": "#f0a05a"},
    {"id": 9, "name": "ingress", "color": "#2fa37a"},
    {"id": 10, "name": "security", "color": "#c94f4f"},
    {"id": 11, "name": "tech-debt", "color": "#7b8794"},
]

SUBJECTS = ["WiFi", "ESP-NOW link", "SSE stream", "ingress worker", "PSRAM allocator", "Wasm sandbox",
            "session cookie", "board view", "search index", "keyboard matrix", "LCD framebuffer",
            "metrics endpoint", "flash reader", "gzip streamer", "scheduler", "watchdog", "OTA updater",
            "heap monitor", "comment composer", "analytics job", "activity log", "label picker",
            "pagination", "JSON encoder", "ring buffer", "mDNS responder", "NTP fallback", "SPI DMA path",
            "task queue", "event bus"]
PROBLEMS = ["timeout after reconnect", "drops frames under load", "leaks 48 bytes per request",
            "returns stale data", "crashes on empty body", "mis-ranks prefix matches", "ignores limit",
            "double-counts resolved issues", "truncates UTF-8 titles", "stalls when heap is fragmented",
            "renders garbage after rotate", "misses key repeat", "races with the SSE writer",
            "blocks the watchdog", "reports negative uptime", "CRC errors above 40 fps",
            "p99 spikes every 30 s", "cookie not cleared on logout", "wrong colour on done column",
            "panics on 1000-char description"]
FEATURES = ["Add", "Support", "Expose", "Cache", "Compress", "Stream", "Batch", "Throttle", "Document",
            "Refactor", "Instrument", "Harden", "Profile", "Migrate", "Simplify"]
OBJECTS = ["issue export", "label colours", "cycle-time report", "keyboard shortcuts", "board truncation",
           "presence counter", "per-node memory graph", "search prefix matching", "multi-project filter",
           "comment editing", "HUD beep volume", "dark theme contrast", "request tracing", "cold boot time",
           "flash wear levelling", "assignee workload view", "estimate rollups", "activity pagination"]
WORDS = ("the ingress proxies requests to the core pod over esp-now and reassembles frames before "
         "handing them to the wasm sandbox we should verify that the index stays consistent when a "
         "comment is appended while a search is running memory is tight so prefer streaming json "
         "repro steps open the board drag a card to review then reload the page expected the card "
         "stays actual it snaps back after two seconds see the attached trace from node one the "
         "watchdog fired twice during the load test p99 went above forty milliseconds").split()
COMMENTS = ["Reproduced on node-02 after a cold boot.", "Looks related to the allocator change last week.",
            "Can we get a trace from the ingress for this?", "Fixed in the latest build, please verify.",
            "Still seeing this at 30 rps.", "Moving to review, PR is up.", "Not a bug: limit is capped at 50 by design.",
            "Bumping priority, it blocks the demo.", "Added a regression test for the prefix matcher.",
            "Heap fragmentation confirmed with the PSRAM monitor.", "Deferring until the OTA work lands.",
            "Nice catch, thanks.", "I can take this one.", "Duplicate of an earlier report? Linking.",
            "Keyboard matrix needs a debounce of at least 8 ms."]


class Store:
    def __init__(self, n_issues, seed):
        self.lock = threading.RLock()
        self.rng = random.Random(seed)
        self.issues = {}
        self.by_key = {}
        self.seq = {p["id"]: 0 for p in PROJECTS}
        self.comments = {}
        self.activity = []
        self.next_issue = 1
        self.next_comment = 1
        self.next_activity = 1
        self.sessions = {}
        self.sessions_lock = threading.Lock()
        self.req_total = 0
        self.req_errors = 0
        self.req_sum_us = 0
        self.req_last_us = 0
        self.lat = []
        self.hud_text = ""
        self.last_viewed_key = "TES-1"
        self.subs = []
        self.subs_lock = threading.Lock()
        self.ingress_window = []
        self.link = {"espnow_frames_rx": 182340, "espnow_frames_tx": 176001, "crc_errors": 3, "reassembled": 41220}
        self.seed(n_issues)

    # -- time -------------------------------------------------------------
    @staticmethod
    def now():
        return EPOCH_BASE_MS + int((time.monotonic() - BOOT) * 1000)

    # -- seeding -----------------------------------------------------------
    def seed(self, n):
        r = self.rng
        now = self.now()
        span = 60 * 86400000
        for i in range(n):
            created = now - int(r.random() ** 1.6 * span)
            status = r.choices(STATUSES, weights=[22, 20, 14, 8, 36])[0]
            age = now - created
            updated = created + int(r.random() * age)
            if status == "done":
                updated = created + int((r.expovariate(1 / 50.0) + 2) * 3600000)
                if updated > now - 600000:
                    updated = created + int(r.random() * age)
            if r.random() < 0.55:
                title = f"{r.choice(SUBJECTS)} {r.choice(PROBLEMS)}"
                labels = [0] + ([r.randrange(2, len(LABELS))] if r.random() < 0.6 else [])
            else:
                title = f"{r.choice(FEATURES)} {r.choice(OBJECTS)}"
                labels = [1] + ([r.randrange(2, len(LABELS))] if r.random() < 0.6 else [])
            labels = sorted(set(labels))
            desc = " ".join(r.choice(WORDS) for _ in range(r.randint(12, 60)))
            desc = desc[0].upper() + desc[1:] + "."
            project = r.choices([1, 2, 3], weights=[6, 2, 2])[0]
            assignee = r.choice([None] + [u["id"] for u in USERS] * 3)
            issue = {
                "id": self.next_issue, "project": project, "title": title, "description": desc,
                "status": status, "priority": r.choices([0, 1, 2, 3, 4], weights=[10, 25, 35, 22, 8])[0],
                "assignee": assignee, "reporter": r.choice(USERS)["id"],
                "labels": labels, "estimate": r.choice([None, 1, 2, 3, 5, 8, 13]),
                "created": created, "updated": updated, "comment_count": 0,
            }
            issue["key"] = self.key_for(issue)
            self.issues[issue["id"]] = issue
            self.next_issue += 1
            self.activity.append({"id": self.next_activity, "ts": created, "actor": issue["reporter"],
                                  "issue": issue["id"], "key": issue["key"], "op": "create",
                                  "field": None, "from": None, "to": None})
            self.next_activity += 1
            for _ in range(r.choices([0, 1, 2, 3, 5], weights=[40, 25, 18, 10, 7])[0]):
                ts = created + int(r.random() * max(1, updated - created))
                c = {"id": self.next_comment, "issue": issue["id"], "author": r.choice(USERS)["id"],
                     "body": r.choice(COMMENTS), "created": ts}
                self.comments.setdefault(issue["id"], []).append(c)
                self.next_comment += 1
                issue["comment_count"] += 1
                self.activity.append({"id": self.next_activity, "ts": ts, "actor": c["author"],
                                      "issue": issue["id"], "key": issue["key"], "op": "comment",
                                      "field": None, "from": None, "to": None})
                self.next_activity += 1
            if status != "backlog":
                self.activity.append({"id": self.next_activity, "ts": updated, "actor": assignee or issue["reporter"],
                                      "issue": issue["id"], "key": issue["key"], "op": "update",
                                      "field": "status", "from": "backlog", "to": status})
                self.next_activity += 1
        self.activity.sort(key=lambda a: a["ts"])
        for i, a in enumerate(self.activity, 1):
            a["id"] = i
        self.next_activity = len(self.activity) + 1

    def key_for(self, issue):
        """Assign the next sequential key inside the issue's project (TES-1, TES-2, HUD-1...)."""
        pk = next(p["key"] for p in PROJECTS if p["id"] == issue["project"])
        self.seq[issue["project"]] += 1
        key = f"{pk}-{self.seq[issue['project']]}"
        if issue.get("key"):
            self.by_key.pop(issue["key"], None)
        self.by_key[key] = issue
        return key

    def find(self, key):
        return self.by_key.get((key or "").upper())

    # -- sessions ----------------------------------------------------------
    def login(self, username, password):
        user = next((u for u in USERS if u["username"] == username), None)
        if not user or password != "demo":
            return None
        with self.sessions_lock:
            now = self.now()
            for sid in [s for s, v in self.sessions.items() if now - v["seen"] > SESSION_IDLE_MS]:
                del self.sessions[sid]
            if len(self.sessions) >= SESSION_LIMIT:
                oldest = min(self.sessions, key=lambda s: self.sessions[s]["seen"])
                del self.sessions[oldest]
            sid = secrets.token_urlsafe(18)
            self.sessions[sid] = {"user": user["id"], "seen": now}
            return sid, user

    def session_user(self, sid):
        if not sid:
            return None
        with self.sessions_lock:
            s = self.sessions.get(sid)
            if not s:
                return None
            if self.now() - s["seen"] > SESSION_IDLE_MS:
                del self.sessions[sid]
                return None
            s["seen"] = self.now()
            return next(u for u in USERS if u["id"] == s["user"])

    def logout(self, sid):
        with self.sessions_lock:
            self.sessions.pop(sid, None)

    # -- search ------------------------------------------------------------
    @staticmethod
    def tokens(text):
        return re.findall(r"[a-z0-9]+", (text or "").lower())

    def score(self, issue, terms):
        """Term-frequency score weighted by field; the last term matches as a prefix."""
        if not terms:
            return 0
        title_t = self.tokens(issue["title"])
        desc_t = self.tokens(issue["description"])
        com_t = []
        for c in self.comments.get(issue["id"], []):
            com_t.extend(self.tokens(c["body"]))
        total = 0
        for i, term in enumerate(terms):
            prefix = i == len(terms) - 1
            match = (lambda t: t.startswith(term)) if prefix else (lambda t: t == term)
            tf = 3 * sum(1 for t in title_t if match(t)) + sum(1 for t in desc_t if match(t)) + sum(1 for t in com_t if match(t))
            if tf == 0:
                return 0
            total += tf
        age_days = (self.now() - issue["updated"]) / 86400000
        return total + max(0, 5 - int(age_days / 3))

    def search(self, q, limit):
        terms = self.tokens(q)
        hits = []
        scanned = 0
        for iss in self.issues.values():
            scanned += 1
            s = self.score(iss, terms)
            if s > 0:
                hits.append((s, iss))
        hits.sort(key=lambda h: (-h[0], -h[1]["updated"]))
        items = [{"key": i["key"], "title": i["title"], "status": i["status"], "priority": i["priority"],
                  "assignee": i["assignee"], "updated": i["updated"], "score": s} for s, i in hits[:limit]]
        return items, scanned, len(hits)

    # -- events ------------------------------------------------------------
    def subscribe(self):
        q = queue.Queue()
        with self.subs_lock:
            self.subs.append(q)
            n = len(self.subs)
        self.broadcast("presence", {"viewers": n})
        return q

    def unsubscribe(self, q):
        with self.subs_lock:
            if q in self.subs:
                self.subs.remove(q)
            n = len(self.subs)
        self.broadcast("presence", {"viewers": n})

    def broadcast(self, event, data):
        payload = f"event: {event}\ndata: {json.dumps(data, separators=(',', ':'))}\n\n"
        with self.subs_lock:
            subs = list(self.subs)
        for q in subs:
            q.put(payload)

    def viewers(self):
        with self.subs_lock:
            return len(self.subs)

    # -- metrics -----------------------------------------------------------
    def record(self, us, error):
        with self.lock:
            self.req_total += 1
            self.req_errors += 1 if error else 0
            self.req_sum_us += us
            self.req_last_us = us
            self.lat.append(us)
            if len(self.lat) > 2000:
                del self.lat[:1000]
            t = time.monotonic()
            self.ingress_window.append((t, us))
            cutoff = t - 5
            while self.ingress_window and self.ingress_window[0][0] < cutoff:
                self.ingress_window.pop(0)

    def stats(self):
        with self.lock:
            lat = sorted(self.lat)
            p99 = lat[int(len(lat) * 0.99)] if lat else 0
            terms = set()
            postings = 0
            for iss in self.issues.values():
                t = set(self.tokens(iss["title"])) | set(self.tokens(iss["description"]))
                terms |= t
                postings += len(t)
            used = min(MEM_LIMIT, 420000 + len(self.issues) * 1100 + sum(len(c) for c in self.comments.values()) * 160 + postings * 6)
            return {"issues": len(self.issues), "comments": sum(len(c) for c in self.comments.values()),
                    "users": len(USERS), "sessions": len(self.sessions),
                    "index": {"terms": len(terms), "postings": postings},
                    "memory": {"used": used, "limit": MEM_LIMIT},
                    "requests": {"total": self.req_total, "errors": self.req_errors,
                                 "avg_us": int(self.req_sum_us / self.req_total) if self.req_total else 0,
                                 "p99_us": p99, "last_us": self.req_last_us},
                    "uptime_ms": self.now() - EPOCH_BASE_MS, "now": self.now()}

    def cluster(self):
        r = random.random
        up = self.now() - EPOCH_BASE_MS
        with self.lock:
            cutoff = time.monotonic() - 5
            while self.ingress_window and self.ingress_window[0][0] < cutoff:
                self.ingress_window.pop(0)
            win = list(self.ingress_window)
        rps = round(len(win) / 5.0, 1)
        ls = sorted(u for _, u in win)
        p50 = ls[len(ls) // 2] if ls else 0
        p99 = ls[int(len(ls) * 0.99)] if ls else 0
        self.link["espnow_frames_rx"] += int(r() * 12) + 2
        self.link["espnow_frames_tx"] += int(r() * 12) + 2
        self.link["reassembled"] += int(r() * 3)
        if r() < 0.02:
            self.link["crc_errors"] += 1
        return {
            "nodes": [
                {"name": "esp32-node-01", "role": "control-plane", "free_heap": 118000 + int(r() * 9000),
                 "min_free_heap": 96400, "free_psram": 0, "uptime_ms": up + 4210, "pods": ["tessera-hud", "ingress"], "ready": True},
                {"name": "esp32-node-02", "role": "worker", "free_heap": 162000 + int(r() * 14000),
                 "min_free_heap": 141200, "free_psram": 6_120_000 + int(r() * 90000), "uptime_ms": up,
                 "pods": ["tessera-core"], "ready": True},
            ],
            "ingress": {"rps": rps, "p50_us": p50, "p99_us": p99, "active_sse": self.viewers(), "workers": 4,
                        "requests": self.req_total},
            "link": dict(self.link),
            "now": self.now(),
        }

    # -- mutations ---------------------------------------------------------
    def log(self, actor, issue, op, field=None, frm=None, to=None):
        a = {"id": self.next_activity, "ts": self.now(), "actor": actor, "issue": issue["id"],
             "key": issue["key"], "op": op, "field": field, "from": frm, "to": to}
        self.next_activity += 1
        self.activity.append(a)
        if len(self.activity) > 20000:
            del self.activity[:5000]
        return a

    def hud_key(self, ch):
        """Simulate a key press from the HUD pod's keyboard."""
        with self.lock:
            if ch == "\n":
                text = self.hud_text.strip()
                self.hud_text = ""
                iss = self.find(self.last_viewed_key) or self.issues.get(min(self.issues)) if self.issues else None
                if text and iss:
                    self.add_comment(iss, 0, text[:280])
                self.broadcast("hud", {"text": ""})
                return
            if ch == "\b":
                self.hud_text = self.hud_text[:-1]
            else:
                self.hud_text += ch
            self.broadcast("hud", {"text": self.hud_text})

    def add_comment(self, iss, author, body):
        c = {"id": self.next_comment, "issue": iss["id"], "author": author, "body": body, "created": self.now()}
        self.next_comment += 1
        self.comments.setdefault(iss["id"], []).append(c)
        iss["comment_count"] += 1
        iss["updated"] = self.now()
        self.log(author, iss, "comment")
        self.broadcast("comment", {"key": iss["key"], "author": author, "id": c["id"]})
        return c


# ---------------------------------------------------------------------------
def summary(i):
    return {k: v for k, v in i.items() if k != "description"}


def validate_issue_fields(body, partial):
    out = {}
    if "title" in body or not partial:
        t = body.get("title")
        if not isinstance(t, str) or not 1 <= len(t.strip()) <= 120:
            return None, "title must be 1..120 characters"
        out["title"] = t.strip()
    if "description" in body:
        d = body.get("description")
        if d is None:
            d = ""
        if not isinstance(d, str) or len(d) > 1000:
            return None, "description must be <= 1000 characters"
        out["description"] = d
    if "project" in body:
        if body["project"] not in [p["id"] for p in PROJECTS]:
            return None, "unknown project"
        out["project"] = body["project"]
    if "status" in body:
        if body["status"] not in STATUSES:
            return None, "unknown status"
        out["status"] = body["status"]
    if "priority" in body:
        p = body["priority"]
        if not isinstance(p, int) or not 0 <= p <= 4:
            return None, "priority must be 0..4"
        out["priority"] = p
    if "assignee" in body:
        a = body["assignee"]
        if a is not None and a not in [u["id"] for u in USERS]:
            return None, "unknown assignee"
        out["assignee"] = a
    if "labels" in body:
        ls = body["labels"]
        if not isinstance(ls, list) or any(l not in [x["id"] for x in LABELS] for l in ls):
            return None, "unknown label"
        out["labels"] = sorted(set(ls))
    if "estimate" in body:
        e = body["estimate"]
        if e is not None and (not isinstance(e, int) or e < 0 or e > 999):
            return None, "estimate must be 0..999"
        out["estimate"] = e
    return out, None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    store = None  # set by main
    server_version = "kube32-ingress/0.4"

    def log_message(self, fmt, *args):
        if self.server.verbose:
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    # -- helpers -----------------------------------------------------------
    def cookie(self):
        raw = self.headers.get("Cookie", "")
        for part in raw.split(";"):
            k, _, v = part.strip().partition("=")
            if k == "tsid":
                return v
        return None

    def send_json(self, status, obj, headers=None):
        data = json.dumps(obj, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)
        self._status = status

    def error(self, status, code, message):
        self.send_json(status, {"error": code, "message": message})

    def read_body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n > 64 * 1024:
            raise ValueError("body too large")
        raw = self.rfile.read(n) if n else b""
        if not raw:
            return {}
        obj = json.loads(raw.decode("utf-8"))
        if not isinstance(obj, dict):
            raise ValueError("object expected")
        return obj

    def serve_static(self, path):
        name = {"/": "index.html", "/index.html": "index.html", "/app.js": "app.js", "/styles.css": "styles.css"}.get(path)
        if not name:
            return self.error(404, "not_found", "no such file")
        ctype = {"html": "text/html; charset=utf-8", "js": "application/javascript; charset=utf-8",
                 "css": "text/css; charset=utf-8"}[name.rsplit(".", 1)[1]]
        try:
            with open(os.path.join(HERE, name), "rb") as f:
                data = f.read()
        except OSError:
            return self.error(404, "not_found", f"{name} missing")
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(data)
        self._status = 200

    # -- dispatch ----------------------------------------------------------
    def do_GET(self):
        self.dispatch()

    def do_POST(self):
        self.dispatch()

    def do_PATCH(self):
        self.dispatch()

    def do_DELETE(self):
        self.dispatch()

    def dispatch(self):
        t0 = time.perf_counter()
        self._status = 200
        parts = urlsplit(self.path)
        path = parts.path
        qs = {k: v[-1] for k, v in parse_qs(parts.query).items()}
        try:
            if path == "/events":
                return self.sse()
            if not path.startswith("/api/"):
                if self.command != "GET":
                    return self.error(405, "method_not_allowed", "GET only")
                return self.serve_static(path)
            self.api(path, qs)
        except (ValueError, json.JSONDecodeError) as e:
            self.error(400, "bad_request", str(e))
        except BrokenPipeError:
            pass
        finally:
            if path.startswith("/api/"):
                us = int((time.perf_counter() - t0) * 1e6)
                # mimic a microcontroller: add a little floor latency to the reported number
                self.store.record(us + 180, self._status >= 400)

    def api(self, path, qs):
        st = self.store
        sid = self.cookie()
        if path == "/api/auth/login" and self.command == "POST":
            body = self.read_body()
            res = st.login(str(body.get("username", "")), str(body.get("password", "")))
            if not res:
                return self.error(401, "unauthorized", "bad username or password")
            sid, user = res
            return self.send_json(200, {"user": user, "now": st.now()},
                                  {"Set-Cookie": f"tsid={sid}; HttpOnly; Path=/; SameSite=Lax"})
        if path == "/api/bootstrap" and self.command == "GET":
            with st.lock:
                by_status = {s: 0 for s in STATUSES}
                for i in st.issues.values():
                    by_status[i["status"]] += 1
                return self.send_json(200, {
                    "now": st.now(), "users": USERS, "projects": PROJECTS, "labels": LABELS,
                    "statuses": STATUSES, "priorities": PRIORITIES,
                    "counts": {"open": len(st.issues) - by_status["done"], "total": len(st.issues), "by_status": by_status}})
        if path == "/api/cluster" and self.command == "GET":
            return self.send_json(200, st.cluster())

        user = st.session_user(sid)
        if not user:
            return self.error(401, "unauthorized", "login required")

        if path == "/api/auth/logout" and self.command == "POST":
            st.logout(sid)
            self.send_response(204)
            self.send_header("Set-Cookie", "tsid=; Max-Age=0; Path=/")
            self.send_header("Content-Length", "0")
            self.end_headers()
            self._status = 204
            return
        if path == "/api/me" and self.command == "GET":
            return self.send_json(200, {"user": user, "now": st.now()})
        if path == "/api/stats" and self.command == "GET":
            return self.send_json(200, st.stats())
        if path == "/api/activity" and self.command == "GET":
            limit = max(1, min(200, int(qs.get("limit", 50))))
            with st.lock:
                items = list(reversed(st.activity[-limit:]))
            return self.send_json(200, {"items": items, "now": st.now()})
        if path == "/api/search" and self.command == "GET":
            t0 = time.perf_counter()
            limit = max(1, min(50, int(qs.get("limit", 20))))
            with st.lock:
                items, scanned, total = st.search(qs.get("q", ""), limit)
            return self.send_json(200, {"items": items, "took_us": int((time.perf_counter() - t0) * 1e6),
                                        "scanned": scanned, "total": total, "now": st.now()})
        if path == "/api/board" and self.command == "GET":
            return self.board(qs)
        if path == "/api/analytics" and self.command == "GET":
            return self.analytics(qs)
        if path == "/api/issues":
            if self.command == "GET":
                return self.list_issues(qs)
            if self.command == "POST":
                return self.create_issue(user)
        m = re.fullmatch(r"/api/issues/([A-Za-z]+-\d+)(/comments)?", path)
        if m:
            with st.lock:
                iss = st.find(m.group(1))
                if not iss:
                    return self.error(404, "not_found", f"issue {m.group(1)} does not exist")
                if m.group(2):
                    if self.command != "POST":
                        return self.error(405, "method_not_allowed", "POST only")
                    body = self.read_body()
                    text = body.get("body")
                    if not isinstance(text, str) or not 1 <= len(text.strip()) <= 280:
                        return self.error(400, "bad_request", "comment body must be 1..280 characters")
                    c = st.add_comment(iss, user["id"], text.strip())
                    return self.send_json(201, c)
                if self.command == "GET":
                    st.last_viewed_key = iss["key"]
                    full = dict(iss)
                    full["comments"] = list(st.comments.get(iss["id"], []))
                    full["activity"] = [a for a in reversed(st.activity) if a["issue"] == iss["id"]][:20]
                    full["now"] = st.now()
                    return self.send_json(200, full)
                if self.command == "PATCH":
                    body = self.read_body()
                    fields, err = validate_issue_fields(body, partial=True)
                    if err:
                        return self.error(400, "bad_request", err)
                    if not fields:
                        return self.error(400, "bad_request", "no editable fields")
                    changed = False
                    for k, v in fields.items():
                        if iss.get(k) != v:
                            st.log(user["id"], iss, "update", k, iss.get(k), v)
                            iss[k] = v
                            changed = True
                    if "project" in fields:
                        iss["key"] = st.key_for(iss)
                    if changed:
                        iss["updated"] = st.now()
                        st.broadcast("issue", {"op": "update", "key": iss["key"], "status": iss["status"],
                                               "actor": user["id"], "title": iss["title"]})
                    full = dict(iss)
                    full["now"] = st.now()
                    return self.send_json(200, full)
                if self.command == "DELETE":
                    st.log(user["id"], iss, "delete")
                    del st.issues[iss["id"]]
                    st.by_key.pop(iss["key"], None)
                    st.comments.pop(iss["id"], None)
                    st.broadcast("issue", {"op": "delete", "key": iss["key"], "status": iss["status"],
                                           "actor": user["id"], "title": iss["title"]})
                    self.send_response(204)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    self._status = 204
                    return
        return self.error(404, "not_found", f"no route for {self.command} {path}")

    # -- routes ------------------------------------------------------------
    def list_issues(self, qs):
        st = self.store
        t0 = time.perf_counter()
        with st.lock:
            items = list(st.issues.values())
            if qs.get("project"):
                items = [i for i in items if i["project"] == int(qs["project"])]
            if qs.get("status"):
                ss = set(qs["status"].split(","))
                items = [i for i in items if i["status"] in ss]
            if qs.get("assignee") not in (None, ""):
                a = qs["assignee"]
                aid = None if a in ("none", "null", "0") else int(a)
                items = [i for i in items if i["assignee"] == aid]
            if qs.get("label") not in (None, ""):
                items = [i for i in items if int(qs["label"]) in i["labels"]]
            if qs.get("priority") not in (None, ""):
                items = [i for i in items if i["priority"] == int(qs["priority"])]
            if qs.get("q"):
                terms = st.tokens(qs["q"])
                scored = [(st.score(i, terms), i) for i in items]
                items = [i for s, i in scored if s > 0]
            sort = qs.get("sort", "updated")
            if sort not in SORTS:
                return self.error(400, "bad_request", "bad sort")
            desc = qs.get("order", "desc") != "asc"
            keyf = {"updated": lambda i: i["updated"], "created": lambda i: i["created"],
                    "priority": lambda i: (i["priority"], i["updated"]), "key": lambda i: i["id"]}[sort]
            items.sort(key=keyf, reverse=desc)
            total = len(items)
            limit = max(1, min(50, int(qs.get("limit", 50))))
            page = max(1, int(qs.get("page", 1)))
            page_items = [summary(i) for i in items[(page - 1) * limit:page * limit]]
        self.send_json(200, {"items": page_items, "total": total, "page": page, "limit": limit,
                             "took_us": int((time.perf_counter() - t0) * 1e6), "now": st.now()})

    def create_issue(self, user):
        st = self.store
        body = self.read_body()
        fields, err = validate_issue_fields(body, partial=False)
        if err:
            return self.error(400, "bad_request", err)
        with st.lock:
            now = st.now()
            iss = {"id": st.next_issue, "project": fields.get("project", 1), "title": fields["title"],
                   "description": fields.get("description", ""), "status": fields.get("status", "backlog"),
                   "priority": fields.get("priority", 0), "assignee": fields.get("assignee"),
                   "reporter": user["id"], "labels": fields.get("labels", []), "estimate": fields.get("estimate"),
                   "created": now, "updated": now, "comment_count": 0}
            iss["key"] = st.key_for(iss)
            st.next_issue += 1
            st.issues[iss["id"]] = iss
            st.log(user["id"], iss, "create")
            st.broadcast("issue", {"op": "create", "key": iss["key"], "status": iss["status"],
                                   "actor": user["id"], "title": iss["title"]})
            full = dict(iss)
            full["comments"] = []
            full["activity"] = [a for a in reversed(st.activity) if a["issue"] == iss["id"]][:20]
            full["now"] = now
        self.send_json(201, full)

    def board(self, qs):
        st = self.store
        with st.lock:
            items = list(st.issues.values())
            if qs.get("project"):
                items = [i for i in items if i["project"] == int(qs["project"])]
            cols = {s: [] for s in STATUSES}
            for i in items:
                cols[i["status"]].append(i)
            truncated = {}
            for s in STATUSES:
                cols[s].sort(key=lambda i: (i["priority"], i["updated"]), reverse=True)
                if len(cols[s]) > BOARD_LIMIT:
                    truncated[s] = True
                cols[s] = [summary(i) for i in cols[s][:BOARD_LIMIT]]
        self.send_json(200, {"now": st.now(), "columns": cols, "truncated": truncated, "limit": BOARD_LIMIT})

    def analytics(self, qs):
        st = self.store
        t0 = time.perf_counter()
        days = max(1, min(90, int(qs.get("days", 14))))
        with st.lock:
            now = st.now()
            items = list(st.issues.values())
            if qs.get("project"):
                items = [i for i in items if i["project"] == int(qs["project"])]
            by_status = {s: 0 for s in STATUSES}
            by_priority = [0] * 5
            by_user = {u["id"]: {"user": u["id"], "open": 0, "done": 0} for u in USERS}
            created = [0] * days
            resolved = [0] * days
            cycle = []
            buckets = {"lt1d": 0, "lt7d": 0, "lt30d": 0, "gte30d": 0}
            day0 = now - days * 86400000
            for i in items:
                by_status[i["status"]] += 1
                by_priority[i["priority"]] += 1
                done = i["status"] == "done"
                if i["assignee"] in by_user:
                    by_user[i["assignee"]]["done" if done else "open"] += 1
                d = int((i["created"] - day0) // 86400000)
                if 0 <= d < days:
                    created[d] += 1
                if done:
                    d = int((i["updated"] - day0) // 86400000)
                    if 0 <= d < days:
                        resolved[d] += 1
                    cycle.append((i["updated"] - i["created"]) / 3600000)
                else:
                    age = now - i["created"]
                    if age < 86400000:
                        buckets["lt1d"] += 1
                    elif age < 7 * 86400000:
                        buckets["lt7d"] += 1
                    elif age < 30 * 86400000:
                        buckets["lt30d"] += 1
                    else:
                        buckets["gte30d"] += 1
            cycle.sort()
            p = lambda q: round(cycle[min(len(cycle) - 1, int(len(cycle) * q))], 1) if cycle else 0
        self.send_json(200, {"now": now, "by_status": by_status, "by_priority": by_priority,
                             "by_assignee": list(by_user.values()), "created_per_day": created,
                             "resolved_per_day": resolved, "cycle_time_hours": {"p50": p(0.5), "p90": p(0.9)},
                             "open_age_buckets": buckets, "took_us": int((time.perf_counter() - t0) * 1e6)})

    # -- SSE ---------------------------------------------------------------
    def sse(self):
        st = self.store
        if not st.session_user(self.cookie()):
            return self.error(401, "unauthorized", "login required")
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        self.wfile.write(b"retry: 2000\n\n")
        self.wfile.flush()
        q = st.subscribe()
        try:
            self.wfile.write(f"event: cluster\ndata: {json.dumps(st.cluster(), separators=(',', ':'))}\n\n".encode())
            self.wfile.flush()
            while True:
                try:
                    msg = q.get(timeout=15)
                except queue.Empty:
                    msg = ": keepalive\n\n"
                self.wfile.write(msg.encode())
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            st.unsubscribe(q)
            self.close_connection = True


def background(store):
    """Synthetic cluster telemetry every 2 s and a HUD typing simulation every 15 s."""
    phrases = ["ack from node two", "heap looks fine", "ship it", "lcd flicker on boot", "retest at 30 rps"]
    rng = random.Random(1)
    tick = 0
    typing = []
    while True:
        time.sleep(1)
        tick += 1
        if tick % 2 == 0:
            store.broadcast("cluster", store.cluster())
        if tick % 15 == 0 and not typing:
            typing = list(rng.choice(phrases)) + ["\n"]
        if typing:
            # a few characters per second so browsers see the text grow
            for _ in range(min(3, len(typing))):
                store.hud_key(typing.pop(0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--issues", type=int, default=600)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    store = Store(args.issues, args.seed)
    Handler.store = store
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    srv.daemon_threads = True
    srv.verbose = args.verbose
    threading.Thread(target=background, args=(store,), daemon=True).start()
    print(f"tessera mock: http://{args.host}:{args.port}/  issues={len(store.issues)}  (password: demo)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
