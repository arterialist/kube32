# Builds the lockstep lane into images/sd/lockstep.wasm and the fault button
# into images/sd/lockstep-injector.wasm (card or inline images, not embedded
# in the firmware), and build/lockstep.yaml, build/lockstep-injector.yaml:
# examples/lockstep.yaml and examples/lockstep-injector.yaml with the modules
# inline.
#
#   make                                   modules, checks and manifests
#   kubectl --context kube32 apply -f apps/lockstep/build/lockstep.yaml
#
# After linking, float_ops.py checks that the lane holds no float instruction
# outside + - * /, neg, compares and the i32 conversions, and wasm_inspect.py
# checks the memory layout against the pods' 16 KiB limit.
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
PYTHON ?= python3
OUT ?= ../../images/sd/lockstep.wasm
INJECTOR ?= ../../images/sd/lockstep-injector.wasm
MEM_CAP    ?= 16384
STACK_SIZE ?= 2048

CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --allow-undefined --stack-first -z stack-size=$(STACK_SIZE) --initial-memory=65536 --max-memory=65536

all: $(OUT) $(INJECTOR) build/lockstep.yaml build/lockstep-injector.yaml

build/%.o: %.c pendulum.h ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c $< -o $@

# build/lockstep.named.wasm keeps the name section for float_ops.py
$(OUT): build/lockstep.o float_ops.py
	@mkdir -p $(dir $(OUT))
	$(RUST_LLD) $(LDFLAGS) -o build/lockstep.named.wasm build/lockstep.o
	$(RUST_LLD) $(LDFLAGS) --strip-all -Map=build/lockstep.map -o $@ build/lockstep.o
	$(PYTHON) float_ops.py build/lockstep.named.wasm
	$(PYTHON) ../tessera/common/wasm_inspect.py $@ --map build/lockstep.map --cap $(MEM_CAP) --stack $(STACK_SIZE)

$(INJECTOR): build/injector.o
	@mkdir -p $(dir $(INJECTOR))
	$(RUST_LLD) $(LDFLAGS) --strip-all -Map=build/injector.map -o $@ build/injector.o
	$(PYTHON) ../tessera/common/wasm_inspect.py $@ --map build/injector.map --cap 8192 --stack $(STACK_SIZE)

build/lockstep.yaml: ../../examples/lockstep.yaml $(OUT)
	sed 's|"@lockstep.wasm"|"'"$$(base64 < $(OUT) | tr -d '\n')"'"|' ../../examples/lockstep.yaml > $@
	@echo "wrote $@"

build/lockstep-injector.yaml: ../../examples/lockstep-injector.yaml $(INJECTOR)
	sed 's|"@lockstep-injector.wasm"|"'"$$(base64 < $(INJECTOR) | tr -d '\n')"'"|' ../../examples/lockstep-injector.yaml > $@
	@echo "wrote $@"

clean:
	rm -rf build $(OUT) $(INJECTOR)

.PHONY: all clean
