# Builds llm-pipeline: its three pod modules, the model files for llm-store,
# and the manifest with the modules inline.
#
#   make                 modules, model files (downloads stories260K once) and manifest
#   make modules         build/llm-{shard,coordinator,store}.wasm
#   make SPLIT=0-1,2,3-4 BITS=8,4,8   other layers and weight bits per stage
#
# The model host (the kube32-sim node that runs llm-store) serves build/card:
# start it with KUBE32_SD_ROOT=<this directory>/build/card.
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld 2>/dev/null | head -1)
WASM_LD ?= $(firstword $(shell command -v wasm-ld 2>/dev/null) $(RUST_LLD))
CLANG ?= clang
PYTHON ?= python3
LLM ?= uv run --script tools/llm.py
SPLIT ?= 0-2,3,4
BITS ?= 8,4,8
PROMPT ?= Once upon a time

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = $(if $(findstring rust-lld,$(WASM_LD)),-flavor wasm) --no-entry --allow-undefined -Map=$@.map

# stack (shadow stack for C locals), linear memory: initial and most pages,
# and the pod memory limit the data must fit below
SHARD_LD = -z stack-size=1024 --initial-memory=65536 --max-memory=1048576
COORDINATOR_LD = -z stack-size=4096 --initial-memory=65536 --max-memory=65536
STORE_LD = -z stack-size=512 --initial-memory=65536 --max-memory=65536
INSPECT = $(PYTHON) ../tessera/common/wasm_inspect.py

MODULES = build/llm-shard.wasm build/llm-coordinator.wasm build/llm-store.wasm

all: modules model manifest

modules: $(MODULES)

build/%.o: %.c llm.h ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

build/llm-shard.wasm: build/shard.o
	$(WASM_LD) $(LDFLAGS) $(SHARD_LD) -o $@ $<
	$(INSPECT) $@ --map $@.map --cap 8192 --stack 1024

build/llm-coordinator.wasm: build/coordinator.o
	$(WASM_LD) $(LDFLAGS) $(COORDINATOR_LD) -o $@ $<
	$(INSPECT) $@ --map $@.map --cap 49152 --stack 4096

build/llm-store.wasm: build/store.o
	$(WASM_LD) $(LDFLAGS) $(STORE_LD) -o $@ $<
	$(INSPECT) $@ --map $@.map --cap 2048 --stack 512

model: build/llm-store.wasm
	$(LLM) build --split $(SPLIT) --bits $(BITS) --store-module build/llm-store.wasm

manifest: $(MODULES)
	$(LLM) manifest --prompt "$(PROMPT)"

clean:
	rm -rf build/*.o build/*.wasm build/*.map build/card build/llm-pipeline.yaml

.PHONY: all modules model manifest clean
