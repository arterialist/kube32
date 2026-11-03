#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["numpy>=1.24"]
# ///
"""Greedy speculative decoding with llm-pipeline's arithmetic: a small draft
model proposes k tokens, the target model computes all k + 1 positions in
one pass over its weights, and the longest prefix the target agrees with is
kept, plus the target's own next token. The text is the target's greedy
text, token for token; what changes is how many passes over the target's
weights it takes.

  spec.py run --target T.bin [--draft D.bin] --k 4 --prompt TEXT
      one generation with its passes, checked against plain greedy decoding
  spec.py measure --target T.bin --k 2,4,6 --prompts FILE
      acceptance over many prompts, for the draft model and for prompt
      lookup (the draft is a copy of what followed the last occurrence of
      the newest n tokens in the text so far)
  spec.py vectors --target T.bin --k 4 --out FILE
      a test vector: prompt tokens, every pass's draft and target tokens
  spec.py blobs --target T.bin --target-bits 4 --out DIR
      llm-speculative's blobs: the target's and the draft's slices (all
      layers, embedding and classifier) and the tokenizer
  spec.py upload --dir DIR --server URL --node NODE
      the blobs to a node's flash blob store, through the API server
  spec.py bench --target T.bin --target-bits 4 --url URL --prompts FILE
      generations on llm-speculative (the target alone, speculative at each
      k, the draft alone), each checked against this reference: the tokens,
      and for speculative decoding every pass's accepted count

Both models are llama2.c checkpoints with the same tokenizer; they are
quantized as `llm.py build` does (--target-bits, --draft-bits), and each
position is computed exactly as llm-shard computes it, so the tokens are the
ones a pipeline of stages produces.
"""
import argparse
import json
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import llm  # noqa: E402

BOS = llm.BOS


def slices(path, bits):
    """The model as one in-memory slice (embedding, every layer, classifier)"""
    cfg, w = llm.read_checkpoint(path)
    data = llm.blob(cfg, w, 0, cfg["n_layers"], llm.FLAG_EMBED | llm.FLAG_HEAD, bits)
    return [llm.read_blob(data)], len(data)


class Model:
    """A Reference with a cursor: the positions its KV cache holds for the
    current text"""

    def __init__(self, sl, ctx):
        self.ref = llm.Reference(sl, ctx)
        self.valid = 0   # positions 0..valid-1 hold the current text's inputs

    def run(self, toks, upto, sample_from):
        """Computes positions valid..upto-1 with inputs toks[pos]; returns the
        token picked after each position >= sample_from"""
        out = []
        for pos in range(self.valid, upto):
            nxt = self.ref.step(toks[pos], pos, pos >= sample_from)
            if nxt is not None:
                out.append(nxt)
        self.valid = upto
        return out


def ngram_draft(toks, k, max_n=3):
    """Prompt lookup: the k tokens that followed the latest earlier
    occurrence of the newest n tokens (n from max_n down to 1)"""
    for n in range(min(max_n, len(toks) - 1), 0, -1):
        tail = toks[-n:]
        for s in range(len(toks) - n - 1, -1, -1):
            if toks[s:s + n] == tail:
                return toks[s + n:s + n + k]
    return []


def speculate(target, draft, prompt, k, steps, ctx):
    """Greedy speculative decoding. draft: a Model, or "ngram". Returns the
    tokens and one record per target pass"""
    toks = list(prompt)
    n_prompt = len(toks)
    target.valid = 0
    if draft != "ngram":
        draft.valid = 0
    passes = []
    while len(toks) - n_prompt < steps and len(toks) < ctx:
        room = min(k, steps - (len(toks) - n_prompt) - 1, ctx - len(toks) - 1)
        if draft == "ngram":
            d = ngram_draft(toks, room) if room > 0 else []
        else:
            d = []
            if room > 0:
                # the draft catches up on the text, then proposes room tokens
                seq = list(toks)
                draft.valid = min(draft.valid, len(seq) - 1)
                d += draft.run(seq, len(seq), len(seq) - 1)
                for _ in range(room - 1):
                    seq.append(d[-1])
                    d += draft.run(seq, len(seq), len(seq) - 1)
        # one target pass: the positions of the newest token and the drafts
        seq = toks + d
        target.valid = min(target.valid, len(toks) - 1)
        outs = target.run(seq, len(seq), len(toks) - 1)
        n = 0
        while n < len(d) and d[n] == outs[n]:
            n += 1
        new = d[:n] + [outs[n]]
        passes.append({"draft": d, "target": outs, "accepted": n})
        # the caches hold the accepted text; later positions are recomputed
        target.valid = min(target.valid, len(toks) + n)
        if draft != "ngram":
            draft.valid = min(draft.valid, len(toks) + n)
        stop = BOS in new
        if stop:
            new = new[:new.index(BOS)]
        toks += new
        if stop:
            break
    return toks, passes


def greedy(target, prompt, steps, ctx):
    toks = list(prompt)
    target.valid = 0
    while len(toks) - len(prompt) < steps and len(toks) < ctx:
        nxt = target.run(toks, len(toks), len(toks) - 1)[-1]
        target.valid = len(toks)
        if nxt == BOS:
            break
        toks.append(nxt)
    return toks


def summary(passes, n_new):
    proposed = sum(len(p["draft"]) for p in passes)
    accepted = sum(p["accepted"] for p in passes)
    return {"passes": len(passes), "tokens": n_new, "proposed": proposed, "accepted": accepted,
            "acceptance": accepted / proposed if proposed else 0.0,
            "tokens_per_pass": n_new / len(passes) if passes else 0.0,
            "target_positions": sum(len(p["target"]) for p in passes)}


def load(a):
    tok = llm.Tokenizer(a.tokenizer)
    tsl, tbytes = slices(a.target, a.target_bits)
    dsl, dbytes = slices(a.draft, a.draft_bits)
    return tok, tsl, tbytes, dsl, dbytes


def cmd_run(a):
    tok, tsl, tbytes, dsl, dbytes = load(a)
    prompt = tok.encode(a.prompt)
    target, draft = Model(tsl, a.context), Model(dsl, a.context)
    ref = greedy(target, prompt, a.steps, a.context)
    for name, d in (("draft model", draft), ("prompt lookup", "ngram")):
        t0 = time.time()
        toks, passes = speculate(target, d, prompt, a.k, a.steps, a.context)
        s = summary(passes, len(toks) - len(prompt))
        print("%s, k=%d: %d tokens in %d target passes (%.2f a pass), acceptance %.3f, %.1f s" %
              (name, a.k, s["tokens"], s["passes"], s["tokens_per_pass"], s["acceptance"], time.time() - t0))
        if toks != ref:
            sys.exit("speculative decoding differs from greedy decoding")
    print("same tokens as greedy decoding\n")
    print(tok.decode(ref))


def cmd_measure(a):
    tok, tsl, tbytes, dsl, dbytes = load(a)
    prompts = [l.rstrip("\n") for l in open(a.prompts) if l.strip()][: a.n]
    target, draft = Model(tsl, a.context), Model(dsl, a.context)
    ks = [int(k) for k in a.k.split(",")]
    res = {"target": os.path.basename(a.target), "target_bytes": tbytes, "target_bits": a.target_bits,
           "draft": os.path.basename(a.draft), "draft_bytes": dbytes, "draft_bits": a.draft_bits,
           "prompts": len(prompts), "steps": a.steps, "runs": {}}
    refs = []
    for p in prompts:
        pt = tok.encode(p)
        refs.append((pt, greedy(target, pt, a.steps, a.context)))
    for method in ("draft", "ngram"):
        for k in ks:
            tot = {"passes": 0, "tokens": 0, "proposed": 0, "accepted": 0, "target_positions": 0}
            t0 = time.time()
            for pt, ref in refs:
                toks, passes = speculate(target, draft if method == "draft" else "ngram", pt, k, a.steps, a.context)
                if toks != ref:
                    sys.exit("speculative decoding differs from greedy decoding")
                s = summary(passes, len(toks) - len(pt))
                for key in tot:
                    tot[key] += s[key]
            tot["acceptance"] = tot["accepted"] / max(1, tot["proposed"])
            tot["tokens_per_pass"] = tot["tokens"] / max(1, tot["passes"])
            res["runs"]["%s-k%d" % (method, k)] = tot
            print("%-6s k=%d  acceptance %.3f  tokens/pass %.2f  passes %d  tokens %d  (%.0f s)" %
                  (method, k, tot["acceptance"], tot["tokens_per_pass"], tot["passes"], tot["tokens"], time.time() - t0),
                  flush=True)
    if a.out:
        with open(a.out, "w") as f:
            json.dump(res, f, indent=1)
            f.write("\n")


def cmd_vectors(a):
    tok, tsl, tbytes, dsl, dbytes = load(a)
    prompt = tok.encode(a.prompt)
    target, draft = Model(tsl, a.context), Model(dsl, a.context)
    out = {"prompt": a.prompt, "prompt_tokens": prompt, "k": a.k, "steps": a.steps, "context": a.context,
           "target": os.path.basename(a.target), "target_bits": a.target_bits,
           "draft": os.path.basename(a.draft), "draft_bits": a.draft_bits}
    out["greedy"] = greedy(target, prompt, a.steps, a.context)
    for name, d in (("draft_model", draft), ("prompt_lookup", "ngram")):
        toks, passes = speculate(target, d, prompt, a.k, a.steps, a.context)
        assert toks == out["greedy"]
        out[name] = {**summary(passes, len(toks) - len(prompt)), "trace": passes}
    out["text"] = tok.decode(out["greedy"])
    with open(a.out, "w") as f:
        json.dump(out, f)
        f.write("\n")
    print("wrote %s: %d tokens, %d passes with the draft model, %d with prompt lookup" %
          (a.out, len(out["greedy"]) - len(prompt), out["draft_model"]["passes"], out["prompt_lookup"]["passes"]))


def cmd_blobs(a):
    os.makedirs(a.out, exist_ok=True)
    for name, path, bits in (("target-lm", a.target, a.target_bits), ("stories260k", a.draft, a.draft_bits)):
        cfg, w = llm.read_checkpoint(path)
        data = llm.blob(cfg, w, 0, cfg["n_layers"], llm.FLAG_EMBED | llm.FLAG_HEAD, bits)
        with open(os.path.join(a.out, name + ".bin"), "wb") as f:
            f.write(data)
        print("%s.bin: %d bytes, %d layers, int%d" % (name, len(data), cfg["n_layers"], bits))
    with open(a.tokenizer, "rb") as src, open(os.path.join(a.out, "tok512.bin"), "wb") as dst:
        dst.write(src.read())
    print("wrote %s" % a.out)


def cmd_upload(a):
    for f in sorted(os.listdir(a.dir)):
        if not f.endswith(".bin"):
            continue
        data = open(os.path.join(a.dir, f), "rb").read()
        url = "%s/api/v1/nodes/%s/proxy/blobs/%s?medium=flash" % (a.server.rstrip("/"), a.node, f[:-4])
        req = urllib.request.Request(url, data=data, method="PUT", headers={"Content-Type": "application/octet-stream"})
        t0 = time.time()
        with urllib.request.urlopen(req, timeout=600) as r:
            print("%s: %d bytes, HTTP %d, %.1f s" % (f[:-4], len(data), r.status, time.time() - t0), flush=True)


def board(url, method="GET", body=None, tries=5):
    """A JSON answer of llm-speculative; a status read is tried again when
    the answer does not arrive whole"""
    for i in range(tries):
        try:
            req = urllib.request.Request(url, data=body, method=method)
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.loads(r.read())
        except (ValueError, OSError):
            if method != "GET" or i == tries - 1:
                raise
            time.sleep(2)


def board_run(a, prompt, mode, k):
    """One generation on llm-speculative; its final status"""
    q = "%s/generate?mode=%s&k=%d&steps=%d" % (a.url.rstrip("/"), mode, k, a.steps)
    t0 = time.time()
    board(q, "POST", prompt.encode())
    while True:
        time.sleep(0.5)
        st = board(a.url)
        if st["state"] in ("done", "failed"):
            st["wall_s"] = time.time() - t0   # the whole generation by this host's clock, prefill included
            return st


def cmd_bench(a):
    tok, tsl, tbytes, dsl, dbytes = load(a)
    prompts = [l.rstrip("\n") for l in open(a.prompts) if l.strip()][: a.n] if a.prompts else [a.prompt]
    ks = [int(k) for k in a.k.split(",") if k]
    ctx = board(a.url)["context"]
    target, draft = Model(tsl, ctx), Model(dsl, ctx)
    runs = [("target", 0)] + [("speculative", k) for k in ks] + [("draft", 0)]
    rows = {r: [] for r in runs}
    for p in prompts:
        pt = tok.encode(p)
        ref = greedy(target, pt, a.steps, ctx)
        for mode, k in runs:
            st = board_run(a, p, mode, k)
            if mode == "draft":
                want = greedy(draft, pt, a.steps, ctx)
            else:
                want = ref
            if st["tokens_list"] != want:
                sys.exit("%s k=%d, prompt %r: the board's tokens differ from the reference" % (mode, k, p))
            if mode == "speculative":
                toks, passes = speculate(target, draft, pt, k, a.steps, ctx)
                if st["passes"] != len(passes) or st["accepted"] != sum(q["accepted"] for q in passes):
                    sys.exit("k=%d, prompt %r: %d passes / %d accepted on the board, %d / %d in the reference" %
                             (k, p, st["passes"], st["accepted"], len(passes), sum(q["accepted"] for q in passes)))
            rows[(mode, k)].append(st)
            print("%-11s k=%d %6.3f tok/s (wall %6.3f)  %3d tokens  passes %3d  acceptance %.3f  verify %7.1f ms  draft %5.1f ms/token  "
                  "%7d B/token  prompt %r" % (mode, k, st["tokens_per_s"], st["tokens"] / st["wall_s"], st["tokens"], st["passes"],
                                              st["acceptance"], st["verify_ms_per_pass"], st["draft_ms_per_token"],
                                              st["target_bytes_per_token"], p),
                  flush=True)
    print("\nmean over %d prompts, %d tokens each at most, same tokens as the reference in every run:" % (len(prompts), a.steps))
    out = []
    for (mode, k), sts in rows.items():
        n = len(sts)
        tokens = sum(s["tokens"] for s in sts)
        decode_ms = sum(s["decode_ms"] for s in sts)
        passes = sum(s["passes"] for s in sts)
        proposed = sum(s["proposed"] for s in sts)
        wall_s = sum(s["wall_s"] for s in sts)
        row = {"mode": mode, "k": k, "prompts": n, "tokens": tokens,
               "tokens_per_s_mean": sum(s["tokens_per_s"] for s in sts) / n,
               "tokens_per_s_wall": tokens / wall_s if wall_s else 0.0,
               "tokens_per_s_pooled": tokens * 1000.0 / decode_ms if decode_ms else 0.0,
               "acceptance": sum(s["accepted"] for s in sts) / proposed if proposed else 0.0,
               "tokens_per_pass": tokens / passes if passes else 0.0,
               "verify_ms_per_pass": sum(s["verify_ms"] for s in sts) / passes if passes and mode != "draft" else 0.0,
               "draft_ms_per_token": sum(s["draft_ms"] for s in sts) / max(1, sum(s["proposed"] if mode == "speculative" else s["passes"] for s in sts))
               if mode != "target" else 0.0,
               "target_bytes_per_token": sum(s["target_bytes"] for s in sts) / tokens if tokens else 0.0,
               "runs": sts}
        out.append(row)
        print("%-11s k=%d  %6.3f tok/s (pooled %6.3f, wall %6.3f)  acceptance %.3f  %.2f tokens/pass  verify %7.1f ms/pass  "
              "draft %5.1f ms/token  %9.0f B/token from flash" % (mode, k, row["tokens_per_s_mean"], row["tokens_per_s_pooled"],
                                                              row["tokens_per_s_wall"], row["acceptance"], row["tokens_per_pass"], row["verify_ms_per_pass"],
                                                              row["draft_ms_per_token"], row["target_bytes_per_token"]))
    if a.out:
        with open(a.out, "w") as f:
            json.dump(out, f, indent=1)
            f.write("\n")


def main():
    model_dir = os.path.join(llm.BUILD, "model")
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    cmds = {"run": cmd_run, "measure": cmd_measure, "vectors": cmd_vectors, "blobs": cmd_blobs, "upload": cmd_upload,
            "bench": cmd_bench}
    up = sub.add_parser("upload")
    up.add_argument("--dir", required=True, help="the blobs' directory (spec.py blobs --out)")
    up.add_argument("--server", required=True, help="the API server, e.g. http://<control-plane-ip>:6443")
    up.add_argument("--node", required=True)
    for name in cmds:
        if name == "upload":
            continue
        s = sub.add_parser(name)
        s.add_argument("--target", required=True, help="the target's llama2.c checkpoint")
        s.add_argument("--draft", default=os.path.join(model_dir, "stories260K.bin"))
        s.add_argument("--tokenizer", default=os.path.join(model_dir, "tok512.bin"))
        s.add_argument("--target-bits", type=int, default=8, choices=(4, 8))
        s.add_argument("--draft-bits", type=int, default=8, choices=(4, 8))
        s.add_argument("--steps", type=int, default=128)
        s.add_argument("--context", type=int, default=256)
        if name in ("measure", "bench"):
            s.add_argument("--k", default="2,4,6")
            s.add_argument("--prompts", required=name == "measure", help="one prompt a line")
            s.add_argument("--n", type=int, default=1000, help="prompts to use at most")
            s.add_argument("--out", help="the results as JSON")
        if name == "bench":
            s.add_argument("--url", required=True, help="llm-speculative, e.g. http://<control-plane-ip>/spec")
            s.add_argument("--prompt", default="Once upon a time")
        elif name == "blobs":
            s.add_argument("--out", required=True, help="directory for the blobs")
        elif name in ("run", "vectors"):
            s.add_argument("--k", type=int, default=4)
            s.add_argument("--prompt", default="Once upon a time")
        if name == "vectors":
            s.add_argument("--out", required=True)
    a = p.parse_args()
    if a.cmd == "upload":
        return cmd_upload(a)
    if not os.path.exists(a.draft) or not os.path.exists(a.tokenizer):
        llm.fetch(model_dir)
    cmds[a.cmd](a)


if __name__ == "__main__":
    main()
