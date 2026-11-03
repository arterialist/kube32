"""Validation loss of tok512 models on TinyStories' validation shard, in float
and with the node's quantization (int8 or int4 weight rows, int8 activations
before every product, an int8 KV cache per head), and the export to
llama2.c's checkpoint format.

  evaluate.py loss --model ckpt.pt|model.bin [--model ...] --length 256
  evaluate.py export --ckpt ckpt.pt --out model.bin
"""
import argparse
import json
import os
import struct
import sys

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from model import Config, Transformer, apply_rope, export_llama2c  # noqa: E402
from train import Tokens, mdlm_loss  # noqa: E402


def load(path, device):
    if path.endswith(".pt"):
        s = torch.load(path, map_location="cpu")
        m = Transformer(Config(**s["cfg"]))
        m.load_state_dict(s["model"])
        return m.to(device).eval()
    raw = open(path, "rb").read()
    dim, hidden, n_layers, n_heads, n_kv_heads, vocab, seq_len = struct.unpack("<7i", raw[:28])
    m = Transformer(Config(dim=dim, hidden=hidden, n_layers=n_layers, n_heads=n_heads, n_kv_heads=n_kv_heads,
                           vocab=abs(vocab), seq_len=seq_len))
    f = torch.from_numpy(np.frombuffer(raw[28:], dtype="<f4").copy())
    o = 0

    def take(shape):
        nonlocal o
        n = int(np.prod(shape))
        t = f[o:o + n].view(*shape)
        o += n
        return t

    hs = dim // n_heads
    kvd = n_kv_heads * hs
    sd = {"tok_embeddings.weight": take((abs(vocab), dim))}
    L = range(n_layers)
    for l in L:
        sd["layers.%d.attention_norm.weight" % l] = take((dim,))
    for name, shape in (("wq", (dim, dim)), ("wk", (kvd, dim)), ("wv", (kvd, dim)), ("wo", (dim, dim))):
        for l in L:
            sd["layers.%d.attention.%s.weight" % (l, name)] = take(shape)
    for l in L:
        sd["layers.%d.ffn_norm.weight" % l] = take((dim,))
    for name, shape in (("w1", (hidden, dim)), ("w2", (dim, hidden)), ("w3", (hidden, dim))):
        for l in L:
            sd["layers.%d.feed_forward.%s.weight" % (l, name)] = take(shape)
    sd["norm.weight"] = take((dim,))
    m.load_state_dict(sd)
    return m.to(device).eval()


# ---- the node's quantization, simulated in float ------------------------------

def q_rows(w, bits):
    top = 127.0 if bits == 8 else 7.0
    s = w.abs().amax(-1, keepdim=True) / top
    s = torch.where(s == 0, torch.ones_like(s), s)
    return torch.clamp(torch.round(w / s), -top, top) * s


def q_act(x):
    """int8 with one scale per vector (last axis), rounded half away from 0"""
    amax = x.abs().amax(-1, keepdim=True)
    inv = torch.where(amax == 0, torch.zeros_like(amax), 127.0 / amax)
    t = x * inv
    r = torch.clamp(torch.sign(t) * torch.floor(t.abs() + 0.5), -127, 127)
    return r * (amax / 127.0)


def rms(x, w):
    return x * torch.rsqrt(x.pow(2).mean(-1, keepdim=True) + 1e-5) * w


@torch.no_grad()
def forward_q(m, tokens, bits):
    c = m.cfg
    T = tokens.shape[1]
    hs = c.dim // c.n_heads
    emb = q_rows(m.tok_embeddings.weight, 8)
    h = emb[tokens]
    cos, sin = m.cos[:T], m.sin[:T]
    for blk in m.layers:
        a = blk.attention
        xq = q_act(rms(h, blk.attention_norm.weight))
        q = F.linear(xq, q_rows(a.wq.weight, bits)).view(*h.shape[:2], c.n_heads, hs)
        k = F.linear(xq, q_rows(a.wk.weight, bits)).view(*h.shape[:2], c.n_kv_heads, hs)
        v = F.linear(xq, q_rows(a.wv.weight, bits)).view(*h.shape[:2], c.n_kv_heads, hs)
        q, k = apply_rope(q, cos, sin), apply_rope(k, cos, sin)
        k, v = q_act(k), q_act(v)
        rep = c.n_heads // c.n_kv_heads
        k = k.repeat_interleave(rep, dim=2)
        v = v.repeat_interleave(rep, dim=2)
        y = F.scaled_dot_product_attention(q.transpose(1, 2), k.transpose(1, 2), v.transpose(1, 2), is_causal=c.causal)
        y = y.transpose(1, 2).reshape(*h.shape[:2], c.dim)
        h = h + F.linear(q_act(y), q_rows(a.wo.weight, bits))
        f = blk.feed_forward
        xq = q_act(rms(h, blk.ffn_norm.weight))
        g = F.silu(F.linear(xq, q_rows(f.w1.weight, bits))) * F.linear(xq, q_rows(f.w3.weight, bits))
        h = h + F.linear(q_act(g), q_rows(f.w2.weight, bits))
    xq = q_act(rms(h, m.norm.weight))
    return F.linear(xq, emb[: c.vocab])


@torch.no_grad()
def val_loss(m, val, length, batches, batch, quant, device, seed=1234):
    rng = np.random.default_rng(seed)
    gen = torch.Generator(device=device).manual_seed(seed)
    tot = 0.0
    for _ in range(batches):
        if m.cfg.causal:
            x = val.batch(rng, batch, length + 1).to(device)
            logits = forward_q(m, x[:, :-1], quant) if quant else m(x[:, :-1])
            tot += F.cross_entropy(logits.reshape(-1, logits.shape[-1]).float(), x[:, 1:].reshape(-1)).item()
        else:
            x = val.batch(rng, batch, length).to(device)
            if quant:
                fwd = lambda inp: forward_q(m, inp, quant)  # noqa: E731
                fwd.cfg = m.cfg
                tot += mdlm_loss(fwd, x, gen).item()
            else:
                tot += mdlm_loss(m, x, gen).item()
    return tot / batches


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    l = sub.add_parser("loss")
    l.add_argument("--model", action="append", required=True)
    l.add_argument("--length", type=int, default=256)
    l.add_argument("--batches", type=int, default=50)
    l.add_argument("--batch", type=int, default=32)
    l.add_argument("--quant", default="0,8,4", help="0 = float, 8, 4: weight bits")
    l.add_argument("--json")
    l.add_argument("--cpu", action="store_true")
    e = sub.add_parser("export")
    e.add_argument("--ckpt", required=True)
    e.add_argument("--out", required=True)
    a = p.parse_args()
    torch.set_num_threads(4)
    device = "mps" if torch.backends.mps.is_available() and not getattr(a, "cpu", False) else "cpu"
    if a.cmd == "export":
        m = load(a.ckpt, "cpu")
        export_llama2c(m, a.out)
        print("wrote %s (%d params)" % (a.out, m.n_params()))
        return
    val = Tokens("val")
    res = {}
    for path in a.model:
        m = load(path, device)
        for qb in (int(q) for q in a.quant.split(",")):
            v = val_loss(m, val, a.length, a.batches, a.batch, qb, device)
            key = "%s@%s" % (os.path.basename(path), "float" if qb == 0 else "int%d" % qb)
            res[key] = v
            print("%-40s length %d  loss %.4f" % (key, a.length, v), flush=True)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=1)


if __name__ == "__main__":
    main()
