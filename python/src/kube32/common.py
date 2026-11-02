"""Exit codes, quantities, output helpers and file locations shared by the commands."""
import os
import re
import subprocess
import sys

EXIT_OK = 0
EXIT_INPUT = 1
EXIT_USAGE = 2
EXIT_CHECK = 3
EXIT_KUBECTL = 4
EXIT_EXISTS = 5
EXIT_ROLLOUT = 6
EXIT_MOVE = 7

EXIT_CODES_HELP = ("exit codes: 0 ok, 1 input/toolchain problem, 2 usage, 3 ABI check failed, 4 kubectl failed, "
                   "5 deploy: pod exists (pass --replace), firmware rollout: one is in progress, "
                   "6 firmware rollout failed, 7 checkpoint or move failed")

# The name the commands were invoked by, kube32 or kube32-cli, for the hints they print
PROG = "kube32"

PACKAGE_DIR = os.path.dirname(os.path.abspath(__file__))

# Files of the repository that the package carries (the build hook in
# python/hatch_build.py copies them into the wheel under kube32/_data/):
# repository path -> path below kube32/_data/
SHARED_FILES = {
    "include/runtime/k32_app_abi.h": "include/runtime/k32_app_abi.h",
    "sdk/rust/examples/counter/src/lib.rs": "templates/rust/lib.rs",
}


def source_root():
    """The kube32 checkout this package runs from (python/src/kube32 in it), or None when installed."""
    root = os.path.dirname(os.path.dirname(os.path.dirname(PACKAGE_DIR)))
    if (os.path.isfile(os.path.join(root, "python", "pyproject.toml"))
            and os.path.isfile(os.path.join(root, "include", "runtime", "k32_app_abi.h"))):
        return root
    return None


def shared_file(repo_path):
    """A file of SHARED_FILES: the copy in the package, or the repository's own in a source checkout."""
    packaged = os.path.join(PACKAGE_DIR, "_data", *SHARED_FILES[repo_path].split("/"))
    if os.path.isfile(packaged):
        return packaged
    root = source_root()
    if root:
        return os.path.join(root, *repo_path.split("/"))
    raise FileNotFoundError("%s is missing from the kube32 package (a broken installation?)" % packaged)


def abi_include_dir():
    """The directory to pass with -I so that #include "runtime/k32_app_abi.h" resolves."""
    return os.path.dirname(os.path.dirname(shared_file("include/runtime/k32_app_abi.h")))


def find_checkout(start=None):
    """The kube32 checkout this package runs from, else the one that contains `start` (default: the
    current directory), else None."""
    root = source_root()
    if root:
        return root
    d = os.path.abspath(start or os.getcwd())
    while True:
        if os.path.isfile(os.path.join(d, "include", "runtime", "k32_app_abi.h")) and os.path.isdir(os.path.join(d, "images")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            return None
        d = parent


# ---------------------------------------------------------------------------
# Quantities
# ---------------------------------------------------------------------------
_QUANTITY_RE = re.compile(r"^\s*(\d+)\s*([KMG]i?|k|m|g)?\s*$")


def parse_quantity(text):
    """Kubernetes quantity ('64Ki', '2Mi', '4096', '64K') to bytes."""
    m = _QUANTITY_RE.match(str(text))
    if not m:
        raise ValueError("invalid quantity '%s' (expected e.g. 64Ki, 1280Ki, 2Mi, 65536)" % text)
    n = int(m.group(1))
    unit = m.group(2) or ""
    mult = {
        "": 1, "k": 1000, "K": 1000, "m": 1000 ** 2, "M": 1000 ** 2, "g": 1000 ** 3, "G": 1000 ** 3,
        "Ki": 1024, "Mi": 1024 ** 2, "Gi": 1024 ** 3,
    }[unit]
    return n * mult


def format_quantity(nbytes):
    if nbytes and nbytes % (1024 * 1024) == 0:
        return "%dMi" % (nbytes // (1024 * 1024))
    if nbytes and nbytes % 1024 == 0:
        return "%dKi" % (nbytes // 1024)
    return str(nbytes)


def fmt_bytes(n):
    return "{:,}".format(n)


# ---------------------------------------------------------------------------
# Output and processes
# ---------------------------------------------------------------------------
def err_exit(msg, code):
    print("error: %s" % msg, file=sys.stderr)
    return code


class Log:
    def __init__(self, verbose):
        self.verbose = verbose

    def __call__(self, s):
        if self.verbose:
            print(s)


def run(cmd, cwd=None, env=None):
    proc = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return proc.returncode, proc.stdout


def print_table(rows, columns):
    widths = [max(len(title), *(len(str(r[key])) for r in rows)) if rows else len(title) for title, key in columns]
    print("  ".join(title.ljust(w) for (title, _), w in zip(columns, widths)).rstrip())
    for r in rows:
        print("  ".join(str(r[key]).ljust(w) for (_, key), w in zip(columns, widths)).rstrip())
