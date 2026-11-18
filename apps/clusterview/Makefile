# Builds the clusterview pod into images/sd/clusterview.wasm (a card or
# inline image, not embedded in the firmware) and build/clusterview.yaml,
# examples/clusterview.yaml with the module inline.
#
#   make                                   module and manifest
#   kubectl --context kube32 apply -f apps/clusterview/build/clusterview.yaml
#
# The module declares one page; the pod's memory limit caps linear memory at
# MEM_CAP, so the stack and the static data must stay below it (checked after
# linking).
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
PYTHON ?= python3
OUT ?= ../../images/sd/clusterview.wasm
MEM_CAP    ?= 16384
STACK_SIZE ?= 1536

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --allow-undefined --stack-first -z stack-size=$(STACK_SIZE) --initial-memory=65536 --max-memory=65536 -Map=build/clusterview.map

all: $(OUT) build/clusterview.yaml

build/clusterview.o: clusterview.c ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c clusterview.c -o $@

$(OUT): build/clusterview.o
	@mkdir -p $(dir $(OUT))
	$(RUST_LLD) $(LDFLAGS) -o $@ build/clusterview.o
	$(PYTHON) ../tessera/common/wasm_inspect.py $@ --map build/clusterview.map --cap $(MEM_CAP) --stack $(STACK_SIZE)

build/clusterview.yaml: ../../examples/clusterview.yaml $(OUT)
	sed 's|"@clusterview.wasm"|"'"$$(base64 < $(OUT) | tr -d '\n')"'"|' ../../examples/clusterview.yaml > $@
	@echo "wrote $@"

clean:
	rm -rf build $(OUT)

.PHONY: all clean
