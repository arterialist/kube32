# Builds llm-speculative: its pod module, the three blobs (the target's int4
# slice, stories260K's int8 slice, the tokenizer) and the manifest with the
# module inline.
#
#   make TARGET_CKPT=<target-lm.bin>   module, blobs and manifest
#   make module                        build/llm-speculative.wasm
#   make blobs TARGET_CKPT=...         build/blobs/{target-lm,stories260k,tok512}.bin
#
# TARGET_CKPT is the target's llama2.c checkpoint (models/target-lm/README.md);
# stories260K and its tokenizer are downloaded once.
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld 2>/dev/null | head -1)
WASM_LD ?= $(firstword $(shell command -v wasm-ld 2>/dev/null) $(RUST_LLD))
CLANG ?= clang
PYTHON ?= python3
SPEC ?= uv run --script ../llm-pipeline/tools/spec.py
LLM ?= uv run --script ../llm-pipeline/tools/llm.py
TARGET_CKPT ?=
TARGET_BITS ?= 4
OPT ?= -Oz

CFLAGS = --target=wasm32 $(OPT) -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = $(if $(findstring rust-lld,$(WASM_LD)),-flavor wasm) --no-entry --allow-undefined -Map=$@.map
# shadow stack for C locals; the linear memory grows to the pod's limit
LD_MEMORY = -z stack-size=8192 --initial-memory=131072 --max-memory=2097152
INSPECT = $(PYTHON) ../tessera/common/wasm_inspect.py
HEADERS = ../llm-pipeline/llm.h ../llm-pipeline/tokenizer.h ../../include/runtime/k32_app_abi.h

all: module blobs manifest

module: build/llm-speculative.wasm

build/speculative.o: speculative.c $(HEADERS)
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

build/llm-speculative.wasm: build/speculative.o
	$(WASM_LD) $(LDFLAGS) $(LD_MEMORY) -o $@ $<
	$(INSPECT) $@ --map $@.map --cap 65536 --stack 8192

blobs:
	@test -n "$(TARGET_CKPT)" || { echo "TARGET_CKPT=<the target's llama2.c checkpoint> is needed"; exit 1; }
	$(SPEC) blobs --target $(TARGET_CKPT) --target-bits $(TARGET_BITS) --out build/blobs

manifest: build/llm-speculative.wasm
	$(LLM) manifest --template ../../examples/llm-speculative.yaml --modules build --out build/llm-speculative.yaml

clean:
	rm -rf build

.PHONY: all module blobs manifest clean
