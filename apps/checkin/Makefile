# Builds the checkin pod into images/sd/checkin.wasm (a card or inline
# image, not embedded in the firmware) and build/checkin.yaml,
# examples/checkin.yaml with the module inline.
#
#   make                                   module and manifest
#   kubectl --context kube32 apply -f apps/checkin/build/checkin.yaml
#
# The module declares one page; the pod's memory limit caps linear memory at
# MEM_CAP, so the stack and the static data must stay below it (checked after
# linking).
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
PYTHON ?= python3
OUT ?= ../../images/sd/checkin.wasm
MEM_CAP    ?= 8192
STACK_SIZE ?= 2048

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --allow-undefined --stack-first -z stack-size=$(STACK_SIZE) --initial-memory=65536 --max-memory=65536 -Map=build/checkin.map

all: $(OUT) build/checkin.yaml

build/checkin.o: checkin.c ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c checkin.c -o $@

$(OUT): build/checkin.o
	@mkdir -p $(dir $(OUT))
	$(RUST_LLD) $(LDFLAGS) -o $@ build/checkin.o
	$(PYTHON) ../tessera/common/wasm_inspect.py $@ --map build/checkin.map --cap $(MEM_CAP) --stack $(STACK_SIZE)

build/checkin.yaml: ../../examples/checkin.yaml $(OUT)
	sed 's|"@checkin.wasm"|"'"$$(base64 < $(OUT) | tr -d '\n')"'"|' ../../examples/checkin.yaml > $@
	@echo "wrote $@"

clean:
	rm -rf build $(OUT)

.PHONY: all clean
