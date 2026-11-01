"""
Kube32 Cluster Tests: kubectl cordon, uncordon and drain
A control plane and two kube32-sim workers on 127.0.0.1. The real kubectl
binary cordons, uncordons and drains a worker with --ignore-daemonsets
--force. The pods that can move go to the other worker with their names and
their state: the request count of each echo pod continues there. A
Deployment counts one pod throughout. A pod with kube32.io/migration: Never
is deleted, a DaemonSet pod stays. kube32-cli move then takes the
Deployment's pod back, its count still going on.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

from tests.integration.framework import BaseTestCase, HTTPClient
from tests.integration.cluster_harness import BIN_PATH, PROJECT_ROOT, SimulatedNode, node_ready_condition

API_PORT = 17043
POD_PORTS = {"drain-w1": 18051, "drain-w2": 18052}


class TestDrain(BaseTestCase):
    def setUp(self):
        super().setUp()
        self.procs = []
        self.api = HTTPClient(f"http://127.0.0.1:{API_PORT}", default_timeout=5.0)
        self.tmp = tempfile.mkdtemp(prefix="k32_drain_")
        # kubectl talks to this cluster only: its own kubeconfig, the server named on every call
        self.kubeconfig = os.path.join(self.tmp, "kubeconfig")
        with open(self.kubeconfig, "w") as f:
            f.write("apiVersion: v1\nkind: Config\ncurrent-context: drain\n"
                    f"clusters:\n- name: drain\n  cluster:\n    server: http://127.0.0.1:{API_PORT}\n"
                    "users:\n- name: drain\n  user: {}\n"
                    "contexts:\n- name: drain\n  context:\n    cluster: drain\n    user: drain\n")
        self.flags = ["--kubeconfig", self.kubeconfig, "--context", "drain", f"--server=http://127.0.0.1:{API_PORT}"]

    def tearDown(self):
        for p in self.procs:
            p.terminate()
        shutil.rmtree(self.tmp, ignore_errors=True)
        super().tearDown()

    def _sim(self, name: str, args):
        d = os.path.join(self.tmp, name)
        os.makedirs(d)
        log = open(os.path.join(d, "sim.log"), "w")
        node = SimulatedNode("sim", name, 0, API_PORT, 0, d)
        node.process = subprocess.Popen([BIN_PATH, "--node-name", name, "--data-dir", d] + args, stdout=log, stderr=subprocess.STDOUT)
        self.procs.append(node)

    def _run(self, cmd, timeout=150):
        return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout,
                              env=dict(os.environ, KUBECONFIG=self.kubeconfig))

    def _kubectl(self, *args):
        return self._run(["kubectl"] + self.flags + list(args))

    def _pods(self, prefix: str = ""):
        items = self.api.get("/api/v1/namespaces/default/pods").json().get("items", [])
        return [p for p in items if p["metadata"]["name"].startswith(prefix) and not p["metadata"].get("deletionTimestamp")]

    def _pod(self, name: str):
        r = self.api.get("/api/v1/namespaces/default/pods/" + name)
        return r.json() if r.status_code == 200 else None

    def _unschedulable(self, node: str) -> bool:
        return bool(self.api.get("/api/v1/nodes/" + node).json().get("spec", {}).get("unschedulable", False))

    def _running_on(self, name: str, node: str):
        p = self._pod(name)
        return p if p and p["spec"].get("nodeName") == node and p.get("status", {}).get("phase") == "Running" else None

    def _count(self, name: str, node: str):
        """The echo pod's request count, from one request to its node's pod server"""
        req = urllib.request.Request(f"http://127.0.0.1:{POD_PORTS[node]}/count", headers={"X-Kube32-Pod": f"default/{name}"})
        try:
            with urllib.request.urlopen(req, timeout=5.0) as resp:
                return json.loads(resp.read())["requests"]
        except (urllib.error.URLError, OSError, ValueError, KeyError):
            return None

    def test_cordon_uncordon_drain(self):
        if shutil.which("kubectl") is None:
            print("[SKIP] kubectl not found")
            return
        self._sim("cp-drain", ["--role", "control-plane", "--node-id", "0", "--port", str(API_PORT), "--ipc-port", "18040",
                               "--lease-interval", "500", "--not-ready-timeout", "4000", "--eviction-timeout", "120000",
                               "--reconcile-interval", "200"])
        self.wait_for_condition(lambda: self.api.get("/version").status_code == 200, timeout=10.0, description="API server")
        for i, name in ((1, "drain-w1"), (2, "drain-w2")):
            self._sim(name, ["--role", "worker", "--node-id", str(i), "--port", str(API_PORT + i), "--ipc-port", str(18040 + i),
                             "--pod-port", str(POD_PORTS[name]), "--join", f"http://127.0.0.1:{API_PORT}", "--lease-interval", "500"])
        for n in POD_PORTS:
            self.wait_for_condition(lambda n=n: (node_ready_condition(self.api.base_url, n) or {}).get("status") == "True",
                                    timeout=15.0, description=f"{n} Ready")

        # cordon and uncordon
        r = self._kubectl("cordon", "drain-w2")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(self._unschedulable("drain-w2"))
        r = self._kubectl("uncordon", "drain-w2")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(self._unschedulable("drain-w2"))

        # A Deployment's pod, a bare pod, a pod that never moves and a DaemonSet, all on one worker
        dep = {"apiVersion": "apps/v1", "kind": "Deployment", "metadata": {"name": "web", "namespace": "default"},
               "spec": {"replicas": 1, "selector": {"matchLabels": {"app": "web"}},
                        "template": {"metadata": {"labels": {"app": "web"}},
                                     "spec": {"containers": [{"name": "echo", "image": "wasm://embedded/echo"}]}}}}
        self.assertEqual(self.api.post("/apis/apps/v1/namespaces/default/deployments", body=dep).status_code, 201)
        web = self.wait_for_condition(lambda: next((p for p in self._pods("web-") if p.get("status", {}).get("phase") == "Running"), None),
                                      timeout=20.0, description="web Running")
        web_name, web_uid, src = web["metadata"]["name"], web["metadata"]["uid"], web["spec"]["nodeName"]
        dst = "drain-w2" if src == "drain-w1" else "drain-w1"
        for name, ann in (("solo", {}), ("keep", {"kube32.io/migration": "Never"})):
            pod = {"apiVersion": "v1", "kind": "Pod", "metadata": {"name": name, "namespace": "default", "annotations": ann},
                   "spec": {"nodeName": src, "containers": [{"name": "echo", "image": "wasm://embedded/echo"}]}}
            self.assertEqual(self.api.post("/api/v1/namespaces/default/pods", body=pod).status_code, 201)
            self.wait_for_condition(lambda name=name: self._running_on(name, src), timeout=15.0, description=f"{name} Running")
        ds = {"apiVersion": "apps/v1", "kind": "DaemonSet", "metadata": {"name": "agent", "namespace": "default"},
              "spec": {"selector": {"matchLabels": {"app": "agent"}},
                       "template": {"metadata": {"labels": {"app": "agent"}},
                                    "spec": {"containers": [{"name": "echo", "image": "wasm://embedded/echo"}]}}}}
        self.assertEqual(self.api.post("/apis/apps/v1/namespaces/default/daemonsets", body=ds).status_code, 201)
        agent = self.wait_for_condition(lambda: next((p for p in self._pods("agent-") if p["spec"].get("nodeName") == src and
                                                      p.get("status", {}).get("phase") == "Running"), None),
                                        timeout=15.0, description="DaemonSet pod on " + src)
        for n in (1, 2, 3):
            self.assertEqual(self._count(web_name, src), n)
        self.assertEqual(self._count("solo", src), 1)

        t0 = time.time()
        r = self._kubectl("drain", src, "--ignore-daemonsets", "--force", "--timeout=120s")
        self.assertEqual(r.returncode, 0, f"kubectl drain {src}: {r.stdout}{r.stderr}")
        self.assertIn("drained", r.stdout + r.stderr)
        print(f"        kubectl drain took {time.time() - t0:.1f} s")
        self.assertTrue(self._unschedulable(src))

        # Moved with their names and their state; the Deployment counted one pod
        moved = self.wait_for_condition(lambda: self._running_on(web_name, dst), timeout=20.0, description="web on " + dst)
        self.assertTrue(moved["metadata"]["uid"] != web_uid)
        self.assertEqual(len(self._pods("web-")), 1)
        self.assertEqual(self._count(web_name, dst), 4)
        self.wait_for_condition(lambda: self._running_on("solo", dst), timeout=20.0, description="solo on " + dst)
        self.assertEqual(self._count("solo", dst), 2)
        self.wait_for_condition(lambda: self._pod("keep") is None, timeout=15.0, description="keep deleted")
        self.assertTrue(self._pod(agent["metadata"]["name"]), "the DaemonSet pod stays on the drained node")
        def moves():
            items = self.api.get("/apis/kube32.io/v1/namespaces/default/checkpoints").json().get("items", [])
            return [c for c in items if c["metadata"].get("labels", {}).get("kube32.io/trigger") == "eviction"]
        self.wait_for_condition(lambda: all(c["status"].get("phase") == "Completed" for c in moves()), timeout=10.0, description="moves Completed")
        self.assertEqual(sorted(c["spec"]["podName"] for c in moves()), sorted([web_name, "solo"]))
        for c in moves():
            self.assertTrue(c["status"].get("sizeBytes", 0) > 0)
            print(f"        {c['spec']['podName']}: {c['status'].get('message')}, {c['status']['sizeBytes']} bytes")
        # The annotation goes once the pod runs
        self.wait_for_condition(lambda: "kube32.io/restore-from" not in (self._pod(web_name) or {}).get("metadata", {}).get("annotations", {}),
                                timeout=10.0, description="restore-from removed")

        r = self._kubectl("uncordon", src)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(self._unschedulable(src))

        # kube32-cli move takes it back
        r = self._run([sys.executable, os.path.join(PROJECT_ROOT, "tools", "kube32-cli"), "move", web_name, "--to", src] + self.flags[:4] +
                      ["--server", f"http://127.0.0.1:{API_PORT}"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f"to node {src}", r.stdout)
        self.wait_for_condition(lambda: self._running_on(web_name, src), timeout=20.0, description="web back on " + src)
        self.assertEqual(self._count(web_name, src), 5)
