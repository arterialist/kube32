# syntax=docker/dockerfile:1
#
# kube32-sim, the host simulator, as a container image: a control plane, its
# kubelet and one worker in one process.
#
#   docker build -t kube32-sim .
#   docker run --rm -p 6443:6443 -p 8080:8080 kube32-sim
#
# The simulator is linked statically against musl and OpenSSL, so the same
# binary runs in the image and on any Linux host:
#
#   docker build --target sim-binary --output type=local,dest=dist .
#
# writes dist/linux-<arch>/kube32-sim. The release workflow builds those
# binaries on native runners and passes them in with
# --build-context sim-binary=<dir>, so the image holds the released bytes.

ARG ALPINE_VERSION=3.22
# Distroless has no linux/riscv64 image: build that platform with
# --build-arg RUNTIME_BASE=alpine:3.22
ARG RUNTIME_BASE=gcr.io/distroless/static-debian12:nonroot

FROM alpine:${ALPINE_VERSION} AS build
RUN apk add --no-cache build-base cmake linux-headers openssl-dev openssl-libs-static python3
WORKDIR /src
COPY . .
# The version nodes and GET /version report; the build context has no git
# history to describe (build arguments reach RUN as environment variables)
ARG KUBE32_VERSION=dev
# Compile jobs, default one per CPU
ARG BUILD_JOBS=
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
        -DOPENSSL_USE_STATIC_LIBS=TRUE -DCMAKE_EXE_LINKER_FLAGS=-static \
    && cmake --build /build --parallel "${BUILD_JOBS:-$(nproc)}" --target kube32-sim \
    && strip /build/kube32-sim \
    && /build/kube32-sim --version

FROM scratch AS sim-binary
ARG TARGETOS
ARG TARGETARCH
COPY --from=build /build/kube32-sim /${TARGETOS}-${TARGETARCH}/kube32-sim

FROM ${RUNTIME_BASE}
ARG TARGETOS
ARG TARGETARCH
LABEL org.opencontainers.image.title="kube32-sim" \
      org.opencontainers.image.description="kube32 host simulator: a Kubernetes-compatible control plane and WebAssembly pod runtime" \
      org.opencontainers.image.source="https://github.com/arterialist/kube32" \
      org.opencontainers.image.licenses="Apache-2.0"
COPY --from=sim-binary /${TARGETOS}-${TARGETARCH}/kube32-sim /usr/local/bin/kube32-sim
# API server, Ingress, pod HTTP server
EXPOSE 6443 8080 17000
ENTRYPOINT ["/usr/local/bin/kube32-sim"]
CMD ["--ingress-port", "8080", "--pod-port", "17000"]
