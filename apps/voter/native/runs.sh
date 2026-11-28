#!/bin/sh
# Builds chaos.c natively with four toolchains, runs each and packs the
# samples into ../native_runs.h:
#
#   clang -O2 on macOS (arm64)
#   gcc -O2 with musl in alpine:3.22 for linux/arm64 and linux/amd64
#   gcc -O2 with glibc in ubuntu:24.04 for linux/arm64
#
# Needs clang on an arm64 Mac and Docker with arm64 and amd64 emulation.
# Writes the runs' output to $OUT (default build/native).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/../build/native}
PYTHON=${PYTHON:-python3}
mkdir -p "$OUT"
clang -O2 "$HERE/chaos.c" -o "$OUT/chaos-mac" -lm
"$OUT/chaos-mac" 40 > "$OUT/mac.txt"
for arch in arm64 amd64; do
  docker run --rm --platform linux/$arch -v "$HERE":/src:ro -v "$OUT":/out alpine:3.22 sh -c \
    "apk add --no-cache gcc musl-dev >/dev/null && gcc -O2 /src/chaos.c -o /out/chaos-$arch-musl -lm && /out/chaos-$arch-musl 40 > /out/$arch-musl.txt"
done
docker run --rm --platform linux/arm64 -v "$HERE":/src:ro -v "$OUT":/out ubuntu:24.04 sh -c \
  "apt-get update >/dev/null && apt-get install -y gcc libc6-dev >/dev/null && gcc -O2 /src/chaos.c -o /out/chaos-arm64-glibc -lm && /out/chaos-arm64-glibc 40 > /out/arm64-glibc.txt"
"$PYTHON" "$HERE/pack.py" \
  "macOS arm64, Apple clang 21=$OUT/mac.txt" \
  "Linux arm64, gcc 14 + musl=$OUT/arm64-musl.txt" \
  "Linux x86_64, gcc 14 + musl=$OUT/amd64-musl.txt" \
  "Linux arm64, gcc 13 + glibc 2.39=$OUT/arm64-glibc.txt" > "$HERE/../native_runs.h"
echo "wrote $HERE/../native_runs.h"
