#!/usr/bin/env python3
"""Build the Tessera frontend for flash.

Writes dist/index.html.gz, dist/app.js.gz and dist/styles.css.gz (gzip -9,
mtime zeroed so the bytes are reproducible) and prints raw and compressed
sizes. Exits 1 if the size budget is exceeded:

  raw  : index.html + app.js + styles.css < 150 KB
  gzip : sum of the three .gz files       < 42 KB
"""
import gzip
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FILES = ["index.html", "app.js", "styles.css"]
RAW_BUDGET = 150 * 1024
GZ_BUDGET = 42 * 1024


def gz(data: bytes) -> bytes:
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as f:
        f.write(data)
    return buf.getvalue()


def main() -> int:
    dist = os.path.join(HERE, "dist")
    os.makedirs(dist, exist_ok=True)
    raw_total = gz_total = 0
    print(f"{'file':<14}{'raw':>10}{'gzip':>10}")
    for name in FILES:
        with open(os.path.join(HERE, name), "rb") as f:
            data = f.read()
        packed = gz(data)
        with open(os.path.join(dist, name + ".gz"), "wb") as f:
            f.write(packed)
        raw_total += len(data)
        gz_total += len(packed)
        print(f"{name:<14}{len(data):>10}{len(packed):>10}")
    print(f"{'total':<14}{raw_total:>10}{gz_total:>10}")
    print(f"{'budget':<14}{RAW_BUDGET:>10}{GZ_BUDGET:>10}")
    ok = raw_total < RAW_BUDGET and gz_total < GZ_BUDGET
    print("OK" if ok else "OVER BUDGET")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
