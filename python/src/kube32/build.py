"""`kube32 build` and `kube32 check`: compile an app directory to .wasm and check modules."""
import glob
import json
import os
import shlex
import shutil
import sys
import tempfile

from .common import (EXIT_CHECK, EXIT_INPUT, EXIT_OK, EXIT_USAGE, Log, abi_include_dir, err_exit, parse_quantity, run)
from .wasm import PAGE, check_module, format_report, read_wasm


# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------
# The flags apps/*/Makefile use for this runtime
C_CFLAGS = ["--target=wasm32", "-O2", "-nostdlib", "-ffreestanding", "-fno-exceptions",
            "-mcpu=mvp", "-msign-ext", "-mbulk-memory", "-Wall", "-Wextra"]


def find_rust_lld():
    cand = os.environ.get("RUST_LLD")
    if cand and os.path.isfile(cand):
        return cand
    for name in ("wasm-ld", "rust-lld"):
        p = shutil.which(name)
        if p:
            return p
    hits = sorted(glob.glob(os.path.join(os.path.expanduser("~"), ".rustup", "toolchains", "*", "lib", "rustlib", "*", "bin", "rust-lld")))
    return hits[0] if hits else None


def detect_language(d):
    if os.path.isfile(os.path.join(d, "Cargo.toml")):
        return "rust"
    if os.path.isfile(os.path.join(d, "asconfig.json")):
        return "assemblyscript"
    pkg = os.path.join(d, "package.json")
    if os.path.isfile(pkg):
        try:
            with open(pkg) as f:
                j = json.load(f)
            deps = {}
            deps.update(j.get("dependencies") or {})
            deps.update(j.get("devDependencies") or {})
            if "assemblyscript" in deps:
                return "assemblyscript"
        except (OSError, ValueError):
            pass
    if os.path.isfile(os.path.join(d, "Makefile")):
        return "c-make"
    if glob.glob(os.path.join(d, "*.c")):
        return "c"
    return None


def build_c_sources(d, out_path, memory_bytes, stack_bytes, log):
    clang = os.environ.get("CLANG", "clang")
    if not shutil.which(clang):
        return EXIT_INPUT, "clang not found in PATH (set CLANG=/path/to/clang)"
    lld = find_rust_lld()
    if not lld:
        return EXIT_INPUT, "no wasm linker: install a Rust toolchain with rustup (it ships rust-lld) or put wasm-ld in PATH"
    sources = sorted(glob.glob(os.path.join(d, "*.c")))
    with tempfile.TemporaryDirectory(prefix="kube32-build-") as objdir:
        objs = []
        for src in sources:
            obj = os.path.join(objdir, os.path.splitext(os.path.basename(src))[0] + ".o")
            cmd = [clang] + C_CFLAGS + ["-I" + abi_include_dir(), "-c", src, "-o", obj]
            log(" ".join(shlex.quote(c) for c in cmd))
            rc, outp = run(cmd)
            if outp.strip():
                log(outp.rstrip())
            if rc != 0:
                if not log.verbose:
                    sys.stderr.write(outp)
                return EXIT_INPUT, "clang failed on %s (exit %d)" % (src, rc)
            objs.append(obj)
        # __heap_base: where the static data and the stack end (wasm.static_memory_bytes)
        cmd = [lld, "-flavor", "wasm", "--no-entry", "--export-dynamic", "--export=__heap_base", "--allow-undefined",
               "-z", "stack-size=%d" % stack_bytes, "--initial-memory=%d" % memory_bytes, "--max-memory=%d" % memory_bytes,
               "-o", out_path] + objs
        log(" ".join(shlex.quote(c) for c in cmd))
        rc, outp = run(cmd)
        if outp.strip():
            log(outp.rstrip())
        if rc != 0:
            if not log.verbose:
                sys.stderr.write(outp)
            return EXIT_INPUT, "link failed (exit %d)" % rc
    return EXIT_OK, None


def makefile_wasm_outputs(d):
    """The .wasm files the Makefile's commands write with -o, from a dry run (make -n -B)."""
    rc, outp = run(["make", "-n", "-B", "-C", d])
    if rc != 0:
        return []
    found = []
    for line in outp.splitlines():
        try:
            words = shlex.split(line)
        except ValueError:
            continue
        for i, w in enumerate(words[:-1]):
            if w == "-o" and words[i + 1].endswith(".wasm"):
                p = os.path.normpath(os.path.join(d, words[i + 1]))
                if p not in found:
                    found.append(p)
    return found


def build_c_make(d, artifact, log):
    if not shutil.which("make"):
        return EXIT_INPUT, None, "make not found in PATH"
    candidates = [artifact] if artifact else makefile_wasm_outputs(d)
    cmd = ["make", "-C", d]
    log(" ".join(shlex.quote(c) for c in cmd))
    rc, outp = run(cmd)
    if outp.strip():
        log(outp.rstrip())
    if rc != 0:
        if not log.verbose:
            sys.stderr.write(outp)
        return EXIT_INPUT, None, "make failed (exit %d)" % rc
    existing = [p for p in candidates if os.path.isfile(p)]
    if not existing:
        return EXIT_INPUT, None, ("make finished but %s does not exist" % artifact if artifact else
                                  "make finished but no command in the Makefile writes a .wasm with -o; pass --artifact <path>")
    if len(existing) > 1:
        return EXIT_INPUT, None, "the Makefile writes several modules (%s); pass --artifact <path>" % ", ".join(existing)
    return EXIT_OK, existing[0], None


def cargo_env():
    """Prefers the rustup toolchain (~/.cargo/bin) over a distribution rustc earlier in PATH:
    rustup targets such as wasm32-unknown-unknown only exist there."""
    env = dict(os.environ)
    rustup_bin = os.path.join(os.path.expanduser("~"), ".cargo", "bin")
    if os.path.isfile(os.path.join(rustup_bin, "cargo")) and not env.get("CARGO"):
        env["PATH"] = rustup_bin + os.pathsep + env.get("PATH", "")
    return env


def build_rust(d, target, memory_bytes, stack_bytes, log):
    env = cargo_env()
    cargo = env.get("CARGO") or shutil.which("cargo", path=env["PATH"])
    if not cargo:
        return EXIT_INPUT, None, None, "cargo not found in PATH (install Rust with rustup)"
    manifest = os.path.join(d, "Cargo.toml")
    rc, outp = run([cargo, "metadata", "--no-deps", "--format-version", "1", "--manifest-path", manifest], env=env)
    if rc != 0:
        sys.stderr.write(outp)
        return EXIT_INPUT, None, None, "cargo metadata failed (exit %d)" % rc
    meta = json.loads(outp)
    pkg = next((p for p in meta.get("packages", [])
                if os.path.realpath(p.get("manifest_path", "")) == os.path.realpath(manifest)), None)
    if pkg is None:
        return EXIT_INPUT, None, None, "no package with manifest %s in cargo metadata" % manifest
    lib = next((t for t in pkg.get("targets", []) if "cdylib" in t.get("crate_types", [])), None)
    if lib is None:
        return EXIT_INPUT, None, None, "%s has no cdylib target: add [lib] crate-type = [\"cdylib\"] to Cargo.toml" % pkg["name"]
    # RUSTFLAGS replaces any target.<triple>.rustflags from .cargo/config.toml, so
    # the memory and stack sizes given here are the ones that reach the linker.
    flags = ["-C", "link-arg=-zstack-size=%d" % stack_bytes,
             "-C", "link-arg=--initial-memory=%d" % memory_bytes,
             "-C", "link-arg=--max-memory=%d" % memory_bytes]
    env["RUSTFLAGS"] = " ".join(([env["RUSTFLAGS"]] if env.get("RUSTFLAGS") else []) + flags)
    cmd = [cargo, "build", "--release", "--lib", "--target", target, "--manifest-path", manifest]
    log("RUSTFLAGS=%s %s" % (shlex.quote(env["RUSTFLAGS"]), " ".join(shlex.quote(c) for c in cmd)))
    rc, outp = run(cmd, env=env)
    if outp.strip():
        log(outp.rstrip())
    if rc != 0:
        if not log.verbose:
            sys.stderr.write(outp)
        hint = ""
        if "may not be installed" in outp or "can't find crate for `core`" in outp:
            hint = "; install the target with: rustup target add %s" % target
        return EXIT_INPUT, None, None, "cargo failed (exit %d)%s" % (rc, hint)
    produced = os.path.join(meta["target_directory"], target, "release", lib["name"].replace("-", "_") + ".wasm")
    if not os.path.isfile(produced):
        return EXIT_INPUT, None, None, "cargo finished but %s does not exist" % produced
    return EXIT_OK, produced, pkg["name"], None


# AssemblyScript: no GC (stub runtime), no assertion code, abort() traps instead
# of importing env.abort, which the host does not provide.
ASC_FLAGS = ["--runtime", "stub", "--noAssert", "--use", "abort=", "-O3z"]


def build_assemblyscript(d, out_path, memory_bytes, log):
    asc = os.path.join(d, "node_modules", ".bin", "asc")
    if not os.path.isfile(asc):
        asc = shutil.which("asc")
    if not asc:
        return EXIT_INPUT, "asc not found: run 'npm install --save-dev assemblyscript' in %s" % d
    entries = []
    cfg = os.path.join(d, "asconfig.json")
    if os.path.isfile(cfg):
        try:
            with open(cfg) as f:
                entries = json.load(f).get("entries") or []
        except (OSError, ValueError) as e:
            return EXIT_INPUT, "cannot read %s: %s" % (cfg, e)
    if not entries:
        entries = ["assembly/index.ts"]
    pages = memory_bytes // PAGE
    cmd = [asc] + entries + ["-o", out_path] + ASC_FLAGS + ["--initialMemory", str(pages), "--maximumMemory", str(pages)]
    log(" ".join(shlex.quote(c) for c in cmd))
    rc, outp = run(cmd, cwd=d)
    if outp.strip():
        log(outp.rstrip())
    if rc != 0:
        if not log.verbose:
            sys.stderr.write(outp)
        return EXIT_INPUT, "asc failed (exit %d)" % rc
    return EXIT_OK, None


def cmd_build(args):
    d = os.path.abspath(args.dir)
    if not os.path.isdir(d):
        return err_exit("no such directory: %s" % args.dir, EXIT_INPUT)
    lang = args.lang or detect_language(d)
    if lang is None:
        return err_exit("cannot tell how to build %s: no Cargo.toml, asconfig.json, package.json with assemblyscript, Makefile or *.c" % d, EXIT_INPUT)
    try:
        memory_bytes = parse_quantity(args.memory)
        stack_bytes = parse_quantity(args.stack)
    except ValueError as e:
        return err_exit(str(e), EXIT_USAGE)
    if memory_bytes == 0 or memory_bytes % PAGE:
        return err_exit("--memory must be a non-zero multiple of 64Ki (one wasm page), got %s" % args.memory, EXIT_USAGE)
    if stack_bytes >= memory_bytes:
        return err_exit("--stack %s does not fit in --memory %s" % (args.stack, args.memory), EXIT_USAGE)
    name = args.name or os.path.basename(d.rstrip(os.sep))
    log = Log(args.verbose)
    print("building %s (%s)" % (d, lang))

    if lang == "c":
        out_path = os.path.abspath(args.out) if args.out else os.path.join(d, "build", name + ".wasm")
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        rc, err = build_c_sources(d, out_path, memory_bytes, stack_bytes, log)
        if rc != EXIT_OK:
            return err_exit(err, rc)
        produced = out_path
    elif lang == "c-make":
        rc, produced, err = build_c_make(d, os.path.abspath(args.artifact) if args.artifact else None, log)
        if rc != EXIT_OK:
            return err_exit(err, rc)
        if args.out:
            out_path = os.path.abspath(args.out)
            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            if os.path.realpath(out_path) != os.path.realpath(produced):
                shutil.copyfile(produced, out_path)
            produced = out_path
    elif lang == "rust":
        rc, built, pkg_name, err = build_rust(d, args.target, memory_bytes, stack_bytes, log)
        if rc != EXIT_OK:
            return err_exit(err, rc)
        name = args.name or pkg_name
        out_path = os.path.abspath(args.out) if args.out else os.path.join(d, "build", name + ".wasm")
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        shutil.copyfile(built, out_path)
        produced = out_path
    elif lang == "assemblyscript":
        out_path = os.path.abspath(args.out) if args.out else os.path.join(d, "build", name + ".wasm")
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        rc, err = build_assemblyscript(d, out_path, memory_bytes, log)
        if rc != EXIT_OK:
            return err_exit(err, rc)
        produced = out_path
    else:
        return err_exit("unknown language '%s' (expected c, rust or assemblyscript)" % lang, EXIT_USAGE)

    rel = os.path.relpath(produced)
    res = check_module(produced if rel.startswith("..") else rel, read_wasm(produced))
    print(format_report(res))
    return EXIT_OK if res.ok else EXIT_CHECK


# ---------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------
def cmd_check(args):
    limit = None
    if args.memory:
        try:
            limit = parse_quantity(args.memory)
        except ValueError as e:
            return err_exit(str(e), EXIT_USAGE)
    try:
        data = read_wasm(args.file)
    except FileNotFoundError as e:
        return err_exit(str(e), EXIT_INPUT)
    res = check_module(args.file, data, memory_limit=limit)
    print(format_report(res))
    return EXIT_OK if res.ok else EXIT_CHECK
