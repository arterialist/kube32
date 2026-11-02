"""The kube32 package the tests exercise.

By default the tests import python/src of this checkout and run tools/kube32-cli.
With KUBE32_TEST_INSTALLED=1 they import the kube32 package installed in the
Python that runs them (a wheel, as `uv tool install kube32` installs it) and run
`python -m kube32`, so the same suite checks the built package.
"""
import os
import sys

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(TESTS_DIR))
SOURCE = os.path.join(REPO, "python", "src")
INSTALLED = os.environ.get("KUBE32_TEST_INSTALLED") == "1"

if not INSTALLED and SOURCE not in sys.path:
    sys.path.insert(0, SOURCE)

import kube32  # noqa: E402

PACKAGE_DIR = os.path.dirname(os.path.realpath(kube32.__file__))
if INSTALLED and PACKAGE_DIR.startswith(os.path.realpath(SOURCE) + os.sep):
    raise RuntimeError("KUBE32_TEST_INSTALLED=1, but kube32 comes from the source tree %s" % SOURCE)


def cli_command():
    """The command line that runs the CLI under test."""
    if INSTALLED:
        return [sys.executable, "-m", "kube32"]
    return [sys.executable, os.path.join(REPO, "tools", "kube32-cli")]
