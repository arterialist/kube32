#!/usr/bin/env python3
"""Build the Tessera web client bundle for the tessera-web pod.

Writes index.html.gz, app.js.gz and styles.css.gz (gzip -9, mtime zeroed so
the bytes are reproducible) into the bundle directory, default
../../../images/web/tessera-web/ (the bundle of the embedded image
tessera-web). index.html links app.js and styles.css with ?v=<content hash>,
so the pod can let browsers cache them for a year. Prints raw and compressed
sizes and exits 1 if the size budget is exceeded:

  raw  : index.html + app.js + styles.css < 150 KB
  gzip : sum of the three .gz files       < 42 KB
"""
import argparse
import gzip
import hashlib
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
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(HERE, "..", "..", "..", "images", "web", "tessera-web"))
    out = os.path.normpath(ap.parse_args().out)
    os.makedirs(out, exist_ok=True)
    src = {}
    for name in FILES:
        with open(os.path.join(HERE, name), "rb") as f:
            src[name] = f.read()
    html = src["index.html"]
    for name in ("app.js", "styles.css"):
        ref = ("/" + name).encode()
        if html.count(b'"' + ref + b'"') != 1:
            print(f"index.html must link {ref.decode()} exactly once")
            return 1
        version = hashlib.sha256(src[name]).hexdigest()[:10]
        html = html.replace(b'"' + ref + b'"', b'"' + ref + b"?v=" + version.encode() + b'"')
    src["index.html"] = html
    raw_total = gz_total = 0
    print(f"{'file':<14}{'raw':>10}{'gzip':>10}")
    for name in FILES:
        data = src[name]
        packed = gz(data)
        with open(os.path.join(out, name + ".gz"), "wb") as f:
            f.write(packed)
        raw_total += len(data)
        gz_total += len(packed)
        print(f"{name:<14}{len(data):>10}{len(packed):>10}")
    print(f"{'total':<14}{raw_total:>10}{gz_total:>10}")
    print(f"{'budget':<14}{RAW_BUDGET:>10}{GZ_BUDGET:>10}")
    ok = raw_total < RAW_BUDGET and gz_total < GZ_BUDGET
    print(("OK -> " + out) if ok else "OVER BUDGET")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
