# Builds the display-node pod into the local image registry.
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
OUT ?= ../../../images/tessera-hud.wasm

CFLAGS = --target=wasm32 -O2 -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --export-dynamic --allow-undefined -z stack-size=16384 --initial-memory=65536 --max-memory=65536

all: $(OUT)

build/hud.o: hud.c ../../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c hud.c -o $@

$(OUT): build/hud.o
	$(RUST_LLD) $(LDFLAGS) -o $@ build/hud.o
	@ls -la $@

clean:
	rm -rf build $(OUT)

.PHONY: all clean
