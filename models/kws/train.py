"""Trains the keyword spotter on data.py's features.

  train.py --out DIR [--epochs 40] [--qat-epochs 0]

A CNN over the 49 x 40 log-mel map: four convolutions with batch norm and
ReLU, the mean over time and frequency, and a linear classifier. After the
float epochs the batch norms fold into the convolutions, each convolution's
input gets a fixed int8 scale from the training data, and --qat-epochs
train with int8 weights and activations simulated. DIR/ckpt.pt keeps the
folded weights, the scales and the input offset.
"""
import argparse
import json
import os
import time

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

import data

# (cin, cout, kernel, stride, padding)
# 49 x 40 -> 24 x 10 x 24 -> 12 x 5 x 32 -> 12 x 5 x 48 -> 12 x 5 x 64 -> mean
LAYERS = ((1, 24, (10, 4), (2, 4), (4, 0)),
          (24, 32, (3, 3), (2, 2), (1, 1)),
          (32, 48, (3, 3), (1, 1), (1, 1)),
          (48, 64, (1, 1), (1, 1), (0, 0)))


def fq(x, scale):
    q = torch.clamp(torch.sign(x / scale) * torch.floor((x / scale).abs() + 0.5), -127, 127) * scale
    return x + (q - x).detach()


def fq_rows(w):
    s = w.detach().abs().flatten(1).amax(1).clamp_min(1e-12) / 127.0
    shape = (-1,) + (1,) * (w.dim() - 1)
    return w + (torch.clamp(torch.round(w / s.view(shape)), -127, 127) * s.view(shape) - w).detach()


def fq_dyn(x):
    """int8 with one scale per vector, as the reference's quantize()"""
    amax = x.detach().abs().amax(-1, keepdim=True).clamp_min(1e-12)
    s = amax / 127.0
    q = torch.clamp(torch.sign(x / s) * torch.floor((x / s).abs() + 0.5), -127, 127) * s
    return x + (q - x).detach()


class KWS(nn.Module):
    def __init__(self, bn=True):
        super().__init__()
        self.convs = nn.ModuleList(nn.Conv2d(ci, co, k, s, p, bias=True) for ci, co, k, s, p in LAYERS)
        self.bns = nn.ModuleList(nn.BatchNorm2d(co) for _, co, _, _, _ in LAYERS) if bn else None
        self.fc = nn.Linear(LAYERS[-1][1], len(data.LABELS))
        self.drop = nn.Dropout(0.2)
        self.scales = None

    def forward(self, x):
        """x: (B, 49, 40) input already in units of the first scale"""
        h = x[:, None]
        for i, conv in enumerate(self.convs):
            if self.scales is not None:
                h = fq(h, self.scales[i])
                h = F.conv2d(h, fq_rows(conv.weight), conv.bias, conv.stride, conv.padding)
            else:
                h = conv(h)
            if self.bns is not None:
                h = self.bns[i](h)
            h = F.relu(h)
        h = h.mean(dim=(2, 3))
        if self.scales is not None:
            return F.linear(fq_dyn(h), fq_rows(self.fc.weight), self.fc.bias)
        return self.fc(self.drop(h))

    def fold(self):
        """a copy without batch norm"""
        m = KWS(bn=False)
        for conv, bn, dst in zip(self.convs, self.bns, m.convs):
            g = bn.weight / torch.sqrt(bn.running_var + bn.eps)
            dst.weight.data = conv.weight.data * g.view(-1, 1, 1, 1)
            dst.bias.data = (conv.bias.data - bn.running_mean) * g + bn.bias.data
        m.fc.load_state_dict(self.fc.state_dict())
        return m


def accuracy(model, x, y, device, batch=512):
    model.eval()
    correct = 0
    with torch.no_grad():
        for i in range(0, len(y), batch):
            out = model(x[i:i + batch].to(device))
            correct += (out.argmax(1).cpu() == y[i:i + batch]).sum().item()
    model.train()
    return correct / len(y)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", required=True)
    p.add_argument("--epochs", type=int, default=40)
    p.add_argument("--qat-epochs", type=int, default=0)
    p.add_argument("--batch", type=int, default=128)
    p.add_argument("--lr", type=float, default=3e-3)
    a = p.parse_args()
    torch.set_num_threads(4)
    torch.manual_seed(0)
    device = "mps" if torch.backends.mps.is_available() else "cpu"
    os.makedirs(a.out, exist_ok=True)
    d = data.load()
    tx, ty = d["train"]
    _, vx, vy = d["val"]
    offset = int(np.round(np.mean(tx)))
    top = float(np.percentile(np.abs(tx.astype(np.int64) - offset), 99.99))
    in_scale = top / 127.0

    def prep(f):
        """the network's input: int8 at the first scale, as the node quantizes it"""
        v = (f.astype(np.float32) - offset) / in_scale
        return torch.from_numpy(np.clip(np.sign(v) * np.floor(np.abs(v) + 0.5), -127, 127) * in_scale)

    Xt, Yt = prep(tx), torch.from_numpy(ty.astype(np.int64))
    Xv, Yv = prep(vx), torch.from_numpy(vy.astype(np.int64))
    model = KWS().to(device)
    n_w = sum(c.weight.numel() for c in model.convs) + model.fc.weight.numel()
    print("train %d, val %d, weights %d, input offset %d, scale %.3f" % (len(Yt), len(Yv), n_w, offset, in_scale))
    log = open(os.path.join(a.out, "log.jsonl"), "a")
    t0 = time.time()

    def run(epochs, lr, phase):
        opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=1e-3)
        steps = epochs * ((len(Yt) + a.batch - 1) // a.batch)
        sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=lr, total_steps=steps, pct_start=0.1)
        for ep in range(epochs):
            perm = torch.randperm(len(Yt))
            for i in range(0, len(Yt), a.batch):
                idx = perm[i:i + a.batch]
                xb = Xt[idx].to(device)
                # SpecAugment-style masks: up to 8 frames and 5 mel bands
                if phase == "float":
                    B = len(idx)
                    t0_ = torch.randint(0, 49, (B, 1), device=device)
                    tw = torch.randint(0, 9, (B, 1), device=device)
                    f0 = torch.randint(0, 40, (B, 1), device=device)
                    fw = torch.randint(0, 6, (B, 1), device=device)
                    tr = torch.arange(49, device=device)[None]
                    fr = torch.arange(40, device=device)[None]
                    mt = ((tr >= t0_) & (tr < t0_ + tw))[:, :, None]
                    mf = ((fr >= f0) & (fr < f0 + fw))[:, None, :]
                    xb = xb.masked_fill(mt | mf, 0.0)
                loss = F.cross_entropy(model(xb), Yt[idx].to(device), label_smoothing=0.05)
                opt.zero_grad(set_to_none=True)
                loss.backward()
                opt.step()
                sched.step()
            acc = accuracy(model, Xv, Yv, device)
            rec = {"phase": phase, "epoch": ep + 1, "loss": loss.item(), "val_acc": acc, "minutes": (time.time() - t0) / 60}
            log.write(json.dumps(rec) + "\n")
            log.flush()
            print(rec, flush=True)

    run(a.epochs, a.lr, "float")
    model = model.cpu().fold().to(device)
    # the input scale of each convolution: the largest value over the training data
    tops = [in_scale * 127.0] + [0.0] * (len(LAYERS) - 1)
    hooks = [conv.register_forward_pre_hook(lambda m, inp, i=i: tops.__setitem__(i, max(tops[i], inp[0].abs().max().item())))
             for i, conv in enumerate(model.convs) if i > 0]
    accuracy(model, Xt[:20000], Yt[:20000], device)
    for h in hooks:
        h.remove()
    model.scales = [t / 127.0 for t in tops]
    model.scales[0] = in_scale
    print("folded: val %.4f; scales %s" % (accuracy(model, Xv, Yv, device), ["%.4f" % s for s in model.scales]))
    if a.qat_epochs:
        run(a.qat_epochs, a.lr * 0.05, "qat")
    torch.save({"model": model.cpu().state_dict(), "scales": model.scales, "offset": offset,
                "minutes": (time.time() - t0) / 60}, os.path.join(a.out, "ckpt.pt"))
    print("finished, %.1f min" % ((time.time() - t0) / 60))


if __name__ == "__main__":
    main()
