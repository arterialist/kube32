# Builds the room-monitor example into the local image registry
# (images/room-monitor.wasm, embedded in the firmware as
# wasm://embedded/room-monitor). The module declares one page; the pod's
# memory limit caps linear memory at MEM_CAP, so the stack and the static data
# must stay below it (checked after linking).
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
OUT ?= ../../images/room-monitor.wasm
MEM_CAP    ?= 4096
STACK_SIZE ?= 512

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --allow-undefined -z stack-size=$(STACK_SIZE) --initial-memory=65536 --max-memory=65536 -Map=build/room-monitor.map

all: $(OUT)

build/room-monitor.o: room-monitor.c ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c room-monitor.c -o $@

$(OUT): build/room-monitor.o
	$(RUST_LLD) $(LDFLAGS) -o $@ build/room-monitor.o
	python3 ../tessera/common/wasm_inspect.py $@ --map build/room-monitor.map --cap $(MEM_CAP) --stack $(STACK_SIZE)

clean:
	rm -rf build $(OUT)

.PHONY: all clean
