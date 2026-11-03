#!/usr/bin/env python3
"""The doodle denoiser with the node's arithmetic: export, reference sampler,
test vectors and sample grids.

  doodle.py export  --ckpt ckpt.pt --out doodle.bin
  doodle.py sample  --model doodle.bin --steps 16 --class cat --seed 1 [--guidance 2]
  doodle.py grid    --model doodle.bin --out grid.png [--per-class 8]
  doodle.py vectors --model doodle.bin --out FILE

Blob (little-endian, every section 4-byte aligned):
  header, 16 uint32: magic "K32U", version 1, c0, c1, mids, n_conv,
      n_cond (7: the classes, then "no class"), n_sched, cond_width (the sum
      of every convolution's output channels but the last's), bytes (the
      whole blob), 6 reserved
  each convolution, in order (in, e1, down, mid..., up, d1, out):
      uint32 cin, cout, stride (1 or 2), flags
          flags: 1 the input is the 7x7 map repeated 2x2 (to 14x14)
                 2 adds the e1 output, 4 adds the in output (both after
                 the bias, before SiLU), 8 the last convolution (no step and
                 class bias, no SiLU)
      float in_scale, in_inv (the input's int8 scale and 1/scale)
      float w_scale[cout], bias[cout]
      int8 w[cout][3][3][cin] (row = ky, kx, ci), zero bytes to a multiple of 4
  int8 class_q[n_cond][cond_width], float class_s[n_cond]
  each schedule: uint32 steps; float alpha[steps + 1], sigma[steps + 1];
      int8 time_q[steps][cond_width], zero bytes to a multiple of 4;
      float time_s[steps]

A denoising step, at step i of a schedule with the class c:
  cond[k] = (float)time_q[i][k] * time_s[i] + (float)class_q[c][k] * class_s[c]
  the 28x28 image x goes in as h[y][x][2 dy + dx] = x[2y + dy][2x + dx] (14x14x4)
  each convolution: its input map quantized, q = round(h * in_inv) half away
  from 0, clamped to -127..127 (the previous convolution's output is
  quantized once and kept as int8); for each output pixel (row-major) the
  3x3 patch (zero outside the map; stride 2 reads rows 2y-1..2y+1) gives
      out[o] = k32_matvec_i8(w, w_scale, patch, in_scale)[o] + bias[o]
      then + cond[off + o] (off: the convolution's first channel in cond)
      then + (float)skip_q * skip_scale (flag 2: e1's input map and
      in_scale of `down`; flag 4: e1's own input map and in_scale)
      then SiLU: y * (1.0f / (1.0f + k32_expf(-y)))
  the last convolution's 14x14x4 output v maps back to 28x28 as the input did
  with guidance w != 1: v = v_none + w * (v_class - v_none)
  DDIM: x0 = a x - s v, clamped to [-1, 1]; eps = s x + a v;
        x = a_next x0 + s_next eps, with a = alpha[i], s = sigma[i]
The image starts as 784 values of xorshift32's gauss() in raster order
(models/common/k32ref.py: Rng); after the last step x is the image in
[-1, 1] (pixel = (x + 1) * 127.5).
"""
import argparse
import hashlib
import json
import math
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "common"))
from k32ref import F32, Rng, k_exp, matvec_i8, quantize_static  # noqa: E402

MAGIC = 0x5532334B   # "K32U"
CLASSES = ("cat", "dog", "bird", "house", "tree", "bed")
NONE = len(CLASSES)
SCHEDULES = (8, 16)
F_UP, F_SKIP_E1, F_SKIP_IN, F_LAST = 1, 2, 4, 8


def q_rows(w):
    w = np.asarray(w, F32)
    amax = np.abs(w.reshape(len(w), -1)).max(1)
    s = (amax / F32(127.0)).astype(F32)
    safe = np.where(s == 0, F32(1), s)
    q = np.clip(np.rint(w.reshape(len(w), -1) / safe[:, None]), -127, 127).astype(np.int8)
    return q, s


def pad4(b):
    return b + b"\0" * (-len(b) % 4)


def export(ckpt, out):
    import torch
    sys.path.insert(0, HERE)
    from net import Denoiser, alpha_sigma
    s = torch.load(ckpt, map_location="cpu")
    m = Denoiser(s["c0"], s["c1"], s["mids"])
    m.load_state_dict(s["model"])
    m.eval()
    scales = s["scales"]
    n_conv = len(m.convs)
    up = 3 + m.mids
    width = sum(m.widths)
    body = b""
    for i, conv in enumerate(m.convs):
        w = conv.weight.detach().numpy().transpose(0, 2, 3, 1)   # cout, ky, kx, cin
        q, ws = q_rows(w)
        flags = (F_UP if i == up else 0) | (F_SKIP_E1 if i == up else 0) | (F_SKIP_IN if i == up + 1 else 0) | \
                (F_LAST if i == n_conv - 1 else 0)
        sc = F32(scales[i])
        body += struct.pack("<4I", conv.in_channels, conv.out_channels, conv.stride[0], flags)
        body += np.array([sc, F32(1.0) / sc], "<f4").tobytes()
        body += ws.astype("<f4").tobytes() + conv.bias.detach().numpy().astype("<f4").tobytes()
        body += pad4(q.tobytes())
    with torch.no_grad():
        cls = m.cls.weight.numpy()
        cq, cs = q_rows(cls)
        body += pad4(cq.tobytes()) + cs.astype("<f4").tobytes()
        for S in SCHEDULES:
            t = torch.tensor([1.0 - i / S for i in range(S + 1)], dtype=torch.float64)
            al = np.cos(0.5 * math.pi * t.numpy()).astype(F32)
            si = np.sin(0.5 * math.pi * t.numpy()).astype(F32)
            tv = m.time(__import__("net").time_features(t[:S].float())).numpy()
            tq, ts = q_rows(tv)
            body += struct.pack("<I", S) + al.astype("<f4").tobytes() + si.astype("<f4").tobytes()
            body += pad4(tq.tobytes()) + ts.astype("<f4").tobytes()
    header = struct.pack("<16I", MAGIC, 1, m.c0, m.c1, m.mids, n_conv, len(cls), len(SCHEDULES), width,
                         64 + len(body), 0, 0, 0, 0, 0, 0)
    data = header + body
    with open(out, "wb") as f:
        f.write(data)
    return len(data), m.n_weights()


class Model:
    def __init__(self, data):
        h = struct.unpack("<16I", data[:64])
        if h[0] != MAGIC or h[1] != 1 or h[9] != len(data):
            raise ValueError("not a doodle blob")
        self.c0, self.c1, self.mids, n_conv, n_cond, n_sched, self.width = h[2:9]
        self.bytes = len(data)
        o = 64

        def take(dtype, n):
            nonlocal o
            a = np.frombuffer(data, dtype=dtype, count=n, offset=o)
            o += a.nbytes
            o += -o % 4
            return a

        self.convs = []
        for _ in range(n_conv):
            cin, cout, stride, flags = take("<u4", 4)
            sc, inv = take("<f4", 2)
            ws = take("<f4", cout)
            b = take("<f4", cout)
            w = take("i1", cout * 9 * cin).reshape(cout, 9 * cin)
            self.convs.append(dict(cin=int(cin), cout=int(cout), stride=int(stride), flags=int(flags), scale=sc, inv=inv,
                                   ws=ws, b=b, w=w))
        self.class_q = take("i1", n_cond * self.width).reshape(n_cond, self.width)
        self.class_s = take("<f4", n_cond)
        self.sched = {}
        for _ in range(n_sched):
            S = int(take("<u4", 1)[0])
            al = take("<f4", S + 1)
            si = take("<f4", S + 1)
            tq = take("i1", S * self.width).reshape(S, self.width)
            ts = take("<f4", S)
            self.sched[S] = dict(alpha=al, sigma=si, tq=tq, ts=ts)
        if o != len(data):
            raise ValueError("doodle blob: %d bytes left over" % (len(data) - o))
        self.weights = sum(c["w"].size for c in self.convs)

    def cond(self, S, i, c):
        t = self.sched[S]
        return ((t["tq"][i].astype(F32) * t["ts"][i]).astype(F32) +
                (self.class_q[c].astype(F32) * self.class_s[c]).astype(F32)).astype(F32)

    @staticmethod
    def patches(q, stride, up):
        """(Hout*Wout, 9*C) int8 3x3 patches, zero outside the map"""
        if up:
            q = q.repeat(2, axis=0).repeat(2, axis=1)
        H, W, C = q.shape
        p = np.zeros((H + 2, W + 2, C), np.int8)
        p[1:-1, 1:-1] = q
        Ho, Wo = (H + stride - 1) // stride, (W + stride - 1) // stride
        cols = [p[ky:ky + stride * Ho:stride, kx:kx + stride * Wo:stride] for ky in range(3) for kx in range(3)]
        return np.concatenate(cols, axis=2).reshape(Ho * Wo, 9 * C), Ho, Wo

    def forward(self, x, cond):
        """x: (28, 28) float32 image; returns v, (28, 28)"""
        h = x.reshape(14, 2, 14, 2).transpose(0, 2, 1, 3).reshape(14, 14, 4)
        q = quantize_static(h, self.convs[0]["inv"])
        skips = []
        off = 0
        for i, c in enumerate(self.convs):
            if i in (1, 2):
                skips.append((q, self.convs[i]["scale"]))
            pt, Ho, Wo = self.patches(q, c["stride"], c["flags"] & F_UP)
            y = matvec_i8(c["w"], c["ws"], pt, c["scale"])
            y = (y + c["b"]).astype(F32)
            if c["flags"] & F_LAST:
                return y.reshape(14, 14, 2, 2).transpose(0, 2, 1, 3).reshape(28, 28)
            y = (y + cond[off:off + c["cout"]]).astype(F32)
            off += c["cout"]
            if c["flags"] & (F_SKIP_E1 | F_SKIP_IN):
                sq, ss = skips[1] if c["flags"] & F_SKIP_E1 else skips[0]
                y = (y + (sq.reshape(-1, c["cout"]).astype(F32) * ss).astype(F32)).astype(F32)
            y = (y * (F32(1.0) / (F32(1.0) + k_exp(-y))).astype(F32)).astype(F32)
            q = quantize_static(y.reshape(Ho, Wo, c["cout"]), self.convs[i + 1]["inv"])

    def sample(self, cls, seed, steps=16, guidance=2.0, trace=None):
        rng = Rng(seed)
        x = np.array([rng.gauss() for _ in range(784)], F32).reshape(28, 28)
        t = self.sched[steps]
        g = F32(guidance)
        for i in range(steps):
            v = self.forward(x, self.cond(steps, i, cls))
            if guidance != 1.0:
                vu = self.forward(x, self.cond(steps, i, NONE))
                v = (vu + (g * (v - vu)).astype(F32)).astype(F32)
            a, s, an, sn = t["alpha"][i], t["sigma"][i], t["alpha"][i + 1], t["sigma"][i + 1]
            x0 = np.clip((a * x - s * v).astype(F32), F32(-1.0), F32(1.0))
            eps = (s * x + a * v).astype(F32)
            x = (an * x0 + sn * eps).astype(F32)
            if trace is not None:
                trace.append(hashlib.sha256(x.astype("<f4").tobytes()).hexdigest()[:16])
        return x


def to_u8(x):
    return np.clip(np.rint((np.asarray(x, np.float64) + 1.0) * 127.5), 0, 255).astype(np.uint8)


def grid(images, rows, cols, scale=3, labels=None):
    from PIL import Image, ImageDraw
    pad = 2
    lw = 40 if labels else 0
    W = lw + cols * (28 * scale + pad) + pad
    H = rows * (28 * scale + pad) + pad
    canvas = Image.new("L", (W, H), 40)
    for k, im in enumerate(images):
        r, c = divmod(k, cols)
        tile = Image.fromarray(255 - to_u8(im)).resize((28 * scale, 28 * scale), Image.NEAREST)
        canvas.paste(tile, (lw + pad + c * (28 * scale + pad), pad + r * (28 * scale + pad)))
    if labels:
        d = ImageDraw.Draw(canvas)
        for r, name in enumerate(labels):
            d.text((3, pad + r * (28 * scale + pad) + 28 * scale // 2 - 5), name, fill=230)
    return canvas


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export")
    e.add_argument("--ckpt", required=True)
    e.add_argument("--out", required=True)
    for name in ("sample", "grid", "vectors"):
        s = sub.add_parser(name)
        s.add_argument("--model", required=True)
        s.add_argument("--steps", type=int, default=16)
        s.add_argument("--guidance", type=float, default=2.0)
        s.add_argument("--seed", type=int, default=1)
        if name == "sample":
            s.add_argument("--class", dest="cls", default="cat")
        if name == "grid":
            s.add_argument("--per-class", type=int, default=8)
        if name in ("grid", "vectors"):
            s.add_argument("--out", required=True)
    a = p.parse_args()
    if a.cmd == "export":
        n, w = export(a.ckpt, a.out)
        print("wrote %s: %d bytes, %d weights" % (a.out, n, w))
        return
    m = Model(open(a.model, "rb").read())
    if a.cmd == "sample":
        x = m.sample(CLASSES.index(a.cls), a.seed, a.steps, a.guidance)
        for row in to_u8(x)[::2]:
            print("".join(" .:-=+*#%@"[v * 10 // 256] for v in row[::1]))
        return
    if a.cmd == "grid":
        imgs = [m.sample(c, a.seed + k, a.steps, a.guidance) for c in range(len(CLASSES)) for k in range(a.per_class)]
        grid(imgs, len(CLASSES), a.per_class, labels=list(CLASSES)).save(a.out)
        print("wrote %s" % a.out)
        return
    vec = []
    for c in range(len(CLASSES)):
        trace = []
        x = m.sample(c, a.seed + c, a.steps, a.guidance, trace)
        vec.append({"class": CLASSES[c], "seed": a.seed + c, "steps": a.steps, "guidance": a.guidance,
                    "image_f32_sha256": hashlib.sha256(x.astype("<f4").tobytes()).hexdigest(),
                    "image_f32_hex": x.astype("<f4").tobytes().hex(),
                    "step_sha256_16": trace, "image_u8": to_u8(x).flatten().tolist()})
    x = np.array([Rng(a.seed).gauss() for _ in range(784)], F32).reshape(28, 28)
    v = m.forward(x, m.cond(a.steps, 0, 0))
    out = {"model": os.path.basename(a.model), "blob_sha256": hashlib.sha256(open(a.model, "rb").read()).hexdigest(),
           "first_forward": {"seed": a.seed, "class": CLASSES[0], "step": 0, "noise_f32": x.flatten().tolist(),
                             "v_f32": v.flatten().tolist()},
           "samples": vec}
    with open(a.out, "w") as f:
        json.dump(out, f)
        f.write("\n")
    # the same samples for doodle_test.c: uint32 class, seed, steps, float guidance, float image[784]
    with open(a.out.replace(".json", "") + "-samples.bin", "wb") as f:
        for c, s in enumerate(vec):
            x = np.frombuffer(bytes.fromhex(s["image_f32_hex"]), "<f4")
            f.write(struct.pack("<3If", c, s["seed"], s["steps"], s["guidance"]) + x.tobytes())
    print("wrote %s" % a.out)


if __name__ == "__main__":
    main()
