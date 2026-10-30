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
ECHO_SIZE = "{:,}".format(os.path.getsize(ECHO))
TESSERA_API = os.path.join(REPO, "images", "tessera-api.wasm")
TESSERA_HUD = os.path.join(REPO, "images", "tessera-hud.wasm")
I2CSCAN = os.path.join(REPO, "images", "i2cscan.wasm")
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
        self.assertIn("images/echo.wasm: %s bytes" % ECHO_SIZE, out)
        self.assertIn("kind: request pod (k32_handle)", out)
        self.assertIn("exports (5): k32_alloc, k32_event, k32_handle, k32_init, memory [memory]", out)
        self.assertIn("imports (9): k32_env_get, k32_event_emit, k32_log, k32_resp_asset, k32_resp_begin, k32_resp_end, k32_resp_header, "
                      "k32_resp_write, k32_time_ms", out)
        self.assertIn("memory: 1 page = 65,536 bytes (64Ki), max 1 page", out)
        self.assertIn("features beyond MVP: none", out)
        self.assertIn("hardware: ipc  (manifest resources: hardware.kube32.io/ipc)", out)

    @unittest.skipUnless(os.path.isfile(I2CSCAN), "images/i2cscan.wasm not built")
    def test_app_main_pod(self):
        rc, out, _ = run_cli("check", I2CSCAN)
        self.assertEqual(rc, 0)
        self.assertIn("kind: app_main pod (app_main runs on the pod thread)", out)

    @unittest.skipUnless(os.path.isfile(TESSERA_HUD), "images/tessera-hud.wasm not built")
    def test_event_pod(self):
        rc, out, _ = run_cli("check", TESSERA_HUD)
        self.assertEqual(rc, 0)
        self.assertIn("kind: event pod (no k32_handle; k32_init/k32_tick/k32_event)", out)
        self.assertIn("hardware: ipc, display, keyboard, speaker", out)

    @unittest.skipUnless(os.path.isfile(SDNOTE), "images/sd/sdnote.wasm not built")
    def test_sd_pod(self):
        rc, out, _ = run_cli("check", SDNOTE)
        self.assertEqual(rc, 0)
        self.assertIn("hardware: sd  (manifest resources: hardware.kube32.io/sd)", out)

    @unittest.skipUnless(os.path.isfile(TESSERA_API), "images/tessera-api.wasm not built")
    def test_memory_limit(self):
        with open(TESSERA_API, "rb") as f:
            initial = cli.parse_module(f.read()).memories[0][0]
        need = cli.format_quantity(initial * 65536)
        rc, out, _ = run_cli("check", TESSERA_API, "--memory", "64Ki")
        self.assertEqual(rc, 3)
        self.assertIn("error: memory: the module declares %s (%d pages) of linear memory but the limit is 64Ki" % (need, initial), out)
        self.assertEqual(out.strip().splitlines()[-1], "FAILED (1 error)")
        rc, _, _ = run_cli("check", TESSERA_API, "--memory", need)
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
    verbs = [a for a in args if a in ("get", "create", "delete", "replace")]
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
    if verb == "replace":
        body = open(args[args.index("-f") + 1], "rb").read()
        with open(log, "a") as f:
            f.write(json.dumps({"put": args[args.index("--raw") + 1], "size": len(body)}) + "\\n")
        if state == "pushfail":
            sys.stderr.write('Error from server (ServiceUnavailable): node w1 reports no kubelet endpoint\\n')
            sys.exit(1)
        print(json.dumps({"path": "x", "bytes": len(body), "ms": 3}))
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
            (("--name", "x", "--hardware", "lasers"), "error: unknown hardware 'lasers' (expected display, keyboard, speaker, ipc, sd, service-call or none)\n"),
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

    @unittest.skipUnless(os.path.isfile(TESSERA_API), "images/tessera-api.wasm not built")
    def test_memory_too_small_and_psram(self):
        with open(TESSERA_API, "rb") as f:
            need = cli.format_quantity(cli.parse_module(f.read()).memories[0][0] * 65536)
        rc, out, err = run_cli("deploy", TESSERA_API, "--name", "api", "--dry-run")
        self.assertEqual(rc, 3)
        self.assertIn("but the limit is 64Ki", out)
        self.assertIn("hint: pass --memory %s, or --psram %s on a node with PSRAM" % (need, need), err)
        rc, out, _ = run_cli("deploy", TESSERA_API, "--name", "api", "--memory", "48Ki", "--psram", need, "--dry-run")
        self.assertEqual(rc, 0)
        self.assertIn("          memory: 48Ki\n          esp32.io/psram: %s\n" % need, out)

    def test_create_when_absent(self):
        rc, out, err = self.deploy("absent", ECHO, "--name", "echo-x", "--context", "sim", "--kubeconfig", "/tmp/kc")
        self.assertEqual(rc, 0, err)
        self.assertEqual(out, "pod/x created\ndeployed default/echo-x: %s bytes inline, 64Ki linear memory, hardware: ipc\n" % ECHO_SIZE)
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
        self.assertEqual(out.splitlines()[:2], ["embedded %s (%s bytes)" % (dest, ECHO_SIZE), "image: wasm://embedded/echo-copy:v1"])
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
        self.assertEqual(out, "copied %s (%s bytes)\nimage: wasm://sd/echo\n" % (dest, ECHO_SIZE))
        with open(ECHO, "rb") as a, open(dest, "rb") as b:
            self.assertEqual(a.read(), b.read())
        rc, out, _ = run_cli("sd-push", ECHO, "--to", card)
        self.assertEqual(out, "replaced %s (%s bytes, was %s)\nimage: wasm://sd/echo\n" % (dest, ECHO_SIZE, ECHO_SIZE))
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
        self.assertIn("pass --to <card mount point> for a card in this machine, or --node <node>", err)
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


class TestPush(TempDirCase):
    def setUp(self):
        super().setUp()
        self.kubectl = self.write("kubectl", FAKE_KUBECTL)
        os.chmod(self.kubectl, os.stat(self.kubectl).st_mode | stat.S_IXUSR)
        self.log = os.path.join(self.tmp, "calls.jsonl")

    def run_with(self, state, *args):
        env = dict(os.environ, FAKE_KUBECTL_STATE=state, FAKE_KUBECTL_LOG=self.log)
        return run_cli(*args, "--kubectl", self.kubectl, "--context", "c1", env=env)

    def puts(self):
        with open(self.log) as f:
            return [json.loads(line) for line in f if '"put"' in line]

    def test_push(self):
        data = self.write("site.css", b"body{}")
        rc, out, err = self.run_with("ok", "push", data, "--node", "w1", "--path", "images/web/site.css")
        self.assertEqual(rc, 0, err)
        self.assertRegex(out, r"^pushed 6 bytes to w1:kube32/images/web/site.css in \d+\.\d s\n$")
        self.assertEqual(self.puts(), [{"put": "/api/v1/nodes/w1/proxy/storage/images/web/site.css", "size": 6}])

    def test_push_default_path_and_errors(self):
        rc, out, _ = self.run_with("ok", "push", ECHO, "--node", "w1")
        self.assertEqual(rc, 0)
        self.assertEqual(self.puts()[-1]["put"], "/api/v1/nodes/w1/proxy/storage/images/echo.wasm")
        rc, _, err = self.run_with("ok", "push", ECHO, "--node", "w1", "--path", "../etc/x")
        self.assertEqual(rc, 2)
        self.assertIn("invalid --path", err)
        rc, _, err = self.run_with("pushfail", "push", ECHO, "--node", "w1")
        self.assertEqual(rc, 4)
        self.assertIn("reports no kubelet endpoint", err)

    def test_push_directory(self):
        d = os.path.join(self.tmp, "db")
        self.write("db/a.bin", b"123")
        self.write("db/sub/deep/b.bin", b"12345")
        rc, out, err = self.run_with("ok", "push", d, "--node", "w1", "--path", "volumes/db")
        self.assertEqual(rc, 0, err)
        self.assertEqual([p["put"] for p in self.puts()], [
            "/api/v1/nodes/w1/proxy/storage/volumes/db/a.bin",
            "/api/v1/nodes/w1/proxy/storage/volumes/db/sub/deep/b.bin"])
        lines = out.splitlines()
        self.assertEqual(len(lines), 3)
        self.assertIn("w1:kube32/volumes/db/sub/deep/b.bin", lines[1])
        self.assertEqual(lines[2], "pushed 2 files, 8 bytes to w1:kube32/volumes/db")

    def test_push_directory_default_path_and_errors(self):
        self.write("db/a.bin", b"1")
        d = os.path.join(self.tmp, "db") + "/"
        rc, _, err = self.run_with("ok", "push", d, "--node", "w1")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.puts()[-1]["put"], "/api/v1/nodes/w1/proxy/storage/images/a.bin")
        os.makedirs(os.path.join(self.tmp, "empty"))
        rc, _, err = self.run_with("ok", "push", os.path.join(self.tmp, "empty"), "--node", "w1")
        self.assertEqual(rc, 1)
        self.assertIn("contains no files", err)
        self.write("db/b.bin", b"2")
        rc, out, err = self.run_with("pushfail", "push", d, "--node", "w1")
        self.assertEqual(rc, 4)
        self.assertEqual(len(self.puts()), 2)          # the first failure ends the run
        self.assertNotIn("pushed 2 files", out)

    def test_sd_push_over_the_network(self):
        rc, out, err = self.run_with("ok", "sd-push", ECHO, "--node", "w1", "--tag", "v2")
        self.assertEqual(rc, 0, err)
        self.assertTrue(out.endswith("image: wasm://sd/echo:v2\n"))
        self.assertEqual(self.puts(), [{"put": "/api/v1/nodes/w1/proxy/storage/images/echo-v2.wasm", "size": os.path.getsize(ECHO)}])


# ---------------------------------------------------------------------------
# firmware
# ---------------------------------------------------------------------------
# A kubectl that answers the control plane's firmware API from a state file:
# the staged image (bytes in <state>.img), the FirmwareRollout and the nodes
FAKE_FIRMWARE_KUBECTL = textwrap.dedent('''\
    #!/usr/bin/env python3
    import hashlib, json, os, sys, urllib.parse
    path_state = os.environ["FAKE_FW_STATE"]
    st = json.load(open(path_state))
    args = sys.argv[1:]
    verb = [a for a in args if a in ("get", "create", "delete", "replace")][0]
    raw = args[args.index("--raw") + 1]
    body = open(args[args.index("-f") + 1], "rb").read() if "-f" in args else b""
    with open(os.environ["FAKE_FW_LOG"], "a") as f:
        f.write(json.dumps({"verb": verb, "path": raw, "size": len(body), "body": body.decode("latin-1") if verb == "create" else ""}) + "\\n")
    url = urllib.parse.urlsplit(raw)
    q = dict(urllib.parse.parse_qsl(url.query))
    img_path = path_state + ".img"

    def save():
        json.dump(st, open(path_state, "w"))

    def status():
        return {"kind": "FirmwareImage", "status": st["staged"]}

    def fail(msg):
        sys.stderr.write(msg + "\\n")
        save()
        sys.exit(1)

    if url.path == "/apis/kube32.io/v1/firmware" and verb == "get":
        print(json.dumps(status()))
    elif url.path == "/apis/kube32.io/v1/firmware/image" and verb == "replace":
        s = st["staged"]
        offset, size, sha = int(q["offset"]), int(q["size"]), q["sha256"]
        if offset == 0:
            open(img_path, "wb").close()
            s.update({"phase": "Receiving", "sha256": sha, "size": size, "received": 0, "version": "", "message": ""})
        elif s["phase"] != "Receiving" or s["sha256"] != sha or offset != s["received"]:
            fail("Error from server (Conflict): the upload continues at offset %d" % s.get("received", 0))
        if st.get("break_at") == offset:
            # The connection breaks half way through the range
            del st["break_at"]
            body = body[:len(body) // 2]
            with open(img_path, "ab") as f:
                f.write(body)
            s["received"] = offset + len(body)
            fail("error: unexpected EOF")
        with open(img_path, "ab") as f:
            f.write(body)
        s["received"] = offset + len(body)
        if s["received"] == size:
            data = open(img_path, "rb").read()
            if hashlib.sha256(data).hexdigest() == sha:
                s["phase"] = "Staged"
                at = data.find(b"kube32-version:")
                s["version"] = data[at + 15:data.find(b"\\0", at)].decode() if at >= 0 else ""
            else:
                s["phase"] = "Failed"
                s["message"] = "the image's SHA-256 does not match"
                fail("Error from server (Invalid): " + s["message"])
        save()
        print(json.dumps(status()))
    elif url.path == "/apis/kube32.io/v1/firmwarerollouts/firmware":
        if verb == "get":
            if not st.get("rollout"):
                fail('Error from server (NotFound): firmwarerollouts.kube32.io "firmware" not found')
            ro = st["rollout"]
            # Each read moves the rollout one step on
            steps = st.get("steps", [])
            if steps:
                ro["status"] = steps.pop(0)
            save()
            print(json.dumps(ro))
        elif verb == "delete":
            st["rollout"] = None
            save()
            print("{}")
    elif url.path == "/apis/kube32.io/v1/firmwarerollouts" and verb == "create":
        st["rollout"] = json.loads(body)
        save()
        print(json.dumps(st["rollout"]))
    elif url.path == "/api/v1/nodes" and verb == "get":
        print(json.dumps({"items": st.get("nodes", [])}))
    else:
        fail("Error from server (NotFound): the server could not find the requested resource")
''')


def firmware_image(version, size=100000, seed=1):
    """An image as esptool writes it: magic byte, the version marker, the
    SHA-256 of the rest in the last 32 bytes."""
    body = bytearray(hashlib.sha256(b"seed%d" % seed).digest() * (size // 32 + 1))[:size - 32]
    body[0] = 0xE9
    marker = b"kube32-version:" + version.encode() + b"\0"
    body[4096:4096 + len(marker)] = marker
    return bytes(body) + hashlib.sha256(bytes(body)).digest()


def fw_node(name, sha, version, role="worker", target="", state="Confirmed", reason="UpToDate", message=""):
    labels = {"node-role.kubernetes.io/" + role: ""}
    ann = {"kube32.io/firmware-version": version, "kube32.io/firmware-sha256": sha, "kube32.io/firmware-slot": "ota_0",
           "kube32.io/firmware-state": state}
    if target:
        ann["kube32.io/firmware"] = target
    return {"metadata": {"name": name, "labels": labels, "annotations": ann},
            "status": {"conditions": [{"type": "FirmwareUpdate", "status": "False", "reason": reason, "message": message}]}}


class TestFirmware(TempDirCase):
    def setUp(self):
        super().setUp()
        self.kubectl = self.write("kubectl", FAKE_FIRMWARE_KUBECTL)
        os.chmod(self.kubectl, os.stat(self.kubectl).st_mode | stat.S_IXUSR)
        self.state = os.path.join(self.tmp, "state.json")
        self.log = os.path.join(self.tmp, "calls.jsonl")
        self.set_state()

    def set_state(self, **kw):
        st = {"staged": {"phase": "Empty", "sha256": "", "size": 0, "received": 0, "version": "", "slot": "ota_1",
                         "slotSize": 3407872, "message": ""}, "rollout": None}
        st.update(kw)
        with open(self.state, "w") as f:
            json.dump(st, f)

    def get_state(self):
        with open(self.state) as f:
            return json.load(f)

    def fw(self, *args):
        env = dict(os.environ, FAKE_FW_STATE=self.state, FAKE_FW_LOG=self.log)
        return run_cli("firmware", *args, "--kubectl", self.kubectl, env=env)

    def calls(self, verb=None):
        if not os.path.isfile(self.log):
            return []
        with open(self.log) as f:
            rows = [json.loads(line) for line in f]
        return [r for r in rows if verb is None or r["verb"] == verb]

    def test_image_info(self):
        img = firmware_image("v1.2.3-4-gabcdef0")
        p = self.write("fw.bin", img)
        version, sha = cli.firmware_image_info(img)
        self.assertEqual(version, "v1.2.3-4-gabcdef0")
        self.assertEqual(sha, hashlib.sha256(img).hexdigest())
        rc, out, _ = run_cli("firmware", "info", p)
        self.assertEqual(rc, 0)
        self.assertIn("version: v1.2.3-4-gabcdef0", out)
        self.assertIn(sha, out)
        # Not an image; an image whose appended SHA-256 does not match
        with self.assertRaises(ValueError):
            cli.firmware_image_info(b"\x00" * 100)
        bad = bytearray(img)
        bad[5000] ^= 1
        rc, _, err = run_cli("firmware", "info", self.write("bad.bin", bytes(bad)))
        self.assertEqual(rc, 1)
        self.assertIn("does not end with its own SHA-256", err)

    def test_push_in_ranges(self):
        img = firmware_image("v2", size=100000)
        rc, out, err = self.fw("push", self.write("fw.bin", img), "--chunk", "16Ki")
        self.assertEqual(rc, 0, err)
        puts = self.calls("replace")
        self.assertEqual(len(puts), 7)
        self.assertTrue(all(p["size"] <= 16384 for p in puts))
        self.assertIn("offset=16384&size=100000&sha256=" + hashlib.sha256(img).hexdigest(), puts[1]["path"])
        with open(self.state + ".img", "rb") as f:
            self.assertEqual(f.read(), img)
        self.assertEqual(self.get_state()["staged"]["phase"], "Staged")
        self.assertIn("staged v2 (%s)" % hashlib.sha256(img).hexdigest()[:12], out)
        # Pushing it again does nothing
        rc, out, _ = self.fw("push", self.write("fw.bin", img))
        self.assertEqual(rc, 0)
        self.assertIn("already staged", out)
        self.assertEqual(len(self.calls("replace")), 7)

    def test_push_resumes(self):
        img = firmware_image("v2", size=100000)
        sha = hashlib.sha256(img).hexdigest()
        # A range that breaks off half way: the upload goes on where the
        # control plane says it stands
        self.set_state(break_at=32768)
        rc, out, err = self.fw("push", self.write("fw.bin", img), "--chunk", "16Ki")
        self.assertEqual(rc, 0, err)
        self.assertIn("retrying", err)
        offsets = [int(re.search(r"offset=(\d+)", p["path"]).group(1)) for p in self.calls("replace")]
        self.assertEqual(offsets[:4], [0, 16384, 32768, 32768 + 8192])
        with open(self.state + ".img", "rb") as f:
            self.assertEqual(f.read(), img)
        # An upload a previous run left half done continues
        st = self.get_state()["staged"]
        st.update({"phase": "Receiving", "received": 49152})
        self.set_state(staged=st)
        with open(self.state + ".img", "wb") as f:
            f.write(img[:49152])
        os.unlink(self.log)
        rc, out, err = self.fw("push", self.write("fw.bin", img), "--chunk", "16Ki")
        self.assertEqual(rc, 0, err)
        self.assertIn("resuming at byte 49,152", out)
        self.assertIn("offset=49152&size=100000&sha256=" + sha, self.calls("replace")[0]["path"])

    def test_push_errors(self):
        img = firmware_image("v2", size=100000)
        st = self.get_state()["staged"]
        st["slotSize"] = 65536
        self.set_state(staged=st)
        rc, _, err = self.fw("push", self.write("fw.bin", img))
        self.assertEqual(rc, 1)
        self.assertIn("update slot", err)
        rc, _, err = self.fw("push", self.write("x.bin", b"not firmware" * 10))
        self.assertEqual(rc, 1)
        self.assertIn("not an ESP32 firmware image", err)
        rc, _, err = self.fw("push", os.path.join(self.tmp, "missing.bin"))
        self.assertEqual(rc, 1)

    def staged(self, img, version):
        sha = hashlib.sha256(img).hexdigest()
        st = self.get_state()["staged"]
        st.update({"phase": "Staged", "sha256": sha, "size": len(img), "received": len(img), "version": version})
        return st, sha

    def test_rollout(self):
        rc, _, err = self.fw("rollout", "--no-wait")
        self.assertEqual(rc, 1)
        self.assertIn("no image is staged", err)
        st, sha = self.staged(firmware_image("v2"), "v2")
        steps = [{"phase": "Progressing", "updatedNodes": 0, "totalNodes": 2, "node": "w1", "message": "updating node w1"},
                 {"phase": "Progressing", "updatedNodes": 1, "totalNodes": 2, "node": "cp", "message": "updating node cp"},
                 {"phase": "Complete", "updatedNodes": 2, "totalNodes": 2, "message": "2 node(s) run v2"}]
        nodes = [fw_node("cp", sha, "v2", role="control-plane", target=sha), fw_node("w1", sha, "v2", target=sha)]
        self.set_state(staged=st, steps=steps, nodes=nodes)
        rc, out, err = self.fw("rollout", "--interval", "0", "--node-timeout", "600")
        self.assertEqual(rc, 0, err)
        manifest = json.loads(self.calls("create")[0]["body"])
        self.assertEqual(manifest["kind"], "FirmwareRollout")
        self.assertEqual(manifest["spec"], {"sha256": sha, "nodeTimeoutSeconds": 600})
        self.assertIn("updating node w1", out)
        self.assertIn("Complete 2/2 nodes", out)
        # Workers before the control plane in the table
        table = out[out.index("NODE"):].splitlines()
        self.assertTrue(table[1].startswith("w1 ") and table[2].startswith("cp "))
        # Another rollout while one runs is refused; a finished one is replaced
        st2 = self.get_state()
        st2["rollout"]["status"] = {"phase": "Progressing"}
        self.set_state(staged=st, rollout=st2["rollout"])
        rc, _, err = self.fw("rollout", "--no-wait")
        self.assertEqual(rc, 5)
        self.assertIn("in progress", err)
        st2["rollout"]["status"] = {"phase": "Complete"}
        self.set_state(staged=st, rollout=st2["rollout"])
        rc, _, err = self.fw("rollout", "--no-wait")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.calls("delete")[-1]["path"], "/apis/kube32.io/v1/firmwarerollouts/firmware")
        rc, _, err = self.fw("rollout", "--no-wait", "--sha256", "0" * 64)
        self.assertEqual(rc, 1)

    def test_rollout_failure_status_and_abort(self):
        st, sha = self.staged(firmware_image("v3"), "v3")
        steps = [{"phase": "Failed", "updatedNodes": 0, "totalNodes": 2, "node": "w1",
                  "message": "node w1: image %s reset before it confirmed itself" % sha[:12]}]
        nodes = [fw_node("w1", "a" * 64, "v2", target=sha, reason="RolledBack", message="image reset before it confirmed itself")]
        self.set_state(staged=st, steps=steps, nodes=nodes)
        rc, _, err = self.fw("rollout", "--interval", "0")
        self.assertEqual(rc, 6)
        self.assertIn("rollout failed: node w1", err)
        rc, out, err = self.fw("status")
        self.assertEqual(rc, 0, err)
        self.assertIn("staged:  Staged  v3 (%s)" % sha[:12], out)
        self.assertIn("rollout: Failed", out)
        self.assertRegex(out, r"w1\s+worker\s+v2\s+aaaaaaaaaaaa\s+ota_0\s+Confirmed\s+%s\s+RolledBack: image reset" % sha[:12])
        rc, out, _ = self.fw("abort")
        self.assertEqual(rc, 0)
        self.assertIsNone(self.get_state()["rollout"])
        rc, out, _ = self.fw("abort")
        self.assertIn("no rollout", out)


if __name__ == "__main__":
    unittest.main()
