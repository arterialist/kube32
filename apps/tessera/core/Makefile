# tessera-core: freestanding C -> wasm32 (rust-lld as linker) + native host tests.

CLANG     ?= clang
RUST_LLD  ?= $(shell ls $$HOME/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld 2>/dev/null | head -1)
ARENA_BYTES    ?= 1425408
SCRATCH_BYTES  ?= 65536
INITIAL_MEMORY ?= 1572864
STACK_SIZE     ?= 65536

SRC      := src/k32lib.c src/store.c src/index.c src/seed.c src/router.c src/app.c
WASM_OBJ := $(patsubst src/%.c,build/wasm/%.o,$(SRC))
NAT_OBJ  := $(patsubst src/%.c,build/native/%.o,$(SRC))

WASM_CFLAGS := --target=wasm32 -O2 -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory \
               -Wall -Wextra -DTESSERA_ARENA_BYTES=$(ARENA_BYTES) -DTESSERA_SCRATCH_BYTES=$(SCRATCH_BYTES)
WASM_LDFLAGS := -flavor wasm --no-entry --export-dynamic --allow-undefined -z stack-size=$(STACK_SIZE) \
                --initial-memory=$(INITIAL_MEMORY) --max-memory=$(INITIAL_MEMORY)
NAT_CFLAGS  := -O2 -std=c11 -Wall -Wextra -DK32_NATIVE -DTESSERA_ARENA_BYTES=$(ARENA_BYTES) -DTESSERA_SCRATCH_BYTES=$(SCRATCH_BYTES) -fno-builtin
NAT_HOST_CFLAGS := -O2 -std=c11 -Wall -Wextra -DK32_NATIVE

.PHONY: all wasm test bench size profile clean

all: wasm

wasm: build/tessera-core.wasm

build/wasm/%.o: src/%.c src/*.h ../../../include/runtime/k32_app_abi.h | build/wasm
	$(CLANG) $(WASM_CFLAGS) -c $< -o $@

build/tessera-core.wasm: $(WASM_OBJ)
	@test -n "$(RUST_LLD)" || (echo "rust-lld not found (install a rust toolchain via rustup)"; exit 1)
	$(RUST_LLD) $(WASM_LDFLAGS) -o $@ $(WASM_OBJ)
	@echo "wasm: $@ ($$(wc -c < $@) bytes)"

build/native/%.o: src/%.c src/*.h ../../../include/runtime/k32_app_abi.h | build/native
	$(CLANG) $(NAT_CFLAGS) -c $< -o $@

build/native/shim.o: host_test/shim.c host_test/shim.h | build/native
	$(CLANG) $(NAT_HOST_CFLAGS) -c $< -o $@

build/test: host_test/test.c host_test/harness.h build/native/shim.o $(NAT_OBJ)
	$(CLANG) $(NAT_HOST_CFLAGS) -o $@ host_test/test.c build/native/shim.o $(NAT_OBJ)

build/bench: host_test/bench.c host_test/harness.h build/native/shim.o $(NAT_OBJ)
	$(CLANG) $(NAT_HOST_CFLAGS) -o $@ host_test/bench.c build/native/shim.o $(NAT_OBJ)

build/size: host_test/size.c host_test/harness.h build/native/shim.o $(NAT_OBJ)
	$(CLANG) $(NAT_HOST_CFLAGS) -o $@ host_test/size.c build/native/shim.o $(NAT_OBJ)

build/profile: host_test/profile.c host_test/harness.h build/native/shim.o $(NAT_OBJ)
	$(CLANG) $(NAT_HOST_CFLAGS) -o $@ host_test/profile.c build/native/shim.o $(NAT_OBJ)

profile: build/profile
	./build/profile

test: build/test
	SEED_ISSUES=500 SEED_COMMENTS=1000 ./build/test

bench: build/bench
	./build/bench

size: build/size wasm
	./build/size
	@printf "wasm binary: %s bytes, initial memory %s bytes, stack %s bytes, scratch %s bytes\n" "$$(wc -c < build/tessera-core.wasm | tr -d ' ')" "$(INITIAL_MEMORY)" "$(STACK_SIZE)" "$(SCRATCH_BYTES)"
	@python3 host_test/wasm_inspect.py build/tessera-core.wasm | grep -E "^memory|^imports|^exports"

build/wasm build/native:
	mkdir -p $@

clean:
	rm -rf build
