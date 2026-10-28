# Builds the sdnote pod. The output lives under images/sd/ (not images/), so it
# is NOT embedded in the firmware: copy it to the microSD card as
# /kube32/images/sdnote.wasm and reference it as wasm://sd/sdnote.
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
OUT ?= ../../images/sd/sdnote.wasm

CFLAGS = --target=wasm32 -O2 -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --export-dynamic --allow-undefined -z stack-size=8192 --initial-memory=65536 --max-memory=65536

all: $(OUT)

build/sdnote.o: sdnote.c ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c sdnote.c -o $@

$(OUT): build/sdnote.o
	@mkdir -p $(dir $(OUT))
	$(RUST_LLD) $(LDFLAGS) -o $@ build/sdnote.o
	@ls -la $@

clean:
	rm -rf build $(OUT)

.PHONY: all clean
