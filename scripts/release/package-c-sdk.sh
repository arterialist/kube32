#!/usr/bin/env bash
# Package the C SDK for a release.
#
#   scripts/release/package-c-sdk.sh <version> [out dir]
#
# Writes <out dir>/kube32-c-sdk-<version>.tar.gz and its .sha256: the ABI
# header (include/runtime/k32_app_abi.h), and the Makefile, example app and
# README of sdk/c, under kube32-c-sdk-<version>/.
set -euo pipefail

VERSION="${1:?usage: scripts/release/package-c-sdk.sh <version> [out dir]}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$(mkdir -p "${2:-$ROOT/dist}" && cd "${2:-$ROOT/dist}" && pwd)"
NAME="kube32-c-sdk-$VERSION"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/$NAME/include/runtime"
cp "$ROOT/include/runtime/k32_app_abi.h" "$STAGE/$NAME/include/runtime/"
cp "$ROOT/sdk/c/Makefile" "$ROOT/sdk/c/hello.c" "$ROOT/sdk/c/README.md" "$ROOT/LICENSE" "$STAGE/$NAME/"

# GNU tar writes the same bytes for the same sources: sorted entries, fixed
# owner and times
if tar --version 2>/dev/null | grep -q GNU; then
    tar -C "$STAGE" --format=ustar --owner=0 --group=0 --numeric-owner --mtime='2000-01-01 00:00:00Z' \
        --sort=name -cf - "$NAME" | gzip -9n > "$OUT/$NAME.tar.gz"
else
    tar -C "$STAGE" -cf - "$NAME" | gzip -9n > "$OUT/$NAME.tar.gz"
fi
(cd "$OUT" && { sha256sum "$NAME.tar.gz" 2>/dev/null || shasum -a 256 "$NAME.tar.gz"; } > "$NAME.tar.gz.sha256")
echo "$OUT/$NAME.tar.gz"
