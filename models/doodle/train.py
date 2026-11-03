"""Trains the doodle denoiser on Quick, Draw! bitmaps (fetch.py).

  train.py --out DIR --c0 20 --c1 32 --steps 30000
      float training with an EMA of the weights, then the input scale of
      each convolution from sampler runs, then --qat-steps of training with
      int8 weights and activations simulated; DIR/ckpt.pt holds the final
      weights and scales
"""
import argparse
import json
import math
import os
import time

import numpy as np
import torch
import torch.nn.functional as F

from net import CLASSES, N_COND, Denoiser, alpha_sigma

DATA = os.path.expanduser("~/.cache/kube32-models/data/quickdraw")
HOLDOUT = 2000   # the last drawings of each class: not trained on


def load(split, device):
    xs, ys = [], []
    for i, c in enumerate(CLASSES):
        a = np.load(os.path.join(DATA, "%s.npy" % c))
        a = a[:-HOLDOUT] if split == "train" else a[-HOLDOUT:]
        xs.append(a)
        ys.append(np.full(len(a), i))
    x = torch.from_numpy(np.concatenate(xs)).to(device)
    y = torch.from_numpy(np.concatenate(ys)).to(device)
    return x, y


def to_float(x8):
    return x8.float().view(-1, 1, 28, 28) / 127.5 - 1.0


@torch.no_grad()
def ddim(model, classes, steps, guidance, gen, device, record=None):
    """float DDIM (eta 0) from t = 1 to 0 in `steps` equal steps"""
    n = len(classes)
    x = torch.randn(n, 1, 28, 28, generator=gen, device=device)
    c = torch.as_tensor(classes, device=device)
    none = torch.full_like(c, N_COND - 1)
    for i in range(steps):
        t = torch.full((n,), 1.0 - i / steps, device=device)
        tn = torch.full((n,), 1.0 - (i + 1) / steps, device=device)
        v = model(x, t, c)
        if guidance != 1.0:
            vu = model(x, t, none)
            v = vu + guidance * (v - vu)
        a, s = alpha_sigma(t)[0].view(-1, 1, 1, 1), alpha_sigma(t)[1].view(-1, 1, 1, 1)
        an, sn = alpha_sigma(tn)[0].view(-1, 1, 1, 1), alpha_sigma(tn)[1].view(-1, 1, 1, 1)
        x0 = (a * x - s * v).clamp(-1, 1)
        eps = s * x + a * v
        x = an * x0 + sn * eps
    return x


@torch.no_grad()
def calibrate(model, steps, guidance, device, n=512, q=0.9999):
    """the q-quantile of |input| of each convolution over sampler runs: rare
    outliers clip instead of costing every value its resolution"""
    model.record = [[] for _ in model.convs]
    gen = torch.Generator(device=device).manual_seed(99)
    classes = [i % len(CLASSES) for i in range(n)]
    for s in (8, steps):
        ddim(model, classes, s, guidance, gen, device)
    tops = [torch.quantile(torch.cat(r)[:1 << 24], q).item() for r in model.record]
    model.record = None
    return tops


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", required=True)
    p.add_argument("--c0", type=int, default=20)
    p.add_argument("--c1", type=int, default=32)
    p.add_argument("--mids", type=int, default=1)
    p.add_argument("--steps", type=int, default=30000)
    p.add_argument("--qat-steps", type=int, default=4000)
    p.add_argument("--batch", type=int, default=256)
    p.add_argument("--lr", type=float, default=2e-3)
    p.add_argument("--ema", type=float, default=0.999)
    p.add_argument("--uncond", type=float, default=0.1)
    p.add_argument("--guidance", type=float, default=2.0)
    p.add_argument("--minutes", type=float, default=60)
    p.add_argument("--from-float", action="store_true", help="start from DIR/float.pt: calibration and QAT only")
    a = p.parse_args()
    torch.set_num_threads(4)
    torch.manual_seed(0)
    device = "mps" if torch.backends.mps.is_available() else "cpu"
    os.makedirs(a.out, exist_ok=True)
    x8, y = load("train", device)
    model = Denoiser(a.c0, a.c1, a.mids).to(device)
    ema = Denoiser(a.c0, a.c1, a.mids).to(device)
    ema.load_state_dict(model.state_dict())
    print("weights %d, parameters %d, train drawings %d" % (model.n_weights(), sum(p_.numel() for p_ in model.parameters()), len(y)))
    log = open(os.path.join(a.out, "log.jsonl"), "a")
    t0 = time.time()
    total = a.steps + a.qat_steps

    def run(steps, lr, phase, deadline):
        """steps at most, and the cosine ends by the deadline (seconds from t0)"""
        opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=0.0)
        avg = 0.0
        start = time.time() - t0
        for step in range(steps):
            r = max(step / steps, (time.time() - t0 - start) / max(1.0, deadline - start))
            if r >= 1.0:
                print("time budget reached")
                break
            for g in opt.param_groups:
                g["lr"] = lr * min(1.0, (step + 1) / 500) * 0.5 * (1 + math.cos(math.pi * r))
            idx = torch.randint(0, len(y), (a.batch,), device=device)
            x0 = to_float(x8[idx])
            flip = torch.rand(a.batch, device=device) < 0.5
            x0 = torch.where(flip.view(-1, 1, 1, 1), x0.flip(3), x0)
            c = torch.where(torch.rand(a.batch, device=device) < a.uncond, torch.full_like(y[idx], N_COND - 1), y[idx])
            t = torch.rand(a.batch, device=device)
            al, si = alpha_sigma(t)
            al, si = al.view(-1, 1, 1, 1), si.view(-1, 1, 1, 1)
            eps = torch.randn_like(x0)
            xt = al * x0 + si * eps
            v = al * eps - si * x0
            loss = F.mse_loss(model(xt, t, c), v)
            opt.zero_grad(set_to_none=True)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            with torch.no_grad():
                for pe, pm in zip(ema.parameters(), model.parameters()):
                    pe.lerp_(pm, 1 - a.ema)
            avg = 0.98 * avg + 0.02 * loss.item() if step else loss.item()
            if (step + 1) % 1000 == 0:
                rec = {"phase": phase, "step": step + 1, "loss": avg, "minutes": (time.time() - t0) / 60}
                log.write(json.dumps(rec) + "\n")
                log.flush()
                print(rec, flush=True)

    if a.from_float:
        ema.load_state_dict(torch.load(os.path.join(a.out, "float.pt"), map_location=device)["model"])
    else:
        run(a.steps, a.lr, "float", a.minutes * 60 * (0.88 if a.qat_steps else 1.0))
        torch.save({"model": ema.state_dict(), "c0": a.c0, "c1": a.c1, "mids": a.mids}, os.path.join(a.out, "float.pt"))
    # the EMA weights, quantization-aware from here on
    model.load_state_dict(ema.state_dict())
    tops = calibrate(model, 16, a.guidance, device)
    scales = [max(t, 1e-6) / 127.0 for t in tops]
    print("input scales", ["%.5g" % s for s in scales])
    for m in (model, ema):
        m.act_scales = scales
        m.quant_weights = True
    if a.qat_steps:
        run(a.qat_steps, a.lr * 0.1, "qat", a.minutes * 60)
        model.load_state_dict(ema.state_dict())
    torch.save({"model": model.state_dict(), "scales": scales, "c0": a.c0, "c1": a.c1, "mids": a.mids,
                "guidance": a.guidance, "minutes": (time.time() - t0) / 60, "steps": total}, os.path.join(a.out, "ckpt.pt"))
    print("finished, %.1f min" % ((time.time() - t0) / 60))


if __name__ == "__main__":
    main()
