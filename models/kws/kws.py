#!/usr/bin/env python3
"""The keyword spotter with the node's arithmetic: export, reference
inference, accuracy and confusion matrix, test vectors.

  kws.py export  --ckpt ckpt.pt --out kws.bin
  kws.py test    --model kws.bin [--json FILE]   test-set accuracy, confusion matrix
  kws.py vectors --model kws.bin --out DIR       one clip of each class: audio,
                                                 features, every layer, logits
  kws.py wav     --model kws.bin FILE.wav        classify a recording

Blob (little-endian, sections 4-byte aligned):
  header, 16 uint32: magic "K32K", version 1, n_conv, n_classes, frames
      (49), mels (40), offset (int32), bytes (the whole blob), 8 reserved
  front end tables (frontend.py): int16 hann[480], tw_re[256], tw_im[256];
      uint16 mel_start[40], mel_len[40]; int16 mel_w[sum of mel_len]
  each convolution: uint32 cin, cout, kh, kw, sh, sw, ph, pw;
      float in_scale, in_inv; float w_scale[cout], bias[cout];
      int8 w[cout][kh][kw][cin], zero bytes to a multiple of 4
  classifier: uint32 cin, cout; float w_scale[cout], bias[cout];
      int8 w[cout][cin], zero bytes to a multiple of 4

Inference on the features L (49 x 40 int32, frontend.py):
  the input map (49 rows of time, 40 columns of mel, 1 channel):
      q = round((float)(L - offset) * in_inv) half away from 0, clamped to
      -127..127
  each convolution, each output pixel in row-major order, its patch
  (ky, kx, ci; zero outside the map; input row = sh y + ky - ph):
      y[o] = k32_matvec_i8(w, w_scale, patch, in_scale)[o] + bias[o]
      y = max(y, 0.0f), then quantized with the next convolution's in_inv
  the last convolution's output (12 x 5 x 64, float, after ReLU): the mean
  of each channel, the sum over pixels in row-major order from 0.0f, then
  divided by (float)(pixels); that vector quantized with one scale
  (k32 quantize: round(v * (127 / max|v|)), scale max|v| / 127), then
      logits[c] = k32_matvec_i8(w, w_scale, vq, scale)[c] + bias[c]
  the class is the first largest logit.
"""
import argparse
import hashlib
import json
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "common"))
import frontend  # noqa: E402
from k32ref import F32, matvec_i8, quantize, quantize_static, seqsum  # noqa: E402

MAGIC = 0x4B32334B   # "K32K"
LABELS = ("cat", "dog", "bird", "house", "tree", "bed", "_unknown_", "_silence_")


def q_rows(w):
    w = np.asarray(w, F32).reshape(len(w), -1)
    s = (np.abs(w).max(1) / F32(127.0)).astype(F32)
    safe = np.where(s == 0, F32(1), s)
    return np.clip(np.rint(w / safe[:, None]), -127, 127).astype(np.int8), s


def pad4(b):
    return b + b"\0" * (-len(b) % 4)


def export(ckpt, out):
    import torch
    s = torch.load(ckpt, map_location="cpu")
    sd = s["model"]
    scales = s["scales"]
    n_conv = len(scales)
    from train import LAYERS
    body = frontend.tables_bytes()
    for i, (ci, co, k, st, pd) in enumerate(LAYERS):
        w = sd["convs.%d.weight" % i].numpy().transpose(0, 2, 3, 1)   # cout, kh, kw, cin
        q, ws = q_rows(w)
        sc = F32(scales[i])
        body += struct.pack("<8I", ci, co, k[0], k[1], st[0], st[1], pd[0], pd[1])
        body += np.array([sc, F32(1.0) / sc], "<f4").tobytes() + ws.astype("<f4").tobytes()
        body += sd["convs.%d.bias" % i].numpy().astype("<f4").tobytes() + pad4(q.tobytes())
    fw = sd["fc.weight"].numpy()
    q, ws = q_rows(fw)
    body += struct.pack("<2I", fw.shape[1], fw.shape[0]) + ws.astype("<f4").tobytes()
    body += sd["fc.bias"].numpy().astype("<f4").tobytes() + pad4(q.tobytes())
    header = struct.pack("<8I", MAGIC, 1, n_conv, len(LABELS), frontend.FRAMES, frontend.MELS,
                         s["offset"] & 0xFFFFFFFF, 64 + len(body)) + b"\0" * 32
    data = header + body
    with open(out, "wb") as f:
        f.write(data)
    return len(data), sum(sd["convs.%d.weight" % i].numel() for i in range(n_conv)) + fw.size


class Model:
    def __init__(self, data):
        h = struct.unpack("<8I", data[:32])
        if h[0] != MAGIC or h[1] != 1 or h[7] != len(data):
            raise ValueError("not a keyword spotter blob")
        n_conv, self.n_classes = h[2], h[3]
        self.offset = struct.unpack("<i", data[24:28])[0]
        self.bytes = len(data)
        o = 64 + len(frontend.tables_bytes())
        if data[64:64 + len(frontend.tables_bytes())] != frontend.tables_bytes():
            raise ValueError("front end tables differ from frontend.py's")

        def take(dtype, n):
            nonlocal o
            a = np.frombuffer(data, dtype=dtype, count=n, offset=o)
            o += a.nbytes
            o += -o % 4
            return a

        self.convs = []
        for _ in range(n_conv):
            ci, co, kh, kw, sh, sw, ph, pw = (int(v) for v in take("<u4", 8))
            sc, inv = take("<f4", 2)
            ws, b = take("<f4", co), take("<f4", co)
            w = take("i1", co * kh * kw * ci).reshape(co, kh * kw * ci)
            self.convs.append(dict(ci=ci, co=co, k=(kh, kw), s=(sh, sw), p=(ph, pw), scale=sc, inv=inv, ws=ws, b=b, w=w))
        ci, co = (int(v) for v in take("<u4", 2))
        self.fc = dict(ws=take("<f4", co), b=take("<f4", co), w=take("i1", co * ci).reshape(co, ci))
        if o != len(data):
            raise ValueError("keyword spotter blob: %d bytes left over" % (len(data) - o))
        self.weights = sum(c["w"].size for c in self.convs) + self.fc["w"].size

    @staticmethod
    def patches(q, k, s, p):
        H, W, C = q.shape
        pad = np.zeros((H + 2 * p[0], W + 2 * p[1], C), np.int8)
        pad[p[0]:p[0] + H, p[1]:p[1] + W] = q
        Ho = (H + 2 * p[0] - k[0]) // s[0] + 1
        Wo = (W + 2 * p[1] - k[1]) // s[1] + 1
        cols = [pad[ky:ky + s[0] * Ho:s[0], kx:kx + s[1] * Wo:s[1]] for ky in range(k[0]) for kx in range(k[1])]
        return np.concatenate(cols, axis=2).reshape(Ho * Wo, -1), Ho, Wo

    def forward(self, L, trace=None):
        """L: (49, 40) int32 features -> logits (n_classes,) float32"""
        x = (np.asarray(L, np.int64) - self.offset).astype(F32)
        q = quantize_static(x, self.convs[0]["inv"])[:, :, None]
        for i, c in enumerate(self.convs):
            pt, Ho, Wo = self.patches(q, c["k"], c["s"], c["p"])
            y = (matvec_i8(c["w"], c["ws"], pt, c["scale"]) + c["b"]).astype(F32)
            y = np.maximum(y, F32(0.0))
            if trace is not None:
                trace.append(y)
            if i + 1 < len(self.convs):
                q = quantize_static(y.reshape(Ho, Wo, c["co"]), self.convs[i + 1]["inv"])
        mean = (seqsum(y.T) / F32(len(y))).astype(F32)
        vq, vs = quantize(mean)
        return (matvec_i8(self.fc["w"], self.fc["ws"], vq, vs) + self.fc["b"]).astype(F32)


def cmd_test(a):
    sys.path.insert(0, HERE)
    import data
    m = Model(open(a.model, "rb").read())
    d = data.load()
    _, tx, ty = d["test"]
    pred = np.array([int(np.argmax(m.forward(f))) for f in tx])
    cm = np.zeros((len(LABELS), len(LABELS)), np.int64)
    for t, p in zip(ty, pred):
        cm[t, p] += 1
    acc = float(np.mean(pred == ty))
    print("test accuracy %.4f over %d clips (int8 reference)" % (acc, len(ty)))
    print("%-10s" % "true\\pred" + "".join("%9s" % l[:8] for l in LABELS))
    for i, l in enumerate(LABELS):
        print("%-10s" % l + "".join("%9d" % v for v in cm[i]))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"accuracy": acc, "clips": int(len(ty)), "labels": LABELS, "confusion": cm.tolist()}, f, indent=1)


def cmd_vectors(a):
    import data
    m = Model(open(a.model, "rb").read())
    d = data.load()
    audio, feats, y = d["test"]
    os.makedirs(a.out, exist_ok=True)
    picks = [int(np.flatnonzero(y == c)[0]) for c in range(len(LABELS))]
    clips = []
    for c, i in zip(range(len(LABELS)), picks):
        trace = []
        L = frontend.features(audio[i])
        assert np.array_equal(L, feats[i])
        logits = m.forward(L, trace)
        clips.append({"label": LABELS[c], "features_sha256": hashlib.sha256(L.astype("<i4").tobytes()).hexdigest(),
                      "features": L.flatten().tolist(),
                      "layers_sha256": [hashlib.sha256(t.astype("<f4").tobytes()).hexdigest() for t in trace],
                      "logits": logits.tolist(), "logits_hex": [struct.pack("<f", v).hex() for v in logits],
                      "class": LABELS[int(np.argmax(logits))]})
    audio[picks].astype("<i2").tofile(os.path.join(a.out, "kws-audio.bin"))
    feats[picks].astype("<i4").tofile(os.path.join(a.out, "kws-features.bin"))
    np.array([c["logits"] for c in clips], "<f4").tofile(os.path.join(a.out, "kws-logits.bin"))
    with open(os.path.join(a.out, "kws.json"), "w") as f:
        json.dump({"model": os.path.basename(a.model), "blob_sha256": hashlib.sha256(open(a.model, "rb").read()).hexdigest(),
                   "audio": "kws-audio.bin: %d clips of 16000 int16, in this order" % len(picks), "clips": clips}, f)
        f.write("\n")
    print("wrote %s" % a.out)


def cmd_wav(a):
    import data
    m = Model(open(a.model, "rb").read())
    logits = m.forward(frontend.features(data.read_wav(a.wav)))
    print(LABELS[int(np.argmax(logits))], " ".join("%s=%.2f" % (l, v) for l, v in zip(LABELS, logits)))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export")
    e.add_argument("--ckpt", required=True)
    e.add_argument("--out", required=True)
    ev = sub.add_parser("test")
    ev.add_argument("--model", required=True)
    ev.add_argument("--json")
    v = sub.add_parser("vectors")
    v.add_argument("--model", required=True)
    v.add_argument("--out", required=True)
    w = sub.add_parser("wav")
    w.add_argument("--model", required=True)
    w.add_argument("wav")
    a = p.parse_args()
    if a.cmd == "export":
        n, nw = export(a.ckpt, a.out)
        print("wrote %s: %d bytes, %d weights" % (a.out, n, nw))
    else:
        {"test": cmd_test, "vectors": cmd_vectors, "wav": cmd_wav}[a.cmd](a)


if __name__ == "__main__":
    main()
