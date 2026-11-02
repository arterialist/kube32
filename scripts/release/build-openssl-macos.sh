#!/usr/bin/env bash
# Build OpenSSL as static libraries for the macOS simulator release, so the
# simulator links no library outside the system's.
#
#   scripts/release/build-openssl-macos.sh <arm64|x86_64> <prefix>
#
# Installs headers and libraries under <prefix>; the simulator links them with
#   cmake -DOPENSSL_USE_STATIC_LIBS=TRUE -DOPENSSL_ROOT_DIR=<prefix> ...
# Builds for $MACOSX_DEPLOYMENT_TARGET (default 13.0), on either kind of Mac,
# with $JOBS compile jobs (default one per CPU).
set -euo pipefail

OPENSSL_VERSION=3.5.9
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a

ARCH="${1:?usage: scripts/release/build-openssl-macos.sh <arm64|x86_64> <prefix>}"
PREFIX="${2:?usage: scripts/release/build-openssl-macos.sh <arm64|x86_64> <prefix>}"
case "$ARCH" in
    arm64) TARGET=darwin64-arm64-cc ;;
    x86_64) TARGET=darwin64-x86_64-cc ;;
    *) echo "unknown architecture $ARCH" >&2; exit 2 ;;
esac
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-13.0}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
curl -fsSL -o "$WORK/openssl.tar.gz" \
    "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz"
echo "$OPENSSL_SHA256  $WORK/openssl.tar.gz" | shasum -a 256 -c -
tar -xzf "$WORK/openssl.tar.gz" -C "$WORK"
cd "$WORK/openssl-$OPENSSL_VERSION"
# /etc/ssl/cert.pem is the CA bundle every macOS installation has
./Configure "$TARGET" no-shared no-tests --prefix="$PREFIX" --libdir=lib --openssldir=/etc/ssl \
    "-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
make -j"${JOBS:-$(sysctl -n hw.ncpu)}" build_libs
make install_dev
