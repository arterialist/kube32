#!/usr/bin/env python3
"""The serial gateway from a source checkout: runs `kube32 gateway` from python/src.

    tools/kube32_serial_gateway.py [--port /dev/cu.usbmodemXXXX] [--map 8080:80]...

Needs pyserial. See docs/cli-reference.md.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "python", "src"))

from kube32.gateway import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
