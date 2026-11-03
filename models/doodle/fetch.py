"""Quick, Draw! 28x28 bitmaps (storage.googleapis.com/quickdraw_dataset/full/numpy_bitmap)
for the classes, the first N drawings of each (an HTTP range of the .npy).

  fetch.py --out DIR [--per-class 60000]
"""
import argparse
import ast
import os
import struct
import urllib.request

import numpy as np

URL = "https://storage.googleapis.com/quickdraw_dataset/full/numpy_bitmap/%s.npy"
CLASSES = ("cat", "dog", "bird", "house", "tree", "bed")


def get(url, first, last):
    req = urllib.request.Request(url, headers={"Range": "bytes=%d-%d" % (first, last)})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def fetch(name, n, out):
    path = os.path.join(out, "%s.npy" % name)
    if os.path.exists(path):
        return np.load(path)
    url = URL % name
    head = get(url, 0, 127)
    if head[:6] != b"\x93NUMPY":
        raise ValueError("%s: not an .npy file" % url)
    hlen = struct.unpack("<H", head[8:10])[0]
    meta = ast.literal_eval(head[10:10 + hlen].decode("latin1"))
    rows, cols = meta["shape"]
    n = min(n, rows)
    body = get(url, 10 + hlen, 10 + hlen + n * cols - 1)
    a = np.frombuffer(body, dtype=np.uint8).reshape(n, cols)
    np.save(path, a)
    print("%s: %d of %d drawings" % (name, n, rows), flush=True)
    return a


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", default=os.path.expanduser("~/.cache/kube32-models/data/quickdraw"))
    p.add_argument("--per-class", type=int, default=60000)
    a = p.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for c in CLASSES:
        fetch(c, a.per_class, a.out)


if __name__ == "__main__":
    main()
