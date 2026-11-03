# syntax=docker/dockerfile:1
#
# The toolchain the integration suites need, on Ubuntu 24.04 like the CI runners:
# the simulator's build tools, clang and lld (wasm-ld) for the wasm32 pod
# modules, numpy for the model tools, and a pinned kubectl.
#
#   docker build -t kube32-ci:24.04 -f tools/ci/Dockerfile tools/ci
#   docker run --rm -e CI=true -v "$PWD":/src -w /src kube32-ci:24.04 \
#       sh -c 'cmake -S . -B build && cmake --build build --target kube32-sim &&
#              python3 tests/integration/runner.py --suite cluster'
#
# With CI=true (or KUBE32_REQUIRE_TOOLS=1) a suite that needs a tool fails when
# the tool is missing instead of skipping.
FROM ubuntu:24.04

# kubectl stays within one minor version of the API level the simulator reports
ARG KUBECTL_VERSION=v1.33.9
ARG KUBECTL_SHA256_AMD64=9e33e3234c0842cd44a12c13e334b4ce930145ea84b855ce7cc0a7b6bc670c22
ARG KUBECTL_SHA256_ARM64=af4dc943a6f447ecb070340efe63c7f8ee2808e6c0bc42126efe7cde0cc1e69b

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates clang cmake curl g++ git lld make python3 python3-numpy \
    && rm -rf /var/lib/apt/lists/* \
    && arch="$(dpkg --print-architecture)" \
    && case "$arch" in \
        amd64) sha="$KUBECTL_SHA256_AMD64" ;; \
        arm64) sha="$KUBECTL_SHA256_ARM64" ;; \
        *) echo "no kubectl checksum for $arch" >&2; exit 1 ;; \
       esac \
    && curl -fsSLo /usr/local/bin/kubectl "https://dl.k8s.io/release/${KUBECTL_VERSION}/bin/linux/${arch}/kubectl" \
    && echo "${sha}  /usr/local/bin/kubectl" | sha256sum -c - \
    && chmod 0755 /usr/local/bin/kubectl \
    && clang --version | head -1 \
    && wasm-ld --version \
    && kubectl version --client \
    && python3 -c 'import numpy; print("numpy", numpy.__version__)'
