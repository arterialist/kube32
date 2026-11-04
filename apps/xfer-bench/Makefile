# Builds xfer-bench: xfer-server (a control-plane pod that answers
# GET /bytes?n=<bytes>), xfer-client (a Cardputer pod that fetches bodies
# from it through k32_http_call and logs calls a second, latency and
# failures per size), and build/xfer-bench.yaml with both modules inline
# and the Service xfer-server.
#
#   make                       modules and manifest
#   make apply                 kubectl apply -f build/xfer-bench.yaml
#   make delete                deletes the pods and the Service
#   kubectl logs xfer-client   the results ("xfer n=..." lines, then "xfer done")
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld 2>/dev/null | head -1)
WASM_LD ?= $(firstword $(shell command -v wasm-ld 2>/dev/null) $(RUST_LLD))
CLANG ?= clang
KUBECTL ?= kubectl --context kube32

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = $(if $(findstring rust-lld,$(WASM_LD)),-flavor wasm) --no-entry --allow-undefined --stack-first -z stack-size=2048 \
          --initial-memory=65536 --max-memory=65536

all: build/xfer-bench.yaml

build/%.o: %.c xfer.h ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

build/xfer-%.wasm: build/%.o
	$(WASM_LD) $(LDFLAGS) -o $@ $<
	@ls -la $@

build/xfer-bench.yaml: xfer-bench.yaml.in build/xfer-server.wasm build/xfer-client.wasm
	sed -e "s|@SERVER_WASM@|$$(base64 < build/xfer-server.wasm | tr -d '\n')|" \
	    -e "s|@CLIENT_WASM@|$$(base64 < build/xfer-client.wasm | tr -d '\n')|" $< > $@

apply: build/xfer-bench.yaml
	$(KUBECTL) apply -f $<

delete:
	$(KUBECTL) delete pod xfer-client xfer-server --ignore-not-found
	$(KUBECTL) delete service xfer-server --ignore-not-found

clean:
	rm -rf build

.PHONY: all apply delete clean
