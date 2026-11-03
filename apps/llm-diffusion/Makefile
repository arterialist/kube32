# Builds the llm-diffusion pod, the model blob it reads in place, and the
# manifest with the module inline.
#
#   make                         build/llm-diffusion.wasm, build/mdlm-blob.bin, build/llm-diffusion.yaml
#   make MEMORY=... STACK=...    other linear memory and shadow stack
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld 2>/dev/null | head -1)
WASM_LD ?= $(firstword $(shell command -v wasm-ld 2>/dev/null) $(RUST_LLD))
CLANG ?= clang
PYTHON ?= python3
MEMORY ?= 262144
STACK ?= 4096
MODEL = ../../models/mdlm/mdlm.bin
TOKENIZER = ../../models/mdlm/tok512.bin

CFLAGS = --target=wasm32 -O2 -Ibuild -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra -I../../include
LDFLAGS = $(if $(findstring rust-lld,$(WASM_LD)),-flavor wasm) --no-entry --export-dynamic --allow-undefined --stack-first -z stack-size=$(STACK) --initial-memory=$(MEMORY) --max-memory=$(MEMORY)

all: build/llm-diffusion.wasm build/mdlm-blob.bin build/llm-diffusion.yaml

build/diffusion.o: diffusion.c build/tables.h ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

build/llm-diffusion.wasm: build/diffusion.o
	$(WASM_LD) $(LDFLAGS) -o $@ $<
	@ls -la $@

# the blob: the model without its rotary table; tables.h: the table and the tokenizer
build/mdlm-blob.bin: $(MODEL) $(TOKENIZER) tools/pack.py
	@mkdir -p build
	$(PYTHON) tools/pack.py $(MODEL) $(TOKENIZER) build/mdlm-blob.bin build/tables.h

build/tables.h: build/mdlm-blob.bin
	@test -f $@

build/llm-diffusion.yaml: ../../examples/llm-diffusion.yaml build/llm-diffusion.wasm
	sed "s|__WASM_BASE64__|$$(base64 < build/llm-diffusion.wasm | tr -d '\n')|" $< > $@

clean:
	rm -rf build

.PHONY: all clean
