"""Tests for what the kube32 package adds to the CLI: the build hook, `new`,
releases (download, SHA-256 check, cache), `flash`, `sim`, `console` and
`version`.

Run from the repository root:

    python3 -m unittest discover -s tools/tests -v

Release downloads come from a local HTTP server (KUBE32_RELEASES_URL), esptool
and pyserial are replaced by fakes, and docker, kubectl and kube32-sim by
scripts that record their arguments. Nothing opens a real serial port.
"""
import hashlib
import http.server
import importlib.util
import io
import json
import os
import select
import shutil
import subprocess
import sys
import tarfile
import tempfile
import textwrap
import threading
import time
import types
import unittest
from unittest import mock

from kube32_under_test import INSTALLED, PACKAGE_DIR, REPO, cli_command

import kube32
from kube32 import common, flash, ports, releases, scaffold, sim

DEV_BUILD = not kube32.is_release(kube32.version())
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
class ReleaseServer:
    """Serves <root>/v<version>/<asset> on 127.0.0.1 and counts the requests per path."""

    def __init__(self, root):
        self.root = root
        self.requests = []
        server = self

        class Handler(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *a, **kw):
                super().__init__(*a, directory=root, **kw)

            def do_GET(self):
                server.requests.append(self.path)
                super().do_GET()

            def log_message(self, *a):
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = "http://127.0.0.1:%d" % self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def add(self, release, name, data, digest=None):
        d = os.path.join(self.root, "v" + release)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name), "wb") as f:
            f.write(data)
        with open(os.path.join(d, name + ".sha256"), "w") as f:
            f.write("%s  %s\n" % (digest or hashlib.sha256(data).hexdigest(), name))

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def merged_image(size=0x9000):
    """A merged image as kube32 flash expects it: bootloader magic at 0x0, partition table at 0x8000."""
    data = bytearray(b"\xff" * size)
    data[0] = 0xE9
    data[0x8000:0x8002] = b"\xaa\x50"
    return bytes(data)


def sim_tarball(script, member="kube32-sim"):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tf:
        data = script.encode()
        info = tarfile.TarInfo(member)
        info.size = len(data)
        info.mode = 0o755
        tf.addfile(info, io.BytesIO(data))
    return buf.getvalue()


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
# Releases
# ---------------------------------------------------------------------------
class TestReleases(TempDirCase):
    def setUp(self):
        super().setUp()
        self.server = ReleaseServer(os.path.join(self.tmp, "www"))
        self.environ = mock.patch.dict(os.environ, {"KUBE32_RELEASES_URL": self.server.url,
                                                    "KUBE32_CACHE_DIR": os.path.join(self.tmp, "cache")})
        self.environ.start()
        self.out = io.StringIO()

    def tearDown(self):
        self.environ.stop()
        self.server.close()
        super().tearDown()

    def test_download_check_and_cache(self):
        self.server.add("1.2.3", "asset.bin", b"payload" * 1000)
        path = releases.fetch("1.2.3", "asset.bin", out=self.out)
        self.assertEqual(path, os.path.join(self.tmp, "cache", "releases", "1.2.3", "asset.bin"))
        self.assertEqual(read(path, "rb"), b"payload" * 1000)
        self.assertEqual(self.server.requests, ["/v1.2.3/asset.bin.sha256", "/v1.2.3/asset.bin"])
        self.assertIn("downloading %s/v1.2.3/asset.bin" % self.server.url, self.out.getvalue())
        # The cached copy matches its .sha256: no request
        self.assertEqual(releases.fetch("1.2.3", "asset.bin", out=self.out), path)
        self.assertEqual(len(self.server.requests), 2)
        # A damaged cached copy is downloaded again
        with open(path, "ab") as f:
            f.write(b"x")
        releases.fetch("1.2.3", "asset.bin", out=self.out)
        self.assertEqual(len(self.server.requests), 4)
        self.assertEqual(read(path, "rb"), b"payload" * 1000)

    def test_checksum_mismatch(self):
        self.server.add("1.2.3", "asset.bin", b"payload", digest="0" * 64)
        with self.assertRaises(releases.ReleaseError) as ctx:
            releases.fetch("1.2.3", "asset.bin", out=self.out)
        self.assertIn("does not match asset.bin.sha256 (%s); the download was discarded" % ("0" * 64), str(ctx.exception))
        self.assertEqual(os.listdir(os.path.join(self.tmp, "cache", "releases", "1.2.3")), [])

    def test_missing_release(self):
        with self.assertRaises(releases.NotFound) as ctx:
            releases.fetch("9.9.9", "asset.bin", out=self.out)
        self.assertIn("release v9.9.9 has no asset.bin", str(ctx.exception))
        self.assertIn("https://github.com/arterialist/kube32/releases", str(ctx.exception))

    def test_bad_sha256_file(self):
        d = os.path.join(self.tmp, "www", "v1.0.0")
        os.makedirs(d)
        with open(os.path.join(d, "a.bin.sha256"), "w") as f:
            f.write("<html>not a digest</html>")
        with self.assertRaises(releases.ReleaseError) as ctx:
            releases.fetch("1.0.0", "a.bin", out=self.out)
        self.assertEqual(str(ctx.exception), "a.bin.sha256 does not start with a SHA-256 digest")

    def test_release_for(self):
        self.assertEqual(releases.release_for("1.2.3", "firmware", "x"), "1.2.3")
        self.assertEqual(releases.release_for("v1.2.3", "firmware", "x"), "1.2.3")
        with self.assertRaises(releases.ReleaseError):
            releases.release_for("1.2", "firmware", "x")
        with mock.patch.object(releases, "version", return_value="1.3.0.dev4+gabc"):
            with self.assertRaises(releases.ReleaseError) as ctx:
                releases.release_for(None, "firmware", "pass --version X.Y.Z")
        self.assertIn("kube32 1.3.0.dev4+gabc is a development build, and no firmware release matches it: pass --version X.Y.Z",
                      str(ctx.exception))
        with mock.patch.object(releases, "version", return_value="1.3.0"):
            self.assertEqual(releases.release_for(None, "firmware", "x"), "1.3.0")

    def test_parse_sha256(self):
        digest = "ab" * 32
        self.assertEqual(releases.parse_sha256(digest + "\n", "f"), digest)
        self.assertEqual(releases.parse_sha256(digest.upper() + "  f.bin\n", "f"), digest)

    def test_cache_dir(self):
        self.assertEqual(releases.cache_dir(), os.path.join(self.tmp, "cache"))
        with mock.patch.dict(os.environ, {"XDG_CACHE_HOME": "/xdg"}):
            del os.environ["KUBE32_CACHE_DIR"]
            expected = (os.path.join(os.path.expanduser("~"), "Library", "Caches", "kube32") if sys.platform == "darwin"
                        else os.path.join("/xdg", "kube32"))
            if os.name != "nt":
                self.assertEqual(releases.cache_dir(), expected)


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
# flash
# ---------------------------------------------------------------------------
FAKE_ESPTOOL = textwrap.dedent('''\
    import json, os
    __version__ = os.environ.get("FAKE_ESPTOOL_VERSION", "5.1.0")
    class FatalError(Exception):
        pass
    def main(argv=None, esp=None):
        log = os.environ["FAKE_ESPTOOL_LOG"]
        with open(log, "a") as f:
            f.write(json.dumps(argv) + "\\n")
        with open(log) as f:
            calls = len(f.readlines())
        if calls <= int(os.environ.get("FAKE_ESPTOOL_FAILS", "0")):
            raise FatalError("Failed to connect to ESP32-S3: No serial data received.")
''')


class FakeEsptool(types.ModuleType):
    def __init__(self, version="5.1.0", fails=0):
        super().__init__("esptool")
        self.__version__ = version
        self.calls = []
        self.fails = fails

    def main(self, argv):
        self.calls.append(argv)
        if len(self.calls) <= self.fails:
            raise RuntimeError("Failed to connect")


class TestFlash(TempDirCase):
    def setUp(self):
        super().setUp()
        # esptool and pyserial stand-ins for the CLI runs: no serial port is listed or opened
        self.modules = os.path.join(self.tmp, "modules")
        os.makedirs(os.path.join(self.modules, "esptool"))
        os.makedirs(os.path.join(self.modules, "serial", "tools"))
        with open(os.path.join(self.modules, "esptool", "__init__.py"), "w") as f:
            f.write(FAKE_ESPTOOL)
        with open(os.path.join(self.modules, "serial", "__init__.py"), "w") as f:
            f.write("class SerialException(OSError):\n    pass\n")
        open(os.path.join(self.modules, "serial", "tools", "__init__.py"), "w").close()
        with open(os.path.join(self.modules, "serial", "tools", "list_ports.py"), "w") as f:
            f.write("def comports():\n    return []\n")
        self.log = os.path.join(self.tmp, "esptool.jsonl")
        self.image = os.path.join(self.tmp, "merged.bin")
        with open(self.image, "wb") as f:
            f.write(merged_image())

    def flash_env(self, **extra):
        return self.env(PYTHONPATH=self.modules, FAKE_ESPTOOL_LOG=self.log, **extra)

    def calls(self):
        with open(self.log) as f:
            return [json.loads(line) for line in f]

    def test_check_image(self):
        flash.check_image(self.image)
        app = os.path.join(self.tmp, "app.bin")
        with open(app, "wb") as f:
            f.write(b"\xe9" + b"\0" * 0x9000)
        with self.assertRaises(ValueError) as ctx:
            flash.check_image(app)
        self.assertIn("no partition table at 0x8000", str(ctx.exception))
        with open(app, "wb") as f:
            f.write(b"\0" * 16)
        with self.assertRaises(ValueError):
            flash.check_image(app)

    def test_esptool_arguments(self):
        with mock.patch.dict(os.environ):
            os.environ.pop("KUBE32_FLASH_BEFORE", None)
            os.environ.pop("KUBE32_FLASH_AFTER", None)
            self.assertEqual(flash.esptool_argv("5.1.0", "/dev/ttyACM0", 921600, "fw.bin", False),
                             ["--chip", "esp32s3", "--port", "/dev/ttyACM0", "--baud", "921600", "--before", "default-reset",
                              "--after", "watchdog-reset", "write-flash", "0x0", "fw.bin"])
            self.assertEqual(flash.esptool_argv("4.8.1", "/dev/ttyUSB0", 460800, "fw.bin", True),
                             ["--chip", "esp32s3", "--port", "/dev/ttyUSB0", "--baud", "460800", "--before", "default_reset",
                              "--after", "hard_reset", "write_flash", "-u", "0x0", "fw.bin"])

    def test_falls_back_to_460800(self):
        fake = FakeEsptool(fails=1)
        out = io.StringIO()
        with mock.patch.dict(sys.modules, {"esptool": fake}), mock.patch.object(flash, "is_bridge", return_value=False):
            self.assertIsNone(flash.write_image("/dev/ttyACM0", "fw.bin", flash.BAUDS, out=out))
        self.assertEqual([c[5] for c in fake.calls], ["921600", "460800"])
        self.assertIn("esptool failed at 921600 baud: Failed to connect; retrying at 460800 baud", out.getvalue())
        fake = FakeEsptool(fails=2)
        with mock.patch.dict(sys.modules, {"esptool": fake}), mock.patch.object(flash, "is_bridge", return_value=False):
            self.assertEqual(flash.write_image("/dev/ttyACM0", "fw.bin", flash.BAUDS, out=io.StringIO()),
                             "esptool failed at 460800 baud: Failed to connect")

    def test_flash_a_file(self):
        rc, out, err = run_cli("flash", "--file", self.image, "--port", "/dev/fake0", "--no-console", env=self.flash_env())
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(self.calls(), [["--chip", "esp32s3", "--port", "/dev/fake0", "--baud", "921600", "--before", "default-reset",
                                         "--after", "watchdog-reset", "write-flash", "0x0", self.image]])
        self.assertIn("flashed /dev/fake0 with %s" % self.image, out)
        self.assertIn("K32_CMD:WIFI:<ssid>:<password>", out)
        self.assertIn("console --port /dev/fake0", out)

    def test_flash_a_release(self):
        server = ReleaseServer(os.path.join(self.tmp, "www"))
        self.addCleanup(server.close)
        image = merged_image()
        server.add("2.0.1", "kube32-firmware-esp32s3-2.0.1.bin", image)
        # The parts, each at its offset: NVS (0x9000) is not among them
        parts = [("bootloader", 0x0, image[:0x100]), ("partitions", 0x8000, image[0x8000:0x8C00]), ("app", 0x20000, b"\xe9app")]
        layout = {"chip": "esp32s3", "version": "2.0.1", "parts": []}
        for name, offset, data in parts:
            fname = "kube32-firmware-esp32s3-2.0.1-%s.bin" % name
            server.add("2.0.1", fname, data)
            layout["parts"].append({"name": name, "offset": offset, "size": len(data), "file": fname,
                                    "sha256": hashlib.sha256(data).hexdigest()})
        server.add("2.0.1", "kube32-firmware-esp32s3-2.0.1-flash.json", json.dumps(layout).encode())
        env = self.flash_env(KUBE32_RELEASES_URL=server.url, FAKE_ESPTOOL_VERSION="4.8.1", FAKE_ESPTOOL_FAILS="1")
        rc, out, err = run_cli("flash", "--version", "2.0.1", "--port", "/dev/fake0", "--no-console", env=env)
        self.assertEqual(rc, 0, out + err)
        cache = os.path.join(self.tmp, "cache", "releases", "2.0.1")
        self.assertEqual([c[5] for c in self.calls()], ["921600", "460800"])
        self.assertEqual(self.calls()[1][-6:], ["0x0", os.path.join(cache, "kube32-firmware-esp32s3-2.0.1-bootloader.bin"),
                                                "0x8000", os.path.join(cache, "kube32-firmware-esp32s3-2.0.1-partitions.bin"),
                                                "0x20000", os.path.join(cache, "kube32-firmware-esp32s3-2.0.1-app.bin")])
        self.assertIn("the board keeps its stored settings", out)
        self.assertIn("flashed /dev/fake0 with kube32 2.0.1", out)
        # --erase-settings: the merged image from 0x0
        rc, out, err = run_cli("flash", "--version", "2.0.1", "--port", "/dev/fake0", "--no-console", "--erase-settings", env=env)
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(self.calls()[-1][-3:], ["write_flash", "0x0", os.path.join(cache, "kube32-firmware-esp32s3-2.0.1.bin")])
        self.assertIn("stored settings (Wi-Fi, node name, cluster) are erased", out)

    def test_refusals(self):
        rc, _, err = run_cli("flash", "--version", "7.7.7", "--port", "/dev/fake0", env=self.flash_env(KUBE32_RELEASES_URL="http://127.0.0.1:9"))
        self.assertEqual(rc, 1)
        self.assertIn("cannot download http://127.0.0.1:9/v7.7.7/kube32-firmware-esp32s3-7.7.7-flash.json.sha256", err)
        rc, _, err = run_cli("flash", "--file", os.path.join(self.tmp, "none.bin"), env=self.flash_env())
        self.assertEqual((rc, err), (1, "error: no such file: %s\n" % os.path.join(self.tmp, "none.bin")))
        rc, _, err = run_cli("flash", "--file", self.image, env=self.flash_env())   # no board listed
        self.assertEqual(rc, 1)
        self.assertIn("no ESP32-S3 board found", err)
        self.assertFalse(os.path.exists(self.log))

    @unittest.skipUnless(DEV_BUILD, "this kube32 is a release")
    def test_development_build(self):
        rc, out, err = run_cli("flash", "--port", "/dev/fake0", env=self.flash_env())
        self.assertEqual((rc, out), (1, ""))
        self.assertIn("is a development build, and no firmware release matches it: pass --version X.Y.Z to flash a release, "
                      "or --file <image> to flash an image file", err)


# ---------------------------------------------------------------------------
# sim
# ---------------------------------------------------------------------------
RECORDER = '#!/bin/sh\necho "%s $*"\nexit 3\n'


class TestSim(TempDirCase):
    def test_platform_tag(self):
        self.assertEqual(sim.platform_tag("Darwin", "arm64"), ("macos", "arm64"))
        self.assertEqual(sim.platform_tag("Darwin", "x86_64"), ("macos", "x86_64"))
        self.assertEqual(sim.platform_tag("Linux", "aarch64"), ("linux", "arm64"))
        self.assertEqual(sim.platform_tag("Linux", "x86_64"), ("linux", "x86_64"))
        with self.assertRaises(releases.ReleaseError):
            sim.platform_tag("Windows", "AMD64")

    def test_path_first_and_arguments_passed_on(self):
        write_exe(os.path.join(self.bin, "kube32-sim"), RECORDER % "path-sim")
        rc, out, _ = run_cli("sim", "--ingress-port", "8080", "--workers", "", "--psram=0", env=self.env())
        self.assertEqual((rc, out), (3, "path-sim --ingress-port 8080 --workers  --psram=0\n"))
        rc, out, _ = run_cli("sim", "--", "--help", env=self.env())
        self.assertEqual(out, "path-sim --help\n")

    def test_release_download(self):
        os_name, arch = sim.platform_tag()
        server = ReleaseServer(os.path.join(self.tmp, "www"))
        self.addCleanup(server.close)
        server.add("1.2.3", "kube32-sim-1.2.3-%s-%s.tar.gz" % (os_name, arch), sim_tarball(RECORDER % "release-sim"))
        write_exe(os.path.join(self.bin, "kube32-sim"), RECORDER % "path-sim")   # --version wins over PATH
        env = self.env(KUBE32_RELEASES_URL=server.url)
        rc, out, err = run_cli("sim", "--version", "1.2.3", "--port", "7443", env=env)
        self.assertEqual(rc, 3, err)
        self.assertTrue(out.endswith("release-sim --port 7443\n"), out)
        exe = os.path.join(self.tmp, "cache", "releases", "1.2.3", "kube32-sim-%s-%s" % (os_name, arch), "kube32-sim")
        self.assertTrue(os.access(exe, os.X_OK))
        rc, out, _ = run_cli("sim", "--version", "1.2.3", env=env)   # cached: no download
        self.assertEqual(out, "release-sim \n")
        self.assertEqual(len(server.requests), 2)

    def test_tarball_without_the_simulator(self):
        tarball = os.path.join(self.tmp, "t.tar.gz")
        with open(tarball, "wb") as f:
            f.write(sim_tarball("x", member="bin/kube32-sim"))
        with self.assertRaises(releases.ReleaseError) as ctx:
            sim.extract_sim(tarball, os.path.join(self.tmp, "kube32-sim"))
        self.assertEqual(str(ctx.exception), "t.tar.gz has no top-level kube32-sim file")

    def test_docker(self):
        write_exe(os.path.join(self.bin, "docker"), RECORDER % "docker")
        rc, out, _ = run_cli("sim", "--docker", "--version", "1.2.3", "--ingress-port", "8080", "--pod-port=17000", env=self.env())
        self.assertEqual(rc, 3)
        self.assertEqual(out, "docker run --rm --init -p 127.0.0.1:6443:6443 -p 127.0.0.1:8080:8080 -p 127.0.0.1:17000:17000 "
                              "ghcr.io/arterialist/kube32-sim:1.2.3 --ingress-port 8080 --pod-port=17000\n")
        rc, _, err = run_cli("sim", "--docker", "--version", "1.2.3", env=self.env(PATH="/nonexistent"))
        self.assertEqual(rc, 1)
        self.assertIn("docker not found in PATH", err)

    @unittest.skipUnless(DEV_BUILD, "this kube32 is a release")
    def test_development_build(self):
        rc, out, err = run_cli("sim", "--ingress-port", "8080", env=self.env())
        self.assertEqual((rc, out), (1, ""))
        self.assertIn("is a development build, and no kube32-sim release matches it: pass --version X.Y.Z to run a release, "
                      "or build kube32-sim from source", err)


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
                        "move", "checkpoints", "mcp", "flash", "console", "gateway", "sim", "version"):
            self.assertRegex(out, r"\n    %s\s" % command)
        rc, out, err = run_cli("version", "--client", "--bogus")
        self.assertEqual(rc, 2)
        self.assertIn("error: unrecognized arguments: --bogus", err)

    def test_kubectl_hint(self):
        rc, _, err = run_cli("devices", env=self.env(PATH="/nonexistent"))
        self.assertEqual(rc, 4)
        self.assertIn("kubectl not found in PATH: this command runs kubectl; install it", err)


# ---------------------------------------------------------------------------
# console
# ---------------------------------------------------------------------------
class Output(io.StringIO):
    def __init__(self):
        super().__init__()
        self.lock = threading.Lock()

    def write(self, s):
        with self.lock:
            return super().write(s)

    def text(self):
        with self.lock:
            return self.getvalue()


@unittest.skipUnless(HAVE_SERIAL, "needs pyserial and a pty")
class TestConsole(unittest.TestCase):
    def test_console(self):
        from kube32 import console
        master, slave = os.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        r, w = os.pipe()
        stdin = os.fdopen(r)
        out = Output()
        t = threading.Thread(target=console.run, args=(os.ttyname(slave),), kwargs={"stdin": stdin, "stdout": out}, daemon=True)
        t.start()
        deadline = time.time() + 5
        while "[console]" not in out.text() and time.time() < deadline:   # the port is open (raw mode)
            time.sleep(0.02)
        self.assertIn("[console] %s at 115200 baud" % os.ttyname(slave), out.text())
        os.write(master, b"[KUBELET] ready\r\n")
        os.write(w, b"K32_CMD:STATUS\n")
        got = b""
        while b"\n" not in got and time.time() < deadline:
            if select.select([master], [], [], 0.1)[0]:
                got += os.read(master, 64)
        self.assertEqual(got, b"K32_CMD:STATUS\n")
        while "[KUBELET] ready" not in out.text() and time.time() < deadline:
            time.sleep(0.05)
        self.assertIn("[KUBELET] ready", out.text())
        os.close(w)
        t.join(5)
        self.assertFalse(t.is_alive())
        stdin.close()


if __name__ == "__main__":
    unittest.main()
