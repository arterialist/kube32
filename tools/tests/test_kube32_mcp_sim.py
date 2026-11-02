"""`kube32 mcp` against kube32-sim: the explore-then-codify loop on a simulated BME688 and scale.

Run from the repository root after building the simulator:

    KUBE32_SIM=build/kube32-sim python3 -m unittest discover -s tools/tests -p test_kube32_mcp_sim.py -v

The simulator (KUBE32_SIM, else build/kube32-sim, else kube32-sim on PATH)
runs one node with a PaHub, a BME688 on channel 2 and a Mini Scales unit on
channel 3. The server reaches it with kubectl, reads both Devices, sends the
scale actions within its limits, and deploys the guide's humidity pod from
C source, which receives the BME688's readings. The test skips itself
without the simulator, kubectl, clang or a wasm linker.
"""
import json
import os
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.request

from kube32_under_test import REPO

from test_kube32_mcp import McpProcess, have_c_toolchain

from kube32.mcp import reference


def find_sim():
    for cand in (os.environ.get("KUBE32_SIM"), os.path.join(REPO, "build", "kube32-sim"), shutil.which("kube32-sim")):
        if cand and os.path.isfile(cand) and os.access(cand, os.X_OK):
            return os.path.abspath(cand)
    return None


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@unittest.skipUnless(find_sim() and shutil.which("kubectl") and have_c_toolchain(),
                     "needs kube32-sim (KUBE32_SIM or build/kube32-sim), kubectl, clang and a wasm linker")
class TestMcpOnSimulator(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = os.path.realpath(tempfile.mkdtemp(prefix="kube32-mcp-sim-"))
        cls.api = "http://127.0.0.1:%d" % free_port()
        port = int(cls.api.rsplit(":", 1)[1])
        cls.log = open(os.path.join(cls.tmp, "sim.log"), "wb")
        cls.sim = subprocess.Popen([find_sim(), "--role", "standalone", "--workers", "", "--node-name", "board-1",
                                    "--port", str(port), "--ipc-port", str(free_port()), "--pod-port", str(free_port()),
                                    "--data-dir", os.path.join(cls.tmp, "data"), "--sim-i2c", "pahub,bme688@2,miniscale@3",
                                    "--reconcile-interval", "500"], stdout=cls.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        cls.devices = {}
        while time.time() < deadline and len(cls.devices) < 2:
            time.sleep(0.5)
            try:
                with urllib.request.urlopen(cls.api + "/apis/kube32.io/v1/devices", timeout=2) as resp:
                    items = json.load(resp).get("items", [])
                cls.devices = {d["spec"]["driver"]: d["metadata"]["name"] for d in items}
            except (OSError, ValueError):
                pass
        if len(cls.devices) < 2:
            cls.tearDownClass()
            raise AssertionError("the simulator found no BME688 and scale within 30 s; see its log")

    @classmethod
    def tearDownClass(cls):
        cls.sim.terminate()
        try:
            cls.sim.wait(timeout=10)
        except subprocess.TimeoutExpired:
            cls.sim.kill()
            cls.sim.wait()
        cls.log.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def patch_device(self, name, annotations):
        req = urllib.request.Request("%s/apis/kube32.io/v1/namespaces/default/devices/%s" % (self.api, name), method="PATCH",
                                     data=json.dumps({"metadata": {"annotations": annotations}}).encode(),
                                     headers={"Content-Type": "application/merge-patch+json"})
        with urllib.request.urlopen(req, timeout=10):
            pass

    def test_explore_then_codify(self):
        env_dev, scale = self.devices["bme688"], self.devices["miniscale"]
        self.patch_device(scale, {"kube32.io/limits": "deny tare: a calibration weight sits on it\nled.g <= 32"})
        s = McpProcess(["--server", self.api, "--allow-writes", "--allow-deploy"],
                       dict(os.environ, KUBE32_CACHE_DIR=os.path.join(self.tmp, "cache")))
        try:
            err, out = s.call("list_devices")
            self.assertFalse(err, out)
            listed = {d["device"]: d for d in out["devices"]}
            self.assertEqual(listed["default/" + env_dev]["phase"], "Idle")
            self.assertEqual(listed["default/" + scale]["limits"], ["deny tare: a calibration weight sits on it", "led.g <= 32"])

            # Explore: the read starts the Idle driver and gets the simulated part's values
            err, out = s.call("read_device", device=env_dev, samples=2, interval_seconds=1)
            self.assertFalse(err, out)
            self.assertEqual(out["units"]["humidity_pct"], "%RH")
            self.assertTrue(0 < out["readings"]["humidity_pct"] < 100, out)
            self.assertIn("humidity_pct", out["summary"])
            err, doc = s.call("describe_device", device=scale)
            self.assertFalse(err, doc)
            self.assertIn("- deny tare: a calibration weight sits on it", doc)
            self.assertIn("| `weight_g` | g |", doc)

            # Writes: the limits refuse, the driver takes what they allow
            err, out = s.call("write_device", device=scale, action="tare")
            self.assertTrue(err)
            self.assertIn("refused by the limit 'deny tare'", out)
            err, out = s.call("write_device", device=scale, action="led", arguments={"r": 0, "g": 64, "b": 0})
            self.assertIn("g is 64", out)
            err, out = s.call("write_device", device=scale, action="led", arguments={"r": 0, "g": 16, "b": 0})
            self.assertFalse(err, out)
            self.assertTrue(out["answer"]["ok"])

            # Codify: the guide's pod, built from C source, receives the BME688's readings as events
            err, out = s.call("deploy_code", name="humidity-watch", source=reference.EXAMPLE_POD, device=env_dev,
                              near_device=True, wait_seconds=30)
            self.assertFalse(err, out)
            self.assertEqual(out["status"]["phase"], "Running", out)
            self.assertIn("humidity-watch: watching " + env_dev, out["log"])
            log = out["log"]
            deadline = time.time() + 15
            while "first reading" not in log and time.time() < deadline:
                time.sleep(1)
                log = s.call("workload_status", name="humidity-watch")[1]["pods"][0]["log"]
            self.assertIn("humidity-watch: first reading", log)

            err, out = s.call("delete_workload", name="humidity-watch")
            self.assertFalse(err, out)
            err, out = s.call("delete_workload", name=env_dev + "-driver")
            self.assertIn("was not deployed by kube32 mcp", out)
        finally:
            rc, err_text = s.close()
        self.assertEqual(rc, 0, err_text)


if __name__ == "__main__":
    unittest.main()
