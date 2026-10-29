# Builds the echo pod into the local image registry and copies its page into
# the static bundle "echo" (images/web/echo/), both embedded in the firmware.
RUST_LLD ?= $(shell ls $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld | head -1)
CLANG ?= clang
OUT ?= ../../images/echo.wasm
BUNDLE ?= ../../images/web/echo

CFLAGS = --target=wasm32 -O2 -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra
LDFLAGS = -flavor wasm --no-entry --export-dynamic --allow-undefined -z stack-size=8192 --initial-memory=65536 --max-memory=65536

all: $(OUT) $(BUNDLE)/index.html $(BUNDLE)/index.html.gz

build/echo.o: echo.c ../../include/runtime/k32_app_abi.h
	@mkdir -p build
	$(CLANG) $(CFLAGS) -c echo.c -o $@

$(OUT): build/echo.o
	$(RUST_LLD) $(LDFLAGS) -o $@ build/echo.o
	@ls -la $@

$(BUNDLE)/index.html: web/index.html
	@mkdir -p $(BUNDLE)
	cp web/index.html $@

# Both variants are served: gzip to clients that accept it, plain to the rest
$(BUNDLE)/index.html.gz: web/index.html
	@mkdir -p $(BUNDLE)
	gzip -9 -n -c web/index.html > $@

clean:
	rm -rf build $(OUT)

.PHONY: all clean
