#!/usr/bin/env python3
"""The masked diffusion language model with the node's arithmetic: export,
reference sampler, test vectors and the cost per token.

  mdlm.py export  --ckpt ckpt.pt --out mdlm.bin       K32M blob (EMBED|HEAD|TIED|BIDIR)
  mdlm.py sample  --model mdlm.bin --steps 16 --seed 1 [--prompt TEXT]
  mdlm.py vectors --model mdlm.bin --out FILE         samples with every step's reveals
  mdlm.py cost    --model mdlm.bin                    MACs and weight bytes per token
  mdlm.py check   --model target.bin                  the batched forward, run
                                                      causally, against llm.py's
                                                      Reference position by position

The blob is llm-pipeline's model slice (apps/llm-pipeline/llm.h) with the
flags TIED (the classifier is the embedding) and BIDIR: every position of
the window attends to every position, and the last vocabulary entry (512)
is MASK.

Sampling (deterministic for a seed): the window holds BOS, the prompt's
tokens, then MASK up to the window's length. Over S steps the masked
positions are revealed: step s reveals floor(M (s+1) / S) - floor(M s / S)
of the M positions masked at the start. At each step the model computes
logits for the whole window; each masked position p gets a token c_p and a
confidence:
  temperature 0: c_p = the first largest logit of 0..511, confidence its
                 probability in k32_softmax_f32(logits[0..511])
  temperature T: probabilities k32_softmax_f32(logits[0..511] * (1.0f / T));
                 u = the next uniform of the xorshift32 stream (one per
                 masked position, in position order); c_p = the first i
                 whose running sum (from 0, in order) exceeds u, else the
                 last i with a nonzero probability; confidence p[c_p]
The step reveals the masked positions with the largest confidence, the
lower position first among equal ones.
"""
import argparse
import json
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "apps", "llm-pipeline", "tools"))
sys.path.insert(0, os.path.join(REPO, "models", "common"))
import llm  # noqa: E402
from k32ref import F32, Rng, matvec_f32_i8, matvec_i8, quantize, rmsnorm, softmax, vecmat_f32_i8, k_exp  # noqa: E402

VOCAB = 512
MASK = 512
TOKENIZER = os.path.expanduser("~/.cache/kube32-models/data/tinyllamas/tok512.bin")


def export(ckpt, out, bits=8):
    import torch
    s = torch.load(ckpt, map_location="cpu")
    c = s["cfg"]
    sd = {k: v.float().numpy() for k, v in s["model"].items()}
    L = range(c["n_layers"])
    hs = c["dim"] // c["n_heads"]
    freqs = 1.0 / (10000.0 ** (np.arange(0, hs, 2)[: hs // 2].astype(np.float32) / hs))
    ang = np.outer(np.arange(c["seq_len"]).astype(np.float32), freqs)
    w = {"tok": sd["tok_embeddings.weight"],
         "rms_att": np.stack([sd["layers.%d.attention_norm.weight" % l] for l in L]),
         "rms_ffn": np.stack([sd["layers.%d.ffn_norm.weight" % l] for l in L]),
         "rms_final": sd["norm.weight"],
         "fcr": np.cos(ang).astype(F32), "fci": np.sin(ang).astype(F32)}
    for n in ("wq", "wk", "wv", "wo"):
        w[n] = np.stack([sd["layers.%d.attention.%s.weight" % (l, n)] for l in L])
    for n in ("w1", "w2", "w3"):
        w[n] = np.stack([sd["layers.%d.feed_forward.%s.weight" % (l, n)] for l in L])
    cfg = dict(dim=c["dim"], hidden=c["hidden"], n_layers=c["n_layers"], n_heads=c["n_heads"],
               n_kv_heads=c["n_kv_heads"], vocab=c["vocab"] + 1, seq_len=c["seq_len"])
    flags = llm.FLAG_EMBED | llm.FLAG_HEAD | llm.FLAG_TIED | llm.FLAG_BIDIR
    data = llm.blob(cfg, w, 0, c["n_layers"], flags, bits)
    with open(out, "wb") as f:
        f.write(data)
    return len(data)


def rope(v, cs):
    """v: (P, n) with heads of len(cs[0]) values; cs: (P, hs) cos, sin pairs"""
    reps = v.shape[1] // cs.shape[1]
    c = np.tile(cs[:, 0::2], (1, reps))
    s = np.tile(cs[:, 1::2], (1, reps))
    v0, v1 = v[:, 0::2], v[:, 1::2]
    out = np.empty_like(v)
    out[:, 0::2] = v0 * c - v1 * s
    out[:, 1::2] = v0 * s + v1 * c
    return out


class Net:
    """The blob's model over a window of positions at once"""

    def __init__(self, data):
        s = llm.read_blob(data)
        self.s, self.cfg, self.bytes = s, s["cfg"], len(data)
        self.bidir = bool(s["flags"] & llm.FLAG_BIDIR)

    def forward(self, tokens, causal=None):
        s, c = self.s, self.cfg
        causal = not self.bidir if causal is None else causal
        tokens = np.asarray(tokens)
        P = len(tokens)
        dim, H, KVH = c["dim"], c["n_heads"], c["n_kv_heads"]
        hs = dim // H
        group = H // KVH
        cs = s["rope"][:P]
        scale = F32(F32(1.0) / np.sqrt(F32(hs)))
        mask = (np.arange(P)[None, :] <= np.arange(P)[:, None])[:, None, :] if causal else None
        x = (s["tok_q"][tokens].astype(F32) * s["tok_s"][tokens][:, None]).astype(F32)
        for L in s["layers"]:
            xq, xs = quantize(rmsnorm(x, L["rms_att"]))
            q = rope(matvec_i8(L["wq"], L["wq_s"], xq, xs), cs)
            k = rope(matvec_i8(L["wk"], L["wk_s"], xq, xs), cs)
            v = matvec_i8(L["wv"], L["wv_s"], xq, xs)
            kq, ks = quantize(k.reshape(P, KVH, hs))             # (P, KVH, hs), (P, KVH)
            vq, vs = quantize(v.reshape(P, KVH, hs))
            heads = np.arange(H) // group
            kh = kq.transpose(1, 0, 2)[heads][None]               # (1, H, P, hs)
            ksh = ks.T[heads][None]                               # (1, H, P)
            vh = vq.transpose(1, 0, 2)[heads][None]
            vsh = vs.T[heads][None]
            att = matvec_f32_i8(kh, ksh, q.reshape(P, H, hs), scale)  # (P, H, P)
            att = softmax(att, mask)
            o = vecmat_f32_i8(vh, vsh, att).reshape(P, dim)
            xq, xs = quantize(o)
            x = (x + matvec_i8(L["wo"], L["wo_s"], xq, xs)).astype(F32)
            xq, xs = quantize(rmsnorm(x, L["rms_ffn"]))
            h1 = matvec_i8(L["w1"], L["w1_s"], xq, xs)
            h3 = matvec_i8(L["w3"], L["w3_s"], xq, xs)
            h1 = (h1 * (F32(1.0) / (F32(1.0) + k_exp(-h1))).astype(F32)).astype(F32)
            h1 = (h1 * h3).astype(F32)
            xq, xs = quantize(h1)
            x = (x + matvec_i8(L["w2"], L["w2_s"], xq, xs)).astype(F32)
        xq, xs = quantize(rmsnorm(x, s["rms_final"]))
        return matvec_i8(s["cls_q"], s["cls_s"], xq, xs)


def sample(net, prompt, steps, seed, temperature=0.0, length=None, trace=None):
    """The window after `steps` steps; prompt: tokens from BOS on"""
    length = length or net.cfg["seq_len"]
    x = list(prompt) + [MASK] * (length - len(prompt))
    masked0 = [p for p in range(length) if x[p] == MASK]
    M = len(masked0)
    rng = Rng(seed)
    inv_t = F32(F32(1.0) / F32(temperature)) if temperature > 0 else None
    for st in range(steps):
        n = M * (st + 1) // steps - M * st // steps
        masked = [p for p in range(length) if x[p] == MASK]
        if not masked or n == 0:
            continue
        logits = net.forward(x)[:, :VOCAB]
        cand, conf = {}, {}
        for p in masked:
            if inv_t is None:
                c = int(np.argmax(logits[p]))
                pr = softmax(logits[p])
            else:
                pr = softmax((logits[p] * inv_t).astype(F32))
                u = rng.uniform()
                run = np.add.accumulate(pr, dtype=F32)
                hit = np.flatnonzero(run > u)
                c = int(hit[0]) if len(hit) else int(np.flatnonzero(pr > 0)[-1])
            cand[p], conf[p] = c, pr[c]
        order = sorted(masked, key=lambda p: (-conf[p], p))[:n]
        for p in sorted(order):
            x[p] = cand[p]
        if trace is not None:
            trace.append({"step": st, "revealed": sorted(order), "tokens": [cand[p] for p in sorted(order)],
                          "confidence": [float(conf[p]) for p in sorted(order)]})
    return x


def decode(tok, toks):
    """text up to the first BOS after position 0"""
    end = next((i for i in range(1, len(toks)) if toks[i] == llm.BOS), len(toks))
    return tok.decode(toks[:end])


def cost(net, steps_list=(8, 16, 32)):
    c = net.cfg
    dim, hidden, L, W = c["dim"], c["hidden"], c["n_layers"], c["seq_len"]
    kvd = c["n_kv_heads"] * dim // c["n_heads"]
    layer_w = 2 * dim * dim + 2 * kvd * dim + 3 * dim * hidden
    cls_w = VOCAB * dim
    weight_bytes = L * layer_w + (c["vocab"]) * dim     # int8 matrices + the embedding/classifier
    per_pos = L * layer_w + cls_w                        # MACs of the products at one position
    att_bidir = L * 2 * W * dim                          # scores and values over the window
    att_ar = L * 2 * (W / 2) * dim                       # a causal model's average over the window
    gen = W - 1
    rows = []
    for S in steps_list:
        macs = S * W * (per_pos + att_bidir) / gen
        rows.append({"model": "masked diffusion, %d steps" % S, "passes_per_token": S / gen,
                     "weight_bytes_per_token": weight_bytes * S / gen, "macs_per_token": macs,
                     "macs_per_weight_byte": macs / (weight_bytes * S / gen)})
    macs = per_pos + att_ar
    rows.append({"model": "autoregressive, same size", "passes_per_token": 1.0, "weight_bytes_per_token": weight_bytes,
                 "macs_per_token": macs, "macs_per_weight_byte": macs / weight_bytes})
    return {"weight_bytes": weight_bytes, "blob_bytes": net.bytes, "macs_per_position": per_pos,
            "window": W, "rows": rows}


def check(target, n=40):
    """the batched causal forward against llm.Reference, logits bit for bit"""
    cfg, w = llm.read_checkpoint(target)
    data = llm.blob(cfg, w, 0, cfg["n_layers"], llm.FLAG_EMBED | llm.FLAG_HEAD, 8)
    net = Net(data)
    tok = llm.Tokenizer(TOKENIZER)
    toks = tok.encode("Once upon a time, there was a little girl named Lily. She loved to play outside")
    toks = (toks * 4)[:n]
    batched = net.forward(toks, causal=True)
    ref = llm.Reference([llm.read_blob(data)], n)
    for pos, t in enumerate(toks):
        x = (ref.tok_q[t].astype(F32) * ref.tok_s[t]).astype(F32)
        for l in range(len(ref.layers)):
            x = ref.layer(l, x, pos, ref.rope[pos])
        xq, s = llm.quantize(llm.rmsnorm(x, ref.rms_final))
        logits = llm.matvec(ref.cls_q, ref.cls_s, xq, s)
        if not np.array_equal(logits.view(np.uint32), batched[pos].view(np.uint32)):
            sys.exit("position %d: logits differ (max %g)" % (pos, np.max(np.abs(logits - batched[pos]))))
    print("%d positions: batched logits equal llm.Reference's bit for bit" % n)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export")
    e.add_argument("--ckpt", required=True)
    e.add_argument("--out", required=True)
    e.add_argument("--bits", type=int, default=8)
    for name in ("sample", "vectors", "cost"):
        s = sub.add_parser(name)
        s.add_argument("--model", required=True)
        s.add_argument("--steps", type=int, default=16)
        s.add_argument("--seed", type=int, default=1)
        s.add_argument("--temperature", type=float, default=0.0)
        s.add_argument("--prompt", default="")
        s.add_argument("--n", type=int, default=1)
        if name == "vectors":
            s.add_argument("--out", required=True)
    c = sub.add_parser("check")
    c.add_argument("--model", required=True)
    a = p.parse_args()
    if a.cmd == "export":
        print("wrote %s: %d bytes" % (a.out, export(a.ckpt, a.out, a.bits)))
        return
    if a.cmd == "check":
        check(a.model)
        return
    net = Net(open(a.model, "rb").read())
    if a.cmd == "cost":
        print(json.dumps(cost(net), indent=1))
        return
    tok = llm.Tokenizer(TOKENIZER)
    prompt = tok.encode(a.prompt) if a.prompt else [llm.BOS]
    if a.cmd == "sample":
        for i in range(a.n):
            x = sample(net, prompt, a.steps, a.seed + i, a.temperature)
            print("[seed %d] %s\n" % (a.seed + i, decode(tok, x)))
        return
    vec = []
    prompts = [("", [llm.BOS]), (a.prompt, prompt)] if a.prompt else [("", [llm.BOS])]
    for text, pt in prompts:
        for steps in (8, 16, 32):
            for temp, seed in ((0.0, 1), (0.8, a.seed), (0.8, a.seed + 1)):
                trace = []
                x = sample(net, pt, steps, seed, temp, trace=trace)
                vec.append({"prompt": text, "prompt_tokens": pt, "steps": steps, "seed": seed, "temperature": temp,
                            "window": x, "text": decode(tok, x), "trace": trace})
    first = net.forward(prompt + [MASK] * (net.cfg["seq_len"] - len(prompt)))
    out = {"model": os.path.basename(a.model), "samples": vec,
           "first_step_logits_sha256": __import__("hashlib").sha256(first.astype("<f4").tobytes()).hexdigest(),
           "first_step_logits_pos1": first[1].tolist()}
    with open(a.out, "w") as f:
        json.dump(out, f)
        f.write("\n")
    # the same samples for mdlm_test.c: uint32 steps, seed, prompt_len, float
    # temperature, uint32 prompt[64], uint32 window[64]
    W = net.cfg["seq_len"]
    with open(a.out.replace(".json", "") + "-samples.bin", "wb") as f:
        for v in vec:
            pt = v["prompt_tokens"] + [0] * (W - len(v["prompt_tokens"]))
            f.write(struct.pack("<3If", v["steps"], v["seed"], len(v["prompt_tokens"]), v["temperature"]))
            f.write(np.array(pt + v["window"], "<u4").tobytes())
    print("wrote %s" % a.out)


if __name__ == "__main__":
    main()
