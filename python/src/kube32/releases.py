"""Release assets from GitHub Releases: downloaded once, checked against their .sha256 files, cached.

A release vX.Y.Z carries the assets this module names; KUBE32_RELEASES_URL replaces
https://github.com/arterialist/kube32/releases/download (a mirror), and KUBE32_CACHE_DIR
the cache directory.
"""
import hashlib
import os
import re
import sys
import urllib.error
import urllib.request

from . import is_release, version

REPO_URL = "https://github.com/arterialist/kube32"
RELEASES_PAGE = REPO_URL + "/releases"
DOWNLOAD_URL = REPO_URL + "/releases/download"
TIMEOUT = 60

_SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")


class ReleaseError(Exception):
    pass


class NotFound(ReleaseError):
    pass


def asset_url(release, name):
    base = os.environ.get("KUBE32_RELEASES_URL") or DOWNLOAD_URL
    return "%s/v%s/%s" % (base.rstrip("/"), release, name)


def cache_dir():
    """KUBE32_CACHE_DIR, else the platform's user cache directory."""
    if os.environ.get("KUBE32_CACHE_DIR"):
        return os.environ["KUBE32_CACHE_DIR"]
    home = os.path.expanduser("~")
    if sys.platform == "darwin":
        return os.path.join(home, "Library", "Caches", "kube32")
    if os.name == "nt":
        return os.path.join(os.environ.get("LOCALAPPDATA") or os.path.join(home, "AppData", "Local"), "kube32", "Cache")
    return os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.join(home, ".cache"), "kube32")


def release_for(requested, what, alternatives):
    """The release to download: `requested` (--version), else this package's own version. Raises
    ReleaseError naming `alternatives` when that is a development build, which no release matches."""
    release = requested[1:] if requested and requested.startswith("v") else requested
    if release:
        if not is_release(release):
            raise ReleaseError("--version takes a release number such as 1.2.0, not '%s'" % requested)
        return release
    release = version()
    if not is_release(release):
        raise ReleaseError("kube32 %s is a development build, and no %s release matches it: %s (releases: %s)"
                           % (release, what, alternatives, RELEASES_PAGE))
    return release


def parse_sha256(text, name):
    """The digest of a .sha256 file: '<hex>' or '<hex>  <file name>' (sha256sum output)."""
    words = text.split()
    if not words or not _SHA256_RE.match(words[0]):
        raise ReleaseError("%s.sha256 does not start with a SHA-256 digest" % name)
    return words[0].lower()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def _open(url):
    req = urllib.request.Request(url, headers={"User-Agent": "kube32/%s" % version()})
    try:
        return urllib.request.urlopen(req, timeout=TIMEOUT)
    except urllib.error.HTTPError as e:
        e.close()
        if e.code == 404:
            raise NotFound("%s: not found" % url)
        raise ReleaseError("%s: HTTP %d %s" % (url, e.code, e.reason))
    except (urllib.error.URLError, OSError) as e:
        raise ReleaseError("cannot download %s: %s" % (url, getattr(e, "reason", None) or e))


def fetch(release, name, out=None):
    """The path of release asset `name` in the cache, downloaded first unless the cached copy
    matches its cached .sha256. A download that does not match the .sha256 is discarded."""
    out = out or sys.stdout
    d = os.path.join(cache_dir(), "releases", release)
    path = os.path.join(d, name)
    sum_path = path + ".sha256"
    if os.path.isfile(path) and os.path.isfile(sum_path):
        try:
            with open(sum_path) as f:
                if parse_sha256(f.read(), name) == sha256_file(path):
                    return path
        except (OSError, ReleaseError):
            pass
    os.makedirs(d, exist_ok=True)
    try:
        with _open(asset_url(release, name + ".sha256")) as resp:
            sum_text = resp.read(4096).decode("utf-8", "replace")
    except NotFound as e:
        raise NotFound("release v%s has no %s (%s); releases: %s" % (release, name, e, RELEASES_PAGE))
    expected = parse_sha256(sum_text, name)
    url = asset_url(release, name)
    out.write("downloading %s\n" % url)
    out.flush()
    part = path + ".part"
    h = hashlib.sha256()
    size = 0
    try:
        with _open(url) as resp, open(part, "wb") as f:
            for block in iter(lambda: resp.read(1 << 16), b""):
                f.write(block)
                h.update(block)
                size += len(block)
    except (OSError, ReleaseError):
        if os.path.exists(part):
            os.unlink(part)
        raise
    if h.hexdigest() != expected:
        os.unlink(part)
        raise ReleaseError("%s: SHA-256 %s does not match %s.sha256 (%s); the download was discarded"
                           % (url, h.hexdigest(), name, expected))
    os.replace(part, path)
    with open(sum_path, "w") as f:
        f.write(sum_text)
    out.write("%s: %s bytes, SHA-256 %s\n" % (name, "{:,}".format(size), expected))
    out.flush()
    return path
