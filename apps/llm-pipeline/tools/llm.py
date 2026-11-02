#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["numpy>=1.24"]
# ///
"""Host side of llm-pipeline: the model's files, the manifest, a reference
run, and a client.

  llm.py fetch                       download stories260K and its tokenizer
  llm.py build --split 0-2,3,4       quantize to int8 and write one slice per
                                     stage plus the tokenizer as llm-store's
                                     bundle; prints the memory of each stage
  llm.py manifest                    examples/llm-pipeline.yaml with the
                                     modules inline, ready for kubectl apply
  llm.py generate --prompt TEXT      the same generation, computed here from
                                     the slices: the tokens the pipeline must
                                     produce
  llm.py run --url URL --prompt TEXT start a generation on the coordinator,
                                     print the text as it arrives, then the
                                     speed and the time of each stage and hop

The reference does every float operation of the pods (llm.h) in the same
order in IEEE binary32, so its tokens match the pipeline's exactly.

Run it with uv (`uv run --script tools/llm.py ...`) or a Python with numpy.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import struct
import sys
import time
import urllib.error
import urllib.request

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(APP))
BUILD = os.path.join(APP, "build")

MODEL_URL = "https://huggingface.co/karpathy/tinyllamas/resolve/main/stories260K/"
MODEL_FILES = {
    "stories260K.bin": "b0a507e7ad0f626624f17112325e66691f9076d622e1d3274d103d00299f2696",
    "tok512.bin": "037cb335abb25d1fa9e8ecae30ed2a3a8ace9302862ebcdc05d51a6bbb10c312",
}

BLOB_MAGIC = 0x4D32334B          # "K32M"
FLAG_EMBED, FLAG_HEAD = 1, 2
BOS = 1
F32 = np.float32


# ---- model files ----------------------------------------------------------

def fetch(model_dir):
    os.makedirs(model_dir, exist_ok=True)
    for name, digest in MODEL_FILES.items():
        path = os.path.join(model_dir, name)
        if os.path.exists(path) and sha256(path) == digest:
            continue
        print("downloading %s%s" % (MODEL_URL, name))
        with urllib.request.urlopen(MODEL_URL + name, timeout=60) as r:
            data = r.read()
        if hashlib.sha256(data).hexdigest() != digest:
            sys.exit("%s: unexpected SHA-256" % name)
        with open(path + ".part", "wb") as f:
            f.write(data)
        os.replace(path + ".part", path)
    return os.path.join(model_dir, "stories260K.bin"), os.path.join(model_dir, "tok512.bin")


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def read_checkpoint(path):
    """llama2.c's checkpoint: 7 int32 of config, then float32 tensors"""
    raw = open(path, "rb").read()
    dim, hidden, n_layers, n_heads, n_kv_heads, vocab, seq_len = struct.unpack("<7i", raw[:28])
    shared = vocab > 0
    vocab = abs(vocab)
    f = np.frombuffer(raw[28:], dtype="<f4")
    hs = dim // n_heads
    kvd = n_kv_heads * hs
    shapes = [("tok", (vocab, dim)), ("rms_att", (n_layers, dim)), ("wq", (n_layers, dim, dim)),
              ("wk", (n_layers, kvd, dim)), ("wv", (n_layers, kvd, dim)), ("wo", (n_layers, dim, dim)),
              ("rms_ffn", (n_layers, dim)), ("w1", (n_layers, hidden, dim)), ("w2", (n_layers, dim, hidden)),
              ("w3", (n_layers, hidden, dim)), ("rms_final", (dim,)), ("fcr", (seq_len, hs // 2)), ("fci", (seq_len, hs // 2))]
    if not shared:
        shapes.append(("cls", (vocab, dim)))
    w, o = {}, 0
    for name, shape in shapes:
        n = int(np.prod(shape))
        w[name] = f[o:o + n].reshape(shape).astype(F32)
        o += n
    if o != len(f):
        sys.exit("%s: %d floats left after the tensors: not a llama2.c checkpoint" % (path, len(f) - o))
    if shared:
        w["cls"] = w["tok"]
    cfg = dict(dim=dim, hidden=hidden, n_layers=n_layers, n_heads=n_heads, n_kv_heads=n_kv_heads, vocab=vocab, seq_len=seq_len)
    return cfg, w


def q8(w, bits=8):
    """Rows of int8 (or int4 in -7..7) with one float32 scale per row
    (max |w| / 127, or / 7)"""
    top = F32(127 if bits == 8 else 7)
    amax = np.abs(w).max(axis=-1)
    s = (amax / top).astype(F32)
    safe = np.where(s == 0, F32(1), s)
    q = np.clip(np.rint(w / safe[..., None]), -top, top).astype(np.int8)
    return q, s


def pack4(q):
    """Two int4 values a byte: column 2j in the low nibble, 2j+1 in the high"""
    u = q.astype(np.uint8) & 0x0F
    return (u[..., 0::2] | (u[..., 1::2] << 4)).astype(np.uint8).view(np.int8)


def unpack4(b):
    b = b.astype(np.int32)
    out = np.empty(b.shape[:-1] + (b.shape[-1] * 2,), np.int8)
    out[..., 0::2] = ((b << 28) >> 28).astype(np.int8)
    out[..., 1::2] = (b >> 4).astype(np.int8)
    return out


def parse_split(text, n_layers):
    stages = []
    for part in text.split(","):
        a, _, b = part.partition("-")
        stages.append((int(a), int(b or a) + 1))
    if stages[0][0] != 0 or stages[-1][1] != n_layers or any(stages[i][1] != stages[i + 1][0] for i in range(len(stages) - 1)):
        sys.exit("--split must cover layers 0-%d in order, e.g. 0-2,3,4" % (n_layers - 1))
    return stages


def blob(cfg, w, begin, end, flags, bits=8):
    dim, hidden, hs = cfg["dim"], cfg["hidden"], cfg["dim"] // cfg["n_heads"]
    parts = []
    if flags & FLAG_EMBED:
        q, s = q8(w["tok"])
        rope = np.stack([w["fcr"], w["fci"]], axis=-1).reshape(cfg["seq_len"], hs)
        parts += [q, s, rope.astype(F32)]
    for l in range(begin, end):
        qs = {k: q8(w[k][l], bits) for k in ("wq", "wk", "wv", "wo", "w1", "w2", "w3")}
        parts += [w["rms_att"][l], qs["wq"][1], qs["wk"][1], qs["wv"][1], qs["wo"][1], w["rms_ffn"][l],
                  qs["w1"][1], qs["w2"][1], qs["w3"][1]]
        parts += [qs[k][0] if bits == 8 else pack4(qs[k][0]) for k in ("wq", "wk", "wv", "wo", "w1", "w2", "w3")]
    if flags & FLAG_HEAD:
        q, s = q8(w["cls"])
        parts += [w["rms_final"], s, q]
    body = b"".join(np.ascontiguousarray(p).astype(p.dtype.newbyteorder("<")).tobytes() for p in parts)
    header = struct.pack("<16I", BLOB_MAGIC, 1, dim, hidden, cfg["n_heads"], cfg["n_kv_heads"], cfg["vocab"], cfg["seq_len"],
                         begin, end, cfg["n_layers"], flags, 64 + len(body), bits, 0, 0)
    return header + body


def read_blob(data):
    h = struct.unpack("<16I", data[:64])
    if h[0] != BLOB_MAGIC or h[1] != 1 or h[12] != len(data):
        raise ValueError("not a model slice")
    cfg = dict(dim=h[2], hidden=h[3], n_heads=h[4], n_kv_heads=h[5], vocab=h[6], seq_len=h[7], n_layers=h[10])
    begin, end, flags, bits = h[8], h[9], h[11], h[13]
    if bits not in (4, 8):
        raise ValueError("model slice: %d-bit weights" % bits)
    dim, hidden, vocab = cfg["dim"], cfg["hidden"], cfg["vocab"]
    hs = dim // cfg["n_heads"]
    kvd = cfg["n_kv_heads"] * hs
    o = 64

    def take(dtype, shape):
        nonlocal o
        n = int(np.prod(shape)) * np.dtype(dtype).itemsize
        a = np.frombuffer(data[o:o + n], dtype=dtype).reshape(shape)
        o += n
        return a

    out = {"cfg": cfg, "begin": begin, "end": end, "flags": flags, "bits": bits, "layers": []}
    if flags & FLAG_EMBED:
        out["tok_q"] = take("i1", (vocab, dim))
        out["tok_s"] = take("<f4", (vocab,))
        out["rope"] = take("<f4", (cfg["seq_len"], hs))
    for _ in range(begin, end):
        L = {}
        for k, n in (("rms_att", dim), ("wq_s", dim), ("wk_s", kvd), ("wv_s", kvd), ("wo_s", dim), ("rms_ffn", dim),
                     ("w1_s", hidden), ("w2_s", dim), ("w3_s", hidden)):
            L[k] = take("<f4", (n,))
        for k, shape in (("wq", (dim, dim)), ("wk", (kvd, dim)), ("wv", (kvd, dim)), ("wo", (dim, dim)),
                         ("w1", (hidden, dim)), ("w2", (dim, hidden)), ("w3", (hidden, dim))):
            L[k] = take("i1", shape) if bits == 8 else unpack4(take("i1", (shape[0], shape[1] // 2)))
        out["layers"].append(L)
    if flags & FLAG_HEAD:
        out["rms_final"] = take("<f4", (dim,))
        out["cls_s"] = take("<f4", (vocab,))
        out["cls_q"] = take("i1", (vocab, dim))
    if o != len(data):
        raise ValueError("model slice: %d bytes left over" % (len(data) - o))
    return out


# ---- the arithmetic of llm.h, operation for operation ----------------------

def k_exp(x):
    x = np.asarray(x, dtype=F32)
    under = x < F32(-87.0)
    x = np.minimum(x, F32(88.0))
    t = x * F32(1.44269504)
    n = (t + np.where(t >= 0, F32(0.5), F32(-0.5))).astype(np.int32)
    nf = n.astype(F32)
    r = x - nf * F32(0.693145752)
    r = r - nf * F32(1.42860677e-6)
    p = np.full_like(x, F32(1.38888889e-3))
    for c in (8.33333333e-3, 4.16666667e-2, 1.66666667e-1, 0.5, 1.0, 1.0):
        p = p * r + F32(c)
    b = (np.clip(n + 127, 1, 254).astype(np.uint32) << 23).view(F32)
    return np.where(under, F32(0.0), p * b).astype(F32)


def seqsum(a, axis=0):
    """a[0] + a[1] + ... from left to right in float32, as a C loop adds"""
    return np.add.accumulate(a.astype(F32), axis=axis, dtype=F32)[-1] if axis == 0 else \
        np.add.accumulate(a.astype(F32), axis=axis, dtype=F32)[..., -1]


def rmsnorm(x, w):
    ss = seqsum(x * x)
    ss = F32(ss / F32(len(x)))
    ss = F32(ss + F32(1e-5))
    ss = F32(F32(1.0) / np.sqrt(ss))
    return (w * (ss * x)).astype(F32)


def quantize(x):
    amax = F32(np.max(np.abs(x))) if len(x) else F32(0)
    if amax == 0:
        return np.zeros(len(x), np.int8), F32(0.0)
    inv = F32(F32(127.0) / amax)
    t = (x * inv).astype(F32)
    v = (t + np.where(t >= 0, F32(0.5), F32(-0.5))).astype(np.int32)
    return np.clip(v, -127, 127).astype(np.int8), F32(amax / F32(127.0))


def matvec(wq, ws, xq, xs):
    acc = wq.astype(np.int32) @ xq.astype(np.int32)
    return (acc.astype(F32) * (ws * xs).astype(F32)).astype(F32)


def rope(v, cs, hs):
    v = v.copy()
    reps = len(v) // hs
    c = np.tile(cs[0::2], reps)
    s = np.tile(cs[1::2], reps)
    v0, v1 = v[0::2].copy(), v[1::2].copy()
    v[0::2] = v0 * c - v1 * s
    v[1::2] = v0 * s + v1 * c
    return v


class Reference:
    """The model as the stages compute it, layer after layer, from their slices"""

    def __init__(self, slices, ctx):
        first, last = slices[0], slices[-1]
        self.cfg = first["cfg"]
        self.tok_q, self.tok_s, self.rope = first["tok_q"], first["tok_s"], first["rope"]
        self.rms_final, self.cls_s, self.cls_q = last["rms_final"], last["cls_s"], last["cls_q"]
        self.layers = [L for s in slices for L in s["layers"]]
        c = self.cfg
        self.hs = c["dim"] // c["n_heads"]
        self.kvd = c["n_kv_heads"] * self.hs
        n = len(self.layers)
        self.kq = np.zeros((n, ctx, self.kvd), np.int8)
        self.vq = np.zeros((n, ctx, self.kvd), np.int8)
        self.ks = np.zeros((n, ctx, c["n_kv_heads"]), F32)
        self.vs = np.zeros((n, ctx, c["n_kv_heads"]), F32)

    def layer(self, l, x, pos, cs):
        c, L, hs, kvh = self.cfg, self.layers[l], self.hs, self.cfg["n_kv_heads"]
        xb = rmsnorm(x, L["rms_att"])
        xq, s = quantize(xb)
        q = rope(matvec(L["wq"], L["wq_s"], xq, s), cs, hs)
        k = rope(matvec(L["wk"], L["wk_s"], xq, s), cs, hs)
        v = matvec(L["wv"], L["wv_s"], xq, s)
        for h in range(kvh):
            self.kq[l, pos, h * hs:(h + 1) * hs], self.ks[l, pos, h] = quantize(k[h * hs:(h + 1) * hs])
            self.vq[l, pos, h * hs:(h + 1) * hs], self.vs[l, pos, h] = quantize(v[h * hs:(h + 1) * hs])
        scale = F32(F32(1.0) / np.sqrt(F32(hs)))
        group = c["n_heads"] // kvh
        out = np.zeros(c["dim"], F32)
        for h in range(c["n_heads"]):
            kh = h // group
            qh = q[h * hs:(h + 1) * hs]
            kt = self.kq[l, :pos + 1, kh * hs:(kh + 1) * hs].astype(F32)
            d = np.zeros(pos + 1, F32)
            for i in range(hs):
                d = (d + qh[i] * kt[:, i]).astype(F32)
            att = ((d * self.ks[l, :pos + 1, kh]).astype(F32) * scale).astype(F32)
            att = k_exp((att - np.max(att)).astype(F32))
            att = (att / seqsum(att)).astype(F32)
            a = (att * self.vs[l, :pos + 1, kh]).astype(F32)
            vt = self.vq[l, :pos + 1, kh * hs:(kh + 1) * hs].astype(F32)
            out[h * hs:(h + 1) * hs] = seqsum((a[:, None] * vt).astype(F32))
        xq, s = quantize(out)
        x = (x + matvec(L["wo"], L["wo_s"], xq, s)).astype(F32)
        xb = rmsnorm(x, L["rms_ffn"])
        xq, s = quantize(xb)
        h1 = matvec(L["w1"], L["w1_s"], xq, s)
        h3 = matvec(L["w3"], L["w3_s"], xq, s)
        h1 = (h1 * (F32(1.0) / (F32(1.0) + k_exp(-h1))).astype(F32)).astype(F32)
        h1 = (h1 * h3).astype(F32)
        xq, s = quantize(h1)
        return (x + matvec(L["w2"], L["w2_s"], xq, s)).astype(F32)

    def step(self, token, pos, sample):
        x = (self.tok_q[token].astype(F32) * self.tok_s[token]).astype(F32)
        cs = self.rope[pos]
        for l in range(len(self.layers)):
            x = self.layer(l, x, pos, cs)
        if not sample:
            return None
        xq, s = quantize(rmsnorm(x, self.rms_final))
        logits = matvec(self.cls_q, self.cls_s, xq, s)
        return int(np.argmax(logits))


# ---- tokenizer (llama2.c run.c) --------------------------------------------

class Tokenizer:
    def __init__(self, path):
        raw = open(path, "rb").read()
        p, self.vocab, self.scores = 4, [], []
        while p < len(raw):
            score, n = struct.unpack("<fi", raw[p:p + 8])
            self.vocab.append(raw[p + 8:p + 8 + n])
            self.scores.append(score)
            p += 8 + n
        self.index = {}
        for i, piece in enumerate(self.vocab):
            self.index.setdefault(piece, i)

    def encode(self, text):
        b = text.encode("utf-8")
        toks = [BOS]
        if b:
            toks.append(self.index[b" "])
        i = 0
        while i < len(b):
            cl = 1
            while i + cl < len(b) and cl < 4 and (b[i + cl] & 0xC0) == 0x80:
                cl += 1
            ch = b[i:i + cl]
            if ch in self.index:
                toks.append(self.index[ch])
            else:
                toks += [c + 3 for c in ch]
            i += cl
        while True:
            best, best_id, best_at = -1e10, -1, -1
            for i in range(len(toks) - 1):
                tid = self.index.get(self.vocab[toks[i]] + self.vocab[toks[i + 1]])
                if tid is not None and self.scores[tid] > best:
                    best, best_id, best_at = self.scores[tid], tid, i
            if best_at < 0:
                return toks
            toks[best_at:best_at + 2] = [best_id]

    def piece(self, prev, tok):
        p = self.vocab[tok]
        if prev == BOS and p.startswith(b" "):
            p = p[1:]
        m = re.fullmatch(rb"<0x([0-9A-Fa-f]{2})>", p)
        if m:
            b = int(m.group(1), 16)
            return b"" if b < 0x20 and b != 0x0A else bytes([b])
        return p

    def decode(self, toks):
        return b"".join(self.piece(toks[i - 1], toks[i]) for i in range(1, len(toks))).decode("utf-8", "replace")


def generate(card, prompt, steps, ctx):
    """The tokens the coordinator ends with for this prompt"""
    files = sorted((f for f in os.listdir(card) if re.fullmatch(r"shard-\d+\.bin", f)), key=lambda f: int(f[6:-4]))
    slices = [read_blob(open(os.path.join(card, f), "rb").read()) for f in files]
    tok = Tokenizer(os.path.join(card, "tokenizer.bin"))
    toks = tok.encode(prompt)
    n_prompt = len(toks)
    if n_prompt >= ctx:
        sys.exit("the prompt (%d tokens) does not fit a context of %d" % (n_prompt, ctx))
    ref = Reference(slices, ctx)
    pos = 0
    while True:
        nxt = ref.step(toks[pos], pos, pos == len(toks) - 1)
        pos += 1
        if nxt is None:
            continue
        if nxt == BOS:
            break
        toks.append(nxt)
        if len(toks) - n_prompt >= steps or len(toks) >= ctx:
            break
    return toks, n_prompt, tok


# ---- commands ---------------------------------------------------------------

def per_position_bytes(layers, cfg):
    hs = cfg["dim"] // cfg["n_heads"]
    kvd = cfg["n_kv_heads"] * hs
    return layers * 2 * (kvd + 4 * cfg["n_kv_heads"]) + 4


def cmd_build(a):
    model, tokfile = (a.model, a.tokenizer) if a.model else fetch(a.model_dir)
    cfg, w = read_checkpoint(model)
    stages = parse_split(a.split, cfg["n_layers"])
    bits = [int(b) for b in a.bits.split(",")] if a.bits else [8] * len(stages)
    if len(bits) != len(stages) or any(b not in (4, 8) for b in bits):
        sys.exit("--bits needs 4 or 8 for each of the %d stages" % len(stages))
    bundle = os.path.join(a.card, "kube32", "images", "llm-store")
    os.makedirs(bundle, exist_ok=True)
    for f in os.listdir(bundle):
        if re.fullmatch(r"shard-\d+\.bin", f):
            os.remove(os.path.join(bundle, f))
    print("model: dim %(dim)d, hidden %(hidden)d, %(n_layers)d layers, %(n_heads)d heads (%(n_kv_heads)d KV), "
          "vocabulary %(vocab)d, context %(seq_len)d" % cfg)
    print("%-6s %-8s %-5s %-28s %9s %15s" % ("stage", "layers", "bits", "parts", "bytes", "KV per position"))
    for i, (b, e) in enumerate(stages):
        flags = (FLAG_EMBED if i == 0 else 0) | (FLAG_HEAD if i == len(stages) - 1 else 0)
        data = blob(cfg, w, b, e, flags, bits[i])
        with open(os.path.join(bundle, "shard-%d.bin" % i), "wb") as f:
            f.write(data)
        parts = ["layers"] + (["embedding", "rotary table"] if flags & FLAG_EMBED else []) + (["classifier"] if flags & FLAG_HEAD else [])
        print("%-6d %-8s %-5d %-28s %9d %15d" % (i, "%d-%d" % (b, e - 1), bits[i], ", ".join(parts), len(data), per_position_bytes(e - b, cfg)))
    with open(tokfile, "rb") as src, open(os.path.join(bundle, "tokenizer.bin"), "wb") as dst:
        dst.write(src.read())
    if a.store_module:
        with open(a.store_module, "rb") as src, open(os.path.join(a.card, "kube32", "images", "llm-store.wasm"), "wb") as dst:
            dst.write(src.read())
    print("wrote %s" % bundle)


def cmd_manifest(a):
    text = open(a.template).read()

    def inline(m):
        path = os.path.join(a.modules, m.group(2))
        data = open(path, "rb").read()
        return '%skube32.io/wasm-base64: "%s"' % (m.group(1), base64.b64encode(data).decode("ascii"))

    text = re.sub(r'(\s)kube32\.io/wasm-base64: "@([\w.-]+)"', inline, text)
    if a.host_node:
        text = text.replace('kube32.io/simulator: "true"', "kubernetes.io/hostname: %s" % a.host_node)
    if a.prompt is not None:
        text = re.sub(r'(- name: PROMPT[^\n]*\n\s+value: )"[^"\n]*"', lambda m: m.group(1) + json.dumps(a.prompt), text)
    with open(a.out, "w") as f:
        f.write(text)
    print("wrote %s" % a.out)


def cmd_generate(a):
    toks, n_prompt, tok = generate(a.card, a.prompt, a.steps, a.context)
    if a.json:
        print(json.dumps({"tokens": toks, "prompt_tokens": n_prompt, "text": tok.decode(toks)}))
    else:
        print(tok.decode(toks))
        print("\n%d prompt tokens, %d generated" % (n_prompt, len(toks) - n_prompt))


def http(method, url, body=None, timeout=10.0):
    req = urllib.request.Request(url, data=body, method=method, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def print_stats(st):
    print("\n%d prompt tokens, %d generated, state %s, %d replays" % (st["prompt_tokens"], st["generated"], st["state"], st["replays"]))
    print("prefill %d ms, decode %.2f tokens/s, token round trip %.1f ms (min %d, max %d)" %
          (st["prefill_ms"], st["tokens_per_s"], st["token_ms"]["mean"], st["token_ms"]["min"], st["token_ms"]["max"]))
    print("%-6s %-7s %-22s %11s %9s %12s" % ("stage", "layers", "node", "compute ms", "wait ms", "next hop ms"))
    for s in st["stages"]:
        print("%-6d %-7s %-22s %11.1f %9.1f %12.1f" % (s["stage"], "%d-%d" % (s["layers"][0], s["layers"][1] - 1), s["node"],
                                                      s["compute_ms"], s["wait_ms"], s["hop_ms"]))
    print("coordinator -> stage 0: %.1f ms" % st["first_hop_ms"])


def cmd_run(a):
    url = a.url.rstrip("/")
    r = http("POST", url + "/generate", json.dumps({"prompt": a.prompt, "steps": a.steps}).encode())
    session = r["session"]
    shown = 0
    deadline = time.time() + a.timeout
    while True:
        if time.time() > deadline:
            sys.exit("no end of the generation within %d s" % a.timeout)
        st = http("GET", url + "/")
        if st["session"] != session and st["replays"] == 0:
            sys.exit("another generation started")
        text = st["text"]
        if len(text) > shown:
            sys.stdout.write(text[shown:])
            sys.stdout.flush()
            shown = len(text)
        if st["state"] != "running":
            break
        time.sleep(0.25)
    print()
    print_stats(st)
    if a.check:
        card = a.card
        toks, _, _ = generate(card, a.prompt, a.steps, st["context"])
        if toks != st["tokens"]:
            sys.exit("the pipeline's tokens differ from the reference's")
        print("tokens match the reference")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch", help="download the model and tokenizer")
    f.add_argument("--model-dir", default=os.path.join(BUILD, "model"))
    b = sub.add_parser("build", help="quantize and split the model into llm-store's bundle")
    b.add_argument("--model-dir", default=os.path.join(BUILD, "model"))
    b.add_argument("--model", help="a llama2.c checkpoint (default: stories260K, downloaded)")
    b.add_argument("--tokenizer", help="its tokenizer.bin (with --model)")
    b.add_argument("--split", default="0-2,3,4", help="layers of each stage (default 0-2,3,4)")
    b.add_argument("--bits", default="8,4,8", help="weight bits of each stage's layers, 8 or 4 (default 8,4,8)")
    b.add_argument("--card", default=os.path.join(BUILD, "card"), help="storage root for the model host (KUBE32_SD_ROOT)")
    b.add_argument("--store-module", help="llm-store.wasm to put next to its bundle")
    m = sub.add_parser("manifest", help="render the manifest with the modules inline")
    m.add_argument("--template", default=os.path.join(REPO, "examples", "llm-pipeline.yaml"))
    m.add_argument("--modules", default=BUILD)
    m.add_argument("--out", default=os.path.join(BUILD, "llm-pipeline.yaml"))
    m.add_argument("--host-node", help="pin the model host's pods to this node instead of the kube32-sim node")
    m.add_argument("--prompt", help="the PROMPT the coordinator generates at start (\"\" for none)")
    g = sub.add_parser("generate", help="the reference generation")
    g.add_argument("--card-bundle", dest="card", default=os.path.join(BUILD, "card", "kube32", "images", "llm-store"))
    g.add_argument("--prompt", default="Once upon a time")
    g.add_argument("--steps", type=int, default=96)
    g.add_argument("--context", type=int, default=128)
    g.add_argument("--json", action="store_true")
    r = sub.add_parser("run", help="generate on the cluster and print speed and hop times")
    r.add_argument("--url", required=True, help="the coordinator, e.g. http://<control plane>/llm")
    r.add_argument("--prompt", default="Once upon a time")
    r.add_argument("--steps", type=int, default=96)
    r.add_argument("--check", action="store_true", help="compare the tokens with the reference")
    r.add_argument("--timeout", type=int, default=600, help="seconds to wait for the generation")
    r.add_argument("--card-bundle", dest="card", default=os.path.join(BUILD, "card", "kube32", "images", "llm-store"))
    a = p.parse_args()
    {"fetch": lambda a: fetch(a.model_dir), "build": cmd_build, "manifest": cmd_manifest,
     "generate": cmd_generate, "run": cmd_run}[a.cmd](a)


if __name__ == "__main__":
    main()
