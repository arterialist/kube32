# Builds the bench pod into images/sd/bench.wasm (a card image, not embedded
# in the firmware); deploy it inline with kube32-cli deploy. The kernels are
# src/bench/bench_kernels.c, the same source the firmware runs natively for
# K32_CMD:BENCH.
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
OUT ?= ../../images/sd/bench.wasm

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra -I../../include
LDFLAGS = -flavor wasm --no-entry --export-dynamic --allow-undefined --stack-first -z stack-size=4096 --initial-memory=65536 --max-memory=65536

all: $(OUT)

build/%.o: %.c ../../include/runtime/k32_app_abi.h ../../include/bench/bench_kernels.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

build/bench_kernels.o: ../../src/bench/bench_kernels.c ../../include/bench/bench_kernels.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

$(OUT): build/bench.o build/bench_kernels.o
	@mkdir -p $(dir $(OUT))
	$(RUST_LLD) $(LDFLAGS) -o $@ build/bench.o build/bench_kernels.o
	@ls -la $@

# The kernels alone, for tools/bench_ops.cpp (build/bench_ops counts the
# wasm3 operations each kernel executes)
ops: build/ops.wasm

build/ops.wasm: build/ops.o build/bench_kernels.o
	$(RUST_LLD) -flavor wasm --no-entry --stack-first -z stack-size=4096 --initial-memory=65536 --max-memory=65536 -o $@ build/ops.o build/bench_kernels.o

clean:
	rm -rf build $(OUT)

.PHONY: all ops clean
