# Builds the svcprobe pod into images/sd/ (a card image, not embedded in the
# firmware): copy it to a node's storage as images/svcprobe.wasm (kube32-cli
# push) and reference it as wasm://sd/svcprobe, or deploy it inline with
# kube32-cli deploy. The stack comes first and is small, so the pod runs
# under a 16Ki memory limit.
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
OUT ?= ../../images/sd/svcprobe.wasm

CFLAGS = --target=wasm32 -Os -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --export-dynamic --allow-undefined --stack-first -z stack-size=3072 --initial-memory=65536 --max-memory=65536

all: $(OUT)

build/svcprobe.o: svcprobe.c ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c svcprobe.c -o $@

$(OUT): build/svcprobe.o
	@mkdir -p $(dir $(OUT))
	$(RUST_LLD) $(LDFLAGS) -o $@ build/svcprobe.o
	@ls -la $@

clean:
	rm -rf build $(OUT)

.PHONY: all clean
