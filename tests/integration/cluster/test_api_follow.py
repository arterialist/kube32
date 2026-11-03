"""
Kube32 Cluster Tests: a worker follows the control plane's announced API address
A worker is started with a --join address that does not answer, as one is
after its machine changed networks, and with the control plane's IPC port as
a static peer, so that HELLO frames flow between the two. The control plane
announces its API address in its HELLO frames (--node-ip); after 30 s without
an answer the worker dials that address instead, registers, and its Node turns
Ready.

Run it in the Linux container image of the integration suites, not on a
developer machine: a worker that dials a non-loopback API server also sends
its HELLO to the UDP broadcast address (port 7400), which any board or
simulator on the local network would hear.
"""

import json
import os
import shutil
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

from tests.integration.framework import BaseTestCase
from tests.integration.cluster_harness import BIN_PATH, node_ready_condition

API_PORT = 17143
CP_IPC_PORT = 18140
WORKER_IPC_PORT = 18141
WORKER_POD_PORT = 18150
# An address in the documentation range (RFC 5737): nothing answers there
SILENT_API_HOST = "192.0.2.1"
FOLLOW_AFTER_S = 30


class TestApiFollow(BaseTestCase):
    def setUp(self):
        super().setUp()
        self.procs = []
        self.tmp = tempfile.mkdtemp(prefix="k32_follow_")

    def tearDown(self):
        for proc, log in self.procs:
            if proc.poll() is None:
                proc.terminate()
        for proc, log in self.procs:
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
            log.close()
        shutil.rmtree(self.tmp, ignore_errors=True)
        super().tearDown()

    def _sim(self, name, args):
        data_dir = os.path.join(self.tmp, name)
        os.makedirs(data_dir)
        log_path = os.path.join(self.tmp, name + ".log")
        log = open(log_path, "w")
        proc = subprocess.Popen([BIN_PATH, "--node-name", name, "--data-dir", data_dir, "--lease-interval", "1000"] + args,
                                stdout=log, stderr=subprocess.STDOUT)
        self.procs.append((proc, log))
        return log_path

    @staticmethod
    def _read(path):
        with open(path, errors="replace") as f:
            return f.read()

    def _node(self, name):
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{API_PORT}/api/v1/nodes/{name}", timeout=2.0) as resp:
                return json.loads(resp.read())
        except (urllib.error.URLError, OSError, ValueError):
            return None

    def test_worker_dials_announced_api_address(self):
        """A worker whose API server is silent switches to the address the control plane announces"""
        # The control plane announces 127.0.0.1: the address it was given
        self._sim("cp-follow", ["--role", "control-plane", "--node-id", "0", "--port", str(API_PORT), "--ipc-port", str(CP_IPC_PORT),
                                "--node-ip", "127.0.0.1", "--not-ready-timeout", "4000", "--reconcile-interval", "500"])
        self.wait_for_condition(lambda: self._version() == 200, timeout=15.0, description="control plane API")

        started = time.time()
        worker_log = self._sim("w-follow", ["--role", "worker", "--node-id", "1", "--ipc-port", str(WORKER_IPC_PORT),
                                            "--pod-port", str(WORKER_POD_PORT), "--join", f"http://{SILENT_API_HOST}:{API_PORT}",
                                            "--peer", f"cp-follow:0:{CP_IPC_PORT}"])

        # The join address does not answer: the worker waits, it does not register
        time.sleep(FOLLOW_AFTER_S / 3)
        self.assertIsNone(self._node("w-follow"), "the worker registered before it followed anything")
        self.assertNotIn("switching", self._read(worker_log))

        # After the silence, it dials the announced address and its Node turns Ready
        def ready():
            cond = node_ready_condition(f"http://127.0.0.1:{API_PORT}", "w-follow")
            return cond if cond and cond.get("status") == "True" else None
        self.wait_for_condition(ready, timeout=FOLLOW_AFTER_S + 40, interval=0.5, description="w-follow Ready at the announced address")
        elapsed = time.time() - started
        self.assertGreaterEqual(elapsed, FOLLOW_AFTER_S - 1, f"the worker switched after {elapsed:.0f} s, before the silence was long enough")

        log = self._read(worker_log)
        line = f"[NET] no answer from {SILENT_API_HOST}; the control plane announces 127.0.0.1, switching"
        self.assertIn(line, log)
        self.assertEqual(log.count("switching"), 1)

        # Its Node reports the address the route to the new API server gives
        addresses = self._node("w-follow").get("status", {}).get("addresses", [])
        self.assertEqual([a["address"] for a in addresses if a.get("type") == "InternalIP"], ["127.0.0.1"])

    def _version(self):
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{API_PORT}/version", timeout=2.0) as resp:
                return resp.status
        except (urllib.error.URLError, OSError):
            return None
