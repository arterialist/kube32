# Builds the irremote pod into the local image registry (images/irremote.wasm,
# embedded in the firmware as wasm://embedded/irremote). The module declares
# one page; the pod's memory limit caps linear memory at MEM_CAP, so the
# stack and the static data must stay below it (checked after linking).
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
OUT ?= ../../images/irremote.wasm
MEM_CAP    ?= 12288
STACK_SIZE ?= 1024

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --allow-undefined -z stack-size=$(STACK_SIZE) --initial-memory=65536 --max-memory=65536 -Map=build/irremote.map

all: $(OUT)

build/irremote.o: irremote.c ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c irremote.c -o $@

$(OUT): build/irremote.o
	$(RUST_LLD) $(LDFLAGS) -o $@ build/irremote.o
	python3 ../tessera/common/wasm_inspect.py $@ --map build/irremote.map --cap $(MEM_CAP) --stack $(STACK_SIZE)

clean:
	rm -rf build $(OUT)

.PHONY: all clean
