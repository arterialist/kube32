"""A llama2.c Transformer in PyTorch: RMSNorm, rotary embedding, SwiGLU,
grouped-query attention and a classifier tied to the token embedding.

causal=True is the autoregressive model llama2.c runs. causal=False is a
masked diffusion model: attention over every position and one more input
token, MASK (id vocab), that it never predicts.
"""
import math
import struct
from dataclasses import dataclass, asdict

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F


@dataclass
class Config:
    dim: int = 256
    hidden: int = 640
    n_layers: int = 5
    n_heads: int = 8
    n_kv_heads: int = 4
    vocab: int = 512
    seq_len: int = 256
    causal: bool = True
    dropout: float = 0.0

    def to_dict(self):
        return asdict(self)


class RMSNorm(nn.Module):
    def __init__(self, dim, eps=1e-5):
        super().__init__()
        self.eps = eps
        self.weight = nn.Parameter(torch.ones(dim))

    def forward(self, x):
        xf = x.float()
        return (xf * torch.rsqrt(xf.pow(2).mean(-1, keepdim=True) + self.eps)).type_as(x) * self.weight


def rope_tables(head_size, seq_len, theta=10000.0):
    freqs = 1.0 / (theta ** (torch.arange(0, head_size, 2)[: head_size // 2].float() / head_size))
    t = torch.arange(seq_len).float()
    f = torch.outer(t, freqs)
    return torch.cos(f), torch.sin(f)


def apply_rope(x, cos, sin):
    # x: (B, T, H, hs); pairs (2i, 2i+1) rotate by the angle of position t
    xr = x.float().reshape(*x.shape[:-1], -1, 2)
    a, b = xr[..., 0], xr[..., 1]
    c = cos[None, :, None, :]
    s = sin[None, :, None, :]
    out = torch.stack([a * c - b * s, a * s + b * c], dim=-1).flatten(3)
    return out.type_as(x)


class Attention(nn.Module):
    def __init__(self, cfg):
        super().__init__()
        self.cfg = cfg
        self.hs = cfg.dim // cfg.n_heads
        self.wq = nn.Linear(cfg.dim, cfg.n_heads * self.hs, bias=False)
        self.wk = nn.Linear(cfg.dim, cfg.n_kv_heads * self.hs, bias=False)
        self.wv = nn.Linear(cfg.dim, cfg.n_kv_heads * self.hs, bias=False)
        self.wo = nn.Linear(cfg.n_heads * self.hs, cfg.dim, bias=False)

    def forward(self, x, cos, sin):
        B, T, _ = x.shape
        c = self.cfg
        q = self.wq(x).view(B, T, c.n_heads, self.hs)
        k = self.wk(x).view(B, T, c.n_kv_heads, self.hs)
        v = self.wv(x).view(B, T, c.n_kv_heads, self.hs)
        q, k = apply_rope(q, cos, sin), apply_rope(k, cos, sin)
        rep = c.n_heads // c.n_kv_heads
        if rep > 1:
            k = k.repeat_interleave(rep, dim=2)
            v = v.repeat_interleave(rep, dim=2)
        q, k, v = q.transpose(1, 2), k.transpose(1, 2), v.transpose(1, 2)
        y = F.scaled_dot_product_attention(q, k, v, is_causal=c.causal,
                                           dropout_p=c.dropout if self.training else 0.0)
        return self.wo(y.transpose(1, 2).reshape(B, T, -1))


class FeedForward(nn.Module):
    def __init__(self, cfg):
        super().__init__()
        self.w1 = nn.Linear(cfg.dim, cfg.hidden, bias=False)
        self.w2 = nn.Linear(cfg.hidden, cfg.dim, bias=False)
        self.w3 = nn.Linear(cfg.dim, cfg.hidden, bias=False)

    def forward(self, x):
        return self.w2(F.silu(self.w1(x)) * self.w3(x))


class Block(nn.Module):
    def __init__(self, cfg):
        super().__init__()
        self.attention_norm = RMSNorm(cfg.dim)
        self.attention = Attention(cfg)
        self.ffn_norm = RMSNorm(cfg.dim)
        self.feed_forward = FeedForward(cfg)

    def forward(self, x, cos, sin):
        x = x + self.attention(self.attention_norm(x), cos, sin)
        return x + self.feed_forward(self.ffn_norm(x))


class Transformer(nn.Module):
    def __init__(self, cfg):
        super().__init__()
        self.cfg = cfg
        n_in = cfg.vocab + (0 if cfg.causal else 1)   # + MASK
        self.tok_embeddings = nn.Embedding(n_in, cfg.dim)
        self.layers = nn.ModuleList(Block(cfg) for _ in range(cfg.n_layers))
        self.norm = RMSNorm(cfg.dim)
        cos, sin = rope_tables(cfg.dim // cfg.n_heads, cfg.seq_len)
        self.register_buffer("cos", cos, persistent=False)
        self.register_buffer("sin", sin, persistent=False)
        self.apply(self._init)
        for n, p in self.named_parameters():
            if n.endswith("wo.weight") or n.endswith("w2.weight"):
                nn.init.normal_(p, mean=0.0, std=0.02 / math.sqrt(2 * cfg.n_layers))

    @staticmethod
    def _init(m):
        if isinstance(m, (nn.Linear, nn.Embedding)):
            nn.init.normal_(m.weight, mean=0.0, std=0.02)

    def forward(self, tokens):
        T = tokens.shape[1]
        h = self.tok_embeddings(tokens)
        for layer in self.layers:
            h = layer(h, self.cos[:T], self.sin[:T])
        h = self.norm(h)
        # the classifier is the embedding's first vocab rows (MASK is never an output)
        return F.linear(h, self.tok_embeddings.weight[: self.cfg.vocab])

    def n_params(self):
        return sum(p.numel() for p in self.parameters())


def export_llama2c(model, path):
    """llama2.c's checkpoint (run.c, version 0): 7 int32 of config, then
    float32 tensors; a positive vocabulary size means the classifier is the
    embedding"""
    c = model.cfg
    sd = {k: v.detach().float().cpu().numpy() for k, v in model.state_dict().items()}
    L = range(c.n_layers)
    hs = c.dim // c.n_heads
    cos, sin = rope_tables(hs, c.seq_len)
    parts = [sd["tok_embeddings.weight"][: c.vocab]]
    parts += [sd["layers.%d.attention_norm.weight" % l] for l in L]
    for name in ("wq", "wk", "wv", "wo"):
        parts += [sd["layers.%d.attention.%s.weight" % (l, name)] for l in L]
    parts += [sd["layers.%d.ffn_norm.weight" % l] for l in L]
    for name in ("w1", "w2", "w3"):
        parts += [sd["layers.%d.feed_forward.%s.weight" % (l, name)] for l in L]
    parts += [sd["norm.weight"], cos.numpy(), sin.numpy()]
    with open(path, "wb") as f:
        f.write(struct.pack("<7i", c.dim, c.hidden, c.n_layers, c.n_heads, c.n_kv_heads, c.vocab, c.seq_len))
        for p in parts:
            f.write(np.ascontiguousarray(p, dtype="<f4").tobytes())
