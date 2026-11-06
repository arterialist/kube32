# Builds wordsketch: its three pod modules, llm-pipeline's shard and
# coordinator for the story, the four blobs, and the manifest with the
# modules inline.
#
#   make                 modules, blobs (downloads stories260K once) and manifest
#   make upload          puts the blobs on the nodes (SERVER: the API server's
#                        network address; the USB gateway takes 64 KB bodies)
#
# Blobs: wordsketch-kws (Cardputer), wordsketch-doodle, wordsketch-story and
# wordsketch-tokenizer (control plane).
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld 2>/dev/null | head -1)
WASM_LD ?= $(firstword $(shell command -v wasm-ld 2>/dev/null) $(RUST_LLD))
CLANG ?= clang
PYTHON ?= python3
LLM ?= uv run --script ../llm-pipeline/tools/llm.py
KUBECTL ?= kubectl --context kube32
SERVER ?=

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = $(if $(findstring rust-lld,$(WASM_LD)),-flavor wasm) --no-entry --allow-undefined -Map=$@.map
INSPECT = $(PYTHON) ../tessera/common/wasm_inspect.py

# shadow stack, linear memory (the pod limit caps a single page)
LISTEN_LD = --stack-first -z stack-size=1024 --initial-memory=65536 --max-memory=65536
DRAW_LD = --stack-first -z stack-size=1024 --initial-memory=196608 --max-memory=196608
STORE_LD = --stack-first -z stack-size=1024 --initial-memory=65536 --max-memory=65536

MODULES = build/wordsketch-listen.wasm build/wordsketch-draw.wasm build/wordsketch-store.wasm \
          build/llm-shard.wasm build/llm-coordinator.wasm
BLOBS = build/blobs/wordsketch-kws build/blobs/wordsketch-doodle build/blobs/wordsketch-story build/blobs/wordsketch-tokenizer

all: modules blobs manifest

modules: $(MODULES)

build/%.o: %.c blobload.h ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

build/kws_%.o: ../../models/kws/kws_%.c ../../models/kws/*.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -DKWS_MAP_MAX=5760 -DKWS_MAP_B_MAX=1920 -c $< -o $@

build/frontend.o: ../../models/kws/frontend.c ../../models/kws/frontend.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

build/doodle_infer.o: ../../models/doodle/doodle_infer.c ../../models/doodle/doodle_infer.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

build/wordsketch-listen.wasm: build/listen.o build/kws_infer.o build/frontend.o
	$(WASM_LD) $(LDFLAGS) $(LISTEN_LD) -o $@ $^
	$(INSPECT) $@ --map $@.map --stack 1024

build/wordsketch-draw.wasm: build/draw.o build/doodle_infer.o
	$(WASM_LD) $(LDFLAGS) $(DRAW_LD) -o $@ $^
	$(INSPECT) $@ --map $@.map --stack 4096

build/wordsketch-store.wasm: build/store.o
	$(WASM_LD) $(LDFLAGS) $(STORE_LD) -o $@ $^
	$(INSPECT) $@ --map $@.map --stack 1024

# the story: llm-pipeline's modules, one stage with all five layers
build/llm-%.wasm: FORCE
	$(MAKE) -C ../llm-pipeline modules
	cp ../llm-pipeline/build/llm-$*.wasm $@

blobs: $(BLOBS)

build/blobs/wordsketch-kws: ../../models/kws/kws.bin
	@mkdir -p build/blobs
	cp $< $@

build/blobs/wordsketch-doodle: ../../models/doodle/doodle-l.bin
	@mkdir -p build/blobs
	cp $< $@

build/blobs/wordsketch-tokenizer: build/blobs/wordsketch-story

build/blobs/wordsketch-story:
	@mkdir -p build/blobs
	$(LLM) build --model-dir ../llm-pipeline/build/model --split 0-4 --bits 8 --card build/story
	cp build/story/kube32/images/llm-store/shard-0.bin build/blobs/wordsketch-story
	cp build/story/kube32/images/llm-store/tokenizer.bin build/blobs/wordsketch-tokenizer

manifest: $(MODULES)
	$(LLM) manifest --template ../../examples/wordsketch.yaml --modules build --out build/wordsketch.yaml

# The blobs go to the node that runs their pods: the Cardputer (by its
# board label) and the control plane. wordsketch-listen, llm-shard and
# llm-coordinator are module blobs (wasm://blob/ images in the manifest)
upload: $(BLOBS) build/wordsketch-listen.wasm build/llm-shard.wasm build/llm-coordinator.wasm
	@cardputer=$$($(KUBECTL) get nodes -l kube32.io/board=cardputer -o name | head -1 | cut -d/ -f2); \
	control=$$($(KUBECTL) get nodes -l node-role.kubernetes.io/control-plane -o name | head -1 | cut -d/ -f2); \
	for b in wordsketch-kws:$$cardputer:build/blobs/wordsketch-kws wordsketch-doodle:$$control:build/blobs/wordsketch-doodle \
	         wordsketch-story:$$control:build/blobs/wordsketch-story wordsketch-tokenizer:$$control:build/blobs/wordsketch-tokenizer \
	         wordsketch-listen:$$cardputer:build/wordsketch-listen.wasm llm-shard:$$control:build/llm-shard.wasm \
	         llm-coordinator:$$control:build/llm-coordinator.wasm; do \
	  name=$${b%%:*}; rest=$${b#*:}; node=$${rest%%:*}; file=$${rest#*:}; size=$$(wc -c < $$file | tr -d ' '); \
	  echo "$$name ($$size B) -> $$node"; \
	  $(KUBECTL) $(if $(SERVER),--server $(SERVER)) replace --raw "/api/v1/nodes/$$node/proxy/blobs/$$name?size=$$size" -f $$file || exit 1; \
	done

clean:
	rm -rf build

FORCE:

.PHONY: all modules blobs manifest upload clean FORCE
