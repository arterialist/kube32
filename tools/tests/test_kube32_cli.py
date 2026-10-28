"""Tests for tools/kube32-cli.

Run from the repository root:

    python3 -m unittest discover -s tools/tests -v

Fixtures are the modules in images/ (images/echo.wasm is tracked; the others
are used when present) plus small modules assembled here byte by byte. The
deploy tests run against a fake kubectl that records its arguments; the
build tests that need clang, rust-lld, make or cargo skip themselves when the
tool is missing.
"""
import base64
import glob
import hashlib
import importlib.machinery
import importlib.util
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(TESTS_DIR))
CLI = os.path.join(REPO, "tools", "kube32-cli")
ECHO = os.path.join(REPO, "images", "echo.wasm")
TESSERA_CORE = os.path.join(REPO, "images", "tessera-core.wasm")
TESSERA_HUD = os.path.join(REPO, "images", "tessera-hud.wasm")
SDNOTE = os.path.join(REPO, "images", "sd", "sdnote.wasm")


def load_cli():
    loader = importlib.machinery.SourceFileLoader("kube32_cli", CLI)
    spec = importlib.util.spec_from_loader("kube32_cli", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


cli = load_cli()


def run_cli(*args, env=None, cwd=None):
    proc = subprocess.run([sys.executable, CLI] + [str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, env=env, cwd=cwd or REPO)
    return proc.returncode, proc.stdout, proc.stderr


# ---------------------------------------------------------------------------
# A minimal wasm assembler for test modules
# ---------------------------------------------------------------------------
I32, I64 = 0x7F, 0x7E


def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def section(sid, payload):
    return bytes([sid]) + leb(len(payload)) + payload


def vec(items):
    return leb(len(items)) + b"".join(items)


def name(s):
    return leb(len(s)) + s.encode()


def wasm_module(imports=(), funcs=(), memory=(1, 1), export_memory=True, import_kinds=(), globals_=()):
    """imports: [(module, name, (params, results))]; funcs: [(export name or None, (params, results), body)].
    body is the instruction bytes, ending with 0x0b; functions have no locals.
    globals_: encoded global entries (valtype, mutability, init expression)."""
    types = []

    def tidx(sig):
        key = (bytes(sig[0]), bytes(sig[1]))
        if key not in types:
            types.append(key)
        return types.index(key)

    imp = [name(m) + name(n) + b"\x00" + leb(tidx(sig)) for (m, n, sig) in imports]
    imp += list(import_kinds)
    fdecl = [leb(tidx(sig)) for (_n, sig, _b) in funcs]
    exports = []
    for i, (n, _sig, _b) in enumerate(funcs):
        if n:
            exports.append(name(n) + b"\x00" + leb(len(imports) + i))
    if memory is not None and export_memory:
        exports.append(name("memory") + b"\x02" + leb(0))
    out = b"\0asm\x01\0\0\0"
    out += section(1, vec([b"\x60" + vec([bytes([p]) for p in t[0]]) + vec([bytes([r]) for r in t[1]]) for t in types]))
    if imp:
        out += section(2, vec(imp))
    if funcs:
        out += section(3, vec(fdecl))
    if memory is not None:
        lim = b"\x01" + leb(memory[0]) + leb(memory[1]) if memory[1] is not None else b"\x00" + leb(memory[0])
        out += section(5, vec([lim]))
    if globals_:
        out += section(6, vec(list(globals_)))
    if exports:
        out += section(7, vec(exports))
    if funcs:
        bodies = []
        for (_n, _sig, body) in funcs:
            code = b"\x00" + body
            bodies.append(leb(len(code)) + code)
        out += section(10, vec(bodies))
    return out


ALLOC = ("k32_alloc", ([I32], [I32]), b"\x41\x80\x08\x0b")            # i32.const 1024
HANDLE = ("k32_handle", ([I32, I32], [I32]), b"\x41\xc8\x01\x0b")     # i32.const 200
APP_MAIN = ("app_main", ([], []), b"\x0b")


def request_pod(**kw):
    return wasm_module(funcs=[ALLOC, HANDLE], **kw)


class TempDirCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="kube32-cli-test-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write(self, rel, data):
        p = os.path.join(self.tmp, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb" if isinstance(data, bytes) else "w") as f:
            f.write(data)
        return p


# ---------------------------------------------------------------------------
# Units
# ---------------------------------------------------------------------------
class TestUnits(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(cli.parse_quantity("64Ki"), 65536)
        self.assertEqual(cli.parse_quantity("1280Ki"), 1310720)
        self.assertEqual(cli.parse_quantity("2Mi"), 2 * 1024 * 1024)
        self.assertEqual(cli.parse_quantity("65536"), 65536)
        self.assertEqual(cli.parse_quantity("64K"), 64000)
        with self.assertRaises(ValueError):
            cli.parse_quantity("lots")

    def test_format(self):
        self.assertEqual(cli.format_quantity(65536), "64Ki")
        self.assertEqual(cli.format_quantity(2 * 1024 * 1024), "2Mi")
        self.assertEqual(cli.format_quantity(1000), "1000")

    def test_abi_tables_match_the_header(self):
        # Every import the header declares is in the table, and nothing else.
        with open(os.path.join(REPO, "include", "runtime", "k32_app_abi.h")) as f:
            header = f.read()
        declared = set(re.findall(r'K32_IMPORT\("([a-z0-9_]+)"\)', header))
        tabled = set(cli.KNOWN_IMPORTS)
        self.assertEqual(declared - tabled, set())
        self.assertEqual(tabled - declared - set(cli.PERIPHERAL_IMPORTS) - {"esp32_get_time_us"}, set())

    def test_abi_signatures_match_the_host(self):
        # The signatures are the ones the host links with m3_LinkRawFunction.
        linked = {}
        for src in ("src/runtime/app_pod.cpp", "src/runtime/host_functions.cpp"):
            with open(os.path.join(REPO, src)) as f:
                for n, sig in re.findall(r'm3_LinkRawFunction\(module, "env", "([a-z0-9_]+)",\s*"([^"]+)"', f.read()):
                    linked[n] = sig
        self.assertEqual(linked, cli.KNOWN_IMPORTS)


# ---------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------
class TestCheckFixtures(unittest.TestCase):
    def test_every_image_passes(self):
        files = sorted(glob.glob(os.path.join(REPO, "images", "*.wasm")) + glob.glob(os.path.join(REPO, "images", "sd", "*.wasm")))
        self.assertIn(ECHO, files)
        for f in files:
            rc, out, err = run_cli("check", f)
            self.assertEqual(rc, 0, "%s:\n%s%s" % (f, out, err))
            self.assertEqual(out.strip().splitlines()[-1], "ok")

    def test_echo_report(self):
        rc, out, _ = run_cli("check", "images/echo.wasm")
        self.assertEqual(rc, 0)
        self.assertIn("images/echo.wasm: 2,035 bytes", out)
        self.assertIn("kind: request pod (k32_handle)", out)
        self.assertIn("exports (5): k32_alloc, k32_event, k32_handle, k32_init, memory [memory]", out)
        self.assertIn("imports (8): k32_env_get, k32_event_emit, k32_log, k32_resp_begin, k32_resp_end, k32_resp_header, "
                      "k32_resp_write, k32_time_ms", out)
        self.assertIn("memory: 1 page = 65,536 bytes (64Ki), max 1 page", out)
        self.assertIn("features beyond MVP: none", out)
        self.assertIn("hardware: ipc  (manifest resources: hardware.kube32.io/ipc)", out)

    @unittest.skipUnless(os.path.isfile(TESSERA_HUD), "images/tessera-hud.wasm not built")
    def test_app_main_pod(self):
        rc, out, _ = run_cli("check", TESSERA_HUD)
        self.assertEqual(rc, 0)
        self.assertIn("kind: app_main pod (app_main runs on the pod thread)", out)
        self.assertIn("hardware: ipc, display, keyboard, speaker", out)

    @unittest.skipUnless(os.path.isfile(SDNOTE), "images/sd/sdnote.wasm not built")
    def test_sd_pod(self):
        rc, out, _ = run_cli("check", SDNOTE)
        self.assertEqual(rc, 0)
        self.assertIn("hardware: sd  (manifest resources: hardware.kube32.io/sd)", out)

    @unittest.skipUnless(os.path.isfile(TESSERA_CORE), "images/tessera-core.wasm not built")
    def test_memory_limit(self):
        with open(TESSERA_CORE, "rb") as f:
            initial = cli.parse_module(f.read()).memories[0][0]
        need = cli.format_quantity(initial * 65536)
        rc, out, _ = run_cli("check", TESSERA_CORE, "--memory", "64Ki")
        self.assertEqual(rc, 3)
        self.assertIn("error: memory: the module declares %s (%d pages) of linear memory but the limit is 64Ki" % (need, initial), out)
        self.assertEqual(out.strip().splitlines()[-1], "FAILED (1 error)")
        rc, _, _ = run_cli("check", TESSERA_CORE, "--memory", need)
        self.assertEqual(rc, 0)


class TestCheckErrors(TempDirCase):
    def check(self, data, *extra):
        p = self.write("m.wasm", data)
        return run_cli("check", p, *extra)

    def test_missing_file(self):
        rc, out, err = run_cli("check", "no/such.wasm")
        self.assertEqual((rc, out), (1, ""))
        self.assertEqual(err, "error: no such file: no/such.wasm\n")

    def test_not_wasm(self):
        rc, out, _ = self.check(b"#!/bin/sh\necho hi\n")
        self.assertEqual(rc, 3)
        self.assertIn("error: malformed module: not a WebAssembly module (missing \\0asm magic)", out)

    def test_truncated(self):
        with open(ECHO, "rb") as f:
            data = f.read()
        rc, out, _ = self.check(data[:len(data) // 2])
        self.assertEqual(rc, 3)
        self.assertIn("error: malformed module:", out)

    def test_bad_quantity(self):
        rc, out, err = self.check(request_pod(), "--memory", "lots")
        self.assertEqual((rc, out), (2, ""))
        self.assertIn("error: invalid quantity 'lots'", err)

    def test_minimal_request_pod(self):
        rc, out, _ = self.check(request_pod())
        self.assertEqual(rc, 0, out)
        self.assertIn("kind: request pod (k32_handle)", out)
        self.assertIn("imports (0)", out)
        self.assertIn("hardware: none", out)

    def test_unknown_import(self):
        m = wasm_module(imports=[("env", "k32_nope", ([], []))], funcs=[ALLOC, HANDLE])
        rc, out, _ = self.check(m)
        self.assertEqual(rc, 3)
        self.assertIn("error: unknown import env.k32_nope (the host does not provide it)", out)

    def test_wrong_import_signature(self):
        m = wasm_module(imports=[("env", "k32_log", ([I32, I32], [I32]))], funcs=[ALLOC, HANDLE])
        rc, out, _ = self.check(m)
        self.assertEqual(rc, 3)
        self.assertIn("error: import env.k32_log has signature i(ii), the host links v(ii)", out)

    def test_import_from_other_module(self):
        m = wasm_module(imports=[("wasi_snapshot_preview1", "fd_write", ([I32] * 4, [I32]))], funcs=[ALLOC, HANDLE])
        rc, out, _ = self.check(m)
        self.assertEqual(rc, 3)
        self.assertIn('error: import wasi_snapshot_preview1.fd_write: the host links functions into the "env" module only', out)

    def test_imported_memory(self):
        imported_mem = name("env") + name("memory") + b"\x02\x00\x01"
        m = wasm_module(funcs=[ALLOC, HANDLE], memory=None, import_kinds=[imported_mem])
        rc, out, _ = self.check(m)
        self.assertEqual(rc, 3)
        self.assertIn("error: import env.memory is a memory; the host provides only functions", out)
        self.assertIn("error: the module imports its memory; the host only runs modules that define their own", out)

    def test_no_memory(self):
        rc, out, _ = self.check(request_pod(memory=None))
        self.assertEqual(rc, 3)
        self.assertIn("error: the module defines no linear memory", out)

    def test_no_entry_points(self):
        m = wasm_module(funcs=[("helper", ([], []), b"\x0b")])
        rc, out, _ = self.check(m)
        self.assertEqual(rc, 3)
        self.assertIn("error: the module exports neither k32_handle (request pod) nor app_main, _start or main (app_main pod); "
                      "exports: helper, memory", out)

    def test_handle_without_alloc(self):
        rc, out, _ = self.check(wasm_module(funcs=[HANDLE]))
        self.assertEqual(rc, 3)
        self.assertIn("error: k32_alloc is missing: the host copies requests and events into memory it obtains from k32_alloc", out)

    def test_handle_wrong_signature(self):
        m = wasm_module(funcs=[ALLOC, ("k32_handle", ([I32], [I32]), b"\x41\x00\x0b")])
        rc, out, _ = self.check(m)
        self.assertEqual(rc, 3)
        self.assertIn("error: export k32_handle has signature i(i), the host calls i(ii)", out)

    def test_app_main_with_arguments(self):
        m = wasm_module(funcs=[("app_main", ([I32], []), b"\x0b")])
        rc, out, _ = self.check(m)
        self.assertEqual(rc, 3)
        self.assertIn("error: export app_main has signature v(i); the host calls it with no arguments (v() or i())", out)

    def test_app_main_pod(self):
        rc, out, _ = self.check(wasm_module(funcs=[APP_MAIN]))
        self.assertEqual(rc, 0, out)
        self.assertIn("kind: app_main pod (app_main runs on the pod thread)", out)

    def test_memory_without_maximum_warns(self):
        rc, out, _ = self.check(request_pod(memory=(1, None)))
        self.assertEqual(rc, 0, out)
        self.assertIn("warning: memory has no maximum", out)

    def test_memory_limit_option(self):
        m = request_pod(memory=(2, 2))
        rc, out, _ = self.check(m, "--memory", "64Ki")
        self.assertEqual(rc, 3)
        self.assertIn("error: memory: the module declares 128Ki (2 pages) of linear memory but the limit is 64Ki", out)
        rc, out, _ = self.check(m, "--memory", "128Ki")
        self.assertEqual(rc, 0, out)

    # Opcodes: what this wasm3 build compiles (third_party/wasm3 with the
    # CMakeLists.txt / platformio.ini defines) and what it does not.
    def opcode_case(self, body):
        return self.check(wasm_module(funcs=[("app_main", ([], []), body)]))

    def test_simd_rejected(self):
        rc, out, _ = self.opcode_case(b"\xfd\x0c" + bytes(16) + b"\x1a\x0b")      # v128.const; drop
        self.assertEqual(rc, 3)
        self.assertIn("error: function 0 uses SIMD (0xFD prefix), which this wasm3 build does not run", out)

    def test_exception_handling_rejected(self):
        rc, out, _ = self.opcode_case(b"\x08\x00\x0b")                            # throw 0
        self.assertEqual(rc, 3)
        self.assertIn("error: function 0 uses throw (exception handling, d_m3HasExceptionHandling=0), which this wasm3 build does not run", out)

    def test_typed_refs_rejected(self):
        rc, out, _ = self.opcode_case(b"\xd0\x70\x14\x00\x0b")                    # ref.null func; call_ref 0
        self.assertEqual(rc, 3)
        self.assertIn("call_ref (typed function references, d_m3HasTypedRefs=0)", out)

    def test_supported_extensions_reported(self):
        cases = [
            (b"\x41\x7f\xc0\x1a\x0b", "sign-ext (i32.extend8_s x1)"),
            (b"\x41\x00\x41\x08\x41\x04\xfc\x0a\x00\x00\x0b", "bulk-memory (memory.copy x1)"),
            (b"\x12\x00\x0b", "tail-call (return_call x1)"),
            (b"\x41\x00\xfe\x10\x02\x00\x1a\x0b", "atomics (0xFE 0x10 x1)"),
            (b"\x42\x01\x42\x02\x42\x03\x42\x04\xfc\x13\x1a\x1a\x0b", "wide-arithmetic (i64.add128 x1)"),
            (b"\x43\x00\x00\x80\x3f\xfc\x00\x1a\x0b", "nontrapping-fptoint (i32.trunc_sat_f32_s x1)"),
        ]
        for body, note in cases:
            rc, out, _ = self.opcode_case(body)
            self.assertEqual(rc, 0, out)
            self.assertIn("features beyond MVP: " + note, out)

    def test_extended_const_global(self):
        m = wasm_module(funcs=[("app_main", ([], []), b"\x23\x00\x1a\x0b")],         # global.get 0; drop
                        globals_=[b"\x7f\x00\x41\x01\x41\x02\x6a\x0b"])                # i32 global = 1 + 2
        rc, out, _ = self.check(m)
        self.assertEqual(rc, 0, out)
        self.assertIn("features beyond MVP: extended-const (i32.add x1)", out)

    def test_floats_are_mvp(self):
        rc, out, _ = self.opcode_case(b"\x43\x00\x00\x80\x3f\x43\x00\x00\x80\x3f\x92\x1a\x0b")   # f32.add
        self.assertEqual(rc, 0, out)
        self.assertIn("features beyond MVP: none", out)


# ---------------------------------------------------------------------------
# deploy
# ---------------------------------------------------------------------------
FAKE_KUBECTL = textwrap.dedent('''\
    #!/usr/bin/env python3
    import json, os, sys
    state = os.environ["FAKE_KUBECTL_STATE"]
    log = os.environ["FAKE_KUBECTL_LOG"]
    args = sys.argv[1:]
    stdin = sys.stdin.read() if "-f" in args else ""
    with open(log, "a") as f:
        f.write(json.dumps({"args": args, "stdin": stdin}) + "\\n")
    verbs = [a for a in args if a in ("get", "create", "delete")]
    verb = verbs[0] if verbs else ""
    if state == "down":
        sys.stderr.write("The connection to the server 127.0.0.1:6443 was refused - did you specify the right host or port?\\n")
        sys.exit(1)
    if verb == "get":
        if state in ("present",):
            sys.stdout.write("0123456789abcdef")
            sys.exit(0)
        sys.stderr.write('Error from server (NotFound): pods "x" not found\\n')
        sys.exit(1)
    if verb == "delete":
        print('pod "x" deleted')
        sys.exit(0)
    if verb == "create":
        if state == "createfail":
            sys.stderr.write("Error from server (Forbidden): nope\\n")
            sys.exit(1)
        print("pod/x created")
        sys.exit(0)
    sys.exit(2)
''')


class TestDeploy(TempDirCase):
    def setUp(self):
        super().setUp()
        self.kubectl = self.write("kubectl", FAKE_KUBECTL)
        os.chmod(self.kubectl, os.stat(self.kubectl).st_mode | stat.S_IXUSR)
        self.log = os.path.join(self.tmp, "calls.jsonl")

    def deploy(self, state, *args):
        env = dict(os.environ, FAKE_KUBECTL_STATE=state, FAKE_KUBECTL_LOG=self.log)
        return run_cli("deploy", *args, "--kubectl", self.kubectl, env=env)

    def calls(self):
        if not os.path.isfile(self.log):
            return []
        with open(self.log) as f:
            return [json.loads(line) for line in f]

    @staticmethod
    def annotation(manifest, key):
        m = re.search(r"^    %s: (\".*\")$" % re.escape(key), manifest, re.M)
        return json.loads(m.group(1)) if m else None

    def test_dry_run_manifest(self):
        rc, out, err = run_cli("deploy", ECHO, "--name", "echo-test", "--node", "esp32-node-02", "--env", "GREETING=hi there",
                               "--env", 'Q=a "quoted": value', "--dry-run")
        self.assertEqual(rc, 0, err)
        with open(ECHO, "rb") as f:
            data = f.read()
        digest = hashlib.sha256(data).hexdigest()
        self.assertEqual(base64.b64decode(self.annotation(out, "kube32.io/wasm-base64")), data)
        self.assertEqual(self.annotation(out, "kube32.io/wasm-sha256"), digest)
        self.assertEqual(self.annotation(out, "kube32.io/wasm-size"), str(len(data)))
        for line in [
            "  name: echo-test",
            "    kubernetes.io/hostname: esp32-node-02",
            '      image: "wasm://inline/echo-test-%s"' % digest[:12],
            "        - containerPort: 7000",
            '          value: "hi there"',
            '          value: "a \\"quoted\\": value"',
            "          memory: 64Ki",
            '          hardware.kube32.io/ipc: "1"',
            '          add: ["CHANNEL_IPC"]',
        ]:
            self.assertIn(line + "\n", out)
        self.assertEqual(err, "")
        self.assertEqual(self.calls(), [])

    def test_name_defaults_to_stem_and_image_key_carries_the_digest(self):
        # images/echo.wasm is embedded in the firmware under the key "echo"; the
        # inline image's key must differ or the node would run the embedded one.
        rc, out, err = run_cli("deploy", ECHO, "--dry-run")
        self.assertEqual((rc, err), (0, ""))
        self.assertIn("  name: echo\n", out)
        image = re.search(r'^      image: "(.*)"$', out, re.M).group(1)
        with open(ECHO, "rb") as f:
            self.assertEqual(image, "wasm://inline/echo-" + hashlib.sha256(f.read()).hexdigest()[:12])
        key = image.rsplit("/", 1)[1].split(":")[0].split("@")[0]      # ImageRegistry::normalize_key
        self.assertNotEqual(key, "echo")

    def test_usage_errors(self):
        cases = [
            (("--name", "Bad_Name"), "error: invalid pod name 'Bad_Name' (lowercase letters, digits and '-', at most 63 characters; pass --name)\n"),
            (("--name", "x", "--env", "NOEQUALS"), "error: --env expects KEY=VALUE, got 'NOEQUALS'\n"),
            (("--name", "x", "--env", "1BAD=v"), "error: invalid environment variable name '1BAD'\n"),
            (("--name", "x", "--hardware", "lasers"), "error: unknown hardware 'lasers' (expected display, keyboard, speaker, ipc, sd or none)\n"),
            (("--name", "x", "--memory", "lots"), "error: invalid quantity 'lots' (expected e.g. 64Ki, 1280Ki, 2Mi, 65536)\n"),
            (("--name", "x", "--image", "docker.io/nginx"), "error: --image must be a wasm:// reference such as wasm://sd/x or wasm://embedded/x:v1\n"),
            (("--name", "x", "--sd-claim", "data"), "error: --sd-claim needs the sd hardware (--hardware ...,sd, or a module that imports k32_fs_*)\n"),
        ]
        for extra, msg in cases:
            rc, out, err = self.deploy("absent", ECHO, *extra)
            self.assertEqual((rc, out, err), (2, "", msg), extra)
        self.assertEqual(self.calls(), [])

    def test_missing_file(self):
        rc, _, err = self.deploy("absent", "nope.wasm")
        self.assertEqual((rc, err), (1, "error: no such file: nope.wasm\n"))

    def test_hardware_none_warns(self):
        rc, out, err = run_cli("deploy", ECHO, "--name", "x", "--hardware", "none", "--dry-run")
        self.assertEqual(rc, 0)
        self.assertIn("warning: the module imports ipc functions but --hardware does not grant 'ipc': those calls will return K32_ERR_PERM", err)
        self.assertNotIn("hardware.kube32.io", out)
        self.assertNotIn("securityContext", out)

    def test_explicit_hardware_list(self):
        rc, out, _ = run_cli("deploy", ECHO, "--name", "x", "--hardware", "speaker,ipc,tft-display", "--dry-run")
        self.assertEqual(rc, 0)
        self.assertIn('          hardware.kube32.io/ipc: "1"\n          hardware.kube32.io/tft-display: "1"\n'
                      '          hardware.kube32.io/speaker: "1"\n', out)
        self.assertIn('          add: ["CHANNEL_IPC", "DISPLAY", "SPEAKER"]\n', out)

    @unittest.skipUnless(os.path.isfile(SDNOTE), "images/sd/sdnote.wasm not built")
    def test_sd_volume(self):
        rc, out, _ = run_cli("deploy", SDNOTE, "--dry-run")
        self.assertEqual(rc, 0)
        self.assertIn("  volumes:\n    - name: data\n      persistentVolumeClaim:\n        claimName: sdnote\n", out)
        self.assertIn('          hardware.kube32.io/sd: "1"\n', out)
        self.assertIn('          add: ["SD"]\n', out)
        rc, out, _ = run_cli("deploy", SDNOTE, "--sd-claim", "notes", "--dry-run")
        self.assertIn("        claimName: notes\n", out)
        rc, out, err = run_cli("deploy", SDNOTE, "--sd-claim", "Bad_Claim", "--dry-run")
        self.assertEqual((rc, out), (2, ""))
        self.assertIn("error: invalid sd volume claim 'Bad_Claim'", err)

    @unittest.skipUnless(os.path.isfile(SDNOTE), "images/sd/sdnote.wasm not built")
    def test_image_reference_drops_the_annotation(self):
        rc, out, _ = run_cli("deploy", SDNOTE, "--image", "wasm://sd/sdnote", "--dry-run")
        self.assertEqual(rc, 0)
        self.assertIsNone(self.annotation(out, "kube32.io/wasm-base64"))
        self.assertIsNotNone(self.annotation(out, "kube32.io/wasm-sha256"))
        self.assertIn('      image: "wasm://sd/sdnote"\n', out)

    @unittest.skipUnless(os.path.isfile(TESSERA_CORE), "images/tessera-core.wasm not built")
    def test_memory_too_small_and_psram(self):
        with open(TESSERA_CORE, "rb") as f:
            need = cli.format_quantity(cli.parse_module(f.read()).memories[0][0] * 65536)
        rc, out, err = run_cli("deploy", TESSERA_CORE, "--name", "core", "--dry-run")
        self.assertEqual(rc, 3)
        self.assertIn("but the limit is 64Ki", out)
        self.assertIn("hint: pass --memory %s, or --psram %s on a node with PSRAM" % (need, need), err)
        rc, out, _ = run_cli("deploy", TESSERA_CORE, "--name", "core", "--memory", "48Ki", "--psram", need, "--dry-run")
        self.assertEqual(rc, 0)
        self.assertIn("          memory: 48Ki\n          esp32.io/psram: %s\n" % need, out)

    def test_create_when_absent(self):
        rc, out, err = self.deploy("absent", ECHO, "--name", "echo-x", "--context", "sim", "--kubeconfig", "/tmp/kc")
        self.assertEqual(rc, 0, err)
        self.assertEqual(out, "pod/x created\ndeployed default/echo-x: 2,035 bytes inline, 64Ki linear memory, hardware: ipc\n")
        calls = self.calls()
        self.assertEqual([c["args"] for c in calls], [
            ["--kubeconfig", "/tmp/kc", "--context", "sim", "get", "pod", "echo-x", "-n", "default", "-o",
             "jsonpath={.metadata.annotations.kube32\\.io/wasm-sha256}"],
            ["--kubeconfig", "/tmp/kc", "--context", "sim", "create", "--validate=false", "-f", "-"],
        ])
        with open(ECHO, "rb") as f:
            self.assertEqual(base64.b64decode(self.annotation(calls[1]["stdin"], "kube32.io/wasm-base64")), f.read())

    def test_existing_pod_needs_replace(self):
        rc, out, err = self.deploy("present", ECHO, "--name", "echo-x")
        self.assertEqual((rc, out), (5, ""))
        self.assertIn("error: pod default/echo-x already exists (module sha256 0123456789ab, this file ", err)
        self.assertIn("pass --replace to delete and recreate it", err)
        self.assertEqual(len(self.calls()), 1)

    def test_replace(self):
        rc, out, err = self.deploy("present", ECHO, "--name", "echo-x", "--namespace", "apps", "--replace", "--timeout", "30")
        self.assertEqual(rc, 0, err)
        self.assertEqual([c["args"][:2] for c in self.calls()], [["get", "pod"], ["delete", "pod"], ["create", "--validate=false"]])
        self.assertEqual(self.calls()[1]["args"], ["delete", "pod", "echo-x", "-n", "apps", "--wait=true", "--timeout=30s"])
        self.assertIn('pod "x" deleted\npod/x created\ndeployed apps/echo-x:', out)

    def test_api_unreachable(self):
        rc, out, err = self.deploy("down", ECHO, "--name", "echo-x")
        self.assertEqual((rc, out), (4, ""))
        self.assertIn("The connection to the server 127.0.0.1:6443 was refused", err)
        self.assertTrue(err.endswith("error: kubectl get pod failed (exit 1)\n"))

    def test_create_fails(self):
        rc, out, err = self.deploy("createfail", ECHO, "--name", "echo-x")
        self.assertEqual((rc, out), (4, ""))
        self.assertTrue(err.endswith("Error from server (Forbidden): nope\nerror: kubectl create failed (exit 1)\n"))

    def test_kubectl_missing(self):
        rc, out, err = run_cli("deploy", ECHO, "--name", "x", "--kubectl", os.path.join(self.tmp, "no-kubectl"))
        self.assertEqual((rc, out), (1, ""))
        self.assertEqual(err, "error: %s not found in PATH\n" % os.path.join(self.tmp, "no-kubectl"))

    def test_bad_module_is_not_deployed(self):
        bad = self.write("bad.wasm", wasm_module(imports=[("env", "k32_nope", ([], []))], funcs=[ALLOC, HANDLE]))
        rc, out, _ = self.deploy("absent", bad, "--name", "bad")
        self.assertEqual(rc, 3)
        self.assertIn("error: unknown import env.k32_nope", out)
        self.assertEqual(self.calls(), [])


# ---------------------------------------------------------------------------
# embed and sd-push
# ---------------------------------------------------------------------------
class TestEmbed(TempDirCase):
    def test_embed(self):
        images = os.path.join(self.tmp, "images")
        os.makedirs(images)
        rc, out, err = run_cli("embed", ECHO, "--name", "echo-copy", "--images-dir", images)
        self.assertEqual(rc, 0, err)
        dest = os.path.join(images, "echo-copy.wasm")
        with open(ECHO, "rb") as a, open(dest, "rb") as b:
            self.assertEqual(a.read(), b.read())
        self.assertEqual(out.splitlines()[:2], ["embedded %s (2,035 bytes)" % dest, "image: wasm://embedded/echo-copy:v1"])
        rc, out, err = run_cli("embed", ECHO, "--name", "echo-copy", "--images-dir", images)
        self.assertEqual((rc, out, err), (1, "", "error: %s exists; pass --force to replace it\n" % dest))
        rc, _, _ = run_cli("embed", ECHO, "--name", "echo-copy", "--images-dir", images, "--force")
        self.assertEqual(rc, 0)

    def test_errors(self):
        rc, _, err = run_cli("embed", ECHO, "--images-dir", os.path.join(self.tmp, "missing"))
        self.assertEqual((rc, err), (1, "error: images directory does not exist: %s\n" % os.path.join(self.tmp, "missing")))
        rc, _, err = run_cli("embed", ECHO, "--name", "Echo", "--images-dir", self.tmp)
        self.assertEqual(rc, 2)
        self.assertEqual(err, "error: invalid image name 'Echo' (lowercase letters, digits, '.', '_' and '-'; pass --name)\n")
        bad = self.write("bad.wasm", b"not wasm")
        rc, out, _ = run_cli("embed", bad, "--images-dir", self.tmp)
        self.assertEqual(rc, 3)
        self.assertIn("malformed module", out)
        self.assertEqual(os.listdir(self.tmp), ["bad.wasm"])


class TestSdPush(TempDirCase):
    def test_push_and_replace(self):
        card = os.path.join(self.tmp, "card")
        os.makedirs(card)
        rc, out, err = run_cli("sd-push", ECHO, "--to", card)
        self.assertEqual(rc, 0, err)
        dest = os.path.join(card, "kube32", "images", "echo.wasm")
        self.assertEqual(out, "copied %s (2,035 bytes)\nimage: wasm://sd/echo\n" % dest)
        with open(ECHO, "rb") as a, open(dest, "rb") as b:
            self.assertEqual(a.read(), b.read())
        rc, out, _ = run_cli("sd-push", ECHO, "--to", card)
        self.assertEqual(out, "replaced %s (2,035 bytes, was 2,035)\nimage: wasm://sd/echo\n" % dest)
        rc, out, _ = run_cli("sd-push", ECHO, "--to", card, "--name", "Echo_2", "--tag", "v2")
        self.assertEqual(rc, 0)
        self.assertIn("image: wasm://sd/Echo_2:v2\n", out)
        self.assertTrue(os.path.isfile(os.path.join(card, "kube32", "images", "Echo_2-v2.wasm")))
        self.assertEqual(sorted(os.listdir(os.path.join(card, "kube32", "images"))), ["Echo_2-v2.wasm", "echo.wasm"])

    def test_size_warnings(self):
        # A custom section pads a valid module past the kubelet's pull limits.
        def padded(n):
            return request_pod() + section(0, name("pad") + bytes(n))
        cases = [(200 * 1024, None),
                 (300 * 1024, "is over the 256 KiB a node without PSRAM pulls from its card"),
                 (2100 * 1024, "is over the 2 MiB the kubelet pulls from a card on any node")]
        for size, warning in cases:
            p = self.write("big.wasm", padded(size))
            rc, _, err = run_cli("sd-push", p, "--to", self.tmp)
            self.assertEqual(rc, 0)
            if warning:
                self.assertIn(warning, err)
            else:
                self.assertEqual(err, "")

    def test_errors(self):
        missing = os.path.join(self.tmp, "nocard")
        rc, out, err = run_cli("sd-push", ECHO, "--to", missing)
        self.assertEqual((rc, out), (1, ""))
        self.assertEqual(err, "error: %s is not a directory: mount the card and pass its mount point, e.g. --to /Volumes/CARD\n" % missing)
        rc, _, err = run_cli("sd-push", ECHO, "--to", self.tmp, "--name", ".hidden")
        self.assertEqual(rc, 2)
        self.assertIn("error: invalid image name '.hidden'", err)
        rc, _, err = run_cli("sd-push", ECHO, "--to", self.tmp, "--tag", "a/b")
        self.assertEqual(rc, 2)
        self.assertIn("error: invalid tag 'a/b'", err)
        rc, _, err = run_cli("sd-push", ECHO)
        self.assertEqual(rc, 2)
        self.assertIn("the following arguments are required: --to", err)
        bad = self.write("bad.wasm", request_pod(memory=None))
        rc, out, _ = run_cli("sd-push", bad, "--to", self.tmp)
        self.assertEqual(rc, 3)
        self.assertIn("error: the module defines no linear memory", out)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "kube32")))


# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------
def have_c_toolchain():
    clang = shutil.which(os.environ.get("CLANG", "clang"))
    if not clang or not cli.find_rust_lld():
        return False
    proc = subprocess.run([clang, "--target=wasm32", "-x", "c", "-c", "-", "-o", os.devnull], input="int x;", text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return proc.returncode == 0


def have_rust_wasm():
    env = cli.cargo_env()
    rustup = shutil.which("rustup", path=env["PATH"])
    if not rustup or not shutil.which("cargo", path=env["PATH"]):
        return False
    proc = subprocess.run([rustup, "target", "list", "--installed"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    return "wasm32-unknown-unknown" in proc.stdout.split()


HELLO_C = textwrap.dedent('''\
    #include "runtime/k32_app_abi.h"
    static unsigned char buf[K32_MAX_REQUEST_BYTES + 512];
    K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
        return size <= sizeof buf ? (uint32_t)(uintptr_t)buf : 0;
    }
    K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
        (void)ptr; (void)len;
        k32_resp_begin(200, "text/plain", 10);
        k32_resp_write("hi\\n", 3);
        k32_resp_end();
        return 200;
    }
''')


class TestBuild(TempDirCase):
    def test_missing_dir(self):
        rc, out, err = run_cli("build", os.path.join(self.tmp, "nope"))
        self.assertEqual((rc, out, err), (1, "", "error: no such directory: %s\n" % os.path.join(self.tmp, "nope")))

    def test_nothing_to_build(self):
        rc, _, err = run_cli("build", self.tmp)
        self.assertEqual(rc, 1)
        self.assertIn("error: cannot tell how to build", err)

    def test_bad_memory(self):
        self.write("app/a.c", HELLO_C)
        rc, _, err = run_cli("build", os.path.join(self.tmp, "app"), "--memory", "100Ki")
        self.assertEqual((rc, err), (2, "error: --memory must be a non-zero multiple of 64Ki (one wasm page), got 100Ki\n"))
        rc, _, err = run_cli("build", os.path.join(self.tmp, "app"), "--stack", "64Ki")
        self.assertEqual((rc, err), (2, "error: --stack 64Ki does not fit in --memory 64Ki\n"))

    def test_detection(self):
        for files, lang in [({"Cargo.toml": ""}, "rust"), ({"asconfig.json": "{}"}, "assemblyscript"),
                            ({"package.json": '{"devDependencies": {"assemblyscript": "^0.28.0"}}'}, "assemblyscript"),
                            ({"Makefile": ""}, "c-make"), ({"x.c": ""}, "c"), ({"README": ""}, None)]:
            d = tempfile.mkdtemp(dir=self.tmp)
            for n, body in files.items():
                with open(os.path.join(d, n), "w") as f:
                    f.write(body)
            self.assertEqual(cli.detect_language(d), lang, files)

    def test_assemblyscript_without_compiler(self):
        app = os.path.join(self.tmp, "as-app")
        self.write("as-app/asconfig.json", '{"entries": ["assembly/index.ts"]}')
        env = dict(os.environ, PATH="/usr/bin:/bin")
        rc, out, err = run_cli("build", app, env=env)
        self.assertEqual(rc, 1)
        self.assertEqual(err, "error: asc not found: run 'npm install --save-dev assemblyscript' in %s\n" % app)

    @unittest.skipUnless(have_c_toolchain(), "clang with the wasm32 target and rust-lld/wasm-ld are needed")
    def test_c_sources(self):
        app = os.path.join(self.tmp, "hello")
        self.write("hello/hello.c", HELLO_C)
        rc, out, err = run_cli("build", app, cwd=self.tmp)
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(out.splitlines()[0], "building %s (c)" % app)
        self.assertIn("hello/build/hello.wasm: ", out)
        self.assertIn("kind: request pod (k32_handle)", out)
        self.assertIn("imports (3): k32_resp_begin, k32_resp_end, k32_resp_write", out)
        self.assertEqual(sorted(os.listdir(app)), ["build", "hello.c"])   # objects go to a temporary directory
        rc, out, _ = run_cli("build", app, "--memory", "128Ki", "-o", os.path.join(self.tmp, "out", "x.wasm"), cwd=self.tmp)
        self.assertEqual(rc, 0)
        self.assertIn("memory: 2 pages = 131,072 bytes (128Ki), max 2 pages", out)

    @unittest.skipUnless(have_c_toolchain(), "clang with the wasm32 target and rust-lld/wasm-ld are needed")
    def test_c_compile_error(self):
        app = os.path.join(self.tmp, "broken")
        self.write("broken/b.c", "int f( {")
        rc, out, err = run_cli("build", app)
        self.assertEqual(rc, 1)
        self.assertIn("error: clang failed on %s (exit 1)\n" % os.path.join(app, "b.c"), err)

    @unittest.skipUnless(have_c_toolchain() and shutil.which("make"), "make, clang (wasm32) and rust-lld are needed")
    def test_makefile_project_writing_outside_its_directory(self):
        lld = cli.find_rust_lld()
        self.write("proj/app/hello.c", HELLO_C)
        self.write("proj/app/Makefile", textwrap.dedent('''\
            CLANG ?= clang
            OUT ?= ../out/hello-make.wasm
            all: $(OUT)
            build/hello.o: hello.c
            \t@mkdir -p build
            \t$(CLANG) --target=wasm32 -O2 -nostdlib -ffreestanding -I%s -c hello.c -o build/hello.o
            $(OUT): build/hello.o
            \t@mkdir -p $(dir $(OUT))
            \t%s -flavor wasm --no-entry --export-dynamic --allow-undefined -z stack-size=16384 --initial-memory=65536 --max-memory=65536 -o $(OUT) build/hello.o
        ''' % (os.path.join(REPO, "include"), lld)))
        app = os.path.join(self.tmp, "proj", "app")
        rc, out, err = run_cli("build", app)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("building %s (c-make)" % app, out)
        self.assertIn(os.path.join(self.tmp, "proj", "out", "hello-make.wasm"), out)
        rc, out, _ = run_cli("build", app)          # up to date: make does nothing, the artifact is still found
        self.assertEqual(rc, 0)
        self.assertIn("hello-make.wasm", out)

    @unittest.skipUnless(have_rust_wasm(), "cargo and the wasm32-unknown-unknown target are needed")
    def test_rust_counter_example(self):
        out_path = os.path.join(self.tmp, "counter.wasm")
        rc, out, err = run_cli("build", os.path.join(REPO, "sdk", "rust", "examples", "counter"), "-o", out_path)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("kind: request pod (k32_handle)", out)
        self.assertIn("exports (8): k32_alloc, k32_event, k32_handle, k32_init, k32_tick", out)
        self.assertIn("hardware: ipc", out)
        with open(out_path, "rb") as f:
            res = cli.check_module(out_path, f.read())
        self.assertTrue(res.ok)
        self.assertLess(res.module.size, 8192)


if __name__ == "__main__":
    unittest.main()
