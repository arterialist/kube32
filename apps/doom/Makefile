# Builds DOOM (doomgeneric) as a kube32 pod: build/doom.wasm.
#
#   make                     the pod, 320 x 200
#   make W=240               the pod at 240 x 135 (build/doom-240.wasm), the Cardputer's screen
#   make sim [W=240]         the sim-only pod (build/doom-sim[-240].wasm): a draw list a frame
#   make view                the Cardputer view pod (build/doom-view.wasm)
#   make check               build/drawlist_check: the split renderer against the engine
#   make verify [TICS=n]     runs it: game states and pixels, both widths, the sim on the sim pack
#   make verify-net          two players in lockstep through a simulated network (port/net.c)
#   make pods                the two pods' modules and the packs as blobs (build/blobs/)
#   make manifest            build/doom.yaml (with the wordsketch pods at 0 replicas, from
#                            apps/wordsketch/build) and build/doom-only.yaml (DOOM alone)
#   make upload SERVER=<url> the blobs to the nodes: doom-sim, doom-wad and doom-web to the
#                            control plane, doom-view and doom-pack to the Cardputer
#   make ZONE_KB=512         the zone (DOOM's heap) size; -kb <n> in DOOM_ARGS overrides it
#
# The module starts with its static data and grows its linear memory with
# memory.grow as the engine allocates (the zone, the screen buffers); the
# pod's memory limit caps it.
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
REPO = ../..
ZONE_KB ?= 512
STACK ?= 65536
MAX_MEMORY ?= 4194304
W ?= 320
H ?= $(if $(filter 240,$(W)),135,200)
SUF = $(if $(filter 320,$(W)),,-$(W))
OUT ?= build/doom$(SUF).wasm
BUILD ?= build/full$(SUF)

# The sim-only build leaves out what never runs without a screen: the menu, automap,
# intermission, finale, wipe, HUD, status bar, savegames, the rasterizers and the screen
# (port/sim_stubs.c answers the calls the rest of the engine still makes)
SIM_EXCLUDE = doomgeneric/m_menu.c doomgeneric/am_map.c doomgeneric/wi_stuff.c doomgeneric/f_finale.c doomgeneric/f_wipe.c \
              doomgeneric/hu_stuff.c doomgeneric/hu_lib.c doomgeneric/st_stuff.c doomgeneric/st_lib.c doomgeneric/p_saveg.c \
              doomgeneric/r_draw.c doomgeneric/v_video.c doomgeneric/i_scale.c doomgeneric/i_endoom.c doomgeneric/statdump.c \
              doomgeneric/w_checksum.c doomgeneric/sha1.c doomgeneric/i_joystick.c doomgeneric/i_cdmus.c doomgeneric/i_sound.c \
              doomgeneric/i_video.c doomgeneric/m_config.c
EXCLUDE ?=
ENGINE = $(filter-out doomgeneric/i_sdlmusic.c doomgeneric/i_sdlsound.c doomgeneric/i_allegromusic.c \
           doomgeneric/i_allegrosound.c doomgeneric/doomgeneric_%.c $(EXCLUDE), $(wildcard doomgeneric/*.c))
PORT = port/libc.c port/doomgeneric_kube32.c port/state_digest.c port/net.c
BUILD ?= build
OBJS = $(ENGINE:doomgeneric/%.c=$(BUILD)/engine/%.o) $(PORT:port/%.c=$(BUILD)/port/%.o)

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory \
         -isystem port/include -DCMAP256 -DDOOMGENERIC_RESX=$(W) -DDOOMGENERIC_RESY=$(H) -DSCREENWIDTH=$(W) -DSCREENHEIGHT=$(H) \
         -DDG_ZONE_BYTES=$(ZONE_KB)*1024 -DDG_ZONE_STATS -DDG_NO_WIPE -DDG_NO_MUSIC -DDG_LUMP_PAD=128 $(EXTRA_CFLAGS)
ENGINE_CFLAGS = $(CFLAGS) -w
# INITIAL_MEMORY: the linear memory the module starts with; the sim-only pod declares all it
# uses (its statics and the zone), so it never grows on the board, where a grow needs room
# beside the pod's other allocations in the pool block
INITIAL_MEMORY ?=
LDFLAGS = -flavor wasm --no-entry --allow-undefined --stack-first -z stack-size=$(STACK) --max-memory=$(MAX_MEMORY) \
          --gc-sections --export=__heap_base --export=__data_end $(if $(INITIAL_MEMORY),--initial-memory=$(INITIAL_MEMORY))

all: $(OUT)

# The draw-list study: the same engine with counting rasterizers (port/drawlist_stats.c)
stats:
	$(MAKE) W=$(W) OUT=build/doom-stats$(SUF).wasm BUILD=build/stats$(SUF) EXTRA_CFLAGS=-DDG_DRAWLIST_STATS PORT="$(PORT) port/drawlist_stats.c"

# The sim-only pod: no rasterizers and no screen buffers; a frame is a draw list (port/drawlist_emit.c)
LIST_BYTES ?= $(if $(filter 240,$(W)),12288,16384)
sim:
	$(MAKE) W=$(W) OUT=build/doom-sim$(SUF).wasm BUILD=build/sim$(SUF) EXTRA_CFLAGS="-DDG_DRAWLIST_STATS -DDG_SIM_ONLY -DLIST_BYTES=$(LIST_BYTES)" INITIAL_MEMORY=655360 STACK=32768 \
	    EXCLUDE="$(SIM_EXCLUDE)" PORT="$(PORT) port/drawlist_emit.c port/sim_stubs.c"

# The view pod: the decoder and the kernels' records, no engine
VIEW_CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -mcpu=mvp -msign-ext -mbulk-memory -Wall -I../../include -Iview
build/doom-view.wasm: view/view.c view/drawlist.c view/drawlist.h ../../include/runtime/k32_app_abi.h
	@mkdir -p build/view
	$(CLANG) $(VIEW_CFLAGS) -c view/view.c -o build/view/view.o
	$(CLANG) $(VIEW_CFLAGS) -c view/drawlist.c -o build/view/drawlist.o
	$(RUST_LLD) -flavor wasm --no-entry --allow-undefined --stack-first -z stack-size=4096 --gc-sections -o $@ build/view/view.o build/view/drawlist.o
	@ls -la $@

view: build/doom-view.wasm

# ---- the boards: blobs, manifests, upload ----
KUBECTL ?= kubectl --context kube32
SERVER ?=
LLM ?= uv run --script ../llm-pipeline/tools/llm.py
IWAD ?= build/wad/doom1-e1m1-min.wad
BLOBS = build/blobs/doom-sim build/blobs/doom-wad build/blobs/doom-view build/blobs/doom-pack build/blobs/doom-web

pods: $(BLOBS)
build/blobs/doom-sim: FORCE
	@mkdir -p build/blobs
	$(MAKE) W=240 sim
	cp build/doom-sim-240.wasm $@
build/blobs/doom-view: build/doom-view.wasm
	@mkdir -p build/blobs
	cp $< $@
build/blobs/doom-wad: $(IWAD) wad.py
	@mkdir -p build/blobs
	$(PYTHON) wad.py simpack $< $@
build/blobs/doom-pack: $(IWAD) wad.py
	@mkdir -p build/blobs
	$(PYTHON) wad.py pack $< $@
# the page for player 2: its engine is the full 320-wide build
build/blobs/doom-web: $(IWAD) wad.py web/index.html web/doom-net.js FORCE
	@mkdir -p build/blobs
	$(MAKE) W=320
	$(PYTHON) wad.py web $(IWAD) build/doom.wasm $@

manifest:
	@mkdir -p build
	$(PYTHON) -c "import sys; d=open('../../examples/doom.yaml').read().split('\n---\n'); n=[x for x in d if 'app: doom' in x or 'name: doom' in x]; open('build/doom-only.yaml','w').write('\n---\n'.join(n)+'\n')"
	@ls ../wordsketch/build/wordsketch-listen.wasm >/dev/null 2>&1 && $(LLM) manifest --template ../../examples/doom.yaml --modules ../wordsketch/build --out build/doom.yaml \
	    || echo "build/doom.yaml needs the wordsketch modules (make -C ../wordsketch modules); build/doom-only.yaml has the DOOM pods alone"

upload: $(BLOBS)
	@cardputer=$$($(KUBECTL) $(if $(SERVER),--server $(SERVER)) get nodes -l kube32.io/board=cardputer -o name | head -1 | cut -d/ -f2); \
	control=$$($(KUBECTL) $(if $(SERVER),--server $(SERVER)) get nodes -l node-role.kubernetes.io/control-plane -o name | head -1 | cut -d/ -f2); \
	for b in doom-sim:$$control doom-wad:$$control doom-web:$$control doom-view:$$cardputer doom-pack:$$cardputer; do \
	  name=$${b%%:*}; node=$${b#*:}; size=$$(wc -c < build/blobs/$$name | tr -d ' '); \
	  echo "$$name ($$size B) -> $$node"; \
	  $(KUBECTL) $(if $(SERVER),--server $(SERVER)) replace --raw "/api/v1/nodes/$$node/proxy/blobs/$$name?size=$$size" -f build/blobs/$$name || exit 1; \
	done

FORCE:

# The host check against the engine (bench/drawlist_check.cpp), with the kernels of
# src/runtime/pod_raster.cpp built alone; needs the repository's wasm3 (cmake --build build --target wasm3)
build/drawlist_check: bench/drawlist_check.cpp view/drawlist.c view/drawlist.h $(REPO)/src/runtime/pod_raster.cpp $(REPO)/build/libwasm3.a
	@mkdir -p build/view
	cc -std=c11 -O2 -c view/drawlist.c -o build/view/drawlist_host.o
	$(BENCH_CXX) $(WASM3_DEFS) -DK32_RASTER_KERNELS_ONLY -o $@ $< build/view/drawlist_host.o $(REPO)/src/runtime/pod_raster.cpp \
	    $(REPO)/src/runtime/pod_memory_pool.cpp $(REPO)/src/osal/system_posix.cpp $(REPO)/build/libwasm3.a

check: build/drawlist_check

# The split renderer against the engine, game state and pixels, at both widths and
# with the sim on the sim pack at 224 KiB (make verify TICS=10000 for the long run)
TICS ?= 2000
PYTHON ?= $(if $(wildcard $(REPO)/.venv/bin/python),$(REPO)/.venv/bin/python,python3)
WAD = build/wad/doom1-e1m1-min.wad
verify: build/drawlist_check build/doom.wasm build/doom-sim.wasm build/doom-240.wasm build/doom-sim-240.wasm build/wad/doom1-e1m1.pack build/wad/doom1-e1m1-sim.wad
	./build/drawlist_check build/doom.wasm build/doom-sim.wasm $(WAD) build/wad/doom1-e1m1.pack --tics $(TICS) --spin --state \
	    --args "-warp 1 1 -skill 3 -kb 640" --sim-args "-warp 1 1 -skill 3 -kb 224" --sim-wad build/wad/doom1-e1m1-sim.wad
	./build/drawlist_check build/doom.wasm build/doom-sim.wasm $(WAD) build/wad/doom1-e1m1.pack --tics $(TICS) --state \
	    --args "-warp 1 1 -skill 3 -kb 640" --sim-args "-warp 1 1 -skill 3 -kb 256" --sim-wad build/wad/doom1-e1m1-sim.wad
	./build/drawlist_check build/doom-240.wasm build/doom-sim-240.wasm $(WAD) build/wad/doom1-e1m1.pack --tics $(TICS) --spin --state --screenblocks 11 \
	    --args "-warp 1 1 -skill 3 -kb 640" --sim-args "-warp 1 1 -skill 3 -kb 224" --sim-wad build/wad/doom1-e1m1-sim.wad

# Two-player lockstep (port/net.c) through a simulated network: the sim pod as the
# authority, the full engine as the browser's client, jitter, loss, an outage, a
# reload and a timeout; both game states compared at every tic, with the sim at 320
# and at 240 wide (the Cardputer's) against the browser's 320 (make verify-net NET_TICS=n)
build/net_check: bench/net_check.cpp $(REPO)/build/libwasm3.a
	$(BENCH_CXX) $(WASM3_DEFS) -o $@ $< $(REPO)/src/runtime/pod_memory_pool.cpp $(REPO)/src/osal/system_posix.cpp $(REPO)/build/libwasm3.a

NET_TICS ?= 6000
verify-net: build/net_check build/doom.wasm build/doom-sim.wasm build/doom-sim-240.wasm build/wad/doom1-e1m1-sim.wad
	./build/net_check build/doom-sim.wasm build/doom.wasm build/wad/doom1-e1m1-sim.wad $(WAD) --tics $(NET_TICS)
	./build/net_check build/doom-sim-240.wasm build/doom.wasm build/wad/doom1-e1m1-sim.wad $(WAD) --tics $(NET_TICS) --screenblocks 11

build/wad/doom1-e1m1.pack: $(WAD) wad.py
	$(PYTHON) wad.py pack $< $@
build/wad/doom1-e1m1-sim.wad: $(WAD) wad.py
	$(PYTHON) wad.py simpack $< $@

$(BUILD)/engine/%.o: doomgeneric/%.c
	@mkdir -p $(BUILD)/engine
	$(CLANG) $(ENGINE_CFLAGS) -c $< -o $@

$(BUILD)/port/%.o: port/%.c port/include/k32libc.h port/net.h ../../include/runtime/k32_app_abi.h
	@mkdir -p $(BUILD)/port
	$(CLANG) $(CFLAGS) -Wall -Wno-unused-function -c $< -o $@

$(OUT): $(OBJS)
	$(RUST_LLD) $(LDFLAGS) -o $@ $(OBJS)
	@ls -la $@

# The host bench (bench/doombench.cpp) against the repository's wasm3 builds
# (cmake --build build --target wasm3 wasm3_opframes): doombench counts
# operations with the interpreter as the boards build it, doombench-fast
# times the host's own build.
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

.PHONY: all stats sim view check verify verify-net pods manifest upload bench clean FORCE
