# Builds the voter pod into images/sd/voter.wasm (a card or inline image, not
# embedded in the firmware) and build/voter.yaml, examples/voter.yaml with the
# module inline.
#
#   make                                   module and manifest
#   kubectl --context kube32 apply -f apps/voter/build/voter.yaml
#
# page.html goes into the module as build/page.h, and native_runs.h holds the
# recorded native runs (native/runs.sh writes it). The module declares three
# pages and the pod's memory limit caps linear memory at MEM_CAP: the stack,
# the screen's frame and the static data must stay below it (checked after
# linking).
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
PYTHON ?= python3
OUT ?= ../../images/sd/voter.wasm
MEM_CAP    ?= 196608
STACK_SIZE ?= 8192

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --allow-undefined --strip-all --stack-first -z stack-size=$(STACK_SIZE) --initial-memory=196608 --max-memory=196608 -Map=build/voter.map

all: $(OUT) build/voter.yaml

build/page.h: page.html
	@mkdir -p build
	$(PYTHON) -c "import json,sys; print('static const char page_html[] = ' + json.dumps(open(sys.argv[1]).read()) + ';')" page.html > $@

build/voter.o: voter.c build/page.h native_runs.h ../lockstep/pendulum.h ../../include/runtime/k32_app_abi.h
	$(CLANG) $(CFLAGS) -c voter.c -o $@

$(OUT): build/voter.o
	@mkdir -p $(dir $(OUT))
	$(RUST_LLD) $(LDFLAGS) -o $@ build/voter.o
	$(PYTHON) ../tessera/common/wasm_inspect.py $@ --map build/voter.map --cap $(MEM_CAP) --stack $(STACK_SIZE)

build/voter.yaml: ../../examples/voter.yaml $(OUT)
	sed 's|"@voter.wasm"|"'"$$(base64 < $(OUT) | tr -d '\n')"'"|' ../../examples/voter.yaml > $@
	@echo "wrote $@"

clean:
	rm -rf build $(OUT)

.PHONY: all clean
