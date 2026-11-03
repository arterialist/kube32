"""Google Speech Commands v2 (download.tensorflow.org/data/speech_commands_v0.02.tar.gz):
streams the archive and keeps the keywords, a share of every other word
(the _unknown_ class), the background noise recordings and the split lists.

  fetch.py --out DIR [--unknown-share 0.25]
"""
import argparse
import os
import tarfile
import urllib.request
import zlib

URL = "http://download.tensorflow.org/data/speech_commands_v0.02.tar.gz"
KEYWORDS = ("cat", "dog", "bird", "house", "tree", "bed")


def keep(name, share):
    parts = name.split("/")
    if len(parts) == 1:
        return name in ("validation_list.txt", "testing_list.txt", "LICENSE", "README.md")
    word = parts[0]
    if word in KEYWORDS or word == "_background_noise_":
        return True
    # a stable share of each other word's files, by the speaker id's hash
    return (zlib.crc32(parts[-1].split("_")[0].encode()) % 1000) < share * 1000


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", default=os.path.expanduser("~/.cache/kube32-models/data/speech_commands"))
    p.add_argument("--unknown-share", type=float, default=0.25)
    a = p.parse_args()
    os.makedirs(a.out, exist_ok=True)
    n = 0
    with urllib.request.urlopen(URL, timeout=60) as r, tarfile.open(fileobj=r, mode="r|gz") as tf:
        for m in tf:
            name = m.name[2:] if m.name.startswith("./") else m.name
            if not m.isfile() or not keep(name, a.unknown_share):
                continue
            dst = os.path.join(a.out, name)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(tf.extractfile(m).read())
            n += 1
            if n % 5000 == 0:
                print(n, name, flush=True)
    open(os.path.join(a.out, ".complete"), "w").write("%d\n" % n)
    print("kept %d files" % n)


if __name__ == "__main__":
    main()
