# Builds the I2C scanner into the local image registry (images/i2cscan.wasm,
# embedded in the firmware as wasm://embedded/i2cscan).
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
OUT ?= ../../images/i2cscan.wasm

CFLAGS = --target=wasm32 -O2 -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --export-dynamic --allow-undefined -z stack-size=4096 --initial-memory=65536 --max-memory=65536

all: $(OUT)

build/i2cscan.o: i2cscan.c ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c i2cscan.c -o $@

$(OUT): build/i2cscan.o
	$(RUST_LLD) $(LDFLAGS) -o $@ build/i2cscan.o
	@ls -la $@

clean:
	rm -rf build $(OUT)

.PHONY: all clean
