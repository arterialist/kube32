# Builds the RFID reader into the local image registry (images/rfid.wasm,
# embedded in the firmware as wasm://embedded/rfid). The module declares one
# page; the pod's memory limit caps linear memory at MEM_CAP, so the stack and
# the static data must stay below it (checked after linking).
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
OUT ?= ../../images/rfid.wasm
MEM_CAP    ?= 4096
STACK_SIZE ?= 2048

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --export-dynamic --allow-undefined -z stack-size=$(STACK_SIZE) --initial-memory=65536 --max-memory=65536 -Map=build/rfid.map

all: $(OUT)

build/rfid.o: rfid.c ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c rfid.c -o $@

$(OUT): build/rfid.o
	$(RUST_LLD) $(LDFLAGS) -o $@ build/rfid.o
	python3 ../tessera/common/wasm_inspect.py $@ --map build/rfid.map --cap $(MEM_CAP) --stack $(STACK_SIZE)

clean:
	rm -rf build $(OUT)

.PHONY: all clean
