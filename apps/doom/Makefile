# Builds DOOM (doomgeneric) as a kube32 pod: build/doom.wasm.
#
#   make                     the pod
#   make ZONE_KB=512         the zone (DOOM's heap) size; -kb <n> in DOOM_ARGS overrides it
#
# The module starts with its static data and grows its linear memory with
# memory.grow as the engine allocates (the zone, the screen buffers); the
# pod's memory limit caps it.
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
ZONE_KB ?= 512
STACK ?= 65536
MAX_MEMORY ?= 4194304
OUT ?= build/doom.wasm

ENGINE = $(filter-out doomgeneric/i_sdlmusic.c doomgeneric/i_sdlsound.c doomgeneric/i_allegromusic.c \
           doomgeneric/i_allegrosound.c doomgeneric/doomgeneric_%.c, $(wildcard doomgeneric/*.c))
PORT = port/libc.c port/doomgeneric_kube32.c
BUILD ?= build
OBJS = $(ENGINE:doomgeneric/%.c=$(BUILD)/engine/%.o) $(PORT:port/%.c=$(BUILD)/port/%.o)

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory \
         -isystem port/include -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
         -DDG_ZONE_BYTES=$(ZONE_KB)*1024 -DDG_ZONE_STATS -DDG_NO_WIPE -DDG_NO_MUSIC $(EXTRA_CFLAGS)
ENGINE_CFLAGS = $(CFLAGS) -w
LDFLAGS = -flavor wasm --no-entry --allow-undefined --stack-first -z stack-size=$(STACK) --max-memory=$(MAX_MEMORY) \
          --gc-sections

all: $(OUT)

# The draw-list study: the same engine with counting rasterizers (port/drawlist_stats.c)
stats:
	$(MAKE) OUT=build/doom-stats.wasm BUILD=build/stats EXTRA_CFLAGS=-DDG_DRAWLIST_STATS PORT="$(PORT) port/drawlist_stats.c"

$(BUILD)/engine/%.o: doomgeneric/%.c
	@mkdir -p $(BUILD)/engine
	$(CLANG) $(ENGINE_CFLAGS) -c $< -o $@

$(BUILD)/port/%.o: port/%.c port/include/k32libc.h ../../include/runtime/k32_app_abi.h
	@mkdir -p $(BUILD)/port
	$(CLANG) $(CFLAGS) -Wall -Wno-unused-function -c $< -o $@

$(OUT): $(OBJS)
	$(RUST_LLD) $(LDFLAGS) -o $@ $(OBJS)
	@ls -la $@

# The host bench (bench/doombench.cpp) against the repository's wasm3 builds
# (cmake --build build --target wasm3 wasm3_opframes): doombench counts
# operations with the interpreter as the boards build it, doombench-fast
# times the host's own build.
REPO = ../..
WASM3_DEFS = -Dd_m3CodePageAlignSize=4096 -Dd_m3MaxFunctionStackHeight=1024 -Dd_m3HasExceptionHandling=0 -Dd_m3HasTypedRefs=0 \
             -Dd_m3HasStackSwitching=0 -Dd_m3HasSnapshots=0 -Dd_m3RecordBacktraces=0 -Dd_m3ParseNameSection=0 -Dd_m3MaxDuplicateFunctionImpl=1
OPFRAMES_DEFS = $(WASM3_DEFS) -Dd_m3MaxConstantTableSize=64 -Dd_m3HasMultiMemory=0 -Dd_m3HasMemory64=0 -Dd_m3HasAtomics=0 \
                -Dd_m3VerboseErrorMessages=0 -DM3_HAS_TAIL_CALL=0 -Dd_m3CountOpFrames=1 -Dd_m3PreloadNextOp=0
BENCH_CXX = c++ -std=c++17 -O2 -I$(REPO)/include -I$(REPO)/third_party/wasm3/source

build/doombench: bench/doombench.cpp $(REPO)/build/libwasm3_opframes.a
	$(BENCH_CXX) $(OPFRAMES_DEFS) -o $@ $< $(REPO)/src/runtime/pod_memory_pool.cpp $(REPO)/src/osal/system_posix.cpp $(REPO)/build/libwasm3_opframes.a

build/doombench-fast: bench/doombench.cpp $(REPO)/build/libwasm3.a
	$(BENCH_CXX) $(WASM3_DEFS) -o $@ $< $(REPO)/src/runtime/pod_memory_pool.cpp $(REPO)/src/osal/system_posix.cpp $(REPO)/build/libwasm3.a

bench: build/doombench build/doombench-fast

clean:
	rm -rf build

.PHONY: all stats bench clean
