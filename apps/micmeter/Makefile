# Builds the micmeter pod into images/sd/micmeter.wasm (a card image, not
# embedded in the firmware); deploy it inline with kube32-cli deploy.
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
OUT ?= ../../images/sd/micmeter.wasm

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra -I../../include
LDFLAGS = -flavor wasm --no-entry --export-dynamic --allow-undefined --stack-first -z stack-size=4096 --initial-memory=65536 --max-memory=65536

all: $(OUT)

build/%.o: %.c ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

$(OUT): build/micmeter.o
	@mkdir -p $(dir $(OUT))
	$(RUST_LLD) $(LDFLAGS) -o $@ build/micmeter.o
	@ls -la $@

clean:
	rm -rf build $(OUT)

.PHONY: all clean
