# Shared rules of the device drivers: builds $(NAME).c into the local image
# registry (images/driver-$(NAME).wasm, embedded in the firmware as
# wasm://embedded/driver-$(NAME)). The module declares one page; the pod's
# memory limit caps linear memory at MEM_CAP, so the stack and the static data
# must stay below it (checked after linking).
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
ROOT := ../../..
OUT ?= $(ROOT)/images/driver-$(NAME).wasm
MEM_CAP    ?= 3072
STACK_SIZE ?= 512

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --allow-undefined -z stack-size=$(STACK_SIZE) --initial-memory=65536 --max-memory=65536 -Map=build/$(NAME).map

all: $(OUT)

build/$(NAME).o: $(NAME).c ../common/driver.h $(ROOT)/include/runtime/k32_app_abi.h $(DEPS)
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $(NAME).c -o $@

$(OUT): build/$(NAME).o
	$(RUST_LLD) $(LDFLAGS) -o $@ build/$(NAME).o
	python3 $(ROOT)/apps/tessera/common/wasm_inspect.py $@ --map build/$(NAME).map --cap $(MEM_CAP) --stack $(STACK_SIZE)

clean:
	rm -rf build $(OUT)

.PHONY: all clean
