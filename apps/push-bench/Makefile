# Builds push-bench: push-pub (a control-plane pod that publishes frames of
# a draw list's size on a channel topic, split into event-sized chunks) and
# push-sub (a Cardputer pod that reassembles them and logs whole frames a
# second, chunk loss and latency per run), and build/push-bench.yaml with
# both modules inline.
#
#   make                       modules and manifest
#   make apply                 kubectl apply -f build/push-bench.yaml
#   make delete                deletes both pods
#   kubectl logs push-sub      the results ("push recv ..." lines)
#   kubectl logs push-pub      what went out ("push sent ..." lines)
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld 2>/dev/null | head -1)
WASM_LD ?= $(firstword $(shell command -v wasm-ld 2>/dev/null) $(RUST_LLD))
CLANG ?= clang
KUBECTL ?= kubectl --context kube32

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = $(if $(findstring rust-lld,$(WASM_LD)),-flavor wasm) --no-entry --allow-undefined --stack-first -z stack-size=2048 \
          --initial-memory=65536 --max-memory=65536

all: build/push-bench.yaml

build/%.o: %.c push.h ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

build/push-%.wasm: build/%.o
	$(WASM_LD) $(LDFLAGS) -o $@ $<
	@ls -la $@

build/push-bench.yaml: push-bench.yaml.in build/push-pub.wasm build/push-sub.wasm
	sed -e "s|@PUB_WASM@|$$(base64 < build/push-pub.wasm | tr -d '\n')|" \
	    -e "s|@SUB_WASM@|$$(base64 < build/push-sub.wasm | tr -d '\n')|" $< > $@

apply: build/push-bench.yaml
	$(KUBECTL) apply -f $<

delete:
	$(KUBECTL) delete pod push-sub push-pub --ignore-not-found

clean:
	rm -rf build

.PHONY: all apply delete clean
