#!/usr/bin/env python3
"""Runs the test vectors of models/mdlm against a llm-diffusion pod and checks
that every window is the reference's, token for token.

  check.py --url http://<control-plane>/diffusion [--steps 8,16,32] [--only N]

Each vector is a generation (prompt, steps, seed, temperature). The script
starts it, reads /state until it is done, compares the tokens, the text and the
number of tokens each step revealed with the vector, and prints the pod's
timing."""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
VECTORS = os.path.join(HERE, "..", "..", "..", "models", "mdlm", "vectors", "mdlm.json")


def get(url, timeout=60):
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=timeout) as r:
                return json.loads(r.read())
        except urllib.error.HTTPError as e:
            return {**json.loads(e.read() or b"{}"), "http": e.code}
        except (urllib.error.URLError, TimeoutError, ConnectionError):
            if attempt == 3:
                raise
            time.sleep(1)


def run(base, v, poll):
    q = {"steps": v["steps"], "seed": v["seed"], "temp": round(v["temperature"] * 100)}
    if v["prompt"]:
        q["prompt"] = v["prompt"]
    start = get(base + "/generate?" + urllib.parse.urlencode(q, quote_via=urllib.parse.quote))
    if start.get("state") != "running":
        sys.exit("generate: %s" % start)
    seen, log = 0, []
    while True:
        time.sleep(poll)
        st = get(base + "/state?from=%d" % seen)
        for e in st["log"]:
            log.append(e)
            seen = e["step"] + 1
        if st["state"] == "done":
            return st, log
        if st["state"] == "idle":
            sys.exit("the generation stopped: %s" % st)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--vectors", default=VECTORS)
    ap.add_argument("--steps", help="comma list: only vectors with these step counts")
    ap.add_argument("--only", help="comma list of vector indexes")
    ap.add_argument("--poll", type=float, default=2.0, help="seconds between state reads")
    ap.add_argument("--json", help="write the results to this file")
    a = ap.parse_args()
    vecs = json.load(open(a.vectors))["samples"]
    pick = [int(x) for x in a.only.split(",")] if a.only else range(len(vecs))
    steps = {int(x) for x in a.steps.split(",")} if a.steps else None
    base = a.url.rstrip("/")
    bad = 0
    results = []
    for i in pick:
        v = vecs[i]
        if steps and v["steps"] not in steps:
            continue
        st, log = run(base, v, a.poll)
        want = [len(t["revealed"]) for t in v["trace"]]
        got = [e["revealed"] for e in log if e["revealed"]]
        text = log[-1]["text"] if log else ""
        ok = st["tokens"] == v["window"] and got == want and text == v["text"]
        # the totals agree with the per-step log
        totals = st["generated"] == 64 - st["prompt_tokens"] and st["compute_us"] == sum(e["us"] for e in log)
        if not totals:
            print("   totals differ from the log: generated %r, compute_us %r, step us sum %d" % (st["generated"], st["compute_us"], sum(e["us"] for e in log)))
        ok = ok and totals
        bad += not ok
        r = {"vector": i, "prompt": v["prompt"], "steps": v["steps"], "seed": v["seed"], "temperature": v["temperature"],
             "ok": ok, "ms_per_step": st["us_per_step"] / 1000.0, "compute_ms": st["compute_us"] / 1000.0,
             "matmul_ms": st["matmul_us"] / 1000.0, "wall_ms": st["wall_us"] / 1000.0,
             "tokens_per_s": st["tokens_per_s_x100"] / 100.0, "weight_bytes_read": st["weight_bytes_read"],
             "generated": st["generated"], "text": text, "step_ms": [e["us"] / 1000.0 for e in log]}
        results.append(r)
        print("%2d %-24s steps %2d seed %d T %.1f  %s  %.0f ms/step  %.1f s  %.2f tokens/s  (matmul %.0f%%)" % (
            i, repr(v["prompt"]), v["steps"], v["seed"], v["temperature"], "match" if ok else "DIFFERS",
            r["ms_per_step"], r["compute_ms"] / 1000, r["tokens_per_s"], 100.0 * r["matmul_ms"] / max(r["compute_ms"], 1)), flush=True)
        if not ok:
            print("   got  ", st["tokens"], "\n   want ", v["window"], "\n   text ", repr(text), "\n   want ", repr(v["text"]))
    if a.json:
        json.dump(results, open(a.json, "w"), indent=1)
    print("%d of %d match the reference" % (len(results) - bad, len(results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
