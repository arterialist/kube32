"""`kube32 new`: a new app project in C or Rust, ready for `kube32 build`."""
import os
import re
import shutil

from . import common, is_release, version
from .common import EXIT_INPUT, EXIT_OK, EXIT_USAGE, err_exit, shared_file

SDK_GIT = "https://github.com/arterialist/kube32"
_NAME_RE = re.compile(r"^[a-z][a-z0-9-]{0,62}$")

C_MAKEFILE = """\
# Builds @NAME@.wasm from the .c files here: kube32 build (or make), then kube32 check build/@NAME@.wasm.
NAME := @NAME@
CLANG ?= clang
# wasm-ld from LLVM's lld, else rust-lld from a rustup toolchain
WASM_LD ?= $(firstword $(shell command -v wasm-ld 2>/dev/null) $(wildcard $(HOME)/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld))
LD_FLAVOR := $(if $(filter rust-lld,$(notdir $(WASM_LD))),-flavor wasm)

# Linear memory in bytes, a multiple of 64 KiB (initial = maximum), and the
# shadow stack for C locals inside it. The pod's memory limit must cover MEMORY.
MEMORY ?= 65536
STACK ?= 16384

# -Oz: on boards without PSRAM each instruction of a running call holds a native frame (docs: Writing pods)
CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory -Wall -Wextra -Iinclude
LDFLAGS = --no-entry --export-dynamic --allow-undefined -z stack-size=$(STACK) --initial-memory=$(MEMORY) --max-memory=$(MEMORY)

SOURCES := $(wildcard *.c)
OBJECTS := $(SOURCES:%.c=build/%.o)

all: build/$(NAME).wasm

build/%.o: %.c include/runtime/k32_app_abi.h
\t@mkdir -p build
\t$(CLANG) $(CFLAGS) -c $< -o $@

build/$(NAME).wasm: $(OBJECTS)
\t@test -n "$(WASM_LD)" || { echo "no wasm linker: install lld (wasm-ld) or Rust with rustup (rust-lld)" >&2; exit 1; }
\t$(WASM_LD) $(LD_FLAVOR) $(LDFLAGS) -o $@ $(OBJECTS)

clean:
\trm -rf build

.PHONY: all clean
"""

C_MAIN = """\
/* @NAME@: a request pod. The node calls k32_handle for each HTTP request
 * routed to the pod and streams the response the pod writes. */
#include "runtime/k32_app_abi.h"

#define LIT(s) (s), (uint32_t)(sizeof(s) - 1)

static unsigned char request_buf[K32_MAX_REQUEST_BYTES + 512];
static uint32_t requests;

K32_EXPORT("k32_init") int32_t k32_init(void) {
    k32_log(LIT("@NAME@: started"));
    return 0;
}

/* The node copies each request block here before it calls k32_handle. */
K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof request_buf ? (uint32_t)(uintptr_t)request_buf : 0;
}

/* The block starts with the request line, "GET /path?query\\n", then the
 * headers, an empty line and the body. */
K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* req = (const char*)(uintptr_t)ptr;
    uint32_t line = 0;
    while (line < len && req[line] != '\\n') line++;

    char num[10], tmp[10];
    uint32_t v = ++requests, n = 0, t = 0;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (t) num[n++] = tmp[--t];

    k32_resp_begin(200, LIT("text/plain"));
    k32_resp_write(LIT("@NAME@: request "));
    k32_resp_write(num, n);
    k32_resp_write(LIT(", "));
    k32_resp_write(req, line);
    k32_resp_write(LIT("\\n"));
    k32_resp_end();
    return 200;
}
"""

RUST_CARGO = """\
[package]
name = "@NAME@"
version = "0.1.0"
edition = "2021"
publish = false

[lib]
crate-type = ["cdylib"]

[dependencies]
@SDK@

# Small modules: nodes interpret them with wasm3
[profile.release]
opt-level = "z"
lto = true
codegen-units = 1
panic = "abort"
strip = true
debug = false

[profile.dev]
panic = "abort"
"""

RUST_CONFIG = """\
# Link flags for `cargo build --release --target wasm32-unknown-unknown`:
# a 16 KiB shadow stack in one 64 KiB page of linear memory. kube32 build
# passes its own (--memory, --stack).
[target.wasm32-unknown-unknown]
rustflags = [
    "-C", "link-arg=-zstack-size=16384",
    "-C", "link-arg=--initial-memory=65536",
    "-C", "link-arg=--max-memory=65536",
]
"""


def sdk_dependency(sdk_path=None, release=None):
    """The kube32-sdk line of Cargo.toml: the crate of this release, a local checkout, or the
    repository for a development build."""
    if sdk_path:
        return 'kube32-sdk = { path = "%s" }' % os.path.abspath(sdk_path).replace("\\", "/")
    release = release or version()
    if is_release(release):
        return 'kube32-sdk = "%s"' % release
    return ('# kube32 %s is a development build: the SDK from the repository. A release\n'
            '# depends on its crate, kube32-sdk = "X.Y.Z".\n'
            'kube32-sdk = { git = "%s" }' % (release, SDK_GIT))


def write(path, text, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write(text)
    os.chmod(path, mode)


def new_c(d, name):
    write(os.path.join(d, "Makefile"), C_MAKEFILE.replace("@NAME@", name))
    write(os.path.join(d, "main.c"), C_MAIN.replace("@NAME@", name))
    header = os.path.join(d, "include", "runtime", "k32_app_abi.h")
    os.makedirs(os.path.dirname(header), exist_ok=True)
    shutil.copyfile(shared_file("include/runtime/k32_app_abi.h"), header)
    write(os.path.join(d, ".gitignore"), "/build/\n")
    return ["Makefile", "main.c", "include/runtime/k32_app_abi.h", ".gitignore"]


def new_rust(d, name, sdk_path=None):
    write(os.path.join(d, "Cargo.toml"), RUST_CARGO.replace("@NAME@", name).replace("@SDK@", sdk_dependency(sdk_path)))
    write(os.path.join(d, ".cargo", "config.toml"), RUST_CONFIG)
    os.makedirs(os.path.join(d, "src"), exist_ok=True)
    shutil.copyfile(shared_file("sdk/rust/examples/counter/src/lib.rs"), os.path.join(d, "src", "lib.rs"))
    write(os.path.join(d, ".gitignore"), "/target/\n/build/\n")
    return ["Cargo.toml", ".cargo/config.toml", "src/lib.rs", ".gitignore"]


def cmd_new(args):
    d = os.path.abspath(args.path)
    name = os.path.basename(d.rstrip(os.sep))
    if not _NAME_RE.match(name) or name.endswith("-"):
        return err_exit("invalid app name '%s' (a lowercase letter, then lowercase letters, digits and '-', "
                        "at most 63 characters): it names the module, the crate and the pod" % name, EXIT_USAGE)
    if os.path.exists(d) and (not os.path.isdir(d) or os.listdir(d)):
        return err_exit("%s exists and is not an empty directory" % args.path, EXIT_INPUT)
    if args.sdk_path and args.lang != "rust":
        return err_exit("--sdk-path is for --lang rust", EXIT_USAGE)
    if args.sdk_path and not os.path.isfile(os.path.join(args.sdk_path, "Cargo.toml")):
        return err_exit("%s has no Cargo.toml: --sdk-path takes the kube32-sdk crate's directory "
                        "(sdk/rust/kube32-sdk in a kube32 checkout)" % args.sdk_path, EXIT_USAGE)
    try:
        files = new_c(d, name) if args.lang == "c" else new_rust(d, name, args.sdk_path)
    except OSError as e:
        return err_exit(str(e), EXIT_INPUT)
    rel = os.path.relpath(d)
    shown = rel if not rel.startswith("..") else d
    print("created %s (%s): %s" % (shown, "C" if args.lang == "c" else "Rust", ", ".join(files)))
    print("next: %s build %s" % (common.PROG, shown))
    print("      %s deploy %s" % (common.PROG, os.path.join(shown, "build", name + ".wasm")))
    return EXIT_OK
