"""How recognizable generated doodles are: a classifier trained on the real
drawings labels samples of each class; the share it labels as the class
they were drawn for is the class accuracy.

  judge.py --model doodle.bin [--model ...] --n 48 --steps 16 --guidance 2
"""
import argparse
import json
import os
import sys

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from doodle import CLASSES, Model  # noqa: E402
from train import load, to_float  # noqa: E402

CACHE = os.path.expanduser("~/.cache/kube32-models/ckpt/doodle-judge.pt")


def classifier():
    return nn.Sequential(nn.Conv2d(1, 32, 3, padding=1), nn.ReLU(), nn.MaxPool2d(2),
                         nn.Conv2d(32, 64, 3, padding=1), nn.ReLU(), nn.MaxPool2d(2),
                         nn.Conv2d(64, 128, 3, padding=1), nn.ReLU(), nn.AdaptiveAvgPool2d(1), nn.Flatten(),
                         nn.Linear(128, len(CLASSES)))


def trained(device):
    net = classifier().to(device)
    if os.path.exists(CACHE):
        net.load_state_dict(torch.load(CACHE, map_location=device))
        return net.eval()
    x8, y = load("train", device)
    opt = torch.optim.AdamW(net.parameters(), lr=2e-3)
    for step in range(6000):
        idx = torch.randint(0, len(y), (256,), device=device)
        loss = F.cross_entropy(net(to_float(x8[idx])), y[idx])
        opt.zero_grad()
        loss.backward()
        opt.step()
    torch.save(net.state_dict(), CACHE)
    return net.eval()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--model", action="append", required=True)
    p.add_argument("--n", type=int, default=48, help="samples per class")
    p.add_argument("--steps", type=int, default=16)
    p.add_argument("--guidance", type=float, default=2.0)
    p.add_argument("--json")
    a = p.parse_args()
    torch.set_num_threads(4)
    device = "mps" if torch.backends.mps.is_available() else "cpu"
    net = trained(device)
    hx, hy = load("holdout", device)
    with torch.no_grad():
        real = (net(to_float(hx)).argmax(1) == hy).float().mean().item()
    print("classifier on held-out real drawings: %.3f" % real)
    res = {"real_holdout": real}
    for path in a.model:
        m = Model(open(path, "rb").read())
        per = []
        for c in range(len(CLASSES)):
            imgs = np.stack([m.sample(c, 1000 * c + k, a.steps, a.guidance) for k in range(a.n)])
            with torch.no_grad():
                pred = net(torch.from_numpy(imgs).float().view(-1, 1, 28, 28).to(device)).argmax(1).cpu().numpy()
            per.append(float(np.mean(pred == c)))
        key = "%s steps %d guidance %g" % (os.path.basename(path), a.steps, a.guidance)
        res[key] = {"mean": float(np.mean(per)), **dict(zip(CLASSES, per))}
        print("%s: class accuracy %.3f  (%s)" % (key, np.mean(per), ", ".join("%s %.2f" % (c, v) for c, v in zip(CLASSES, per))),
              flush=True)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=1)


if __name__ == "__main__":
    main()
