# tessera-web: the web client bundle (build.py) and the pod that serves it.
CLANG    ?= clang
RUST_LLD ?= $(shell ls $$HOME/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld 2>/dev/null | head -1)
OUT      ?= ../../../images/tessera-web.wasm
BUNDLE   ?= ../../../images/web/tessera-web
# Linear memory cap (bytes): stack + data + the request block stay below it.
MEM_CAP    ?= 4096
STACK_SIZE ?= 512

HDR := ../common/k32_host.h ../../../include/runtime/k32_app_abi.h
WASM_CFLAGS := --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory \
               -Wall -Wextra -DWEB_MEM_CAP=$(MEM_CAP)
WASM_LDFLAGS := -flavor wasm --no-entry --allow-undefined -z stack-size=$(STACK_SIZE) \
                --initial-memory=65536 --max-memory=65536 -Map=build/tessera-web.map

.PHONY: all wasm bundle test clean
all: bundle wasm

bundle:
	python3 build.py --out $(BUNDLE)

wasm: $(OUT)

build/server.o: server.c $(HDR)
	@mkdir -p build
	$(CLANG) $(WASM_CFLAGS) -c server.c -o $@
build/minilib.o: ../common/minilib.c
	@mkdir -p build
	$(CLANG) $(WASM_CFLAGS) -c $< -o $@

$(OUT): build/server.o build/minilib.o
	@test -n "$(RUST_LLD)" || (echo "rust-lld not found (install a rust toolchain via rustup)"; exit 1)
	$(RUST_LLD) $(WASM_LDFLAGS) -o $@ build/server.o build/minilib.o
	python3 ../common/wasm_inspect.py $@ --map build/tessera-web.map --cap $(MEM_CAP) --stack $(STACK_SIZE) --heap-min 2048

build/test: host_test/test.c server.c $(HDR) ../common/host/shim.c ../common/host/shim.h
	@mkdir -p build
	$(CLANG) -O2 -std=c11 -Wall -Wextra -DK32_NATIVE -DWEB_MEM_CAP=$(MEM_CAP) -o $@ host_test/test.c server.c ../common/host/shim.c

test: build/test bundle
	./build/test $(BUNDLE)

clean:
	rm -rf build
