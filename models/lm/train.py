"""Trains a tok512 Transformer on TinyStories (tinystories.py's tokens).

  train.py --mode ar   --out DIR ...   autoregressive (llama2.c), the
                                       speculative-decoding target
  train.py --mode mdlm --out DIR ...   masked diffusion (MDLM/LLaDA): every
                                       position attends to every other, a
                                       random fraction t of the tokens is
                                       MASK and the loss is their cross
                                       entropy over t

The learning rate warms up, then follows a cosine to --min-lr that ends at
--steps or after --minutes, whichever comes first; so does the run. It
keeps DIR/ckpt.pt (resumable with --resume) and DIR/log.jsonl.
"""
import argparse
import glob
import json
import math
import os
import time

import numpy as np
import torch
import torch.nn.functional as F

from model import Config, Transformer

DATA = os.path.expanduser("~/.cache/kube32-models/data/tinystories/tok512")
BOS = 1


class Tokens:
    """The train shards (data01..) or the validation shard (data00) as one
    array, and the offsets where a story starts"""

    def __init__(self, split):
        files = sorted(glob.glob(os.path.join(DATA, "data*.bin")))
        files = files[:1] if split == "val" else files[1:]
        self.data = np.concatenate([np.fromfile(f, dtype=np.uint16) for f in files])
        self.starts = np.flatnonzero(self.data == BOS)

    def batch(self, rng, n, length, story_start=0.0):
        """n windows of length tokens; a fraction story_start of them begin
        at a story's BOS"""
        pos = rng.integers(0, len(self.data) - length, n)
        at = rng.random(n) < story_start
        if at.any():
            s = self.starts[rng.integers(0, len(self.starts), at.sum())]
            pos[at] = np.minimum(s, len(self.data) - length)
        idx = pos[:, None] + np.arange(length)[None, :]
        return torch.from_numpy(self.data[idx].astype(np.int64))


def mdlm_loss(model, x, gen, eps=1e-3):
    """MDLM's bound with a linear schedule: each sequence draws t (stratified
    over the batch), masks each token with probability t and pays 1/t times
    the cross entropy of the masked ones, per position"""
    B, L = x.shape
    t = (torch.rand(1, generator=gen, device=x.device) + torch.arange(B, device=x.device) / B) % 1.0
    t = eps + (1 - eps) * t
    masked = torch.rand(B, L, generator=gen, device=x.device) < t[:, None]
    inp = torch.where(masked, torch.full_like(x, model.cfg.vocab), x)
    logits = model(inp)
    ce = F.cross_entropy(logits.float().reshape(-1, logits.shape[-1]), x.reshape(-1), reduction="none").view(B, L)
    return ((ce * masked) / t[:, None]).sum() / (B * L)


def ar_loss(model, x):
    logits = model(x[:, :-1])
    return F.cross_entropy(logits.float().reshape(-1, logits.shape[-1]), x[:, 1:].reshape(-1))


@torch.no_grad()
def evaluate(model, val, mode, device, batches=20, batch=64, seed=1234):
    model.eval()
    rng = np.random.default_rng(seed)
    gen = torch.Generator(device=device).manual_seed(seed)
    L = model.cfg.seq_len + (1 if mode == "ar" else 0)
    tot = 0.0
    for _ in range(batches):
        x = val.batch(rng, batch, L, 0.5 if mode == "mdlm" else 0.0).to(device)
        tot += (ar_loss(model, x) if mode == "ar" else mdlm_loss(model, x, gen)).item()
    model.train()
    return tot / batches


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--mode", choices=["ar", "mdlm"], default="ar")
    p.add_argument("--out", required=True)
    p.add_argument("--dim", type=int, default=256)
    p.add_argument("--hidden", type=int, default=640)
    p.add_argument("--layers", type=int, default=5)
    p.add_argument("--heads", type=int, default=8)
    p.add_argument("--kv-heads", type=int, default=4)
    p.add_argument("--seq-len", type=int, default=256)
    p.add_argument("--batch", type=int, default=64)
    p.add_argument("--lr", type=float, default=2e-3)
    p.add_argument("--min-lr", type=float, default=2e-4)
    p.add_argument("--wd", type=float, default=0.1)
    p.add_argument("--warmup", type=int, default=300)
    p.add_argument("--steps", type=int, default=20000)
    p.add_argument("--minutes", type=float, default=60)
    p.add_argument("--eval-every", type=int, default=500)
    p.add_argument("--bf16", action="store_true")
    p.add_argument("--compile", action="store_true")
    p.add_argument("--resume", action="store_true")
    p.add_argument("--seed", type=int, default=0)
    a = p.parse_args()

    torch.set_num_threads(4)
    torch.manual_seed(a.seed)
    device = "mps" if torch.backends.mps.is_available() else "cpu"
    os.makedirs(a.out, exist_ok=True)
    cfg = Config(dim=a.dim, hidden=a.hidden, n_layers=a.layers, n_heads=a.heads, n_kv_heads=a.kv_heads,
                 vocab=512, seq_len=a.seq_len, causal=a.mode == "ar")
    model = Transformer(cfg).to(device)
    decay = [p_ for n, p_ in model.named_parameters() if p_.dim() >= 2]
    other = [p_ for n, p_ in model.named_parameters() if p_.dim() < 2]
    opt = torch.optim.AdamW([{"params": decay, "weight_decay": a.wd}, {"params": other, "weight_decay": 0.0}],
                            lr=a.lr, betas=(0.9, 0.95), fused=False)
    step, best, elapsed0 = 0, float("inf"), 0.0
    ck = os.path.join(a.out, "ckpt.pt")
    if a.resume and os.path.exists(ck):
        s = torch.load(ck, map_location=device)
        model.load_state_dict(s["model"])
        opt.load_state_dict(s["opt"])
        step, best, elapsed0 = s["step"], s["best"], s.get("elapsed", 0.0)
        print("resumed at step %d" % step)
    print("params %d (%.2fM), mode %s, device %s" % (model.n_params(), model.n_params() / 1e6, a.mode, device))
    fwd = torch.compile(model) if a.compile else model
    train, val = Tokens("train"), Tokens("val")
    print("train tokens %d, val tokens %d" % (len(train.data), len(val.data)))
    rng = np.random.default_rng(a.seed + step)
    gen = torch.Generator(device=device).manual_seed(a.seed + step)
    L = a.seq_len + (1 if a.mode == "ar" else 0)
    log = open(os.path.join(a.out, "log.jsonl"), "a")
    t0 = time.time()

    def lr_at(s):
        if s < a.warmup:
            return a.lr * (s + 1) / a.warmup
        r = min(1.0, (s - a.warmup) / max(1, a.steps - a.warmup))
        # the schedule also ends with the time budget, whichever comes first
        r = max(r, min(1.0, (elapsed0 + time.time() - t0) / (a.minutes * 60)))
        return a.min_lr + 0.5 * (a.lr - a.min_lr) * (1 + math.cos(math.pi * r))

    def save():
        torch.save({"model": model.state_dict(), "opt": opt.state_dict(), "step": step, "best": best,
                    "cfg": cfg.to_dict(), "elapsed": elapsed0 + time.time() - t0, "mode": a.mode}, ck + ".tmp")
        os.replace(ck + ".tmp", ck)

    while step < a.steps:
        for g in opt.param_groups:
            g["lr"] = lr_at(step)
        x = train.batch(rng, a.batch, L, 0.5 if a.mode == "mdlm" else 0.0).to(device)
        with torch.autocast(device_type=device, dtype=torch.bfloat16, enabled=a.bf16):
            loss = ar_loss(fwd, x) if a.mode == "ar" else mdlm_loss(fwd, x, gen)
        opt.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()
        step += 1
        el = elapsed0 + time.time() - t0
        if step % 50 == 0:
            tok_s = step * a.batch * a.seq_len / max(el, 1e-9)
            print("step %d loss %.4f lr %.2e %.0f tok/s %.1f min" % (step, loss.item(), lr_at(step), tok_s, el / 60), flush=True)
        done = step >= a.steps or el > a.minutes * 60
        if step % a.eval_every == 0 or done:
            with torch.autocast(device_type=device, dtype=torch.bfloat16, enabled=a.bf16):
                vl = evaluate(fwd, val, a.mode, device)
            best = min(best, vl)
            rec = {"step": step, "train": loss.item(), "val": vl, "minutes": el / 60, "tokens": step * a.batch * a.seq_len}
            log.write(json.dumps(rec) + "\n")
            log.flush()
            print("eval", rec, flush=True)
            save()
        if done:
            break
    print("finished at step %d, %.1f min" % (step, (elapsed0 + time.time() - t0) / 60))


if __name__ == "__main__":
    main()
