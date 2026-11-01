"""
Kube32 Cluster Tests: Pod Migration
A stateful request pod moves between two worker processes by the manual flow:
a checkpoint through the node proxy that leaves the pod frozen, the snapshot
stored as a Checkpoint's data, the old pod deleted, a new one created on the
other node with kube32.io/restore-from. Its request count continues there.
Also: blocked checkpoints, a failed restore, interpreter speed annotations,
and a worker leaving the cluster on SIGINT.
"""

import json
import signal
import time
import urllib.error
import urllib.request

from tests.integration.framework import BaseTestCase
from tests.integration.cluster_harness import ClusterHarness


def http(method, url, body=None, headers=None, timeout=15.0):
    """(status, headers, body bytes) of one request; HTTP errors are answers too"""
    data = body if isinstance(body, (bytes, type(None))) else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    if data is not None and "Content-Type" not in (headers or {}):
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def echo_pod(name, node, annotations=None):
    return {
        "apiVersion": "v1", "kind": "Pod",
        "metadata": {"name": name, "namespace": "default", "annotations": annotations or {}},
        "spec": {"nodeName": node, "restartPolicy": "Always",
                 "containers": [{"name": "echo", "image": "wasm://embedded/echo"}]},
    }


class TestPodMigration(BaseTestCase):
    def setUp(self):
        super().setUp()
        self.harness = ClusterHarness(base_api_port=16943, base_ipc_port=17900)

    def tearDown(self):
        self.harness.teardown()
        super().tearDown()

    def _pod(self, cp, name):
        status, _, body = http("GET", f"{cp.api_url}/api/v1/namespaces/default/pods/{name}")
        return json.loads(body) if status == 200 else None

    def _running(self, cp, name):
        pod = self._pod(cp, name)
        return pod if pod and pod.get("status", {}).get("phase") == "Running" else None

    def _ready(self, cp, name):
        pod = self._pod(cp, name) or {}
        conds = pod.get("status", {}).get("conditions", [])
        return next((c.get("status") for c in conds if c.get("type") == "Ready"), None)

    def _requests(self, worker, name):
        """The echo pod's request count, from one request to its node's pod server"""
        status, headers, body = http("GET", f"http://127.0.0.1:{worker.pod_port}/count",
                                     headers={"X-Kube32-Pod": f"default/{name}"})
        if status != 200:
            return status, headers
        return 200, json.loads(body)["requests"]

    def test_manual_move_continues_state(self):
        """echo moves from one worker to another; its count and clock continue"""
        cp = self.harness.start_control_plane(name="cp-move", lease_interval_ms=500, reconcile_interval_ms=200)
        a = self.harness.start_worker("worker-move-a", 1, cp, lease_interval_ms=500, pod_port=17931)
        b = self.harness.start_worker("worker-move-b", 2, cp, lease_interval_ms=500, pod_port=17932)
        api = cp.api_url

        # Each node reports its interpreter speed
        for w in (a, b):
            def mops(w=w):
                _, _, body = http("GET", f"{api}/api/v1/nodes/{w.name}")
                v = json.loads(body).get("metadata", {}).get("annotations", {}).get("kube32.io/interp-mops")
                return float(v) if v else None
            value = self.wait_for_condition(mops, timeout=10.0, description=f"interp-mops on {w.name}")
            self.assertTrue(value > 1.0, f"{w.name} reports {value} Mops")

        status, _, _ = http("POST", f"{api}/api/v1/namespaces/default/pods", echo_pod("echo", a.name))
        self.assertEqual(status, 201)
        self.wait_for_condition(lambda: self._running(cp, "echo"), timeout=15.0, description="echo Running on worker a")
        for n in range(1, 4):
            self.assertEqual(self._requests(a, "echo"), (200, n))

        # The checkpoint, through the node proxy, leaves the pod frozen
        status, _, _ = http("POST", f"{api}/apis/kube32.io/v1/namespaces/default/checkpoints",
                            {"apiVersion": "kube32.io/v1", "kind": "Checkpoint", "metadata": {"name": "echo-move"},
                             "spec": {"podName": "echo", "action": "Move", "target": {"nodeName": b.name}}})
        self.assertEqual(status, 201)
        t0 = time.time()
        status, headers, snap = http("POST", f"{api}/api/v1/nodes/{a.name}/proxy/checkpoint/default/echo/echo?leave=frozen&timeoutMs=5000")
        self.assertEqual(status, 200, snap[:200])
        self.assertEqual(snap[:4], b"K32S")
        self.assertEqual(int(headers["Content-Length"]), len(snap))
        self.assertIn("X-Kube32-Snapshot-Sha256", headers)
        pod_time = int(headers["X-Kube32-Pod-Time-Ms"])

        # Frozen: 503 with Retry-After, and not Ready
        status, h = self._requests(a, "echo")
        self.assertEqual(status, 503)
        self.assertEqual(h.get("Retry-After"), "1")
        self.wait_for_condition(lambda: self._ready(cp, "echo") == "False", timeout=8.0, description="echo not Ready while frozen")

        # The snapshot becomes the Checkpoint's data
        status, _, body = http("PUT", f"{api}/apis/kube32.io/v1/namespaces/default/checkpoints/echo-move/data", snap,
                               headers={"Content-Type": "application/octet-stream"})
        self.assertEqual(status, 200, body[:200])
        ckpt = json.loads(body)
        self.assertEqual(ckpt["status"]["sizeBytes"], len(snap))
        self.assertEqual(ckpt["status"]["snapshotSha256"], headers["X-Kube32-Snapshot-Sha256"])
        status, _, table = http("GET", f"{api}/apis/kube32.io/v1/namespaces/default/checkpoints",
                                headers={"Accept": "application/json;as=Table;v=v1;g=meta.k8s.io"})
        names = [c["name"] for c in json.loads(table)["columnDefinitions"]]
        for col in ("NAME", "POD", "ACTION", "PHASE", "NODE", "SIZE", "AGE"):
            self.assertIn(col, names)

        # The old pod goes, the new one restores on the other worker
        status, _, _ = http("DELETE", f"{api}/api/v1/namespaces/default/pods/echo")
        self.assertIn(status, (200, 202))
        self.wait_for_condition(lambda: self._pod(cp, "echo") is None, timeout=15.0, description="old echo gone")
        status, _, _ = http("POST", f"{api}/api/v1/namespaces/default/pods",
                            echo_pod("echo", b.name, {"kube32.io/restore-from": "echo-move"}))
        self.assertEqual(status, 201)
        self.wait_for_condition(lambda: self._running(cp, "echo"), timeout=15.0, description="echo Running on worker b")
        status, count = self._requests(b, "echo")
        hop_ms = int((time.time() - t0) * 1000)
        self.assertEqual((status, count), (200, 4))
        # Its clock went on from the snapshot's
        _, _, body = http("GET", f"http://127.0.0.1:{b.pod_port}/count", headers={"X-Kube32-Pod": "default/echo"})
        uptime = json.loads(body)["uptime_ms"]
        self.assertTrue(pod_time <= uptime < pod_time + 30000, f"pod clock {uptime} after a snapshot at {pod_time}")
        print(f"        moved in {hop_ms} ms (freeze to the first answer on the new node), {len(snap)} byte snapshot")

        # A pod that cannot move is refused with the reason
        status, _, _ = http("POST", f"{api}/api/v1/namespaces/default/pods",
                            echo_pod("pinned", a.name, {"kube32.io/migration": "Never"}))
        self.assertEqual(status, 201)
        self.wait_for_condition(lambda: self._running(cp, "pinned"), timeout=15.0, description="pinned Running")
        status, _, body = http("POST", f"{api}/api/v1/nodes/{a.name}/proxy/checkpoint/default/pinned/echo")
        self.assertEqual(status, 409)
        self.assertEqual(json.loads(body), {"reason": "NotMovable", "blockers": ["annotation kube32.io/migration: Never"]})
        self.assertEqual(self._requests(a, "pinned"), (200, 1))

        # A restore without data fails the container with RestoreError
        http("POST", f"{api}/apis/kube32.io/v1/namespaces/default/checkpoints",
             {"apiVersion": "kube32.io/v1", "kind": "Checkpoint", "metadata": {"name": "empty"}, "spec": {"podName": "lost"}})
        status, _, _ = http("POST", f"{api}/api/v1/namespaces/default/pods",
                            echo_pod("lost", b.name, {"kube32.io/restore-from": "empty"}))
        self.assertEqual(status, 201)

        def restore_error():
            pod = self._pod(cp, "lost") or {}
            for cs in pod.get("status", {}).get("containerStatuses", []):
                term = cs.get("state", {}).get("terminated", {})
                if term.get("reason") == "RestoreError":
                    return term
            return None
        term = self.wait_for_condition(restore_error, timeout=15.0, description="RestoreError")
        self.assertIn("has no data", term.get("message", ""))
        self.assertEqual(self._pod(cp, "lost")["status"]["phase"], "Failed")

    def test_worker_leaves_on_sigint(self):
        """A worker cordons itself on SIGINT, asks for evictions and exits"""
        cp = self.harness.start_control_plane(name="cp-leave", lease_interval_ms=500, reconcile_interval_ms=200)
        w = self.harness.start_worker("worker-leave", 1, cp, lease_interval_ms=500, pod_port=17941)
        api = cp.api_url
        http("POST", f"{api}/api/v1/namespaces/default/pods", echo_pod("echo", w.name))
        self.wait_for_condition(lambda: self._running(cp, "echo"), timeout=15.0, description="echo Running")

        t0 = time.time()
        w.process.send_signal(signal.SIGINT)
        w.process.wait(timeout=25.0)
        self.assertTrue(time.time() - t0 < 25.0)
        _, _, body = http("GET", f"{api}/api/v1/nodes/{w.name}")
        node = json.loads(body)
        self.assertTrue(node.get("spec", {}).get("unschedulable"), "the node cordoned itself")
        self.assertEqual(node["metadata"].get("annotations", {}).get("kube32.io/left"), "true")
        out = w.process.stdout.read().decode(errors="replace")
        self.assertIn("[LEAVE]", out)

        # Started again, it is schedulable
        self.harness.restart_node(w.name, cp, lease_interval_ms=500)
        def schedulable():
            _, _, body = http("GET", f"{api}/api/v1/nodes/{w.name}")
            return not json.loads(body).get("spec", {}).get("unschedulable")
        self.wait_for_condition(schedulable, timeout=10.0, description="node schedulable after restart")
