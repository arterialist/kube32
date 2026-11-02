# Builds a kube32 pod from C sources into one WebAssembly module, with the
# flags the runtime expects (as apps/echo builds).
#
#   make                      build/hello.wasm from *.c
#   make APP=myapp            build/myapp.wasm
#   make CLANG=/opt/homebrew/opt/llvm/bin/clang
#
# Needs clang with the wasm32 target and a WebAssembly linker: wasm-ld (LLVM's
# lld) or rust-lld from a Rust toolchain.
APP ?= hello
SRCS ?= $(wildcard *.c)
CLANG ?= clang
WASM_LD ?= $(firstword $(shell command -v wasm-ld 2>/dev/null) $(wildcard $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld))
# k32_app_abi.h: next to this Makefile in the SDK archive, in the repository's include/ otherwise
KUBE32_INCLUDE ?= $(if $(wildcard include/runtime/k32_app_abi.h),include,../../include)
# Linear memory (one 64 KiB page) and the shadow stack for C locals; raise
# both memory sizes together, and the pod's memory limit with them
MEMORY ?= 65536
STACK ?= 8192

CFLAGS = --target=wasm32 -O2 -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra -I$(KUBE32_INCLUDE)
LDFLAGS = $(if $(findstring rust-lld,$(WASM_LD)),-flavor wasm) --no-entry --export-dynamic --allow-undefined \
          -z stack-size=$(STACK) --initial-memory=$(MEMORY) --max-memory=$(MEMORY)
OBJS = $(SRCS:%.c=build/%.o)

all: build/$(APP).wasm

build/%.o: %.c $(KUBE32_INCLUDE)/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

build/$(APP).wasm: $(OBJS)
	@test -n "$(WASM_LD)" || { echo "no WebAssembly linker: install lld (wasm-ld) or Rust (rust-lld), or set WASM_LD"; exit 1; }
	$(WASM_LD) $(LDFLAGS) -o $@ $(OBJS)
	@ls -l $@

clean:
	rm -rf build

.PHONY: all clean
