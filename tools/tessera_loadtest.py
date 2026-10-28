#!/usr/bin/env python3
"""
Load test for the Tessera workload on a kube32 cluster.

Runs N concurrent HTTP clients against the ingress for a fixed duration and
reports measured latency percentiles, throughput and error counts per
endpoint. Nothing here is simulated: every number comes from a real request.

    tools/tessera_loadtest.py http://192.0.2.119 --clients 4 --seconds 60

Endpoints are weighted roughly like a browser session: lists and board views
dominate, writes are rare, search and analytics sit in between.
"""
import argparse
import http.client
import json
import random
import statistics
import sys
import threading
import time
from urllib.parse import urlparse

MIX = [
    ("GET", "/api/stats", 10),
    ("GET", "/api/issues?limit=25", 25),
    ("GET", "/api/issues?limit=25&page=2", 8),
    ("GET", "/api/issues?limit=25&status=in_progress", 8),
    ("GET", "/api/search?q=wifi", 10),
    ("GET", "/api/search?q=scheduler+crash", 5),
    ("GET", "/api/board?project=1", 10),
    ("GET", "/api/analytics?project=1&days=14", 5),
    ("GET", "/api/activity?limit=50", 8),
    ("GET", "/api/bootstrap", 3),
    ("POST", "/api/issues", 2),
    ("GET", "/api/issues/TES-42", 6),
]


def pct(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    k = (len(s) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


class Client(threading.Thread):
    def __init__(self, base, deadline, results, lock, keepalive):
        super().__init__(daemon=True)
        u = urlparse(base)
        self.host, self.port = u.hostname, u.port or 80
        self.deadline = deadline
        self.results = results
        self.lock = lock
        self.keepalive = keepalive
        self.cookie = None
        self.conn = None
        self.rng = random.Random()

    def _connect(self):
        self.conn = http.client.HTTPConnection(self.host, self.port, timeout=30)

    def _request(self, method, path, body=None):
        if self.conn is None:
            self._connect()
        headers = {"Accept-Encoding": "gzip"}
        if self.cookie:
            headers["Cookie"] = self.cookie
        if body is not None:
            headers["Content-Type"] = "application/json"
        if not self.keepalive:
            headers["Connection"] = "close"
        t0 = time.perf_counter()
        try:
            self.conn.request(method, path, body=body, headers=headers)
            resp = self.conn.getresponse()
            data = resp.read()
            dt = time.perf_counter() - t0
            if not self.keepalive or resp.getheader("Connection", "").lower() == "close":
                self.conn.close()
                self.conn = None
            return resp.status, dt, data, resp.getheader("Set-Cookie")
        except (OSError, http.client.HTTPException) as e:
            dt = time.perf_counter() - t0
            try:
                self.conn.close()
            except Exception:
                pass
            self.conn = None
            return 0, dt, repr(e).encode(), None

    def run(self):
        status, dt, data, cookie = self._request("POST", "/api/auth/login",
                                                 json.dumps({"username": "mara", "password": "demo"}))
        if cookie:
            self.cookie = cookie.split(";")[0]
        with self.lock:
            self.results.setdefault("POST /api/auth/login", []).append((status, dt))
        weights = [w for _, _, w in MIX]
        while time.time() < self.deadline:
            method, path, _ = self.rng.choices(MIX, weights=weights)[0]
            body = None
            if method == "POST":
                body = json.dumps({"project": 1, "title": f"load probe {self.rng.randrange(1 << 20)}", "priority": 2})
            status, dt, data, _ = self._request(method, path, body)
            key = f"{method} {path.split('?')[0]}"
            with self.lock:
                self.results.setdefault(key, []).append((status, dt))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base", help="ingress base URL, e.g. http://192.0.2.119")
    ap.add_argument("--clients", type=int, default=4)
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--no-keepalive", action="store_true", help="open a new TCP connection per request")
    args = ap.parse_args()

    results, lock = {}, threading.Lock()
    deadline = time.time() + args.seconds
    clients = [Client(args.base, deadline, results, lock, not args.no_keepalive) for _ in range(args.clients)]
    t0 = time.time()
    for c in clients:
        c.start()
    for c in clients:
        c.join(args.seconds + 60)
    elapsed = time.time() - t0

    total = sum(len(v) for v in results.values())
    errors = sum(1 for v in results.values() for s, _ in v if s == 0 or s >= 500)
    print(f"\n{args.clients} clients, {elapsed:.1f} s, {total} requests, {total / elapsed:.1f} req/s, {errors} errors\n")
    print(f"{'endpoint':<34}{'n':>6}{'ok':>6}{'p50 ms':>9}{'p90 ms':>9}{'p99 ms':>9}{'max ms':>9}")
    for key in sorted(results):
        v = results[key]
        lat = [dt * 1000 for s, dt in v if 200 <= s < 500]
        ok = len(lat)
        print(f"{key:<34}{len(v):>6}{ok:>6}{pct(lat, 50):>9.1f}{pct(lat, 90):>9.1f}{pct(lat, 99):>9.1f}{(max(lat) if lat else 0):>9.1f}")
    bad = {}
    for key, v in results.items():
        for s, _ in v:
            if s == 0 or s >= 500:
                bad[(key, s)] = bad.get((key, s), 0) + 1
    if bad:
        print("\nfailures:")
        for (key, s), n in sorted(bad.items()):
            print(f"  {key}: status {s} x{n}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
