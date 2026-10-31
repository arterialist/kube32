"""
Kube32 Device Tests: a part on one node, the app that reads it on another
Two kube32-sim processes: "board-1" runs the control plane and has simulated
parts on its I2C bus (a PaHub with a BME688 on channel 2); "laptop" is a
worker that joined it and has no parts. Discovery on board-1 finds and
identifies the BME688, the control plane creates its Device, the driver pod
runs on board-1 only, and an app pod on the laptop reaches it through the
Device's Service and through a Service of its own that selects drivers by
Device labels. In the other direction, board-1's Ingress controller reaches
the app on the laptop at the laptop's pod port.
"""

import json
import os
import shutil
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request

from tests.integration.framework import BaseTestCase, HTTPClient
from tests.integration.cluster_harness import BIN_PATH, PROJECT_ROOT, SimulatedNode, node_ready_condition


def _uleb(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def _sleb(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if (n == 0 and not b & 0x40) or (n == -1 and b & 0x40):
            out.append(b)
            return bytes(out)
        out.append(b | 0x80)


def _vec(items) -> bytes:
    return _uleb(len(items)) + b"".join(items)


def _name(s: str) -> bytes:
    return _uleb(len(s)) + s.encode()


def _section(sid: int, payload: bytes) -> bytes:
    return bytes([sid]) + _uleb(len(payload)) + payload


def stub_driver_wasm(body: bytes) -> bytes:
    """A request-handling pod in 8 KiB of linear memory that answers every
    request 200 with `body`: what the tests run in place of a driver image."""
    i32 = 0x7F
    def functype(params, results):
        return b"\x60" + _vec([bytes([p]) for p in params]) + _vec([bytes([r]) for r in results])
    def const(v):
        return b"\x41" + _sleb(v)
    ctype = b"application/json"
    ctype_at, body_at = 16, 64
    types = [functype([i32, i32, i32], []), functype([i32, i32], [i32]), functype([], []), functype([i32], [i32])]
    imports = [_name("env") + _name("k32_resp_begin") + b"\x00" + _uleb(0),
               _name("env") + _name("k32_resp_write") + b"\x00" + _uleb(1),
               _name("env") + _name("k32_resp_end") + b"\x00" + _uleb(2)]
    exports = [_name("k32_alloc") + b"\x00" + _uleb(3), _name("k32_handle") + b"\x00" + _uleb(4), _name("memory") + b"\x02" + _uleb(0)]
    alloc = b"\x00" + const(1024) + b"\x0b"
    handle = (b"\x00" + const(200) + const(ctype_at) + const(len(ctype)) + b"\x10" + _uleb(0) +
              const(body_at) + const(len(body)) + b"\x10" + _uleb(1) + b"\x1a" + b"\x10" + _uleb(2) + const(200) + b"\x0b")
    data = [b"\x00" + const(ctype_at) + b"\x0b" + _uleb(len(ctype)) + ctype,
            b"\x00" + const(body_at) + b"\x0b" + _uleb(len(body)) + body]
    return (b"\x00asm\x01\x00\x00\x00" + _section(1, _vec(types)) + _section(2, _vec(imports)) +
            _section(3, _vec([_uleb(3), _uleb(1)])) + _section(5, _vec([b"\x00" + _uleb(1)])) +
            _section(7, _vec(exports)) + _section(10, _vec([_uleb(len(alloc)) + alloc, _uleb(len(handle)) + handle])) +
            _section(11, _vec(data)))


class TestLaptopCompute(BaseTestCase):
    API_PORT = 16743
    BOARD_POD_PORT = 17710
    LAPTOP_POD_PORT = 17711
    INGRESS_PORT = 17712
    DEVICE = "bme688-board-1-b0-m70c2-77"

    def setUp(self):
        super().setUp()
        self.nodes = []
        self.dirs = []
        self.api = HTTPClient(f"http://127.0.0.1:{self.API_PORT}", default_timeout=5.0)

    def tearDown(self):
        for n in self.nodes:
            n.terminate()
        for d in self.dirs:
            shutil.rmtree(d, ignore_errors=True)
        super().tearDown()

    def _dir(self, name: str) -> str:
        d = f"/tmp/k32_devices_{name}_{os.getpid()}_{int(time.time() * 1000) % 100000}"
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(os.path.join(d, "card", "kube32", "images"))
        self.dirs.append(d)
        return d

    def _start(self, name: str, cmd, data_dir: str) -> SimulatedNode:
        env = dict(os.environ, KUBE32_SD_ROOT=os.path.join(data_dir, "card"))
        log = open(os.path.join(data_dir, "sim.log"), "w")
        node = SimulatedNode("standalone", name, 0, self.API_PORT, 0, data_dir)
        node.process = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
        self.nodes.append(node)
        return node

    def _get(self, path: str):
        r = self.api.get(path)
        return r.json() if r.status_code == 200 else None

    def _probe(self, url: str):
        """GET /call on the app pod through the laptop's pod server: the app calls `url`"""
        req = urllib.request.Request(f"http://127.0.0.1:{self.LAPTOP_POD_PORT}/call?u={urllib.parse.quote(url, safe='')}",
                                     headers={"X-Kube32-Pod": "default/app"})
        with urllib.request.urlopen(req, timeout=5.0) as resp:
            return json.loads(resp.read().decode())

    def _ingress(self, path: str):
        """GET `path` through board-1's Ingress controller: the body, None until it answers 200"""
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{self.INGRESS_PORT}{path}", timeout=5.0) as resp:
                return resp.read().decode()
        except (urllib.error.URLError, OSError):
            return None

    def test_app_on_laptop_reads_device_on_board(self):
        """The driver runs where the part is wired; an app on a node without GPIO reaches it by Service"""
        board_dir = self._dir("board")
        laptop_dir = self._dir("laptop")
        with open(os.path.join(board_dir, "card", "kube32", "images", "stub-driver.wasm"), "wb") as f:
            f.write(stub_driver_wasm(b'{"driver":"stub","ok":true}'))
        shutil.copy(os.path.join(PROJECT_ROOT, "images", "sd", "svcprobe.wasm"), os.path.join(laptop_dir, "card", "kube32", "images", "svcprobe.wasm"))

        self._start("board-1", [BIN_PATH, "--role", "standalone", "--workers", "", "--node-name", "cp-dev", "--kubelet-name", "board-1",
                                "--port", str(self.API_PORT), "--ipc-port", "17700", "--pod-port", str(self.BOARD_POD_PORT),
                                "--ingress-port", str(self.INGRESS_PORT), "--data-dir", board_dir, "--sim-i2c", "pahub,bme688@2",
                                "--lease-interval", "1000", "--reconcile-interval", "500"], board_dir)
        self.wait_for_condition(lambda: self._get("/version") is not None, timeout=10.0, description="API server")
        self._start("laptop", [BIN_PATH, "--role", "worker", "--node-name", "laptop", "--node-id", "1", "--ipc-port", "17701",
                               "--join", f"http://127.0.0.1:{self.API_PORT}", "--pod-port", str(self.LAPTOP_POD_PORT),
                               "--data-dir", laptop_dir, "--lease-interval", "1000"], laptop_dir)
        for name in ("board-1", "laptop"):
            self.wait_for_condition(lambda name=name: (node_ready_condition(self.api.base_url, name) or {}).get("status") == "True",
                                    timeout=15.0, description=f"{name} Ready")

        # Discovery found and identified the BME688 behind the PaHub; the Device follows
        node = self._get("/api/v1/nodes/board-1")
        self.assertEqual(node["metadata"]["labels"].get("i2c.kube32.io/0x77"), "bus-0.mux-70.2")
        device_path = f"/apis/kube32.io/v1/namespaces/default/devices/{self.DEVICE}"
        dev = self.wait_for_condition(lambda: self._get(device_path), timeout=10.0, description="discovered Device")
        self.assertEqual(dev["metadata"]["labels"]["kube32.io/discovered"], "true")
        self.assertEqual(dev["spec"]["i2c"], {"bus": 0, "address": "0x77", "mux": {"address": "0x70", "channel": 2}})
        # The laptop's bus has no catalog part: no Device names it
        devices = self._get("/apis/kube32.io/v1/devices")["items"]
        self.assertEqual({d["spec"]["nodeName"] for d in devices}, {"board-1"})

        # The stub driver in place of the catalog image; a label to select it by
        r = self.api.patch(device_path, {"metadata": {"labels": {"location": "kitchen"}}, "spec": {"image": "wasm://sd/stub-driver"}},
                           headers={"Content-Type": "application/merge-patch+json"})
        self.assertEqual(r.status_code, 200, r.text)
        dev = self.wait_for_condition(lambda: (lambda d: d if d and d.get("status", {}).get("phase") == "Running" else None)(self._get(device_path)),
                                      timeout=20.0, description="Device Running")
        self.assertEqual(dev["status"]["chip"], "BME688")
        self.assertEqual(dev["status"]["nodeName"], "board-1")
        pod = self._get(f"/api/v1/namespaces/default/pods/{self.DEVICE}-driver")
        self.assertEqual(pod["spec"]["nodeName"], "board-1")
        self.assertEqual(pod["metadata"]["labels"]["location"], "kitchen")
        self.assertEqual(pod["metadata"]["annotations"]["kube32.io/i2c-device"], "bus=0,addr=0x77,mux=0x70:2")

        # The app's own Service: drivers by Device labels
        r = self.api.post("/api/v1/namespaces/default/services", {
            "apiVersion": "v1", "kind": "Service", "metadata": {"name": "kitchen-air"},
            "spec": {"selector": {"kube32.io/driver": "bme688", "location": "kitchen"}, "ports": [{"port": 80, "targetPort": "http"}]}})
        self.assertEqual(r.status_code, 201, r.text)
        # The app, on the laptop
        r = self.api.post("/api/v1/namespaces/default/pods", {
            "apiVersion": "v1", "kind": "Pod", "metadata": {"name": "app", "labels": {"app": "probe"}},
            "spec": {"nodeSelector": {"kubernetes.io/hostname": "laptop"},
                     "containers": [{"name": "probe", "image": "wasm://sd/svcprobe", "ports": [{"name": "http", "containerPort": 7000}],
                                     "env": [{"name": "HOSTNAME", "value": "app"}],
                                     "resources": {"requests": {"memory": "16Ki"}, "limits": {"memory": "16Ki"}},
                                     "securityContext": {"capabilities": {"add": ["SERVICE_CALL"]}}}]}})
        self.assertEqual(r.status_code, 201, r.text)
        self.wait_for_condition(lambda: (self._get("/api/v1/namespaces/default/pods/app") or {}).get("status", {}).get("phase") == "Running",
                                timeout=20.0, description="app Running on the laptop")
        self.assertEqual(self._get("/api/v1/namespaces/default/pods/app")["spec"]["nodeName"], "laptop")

        for url in (f"http://{self.DEVICE}/", "http://kitchen-air/", f"http://{self.DEVICE}.default.svc/"):
            answer = self.wait_for_condition(lambda url=url: (lambda a: a if a.get("rc") == 200 else None)(self._probe(url)),
                                             timeout=15.0, description=f"the app's call to {url}")
            self.assertIn('"driver":"stub"', answer["body"])

        # Board to laptop: a Service for the app and an Ingress on board-1. The
        # endpoint is the laptop's pod server, not the containerPort
        r = self.api.post("/api/v1/namespaces/default/services", {
            "apiVersion": "v1", "kind": "Service", "metadata": {"name": "probe"},
            "spec": {"selector": {"app": "probe"}, "ports": [{"name": "http", "port": 80, "targetPort": "http"}]}})
        self.assertEqual(r.status_code, 201, r.text)
        ep = self.wait_for_condition(lambda: (lambda e: e if e and e.get("subsets") else None)(self._get("/api/v1/namespaces/default/endpoints/probe")),
                                     timeout=10.0, description="probe endpoints")
        self.assertEqual([(a["ip"], p["port"]) for s in ep["subsets"] for a in s["addresses"] for p in s["ports"]],
                         [("127.0.0.1", self.LAPTOP_POD_PORT)])
        r = self.api.post("/apis/networking.k8s.io/v1/namespaces/default/ingresses", {
            "apiVersion": "networking.k8s.io/v1", "kind": "Ingress", "metadata": {"name": "probe"},
            "spec": {"rules": [{"http": {"paths": [{"path": "/hello", "pathType": "Prefix",
                                                    "backend": {"service": {"name": "probe", "port": {"name": "http"}}}}]}}]}})
        self.assertEqual(r.status_code, 201, r.text)
        body = self.wait_for_condition(lambda: self._ingress("/hello"), timeout=15.0, description="the app through board-1's Ingress")
        self.assertEqual(body, "hello from app\n")

        # Whatever happens, the driver never leaves the board with the part
        self.assertEqual(self._get(f"/api/v1/namespaces/default/pods/{self.DEVICE}-driver")["spec"]["nodeName"], "board-1")
