# mosaico-verify: end-to-end Wasm verification pod for ESP-Mosaico modules
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= /opt/homebrew/opt/llvm@22/bin/clang
ROOT := ../..
OUT := $(ROOT)/images/mosaico-verify.wasm

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --allow-undefined -z stack-size=4096 --initial-memory=262144 --max-memory=262144

all: $(OUT)

build/mosaico-verify.o: mosaico-verify.c $(ROOT)/include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c mosaico-verify.c -o $@

$(OUT): build/mosaico-verify.o
	$(RUST_LLD) $(LDFLAGS) -o $@ build/mosaico-verify.o

clean:
	rm -rf build $(OUT)

.PHONY: all clean
