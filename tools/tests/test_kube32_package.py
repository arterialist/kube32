"""Tests for what the kube32 package adds to the CLI: the build hook, `new`,
port detection and `version`.

Run from the repository root:

    python3 -m unittest discover -s tools/tests -v

kubectl is replaced by a script that records its arguments. Nothing opens a
real serial port.
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import types
import unittest
from unittest import mock

from kube32_under_test import INSTALLED, PACKAGE_DIR, REPO, cli_command

import kube32
from kube32 import common, ports, scaffold

HEADER = os.path.join(REPO, "include", "runtime", "k32_app_abi.h")
COUNTER = os.path.join(REPO, "sdk", "rust", "examples", "counter", "src", "lib.rs")

try:
    import serial  # noqa: F401
    HAVE_SERIAL = hasattr(os, "openpty")
except ImportError:
    HAVE_SERIAL = False


def read(path, mode="r"):
    with open(path, mode) as f:
        return f.read()


def run_cli(*args, env=None, cwd=None):
    proc = subprocess.run(cli_command() + [str(a) for a in args], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True, env=env, cwd=cwd or REPO, timeout=300)
    return proc.returncode, proc.stdout, proc.stderr


def write_exe(path, text):
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, 0o755)
    return path


class TempDirCase(unittest.TestCase):
    def setUp(self):
        self.tmp = os.path.realpath(tempfile.mkdtemp(prefix="kube32-package-test-"))
        self.bin = os.path.join(self.tmp, "bin")
        os.makedirs(self.bin)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def env(self, **extra):
        """The environment of a CLI run: PATH with only the test's scripts and the system tools, a private cache."""
        env = dict(os.environ, PATH=os.pathsep.join([self.bin, "/usr/bin", "/bin"]), KUBE32_CACHE_DIR=os.path.join(self.tmp, "cache"))
        env.pop("KUBE32_SERIAL_PORT", None)
        env.update(extra)
        return env


# ---------------------------------------------------------------------------
# A release server
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Files the package carries from the repository
# ---------------------------------------------------------------------------
def load_build_hook():
    """python/hatch_build.py, with a stand-in for hatchling when it is not installed."""
    try:
        import hatchling.builders.hooks.plugin.interface  # noqa: F401
    except ImportError:
        iface = types.ModuleType("hatchling.builders.hooks.plugin.interface")
        iface.BuildHookInterface = object
        for name in ("hatchling", "hatchling.builders", "hatchling.builders.hooks", "hatchling.builders.hooks.plugin"):
            sys.modules.setdefault(name, types.ModuleType(name))
        sys.modules["hatchling.builders.hooks.plugin.interface"] = iface
    spec = importlib.util.spec_from_file_location("kube32_hatch_build", os.path.join(REPO, "python", "hatch_build.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestSharedFiles(TempDirCase):
    def test_hook_and_package_use_one_table(self):
        self.assertEqual(load_build_hook().SHARED_FILES, common.SHARED_FILES)

    def test_hook_copies_the_repository_files(self):
        hook = load_build_hook()
        project = os.path.join(REPO, "python")
        self.assertEqual(hook.shared_sources(project, "wheel"), {
            HEADER: "kube32/_data/include/runtime/k32_app_abi.h",
            COUNTER: "kube32/_data/templates/rust/lib.rs",
        })
        self.assertEqual(set(hook.shared_sources(project, "sdist").values()),
                         {"src/kube32/_data/include/runtime/k32_app_abi.h", "src/kube32/_data/templates/rust/lib.rs"})

    def test_hook_in_an_unpacked_sdist(self):
        hook = load_build_hook()
        project = os.path.join(self.tmp, "kube32-1.0.0")
        with self.assertRaises(FileNotFoundError):
            hook.shared_sources(project, "wheel")
        for data_path in common.SHARED_FILES.values():
            p = os.path.join(project, "src", "kube32", "_data", *data_path.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "w").close()
        self.assertEqual(hook.shared_sources(project, "wheel"), {})

    def test_packaged_copies_equal_the_repository(self):
        for repo_path in common.SHARED_FILES:
            path = common.shared_file(repo_path)
            if INSTALLED:
                self.assertTrue(path.startswith(os.path.join(PACKAGE_DIR, "_data") + os.sep), path)
            self.assertEqual(read(path, "rb"), read(os.path.join(REPO, *repo_path.split("/")), "rb"), repo_path)

    def test_include_dir(self):
        self.assertTrue(os.path.isfile(os.path.join(common.abi_include_dir(), "runtime", "k32_app_abi.h")))


# ---------------------------------------------------------------------------
# new
# ---------------------------------------------------------------------------
def have_c_toolchain():
    from kube32 import build as kbuild
    clang = shutil.which(os.environ.get("CLANG", "clang"))
    if not clang or not shutil.which("make") or not kbuild.find_rust_lld():
        return False
    proc = subprocess.run([clang, "--target=wasm32", "-x", "c", "-c", "-", "-o", os.devnull], input="int x;", text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return proc.returncode == 0


def have_rust_wasm():
    from kube32 import build as kbuild
    env = kbuild.cargo_env()
    rustup = shutil.which("rustup", path=env["PATH"])
    if not rustup or not shutil.which("cargo", path=env["PATH"]):
        return False
    proc = subprocess.run([rustup, "target", "list", "--installed"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    return "wasm32-unknown-unknown" in proc.stdout.split()


class TestNew(TempDirCase):
    def test_c_project(self):
        rc, out, err = run_cli("new", "hello-c", cwd=self.tmp)
        self.assertEqual(rc, 0, err)
        d = os.path.join(self.tmp, "hello-c")
        self.assertEqual(out.splitlines()[0], "created hello-c (C): Makefile, main.c, include/runtime/k32_app_abi.h, .gitignore")
        self.assertEqual(read(os.path.join(d, "include", "runtime", "k32_app_abi.h"), "rb"), read(HEADER, "rb"))
        makefile = read(os.path.join(d, "Makefile"))
        self.assertIn("NAME := hello-c\n", makefile)
        self.assertIn("CFLAGS = --target=wasm32 -Oz -nostdlib -ffreestanding -fno-exceptions -mcpu=mvp -msign-ext -mbulk-memory", makefile)
        self.assertIn("\t$(WASM_LD) $(LD_FLAVOR) $(LDFLAGS) -o $@ $(OBJECTS)\n", makefile)
        main_c = read(os.path.join(d, "main.c"))
        self.assertIn('#include "runtime/k32_app_abi.h"', main_c)
        self.assertIn('K32_EXPORT("k32_handle")', main_c)
        self.assertIn('LIT("hello-c: request ")', main_c)

    def test_rust_project(self):
        rc, out, err = run_cli("new", os.path.join(self.tmp, "apps", "tally"), "--lang", "rust")
        self.assertEqual(rc, 0, err)
        d = os.path.join(self.tmp, "apps", "tally")
        self.assertEqual(sorted(os.listdir(d)), [".cargo", ".gitignore", "Cargo.toml", "src"])
        self.assertEqual(read(os.path.join(d, "src", "lib.rs"), "rb"), read(COUNTER, "rb"))
        cargo = read(os.path.join(d, "Cargo.toml"))
        self.assertIn('name = "tally"', cargo)
        self.assertIn('crate-type = ["cdylib"]', cargo)
        self.assertIn(scaffold.sdk_dependency(), cargo)
        self.assertIn("link-arg=--max-memory=65536", read(os.path.join(d, ".cargo", "config.toml")))

    def test_sdk_dependency(self):
        self.assertEqual(scaffold.sdk_dependency(release="1.4.2"), 'kube32-sdk = "1.4.2"')
        dev = scaffold.sdk_dependency(release="1.5.0.dev3+gabc1234")
        self.assertTrue(dev.endswith('kube32-sdk = { git = "https://github.com/arterialist/kube32" }'), dev)
        self.assertIn("development build", dev)
        sdk = os.path.join(REPO, "sdk", "rust", "kube32-sdk")
        self.assertEqual(scaffold.sdk_dependency(sdk_path=sdk), 'kube32-sdk = { path = "%s" }' % sdk)

    def test_refusals(self):
        os.makedirs(os.path.join(self.tmp, "taken"))
        open(os.path.join(self.tmp, "taken", "x"), "w").close()
        cases = [
            (("new", "taken"), 1, "error: taken exists and is not an empty directory\n"),
            (("new", "Bad_Name"), 2, "error: invalid app name 'Bad_Name'"),
            (("new", "9lives"), 2, "error: invalid app name '9lives'"),
            (("new", "app", "--sdk-path", self.tmp), 2, "error: --sdk-path is for --lang rust\n"),
            (("new", "app", "--lang", "rust", "--sdk-path", self.tmp), 2, "has no Cargo.toml"),
            (("new", "app", "--lang", "go"), 2, "invalid choice: 'go'"),
        ]
        for args, code, msg in cases:
            rc, _, err = run_cli(*args, cwd=self.tmp)
            self.assertEqual(rc, code, args)
            self.assertIn(msg, err, args)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "app")))

    @unittest.skipUnless(have_c_toolchain(), "make, clang with the wasm32 target and wasm-ld/rust-lld are needed")
    def test_new_then_build_c(self):
        self.assertEqual(run_cli("new", "demo", cwd=self.tmp)[0], 0)
        rc, out, err = run_cli("build", "demo", cwd=self.tmp)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("building %s (c-make)" % os.path.join(self.tmp, "demo"), out)
        self.assertIn("demo/build/demo.wasm: ", out)
        self.assertIn("kind: request pod (k32_handle)", out)
        self.assertIn("imports (4): k32_log, k32_resp_begin, k32_resp_end, k32_resp_write", out)
        self.assertEqual(out.splitlines()[-1], "ok")
        # kube32 build without a directory builds the current one
        rc, out, _ = run_cli("build", cwd=os.path.join(self.tmp, "demo"))
        self.assertEqual(rc, 0)
        self.assertIn("build/demo.wasm", out)

    @unittest.skipUnless(have_rust_wasm(), "cargo and the wasm32-unknown-unknown target are needed")
    def test_new_then_build_rust(self):
        sdk = os.path.join(REPO, "sdk", "rust", "kube32-sdk")
        self.assertEqual(run_cli("new", "tally", "--lang", "rust", "--sdk-path", sdk, cwd=self.tmp)[0], 0)
        rc, out, err = run_cli("build", "tally", cwd=self.tmp)
        self.assertEqual(rc, 0, out + err)
        self.assertIn("tally/build/tally.wasm: ", out)
        self.assertIn("kind: request pod (k32_handle)", out)
        self.assertEqual(out.splitlines()[-1], "ok")


# ---------------------------------------------------------------------------
# Ports
# ---------------------------------------------------------------------------
def port(device, vid):
    return types.SimpleNamespace(device=device, vid=vid)


class TestPorts(unittest.TestCase):
    PORTS = [port("/dev/ttyS0", None), port("/dev/ttyUSB0", 0x1A86), port("/dev/ttyACM1", 0x303A), port("/dev/ttyACM0", 0x303A),
             port("/dev/ttyUSB1", 0x0403)]

    def test_board_ports(self):
        self.assertEqual(ports.board_ports(self.PORTS),
                         [("/dev/ttyACM0", "native USB"), ("/dev/ttyACM1", "native USB"), ("/dev/ttyUSB0", "CH340")])
        self.assertTrue(ports.is_bridge("/dev/ttyUSB0", self.PORTS))
        self.assertFalse(ports.is_bridge("/dev/ttyACM0", self.PORTS))

    def test_choose_port(self):
        with mock.patch.dict(os.environ):
            os.environ.pop("KUBE32_SERIAL_PORT", None)
            self.assertEqual(ports.choose_port("/dev/given", self.PORTS), "/dev/given")
            self.assertEqual(ports.choose_port(None, [port("/dev/ttyUSB0", 0x1A86), port("/dev/ttyS0", None)]), "/dev/ttyUSB0")
            with self.assertRaises(ports.PortError) as ctx:
                ports.choose_port(None, self.PORTS)
            self.assertEqual(str(ctx.exception), "3 boards are connected (/dev/ttyACM0 native USB, /dev/ttyACM1 native USB, "
                                                 "/dev/ttyUSB0 CH340): pass --port")
            with self.assertRaises(ports.PortError) as ctx:
                ports.choose_port(None, [port("/dev/ttyS0", None)])
            self.assertIn("no ESP32-S3 board found (USB vendor ID 303a for native USB, 1a86 for a CH340 bridge)", str(ctx.exception))
            os.environ["KUBE32_SERIAL_PORT"] = "/dev/from-env"
            self.assertEqual(ports.choose_port(None, self.PORTS), "/dev/from-env")

    @unittest.skipUnless(HAVE_SERIAL, "needs pyserial")
    def test_gateway_candidates(self):
        from kube32 import gateway
        found = self.PORTS + [port("/dev/cu.usbmodem9", 0x2341)]
        with mock.patch.object(gateway, "comports", return_value=found):
            self.assertEqual(gateway.candidate_ports(), ["/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/cu.usbmodem9"])


# ---------------------------------------------------------------------------
# version, help
# ---------------------------------------------------------------------------
FAKE_KUBECTL_VERSION = textwrap.dedent('''\
    #!/bin/sh
    echo "$*" > "$(dirname "$0")/args"
    echo '{"major":"1","minor":"33","gitVersion":"v1.33.0-kube32","gitCommit":"0123456789abcdef0123","platform":"linux/arm64"}'
''')


class TestVersionAndHelp(TempDirCase):
    def test_version(self):
        write_exe(os.path.join(self.bin, "kubectl"), FAKE_KUBECTL_VERSION)
        rc, out, _ = run_cli("version", "--context", "lab", env=self.env())
        self.assertEqual(rc, 0)
        self.assertEqual(out, "kube32 %s\nserver: v1.33.0-kube32 (linux/arm64, commit 0123456789ab)\n" % kube32.version())
        self.assertEqual(read(os.path.join(self.bin, "args")), "--context lab get --raw /version --request-timeout=5s\n")

    def test_version_without_a_cluster(self):
        rc, out, _ = run_cli("version", env=self.env(PATH="/nonexistent"))
        self.assertEqual(rc, 0)
        self.assertIn("server: unknown (kubectl not found in PATH: this command runs kubectl; install it "
                      "(https://kubernetes.io/docs/tasks/tools/) or pass --kubectl <path>)", out)
        write_exe(os.path.join(self.bin, "kubectl"), "#!/bin/sh\necho 'The connection to the server was refused' >&2\nexit 1\n")
        rc, out, _ = run_cli("version", env=self.env())
        self.assertEqual((rc, out.splitlines()[1]), (0, "server: not reachable (The connection to the server was refused)"))
        rc, out, _ = run_cli("version", "--client", env=self.env())
        self.assertEqual(out, "kube32 %s\n" % kube32.version())
        rc, out, _ = run_cli("--version")
        self.assertEqual(out.split()[1], kube32.version())

    def test_help_lists_every_command(self):
        rc, out, _ = run_cli("--help")
        self.assertEqual(rc, 0)
        for command in ("new", "build", "check", "deploy", "embed", "sd-push", "push", "firmware", "devices", "checkpoint",
                        "move", "checkpoints", "gateway", "version"):
            self.assertRegex(out, r"\n    %s\s" % command)
        rc, out, err = run_cli("version", "--client", "--bogus")
        self.assertEqual(rc, 2)
        self.assertIn("error: unrecognized arguments: --bogus", err)

    def test_kubectl_hint(self):
        rc, _, err = run_cli("devices", env=self.env(PATH="/nonexistent"))
        self.assertEqual(rc, 4)
        self.assertIn("kubectl not found in PATH: this command runs kubectl; install it", err)


if __name__ == "__main__":
    unittest.main()
