# Builds the webhook forwarder into the local image registry
# (images/webhook.wasm, embedded in the firmware as wasm://embedded/webhook).
# The module declares one page; the pod's memory limit caps linear memory at
# MEM_CAP, so the stack and the static data must stay below it (checked after
# linking).
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
OUT ?= ../../images/webhook.wasm
MEM_CAP    ?= 8192
STACK_SIZE ?= 1024

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --allow-undefined -z stack-size=$(STACK_SIZE) --initial-memory=65536 --max-memory=65536 -Map=build/webhook.map

all: $(OUT)

build/webhook.o: webhook.c ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c webhook.c -o $@

$(OUT): build/webhook.o
	$(RUST_LLD) $(LDFLAGS) -o $@ build/webhook.o
	python3 ../tessera/common/wasm_inspect.py $@ --map build/webhook.map --cap $(MEM_CAP) --stack $(STACK_SIZE)

clean:
	rm -rf build $(OUT)

.PHONY: all clean
