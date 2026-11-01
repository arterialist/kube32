"""
Kube32 Cluster Tests: kubectl cordon, uncordon and drain
A control plane and two kube32-sim workers on 127.0.0.1, plus two nodes
played by this test ("fake-a", "fake-b"): each registers a Node, renews its
Lease, reports the pods bound to it Running, finalizes deleted ones, and
answers the kubelet's checkpoint and thaw endpoints (docs/migration.md) with
fixed bytes. The real kubectl binary cordons, uncordons and drains nodes
with --ignore-daemonsets.

Draining a fake node: the Deployment's pod and a bare pod (--force) can move,
so their eviction becomes a Move. With the API server's checkpoint data
served they move to the other fake node with their names, the Deployment
counting one pod; without it the move fails at the upload, the source thaws
the pod and the eviction deletes it. A pod with kube32.io/migration: Never
is deleted, a DaemonSet pod stays. Draining a kube32-sim worker leaves the
Deployment's pod running on the other worker.
"""

import hashlib
import os
import shutil
import subprocess
import tempfile
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from tests.integration.framework import BaseTestCase, HTTPClient
from tests.integration.cluster_harness import BIN_PATH, SimulatedNode, node_ready_condition

API_PORT = 16943
# Stand-in state with the K32S trailer: the SHA-256 of the bytes before it
_STATE = b"K32S" + bytes(range(60))
SNAPSHOT = _STATE + hashlib.sha256(_STATE).digest()


class FakeKubelet:
    """A node without a runtime: the API calls a kubelet makes, and its checkpoint endpoints."""

    def __init__(self, api: HTTPClient, name: str, port: int):
        self.api, self.name, self.port = api, name, port
        self.requests = []
        self.lock = threading.Lock()
        self.running = True
        fake = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                with fake.lock:
                    fake.requests.append(self.path)
                if self.path.startswith("/checkpoint/"):
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("X-Kube32-Image-Sha256", "a" * 64)
                    self.send_header("X-Kube32-Snapshot-Sha256", "b" * 64)
                    self.send_header("X-Kube32-Pod-Time-Ms", "1234")
                    self.send_header("Content-Length", str(len(SNAPSHOT)))
                    self.end_headers()
                    self.wfile.write(SNAPSHOT)
                elif self.path.startswith("/thaw/"):
                    self.send_response(200)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                else:
                    self.send_response(404)
                    self.send_header("Content-Length", "0")
                    self.end_headers()

        self.server = ThreadingHTTPServer(("127.0.0.1", port), Handler)

    def start(self):
        node = {
            "apiVersion": "v1", "kind": "Node",
            "metadata": {"name": self.name, "labels": {"kubernetes.io/hostname": self.name, "kube32.io/test-node": "fake"}},
            "status": {
                "conditions": [{"type": "Ready", "status": "True"}],
                "addresses": [{"type": "InternalIP", "address": "127.0.0.1"}],
                "daemonEndpoints": {"kubeletEndpoint": {"Port": self.port}},
                "capacity": {"memory": "1Mi"}, "allocatable": {"memory": "1Mi"},
            },
        }
        self.api.post("/api/v1/nodes", body=node)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        threading.Thread(target=self.loop, daemon=True).start()

    def stop(self):
        self.running = False
        self.server.shutdown()
        self.server.server_close()

    def seen(self, prefix: str):
        with self.lock:
            return [p for p in self.requests if p.startswith(prefix)]

    def loop(self):
        lease_path = "/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/" + self.name
        selector = urllib.parse.quote("spec.nodeName=" + self.name)
        while self.running:
            try:
                lease = {"apiVersion": "coordination.k8s.io/v1", "kind": "Lease",
                         "metadata": {"name": self.name, "namespace": "kube-node-lease"},
                         "spec": {"holderIdentity": self.name, "leaseDurationSeconds": 15,
                                  "renewTime": time.strftime("%Y-%m-%dT%H:%M:%S.000000Z", time.gmtime()) + str(time.time())[-3:]}}
                if self.api.put(lease_path, body=lease).status_code == 404:
                    self.api.post("/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases", body=lease)
                pods = self.api.get("/api/v1/pods?fieldSelector=" + selector).json().get("items", [])
                for p in pods:
                    meta, st = p["metadata"], p.get("status", {})
                    path = "/api/v1/namespaces/%s/pods/%s" % (meta.get("namespace", "default"), meta["name"])
                    if meta.get("deletionTimestamp"):
                        self.api.delete(path + "?gracePeriodSeconds=0")
                    elif st.get("phase") != "Running" or not st.get("containerStatuses"):
                        status = {"status": {"phase": "Running", "podIP": "127.0.0.1",
                                             "conditions": [{"type": "Ready", "status": "True"}],
                                             "containerStatuses": [{"name": c["name"], "ready": True, "started": True, "restartCount": 0,
                                                                    "state": {"running": {}}} for c in p["spec"]["containers"]]}}
                        self.api.patch(path + "/status", body=status, headers={"Content-Type": "application/merge-patch+json"})
            except Exception:
                pass
            time.sleep(0.3)


class TestDrain(BaseTestCase):
    def setUp(self):
        super().setUp()
        self.procs, self.dirs, self.fakes = [], [], []
        self.api = HTTPClient(f"http://127.0.0.1:{API_PORT}", default_timeout=5.0)
        self.tmp = tempfile.mkdtemp(prefix="k32_drain_")
        self.dirs.append(self.tmp)
        # kubectl talks to this cluster only: its own kubeconfig, the server named on every call
        self.kubeconfig = os.path.join(self.tmp, "kubeconfig")
        with open(self.kubeconfig, "w") as f:
            f.write("apiVersion: v1\nkind: Config\ncurrent-context: drain\n"
                    f"clusters:\n- name: drain\n  cluster:\n    server: http://127.0.0.1:{API_PORT}\n"
                    "users:\n- name: drain\n  user: {}\n"
                    "contexts:\n- name: drain\n  context:\n    cluster: drain\n    user: drain\n")

    def tearDown(self):
        for f in self.fakes:
            f.stop()
        for p in self.procs:
            p.terminate()
        for d in self.dirs:
            shutil.rmtree(d, ignore_errors=True)
        super().tearDown()

    def _sim(self, name: str, args):
        d = os.path.join(self.tmp, name)
        os.makedirs(d)
        log = open(os.path.join(d, "sim.log"), "w")
        node = SimulatedNode("sim", name, 0, API_PORT, 0, d)
        node.process = subprocess.Popen([BIN_PATH, "--node-name", name, "--data-dir", d] + args, stdout=log, stderr=subprocess.STDOUT)
        self.procs.append(node)
        return node

    def _kubectl(self, *args, timeout=150):
        cmd = ["kubectl", "--kubeconfig", self.kubeconfig, "--context", "drain", f"--server=http://127.0.0.1:{API_PORT}"] + list(args)
        return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout,
                              env=dict(os.environ, KUBECONFIG=self.kubeconfig))

    def _pods(self, prefix: str = ""):
        items = self.api.get("/api/v1/namespaces/default/pods").json().get("items", [])
        return [p for p in items if p["metadata"]["name"].startswith(prefix)]

    def _pod(self, name: str):
        r = self.api.get("/api/v1/namespaces/default/pods/" + name)
        return r.json() if r.status_code == 200 else None

    def _unschedulable(self, node: str) -> bool:
        return bool(self.api.get("/api/v1/nodes/" + node).json().get("spec", {}).get("unschedulable", False))

    def _deployment(self, name: str, selector, annotations=None):
        tmpl_meta = {"labels": {"app": name}}
        if annotations:
            tmpl_meta["annotations"] = annotations
        body = {"apiVersion": "apps/v1", "kind": "Deployment", "metadata": {"name": name, "namespace": "default"},
                "spec": {"replicas": 1, "selector": {"matchLabels": {"app": name}},
                         "template": {"metadata": tmpl_meta, "spec": {"nodeSelector": selector, "containers": [
                             {"name": "app", "image": "wasm://embedded/echo", "resources": {"requests": {"memory": "16Ki"}}}]}}}}
        self.assertEqual(self.api.post("/apis/apps/v1/namespaces/default/deployments", body=body).status_code, 201)

    def _bare(self, name: str, node: str, annotations=None):
        body = {"apiVersion": "v1", "kind": "Pod", "metadata": {"name": name, "namespace": "default", "annotations": annotations or {}},
                "spec": {"nodeName": node, "containers": [{"name": "app", "image": "wasm://embedded/echo",
                                                           "resources": {"requests": {"memory": "16Ki"}}}]}}
        self.assertEqual(self.api.post("/api/v1/namespaces/default/pods", body=body).status_code, 201)

    def _running_on(self, prefix: str, node: str):
        for p in self._pods(prefix):
            if p["spec"].get("nodeName") == node and p.get("status", {}).get("phase") == "Running" and not p["metadata"].get("deletionTimestamp"):
                return p
        return None

    def test_cordon_uncordon_drain(self):
        if shutil.which("kubectl") is None:
            print("[SKIP] kubectl not found")
            return
        self._sim("cp-drain", ["--role", "control-plane", "--node-id", "0", "--port", str(API_PORT), "--ipc-port", "17940",
                               "--lease-interval", "500", "--not-ready-timeout", "4000", "--eviction-timeout", "120000",
                               "--reconcile-interval", "200"])
        self.wait_for_condition(lambda: self.api.get("/version").status_code == 200, timeout=10.0, description="API server")
        for i, name in ((1, "drain-w1"), (2, "drain-w2")):
            self._sim(name, ["--role", "worker", "--node-id", str(i), "--port", str(API_PORT + i), "--ipc-port", str(17940 + i),
                             "--pod-port", str(17950 + i), "--join", f"http://127.0.0.1:{API_PORT}", "--lease-interval", "500"])
        for i, name in ((1, "fake-a"), (2, "fake-b")):
            fake = FakeKubelet(self.api, name, 17960 + i)
            fake.start()
            self.fakes.append(fake)
        for n in ("drain-w1", "drain-w2", "fake-a", "fake-b"):
            self.wait_for_condition(lambda n=n: (node_ready_condition(self.api.base_url, n) or {}).get("status") == "True",
                                    timeout=15.0, description=f"{n} Ready")

        # cordon and uncordon
        r = self._kubectl("cordon", "fake-b")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(self._unschedulable("fake-b"))
        r = self._kubectl("uncordon", "fake-b")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(self._unschedulable("fake-b"))

        # Workloads on the fake nodes
        self._deployment("calc", {"kube32.io/test-node": "fake"})
        calc = self.wait_for_condition(lambda: next((p for p in self._pods("calc-") if p.get("status", {}).get("phase") == "Running"), None),
                                       timeout=20.0, description="calc Running")
        src = calc["spec"]["nodeName"]
        dst = "fake-b" if src == "fake-a" else "fake-a"
        self._bare("solo", src)
        self._bare("keep", src, {"kube32.io/migration": "Never"})
        ds = {"apiVersion": "apps/v1", "kind": "DaemonSet", "metadata": {"name": "agent", "namespace": "default"},
              "spec": {"selector": {"matchLabels": {"app": "agent"}},
                       "template": {"metadata": {"labels": {"app": "agent"}},
                                    "spec": {"nodeSelector": {"kube32.io/test-node": "fake"},
                                             "containers": [{"name": "app", "image": "wasm://embedded/echo"}]}}}}
        self.assertEqual(self.api.post("/apis/apps/v1/namespaces/default/daemonsets", body=ds).status_code, 201)
        for name in ("solo", "keep"):
            self.wait_for_condition(lambda name=name: (self._pod(name) or {}).get("status", {}).get("phase") == "Running",
                                    timeout=10.0, description=f"{name} Running")
        self.wait_for_condition(lambda: self._running_on("agent-", src), timeout=15.0, description="DaemonSet pod on " + src)
        calc_name, calc_uid = calc["metadata"]["name"], calc["metadata"]["uid"]
        solo_uid = self._pod("solo")["metadata"]["uid"]

        r = self._kubectl("drain", src, "--ignore-daemonsets", "--force", "--timeout=120s")
        self.assertEqual(r.returncode, 0, f"kubectl drain {src}: {r.stdout}{r.stderr}")
        self.assertIn("drained", r.stdout + r.stderr)
        self.assertTrue(self._unschedulable(src))
        fake_src = self.fakes[0] if src == "fake-a" else self.fakes[1]
        self.assertTrue(fake_src.seen(f"/checkpoint/default/{calc_name}/app?leave=frozen"), f"requests: {fake_src.requests}")
        self.assertTrue(fake_src.seen("/checkpoint/default/solo/app?leave=frozen"), f"requests: {fake_src.requests}")
        self.assertFalse(fake_src.seen("/checkpoint/default/keep/"))
        self.assertIsNone(self._pod("keep"))
        self.assertTrue(self._running_on("agent-", src), "the DaemonSet pod stays on the drained node")

        data_served = self.api.get("/apis/kube32.io/v1/namespaces/default/checkpoints").status_code == 200
        if data_served:
            # Moved with their names; the Deployment counted one pod
            moved = self.wait_for_condition(lambda: self._running_on(calc_name, dst), timeout=20.0, description="calc on " + dst)
            self.assertTrue(moved["metadata"]["uid"] != calc_uid)
            self.assertEqual(len([p for p in self._pods("calc-") if not p["metadata"].get("deletionTimestamp")]), 1)
            solo = self.wait_for_condition(lambda: self._running_on("solo", dst), timeout=20.0, description="solo on " + dst)
            self.assertTrue(solo["metadata"]["uid"] != solo_uid)
            self.assertFalse(fake_src.seen("/thaw/"))
            items = self.api.get("/apis/kube32.io/v1/namespaces/default/checkpoints").json().get("items", [])
            self.wait_for_condition(lambda: all(c.get("status", {}).get("phase") == "Completed" for c in
                                                self.api.get("/apis/kube32.io/v1/namespaces/default/checkpoints").json().get("items", [])),
                                    timeout=20.0, description="moves Completed")
            self.assertEqual(len(items), 2)
        else:
            # No checkpoint data on this API server: the upload fails, the
            # source thaws the pods and the eviction deletes them
            self.assertTrue(fake_src.seen(f"/thaw/default/{calc_name}/app"), f"requests: {fake_src.requests}")
            self.assertTrue(fake_src.seen("/thaw/default/solo/app"), f"requests: {fake_src.requests}")
            self.assertIsNone(self._pod("solo"))
            self.wait_for_condition(lambda: self._running_on("calc-", dst), timeout=20.0, description="calc replaced on " + dst)

        # Draining a kube32-sim worker: the Deployment's pod ends up on the other worker
        self._deployment("web", {"kube32.io/simulator": "true"})
        web = self.wait_for_condition(lambda: next((p for p in self._pods("web-") if p.get("status", {}).get("phase") == "Running"), None),
                                      timeout=30.0, description="web Running")
        w_src = web["spec"]["nodeName"]
        w_dst = "drain-w2" if w_src == "drain-w1" else "drain-w1"
        r = self._kubectl("drain", w_src, "--ignore-daemonsets", "--timeout=120s")
        self.assertEqual(r.returncode, 0, f"kubectl drain {w_src}: {r.stdout}{r.stderr}")
        self.wait_for_condition(lambda: self._running_on("web-", w_dst), timeout=30.0, description="web on " + w_dst)

        for node in (src, w_src):
            r = self._kubectl("uncordon", node)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertFalse(self._unschedulable(node))
