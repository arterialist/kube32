# tessera-db: freestanding C -> wasm32 (rust-lld links) + native host tests.
CLANG    ?= clang
RUST_LLD ?= $(shell ls $$HOME/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld 2>/dev/null | head -1)
OUT      ?= ../../../images/tessera-db.wasm
# Linear memory cap (bytes): stack + data + the request block stay below it.
MEM_CAP    ?= 8192
STACK_SIZE ?= 768

SRC      := src/main.c src/tdb.c ../common/tlog.c ../common/minilib.c
HDR      := src/tdb.h ../common/tlog.h ../common/k32_host.h ../../../include/runtime/k32_app_abi.h
WASM_CFLAGS := --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory \
               -Wall -Wextra -DTDB_MEM_CAP=$(MEM_CAP)
WASM_LDFLAGS := -flavor wasm --no-entry --allow-undefined -z stack-size=$(STACK_SIZE) \
                --initial-memory=65536 --max-memory=65536 -Map=build/tessera-db.map
NAT_CFLAGS := -O2 -std=c11 -Wall -Wextra -g -DK32_NATIVE

.PHONY: all wasm test size clean
all: wasm

wasm: $(OUT)

build/wasm/%.o: %.c $(HDR) | build/wasm
	@mkdir -p $(dir $@)
	$(CLANG) $(WASM_CFLAGS) -c $< -o $@

WASM_OBJ := $(patsubst %.c,build/wasm/%.o,$(SRC))

$(OUT): $(WASM_OBJ)
	@test -n "$(RUST_LLD)" || (echo "rust-lld not found (install a rust toolchain via rustup)"; exit 1)
	$(RUST_LLD) $(WASM_LDFLAGS) -o $@ $(WASM_OBJ)
	python3 ../common/wasm_inspect.py $@ --map build/tessera-db.map --cap $(MEM_CAP) --stack $(STACK_SIZE) --heap-min 2400

build/test: host_test/test.c $(SRC) $(HDR) ../common/host/shim.c ../common/host/shim.h
	@mkdir -p build
	$(CLANG) $(NAT_CFLAGS) -o $@ host_test/test.c src/main.c src/tdb.c ../common/tlog.c ../common/host/shim.c

test: build/test
	./build/test

size: wasm

build/wasm:
	mkdir -p $@

clean:
	rm -rf build
