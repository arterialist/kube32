# tessera-api: freestanding C -> wasm32 (rust-lld links) + native host tests.
# The image is images/tessera-api.wasm: 20 pages (1,310,720 bytes) of linear
# memory, sized for the default capacity (MAX_ISSUES=2900, MAX_COMMENTS=5300).

CLANG     ?= clang
RUST_LLD  ?= $(shell ls $$HOME/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld 2>/dev/null | head -1)
OUT       ?= ../../../images/tessera-api.wasm
ARENA_BYTES    ?= 1163264
SCRATCH_BYTES  ?= 65536
INITIAL_MEMORY ?= 1310720
STACK_SIZE     ?= 65536

SRC      := src/k32lib.c src/store.c src/index.c src/journal.c src/storage.c src/router.c src/app.c ../common/tlog.c
HDR      := $(wildcard src/*.h) ../common/tlog.h ../common/k32_host.h ../../../include/runtime/k32_app_abi.h
WASM_OBJ := $(patsubst %.c,build/wasm/%.o,$(notdir $(SRC)))
NAT_OBJ  := $(patsubst %.c,build/native/%.o,$(notdir $(SRC)))
VPATH    := src ../common

DEFS        := -DTESSERA_ARENA_BYTES=$(ARENA_BYTES) -DTESSERA_SCRATCH_BYTES=$(SCRATCH_BYTES) -DTLOG_CRC_TABLE256
WASM_CFLAGS := --target=wasm32 -O2 -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra $(DEFS)
WASM_LDFLAGS := -flavor wasm --no-entry --allow-undefined -z stack-size=$(STACK_SIZE) \
                --initial-memory=$(INITIAL_MEMORY) --max-memory=$(INITIAL_MEMORY)
NAT_CFLAGS  := -O2 -std=c11 -Wall -Wextra -DK32_NATIVE $(DEFS) -fno-builtin
HOST_CFLAGS := -O2 -std=c11 -Wall -Wextra -DK32_NATIVE -DTLOG_CRC_TABLE256

# tessera-db and the dataset generator, linked into the host tools: the API
# loads from (and writes to) the real store code over a native service call.
DB_DEFS  := -Dk32_init=tdb_k32_init -Dk32_alloc=tdb_k32_alloc -Dk32_handle=tdb_k32_handle -DTDB_MEM_CAP=16384
SIDE_OBJ := build/native/db_main.o build/native/tdb.o build/native/seed.o build/native/dataset.o build/native/shim.o

.PHONY: all wasm test bench size profile clean

all: wasm

wasm: $(OUT)

build/wasm/%.o: %.c $(HDR) | build/wasm
	$(CLANG) $(WASM_CFLAGS) -c $< -o $@

$(OUT): $(WASM_OBJ)
	@test -n "$(RUST_LLD)" || (echo "rust-lld not found (install a rust toolchain via rustup)"; exit 1)
	$(RUST_LLD) $(WASM_LDFLAGS) -o $@ $(WASM_OBJ)
	@echo "wasm: $@ ($$(wc -c < $@) bytes)"

build/native/%.o: %.c $(HDR) | build/native
	$(CLANG) $(NAT_CFLAGS) -c $< -o $@

build/native/db_main.o: ../db/src/main.c ../db/src/tdb.h $(HDR) | build/native
	$(CLANG) $(HOST_CFLAGS) $(DB_DEFS) -c $< -o $@
build/native/tdb.o: ../db/src/tdb.c ../db/src/tdb.h $(HDR) | build/native
	$(CLANG) $(HOST_CFLAGS) -c $< -o $@
build/native/seed.o: ../tools/dataset/seed.c ../tools/dataset/seed.h src/k32lib.h | build/native
	$(CLANG) $(NAT_CFLAGS) -c $< -o $@
build/native/dataset.o: ../tools/dataset/dataset.c ../tools/dataset/dataset.h ../db/src/tdb.h | build/native
	$(CLANG) $(HOST_CFLAGS) -c $< -o $@
build/native/shim.o: ../common/host/shim.c ../common/host/shim.h ../common/k32_host.h | build/native
	$(CLANG) $(HOST_CFLAGS) -c $< -o $@

TOOLS := build/test build/bench build/size build/profile
$(TOOLS): build/%: host_test/%.c host_test/harness.h $(NAT_OBJ) $(SIDE_OBJ)
	$(CLANG) $(HOST_CFLAGS) -o $@ $< $(NAT_OBJ) $(SIDE_OBJ)

test: build/test
	MAX_ISSUES=1000 MAX_COMMENTS=1500 ./build/test

bench: build/bench
	./build/bench

size: build/size wasm
	./build/size
	@printf "wasm binary: %s bytes, initial memory %s bytes, stack %s bytes, scratch %s bytes\n" "$$(wc -c < $(OUT) | tr -d ' ')" "$(INITIAL_MEMORY)" "$(STACK_SIZE)" "$(SCRATCH_BYTES)"
	@python3 ../common/wasm_inspect.py $(OUT) | grep -E "^memory|^imports|^exports"

profile: build/profile
	./build/profile

build/wasm build/native:
	mkdir -p $@

clean:
	rm -rf build
