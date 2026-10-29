# Builds the tessera-hud pod (images/tessera-hud.wasm). The module declares
# one page; the pod's memory limit caps linear memory at MEM_CAP, so the
# stack and the static data must stay below it (checked after linking).
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
OUT ?= ../../../images/tessera-hud.wasm
MEM_CAP    ?= 1024
STACK_SIZE ?= 256

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --allow-undefined -z stack-size=$(STACK_SIZE) --initial-memory=65536 --max-memory=65536 -Map=build/tessera-hud.map

all: $(OUT)

build/hud.o: hud.c ../../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c hud.c -o $@

$(OUT): build/hud.o
	$(RUST_LLD) $(LDFLAGS) -o $@ build/hud.o
	python3 ../common/wasm_inspect.py $@ --map build/tessera-hud.map --cap $(MEM_CAP) --stack $(STACK_SIZE)

clean:
	rm -rf build

.PHONY: all clean
