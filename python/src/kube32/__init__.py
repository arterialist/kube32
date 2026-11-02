"""kube32: build, check and deploy WebAssembly pods, flash ESP32-S3 boards and run kube32-sim."""
import re
import subprocess

RELEASE_RE = re.compile(r"^\d+\.\d+\.\d+$")

_version = None


def version():
    """The package version. A release is X.Y.Z, the version of its git tag vX.Y.Z; anything else
    (X.Y.devN+g<commit>, or a source checkout off a release tag) is a development build."""
    global _version
    if _version is None:
        from .common import source_root
        root = source_root()
        _version = _checkout_version(root) if root else _installed_version()
    return _version


def is_release(v):
    return bool(RELEASE_RE.match(v))


def _installed_version():
    from importlib.metadata import PackageNotFoundError, version as dist_version
    try:
        return dist_version("kube32")
    except PackageNotFoundError:
        return "0.0.0.dev0"


def _checkout_version(root):
    """The release when the checkout is at a vX.Y.Z tag, else a development version."""
    def git(*args):
        proc = subprocess.run(["git", "-C", root] + list(args), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              text=True, timeout=10)
        return proc.stdout.strip() if proc.returncode == 0 else ""
    try:
        tag = git("describe", "--tags", "--exact-match", "--match", "v[0-9]*", "HEAD")
        if tag.startswith("v") and is_release(tag[1:]):
            return tag[1:]
        commit = git("rev-parse", "--short", "HEAD")
    except (OSError, subprocess.SubprocessError):
        commit = ""
    return "0.0.0.dev0+g" + commit if commit else "0.0.0.dev0"


def __getattr__(name):
    if name == "__version__":
        return version()
    raise AttributeError("module 'kube32' has no attribute %r" % name)
