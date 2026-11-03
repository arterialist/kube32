"""
Kube32 Cluster Tests: the examples on nodes with board-like memory
Two kube32-sim processes: "devkit" runs the control plane and its kubelet
with the memory of a board with PSRAM (the 1284 KiB pool block and 448 KiB of
PSRAM besides it), "cardputer" is a worker modelled as a board without PSRAM
(the 80 KiB pool block and a 51 KiB heap). The nodes advertise the memory
their kubelets can ever give pods, the figures stay put while pods start,
kubectl apply of the examples binds every pod to the node its manifest
selects, while the oversized example stays Pending, and the node's
kube32.io/memory-free annotation follows what the running pods take.
"""

import os
import shutil
import subprocess
import time

from tests.integration.framework import BaseTestCase, HTTPClient, skip_missing
from tests.integration.cluster_harness import BIN_PATH, PROJECT_ROOT, SimulatedNode, node_ready_condition

KIB = 1024


def kib(quantity: str) -> int:
    for suffix, scale in (("Ki", 1), ("Mi", 1024), ("Gi", 1024 * 1024)):
        if quantity.endswith(suffix):
            return int(quantity[:-len(suffix)]) * scale
    return int(quantity) // KIB


class TestExamplePlacement(BaseTestCase):
    API_PORT = 16843
    POD_PORTS = (17810, 17811)

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
        d = f"/tmp/k32_placement_{name}_{os.getpid()}_{int(time.time() * 1000) % 100000}"
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(os.path.join(d, "card", "kube32"))
        self.dirs.append(d)
        return d

    def _start(self, name: str, args, data_dir: str):
        env = dict(os.environ, KUBE32_SD_ROOT=os.path.join(data_dir, "card"))
        log = open(os.path.join(data_dir, "sim.log"), "w")
        node = SimulatedNode("standalone", name, 0, self.API_PORT, 0, data_dir)
        node.process = subprocess.Popen([BIN_PATH] + args + ["--data-dir", data_dir, "--lease-interval", "1000"],
                                        stdout=log, stderr=subprocess.STDOUT, env=env)
        self.nodes.append(node)

    def _get(self, path: str):
        r = self.api.get(path)
        return r.json() if r.status_code == 200 else None

    def _kubectl(self, *args):
        # A kubeconfig of its own: nothing of the user's is read
        config = os.path.join(self.dirs[0], "kubeconfig")
        with open(config, "w") as f:
            f.write("apiVersion: v1\nkind: Config\nclusters: []\ncontexts: []\nusers: []\n")
        env = dict(os.environ, KUBECONFIG=config)
        return subprocess.run(["kubectl", f"--server=http://127.0.0.1:{self.API_PORT}"] + list(args),
                              capture_output=True, text=True, timeout=60, env=env)

    def _memory(self, node: str):
        """(list, resource) -> KiB of the node's memory and PSRAM in capacity and allocatable"""
        status = self._get(f"/api/v1/nodes/{node}")["status"]
        return {(list_, k): kib(v) for list_ in ("capacity", "allocatable") for k, v in status.get(list_, {}).items()
                if k in ("memory", "esp32.io/psram")}

    def _memory_free(self, node: str) -> int:
        """KiB the node can give pods now (its kube32.io/memory-free annotation)"""
        annotations = self._get(f"/api/v1/nodes/{node}")["metadata"].get("annotations", {})
        return kib(annotations.get("kube32.io/memory-free", "0"))

    def _pod_node(self, prefix: str) -> str:
        pods = self._get("/api/v1/namespaces/default/pods")["items"]
        names = [p for p in pods if p["metadata"]["name"] == prefix or p["metadata"]["name"].startswith(prefix + "-")]
        return names[0]["spec"].get("nodeName", "") if names else ""

    def test_examples_schedule_onto_their_nodes(self):
        """echo, webhook and Tessera land where their nodeSelectors point; nodes advertise what pods can be given"""
        devkit_dir = self._dir("devkit")
        cardputer_dir = self._dir("cardputer")
        self._start("devkit", ["--role", "standalone", "--workers", "", "--node-name", "cp-0", "--kubelet-name", "devkit",
                               "--port", str(self.API_PORT), "--ipc-port", "17800", "--pod-port", str(self.POD_PORTS[0]),
                               "--psram", "1284Ki", "--heap-budget", "448Ki", "--reconcile-interval", "500"], devkit_dir)
        self.wait_for_condition(lambda: self._get("/version") is not None, timeout=10.0, description="API server")
        self._start("cardputer", ["--role", "worker", "--node-name", "cardputer", "--node-id", "1", "--ipc-port", "17801",
                                  "--join", f"http://127.0.0.1:{self.API_PORT}", "--pod-port", str(self.POD_PORTS[1]),
                                  "--board", "cardputer", "--psram", "0"], cardputer_dir)
        for name in ("devkit", "cardputer"):
            self.wait_for_condition(lambda name=name: (node_ready_condition(self.api.base_url, name) or {}).get("status") == "True",
                                    timeout=15.0, description=f"{name} Ready")

        # The pool block and the PSRAM above its 16 KiB reserve, for memory and
        # for esp32.io/psram alike (a pod's memory is all PSRAM there); without
        # PSRAM the pool block and the heap above the 24 KiB reserve
        devkit = self._memory("devkit")
        self.assertEqual(devkit[("allocatable", "memory")], 1284 + 448 - 16, f"devkit: {devkit}")
        self.assertEqual(devkit[("allocatable", "esp32.io/psram")], 1284 + 448 - 16, f"devkit: {devkit}")
        self.assertEqual(devkit[("capacity", "esp32.io/psram")], 1284 + 448, f"devkit: {devkit}")
        self.assertTrue(devkit[("capacity", "memory")] >= devkit[("allocatable", "memory")], f"devkit: {devkit}")
        self.assertEqual(self._memory_free("devkit"), 1284 + 448 - 16)
        cardputer = self._memory("cardputer")
        self.assertEqual(cardputer[("allocatable", "memory")], 80 + 51 - 24, f"cardputer: {cardputer}")
        self.assertEqual(cardputer[("capacity", "memory")], 80 + 51, f"cardputer: {cardputer}")
        self.assertFalse(("allocatable", "esp32.io/psram") in cardputer, f"cardputer: {cardputer}")
        # Heartbeats keep the figures
        time.sleep(2.5)
        self.assertEqual(self._memory("devkit"), devkit)
        self.assertEqual(self._memory("cardputer"), cardputer)

        if shutil.which("kubectl") is None:
            skip_missing("kubectl not found: the examples are not applied")
            return
        examples = os.path.join(PROJECT_ROOT, "examples")
        files = ["echo.yaml", "webhook.yaml", "tessera.yaml", "oversized-pod.yaml"]
        args = ["apply"]
        for f in files:
            args += ["-f", os.path.join(examples, f)]
        r = self._kubectl(*args)
        self.assertEqual(r.returncode, 0, f"kubectl apply: {r.stdout}{r.stderr}")

        want = {"echo": "devkit", "webhook": "devkit", "tessera-api": "devkit", "tessera-web": "devkit",
                "tessera-db": "cardputer", "tessera-hud": "cardputer"}
        self.wait_for_condition(lambda: all(self._pod_node(p) for p in want), timeout=20.0, description="the examples' pods bound")
        self.assertEqual({p: self._pod_node(p) for p in want}, want)
        # The kubelet admits what the scheduler bound: echo runs on the control plane
        self.wait_for_condition(lambda: (self._get("/api/v1/namespaces/default/pods/echo") or {}).get("status", {}).get("phase") == "Running",
                                timeout=20.0, description="echo Running")
        # Allocatable stays while pods run; what is free now goes down
        self.assertEqual(self._memory("devkit"), devkit)
        self.wait_for_condition(lambda: self._memory_free("devkit") < 1284 + 448 - 16, timeout=10.0,
                                description="devkit memory-free below allocatable")

        oversized = self._get("/api/v1/namespaces/default/pods/oversized-pod")
        self.assertEqual(oversized["spec"].get("nodeName", ""), "")
        cond = next(c for c in oversized["status"]["conditions"] if c["type"] == "PodScheduled")
        self.assertEqual((cond["status"], cond["reason"]), ("False", "Unschedulable"))
        self.assertIn("Insufficient memory", cond["message"])

    def test_system_reserved_is_left_out_of_allocatable(self):
        """--system-reserved keeps memory back for the control plane: allocatable is capacity less that figure, and the Node reports it"""
        devkit_dir = self._dir("reserved")
        self._start("devkit", ["--role", "standalone", "--workers", "", "--node-name", "cp-0", "--kubelet-name", "devkit",
                               "--port", str(self.API_PORT), "--ipc-port", "17800", "--pod-port", str(self.POD_PORTS[0]),
                               "--psram", "1284Ki", "--heap-budget", "448Ki", "--system-reserved", "192Ki",
                               "--reconcile-interval", "500"], devkit_dir)
        self.wait_for_condition(lambda: self._get("/version") is not None, timeout=10.0, description="API server")
        self.wait_for_condition(lambda: (node_ready_condition(self.api.base_url, "devkit") or {}).get("status") == "True",
                                timeout=15.0, description="devkit Ready")
        memory = self._memory("devkit")
        self.assertEqual(memory[("allocatable", "memory")], 1284 + 448 - 192, f"devkit: {memory}")
        self.assertEqual(memory[("allocatable", "esp32.io/psram")], 1284 + 448 - 192, f"devkit: {memory}")
        self.assertEqual(memory[("capacity", "esp32.io/psram")], 1284 + 448, f"devkit: {memory}")
        annotations = self._get("/api/v1/nodes/devkit")["metadata"].get("annotations", {})
        self.assertEqual(kib(annotations.get("kube32.io/system-reserved", "0")), 192)
        self.assertEqual(self._memory_free("devkit"), 1284 + 448 - 192)
