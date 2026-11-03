"""TinyStories (huggingface.co/datasets/roneneldan/TinyStories) as tok512
tokens: one uint16 file per shard of the archive, each story BOS first, as
llama2.c pretokenizes it. data00 is the validation shard (as in llama2.c,
whose stories260K was trained on the other shards).

  tinystories.py --out DIR --shards 20     download (once) and tokenize
"""
import argparse
import json
import os
import sys
import tarfile
import time
import urllib.request

import numpy as np

URL = "https://huggingface.co/datasets/roneneldan/TinyStories/resolve/main/TinyStories_all_data.tar.gz"
TOK_URL = "https://huggingface.co/karpathy/tinyllamas/resolve/main/stories260K/"


def download(url, path):
    if os.path.exists(path):
        return path
    os.makedirs(os.path.dirname(path), exist_ok=True)
    print("downloading", url)
    with urllib.request.urlopen(url, timeout=60) as r, open(path + ".part", "wb") as f:
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)
    os.replace(path + ".part", path)
    return path


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", default=os.path.expanduser("~/.cache/kube32-models/data/tinystories"))
    p.add_argument("--shards", type=int, default=20, help="train shards to tokenize after data00")
    p.add_argument("--threads", type=int, default=4)
    a = p.parse_args()
    import sentencepiece as spm
    tok = download(TOK_URL + "tok512.model", os.path.join(a.out, "..", "tinyllamas", "tok512.model"))
    sp = spm.SentencePieceProcessor(model_file=tok)
    archive = download(URL, os.path.join(a.out, "TinyStories_all_data.tar.gz"))
    dst = os.path.join(a.out, "tok512")
    os.makedirs(dst, exist_ok=True)
    want = {"data%02d.json" % i for i in range(a.shards + 1)}
    with tarfile.open(archive, "r|gz") as tf:
        for m in tf:
            name = os.path.basename(m.name)
            if name not in want:
                continue
            out = os.path.join(dst, name.replace(".json", ".bin"))
            want.discard(name)
            if os.path.exists(out):
                continue
            t0 = time.time()
            stories = [s["story"].strip() for s in json.load(tf.extractfile(m))]
            ids = sp.encode(stories, add_bos=True, num_threads=a.threads)
            arr = np.fromiter((t for s in ids for t in s), dtype=np.uint16)
            arr.tofile(out + ".part")
            os.replace(out + ".part", out)
            print("%s: %d stories, %d tokens, %.1f s" % (name, len(stories), len(arr), time.time() - t0), flush=True)
            if not want:
                break
    print("done", file=sys.stderr)


if __name__ == "__main__":
    main()
