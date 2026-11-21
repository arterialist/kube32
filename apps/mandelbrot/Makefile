# Builds the mandelbrot pod into images/sd/mandelbrot.wasm (a card or inline
# image, not embedded in the firmware) and build/mandelbrot.yaml,
# examples/mandelbrot.yaml with the module inline.
#
#   make                                   module and manifest
#   kubectl --context kube32 apply -f apps/mandelbrot/build/mandelbrot.yaml
#
# The module declares one page; the pod's memory limit caps linear memory at
# MEM_CAP. The image takes what lies between __heap_base and the limit, so the
# check after linking keeps IMAGE_BYTES free there: one byte a row and half a
# byte a pixel, 256x192 by default.
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
PYTHON ?= python3
OUT ?= ../../images/sd/mandelbrot.wasm
MEM_CAP     ?= 32768
STACK_SIZE  ?= 1024
IMAGE_BYTES ?= 24768

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --allow-undefined --strip-all --stack-first -z stack-size=$(STACK_SIZE) --initial-memory=65536 --max-memory=65536 -Map=build/mandelbrot.map

all: $(OUT) build/mandelbrot.yaml

build/mandelbrot.o: mandelbrot.c ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c mandelbrot.c -o $@

$(OUT): build/mandelbrot.o
	@mkdir -p $(dir $(OUT))
	$(RUST_LLD) $(LDFLAGS) -o $@ build/mandelbrot.o
	$(PYTHON) ../tessera/common/wasm_inspect.py $@ --map build/mandelbrot.map --cap $(MEM_CAP) --stack $(STACK_SIZE) --heap-min $(IMAGE_BYTES)

build/mandelbrot.yaml: ../../examples/mandelbrot.yaml $(OUT)
	sed 's|"@mandelbrot.wasm"|"'"$$(base64 < $(OUT) | tr -d '\n')"'"|' ../../examples/mandelbrot.yaml > $@
	@echo "wrote $@"

clean:
	rm -rf build $(OUT)

.PHONY: all clean
