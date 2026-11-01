"""
Kube32 Cluster Tests: Pod Migration
A stateful request pod moves between two worker processes through a Move
Checkpoint, which the Checkpoint controller carries out: a checkpoint that
leaves the pod frozen, the snapshot stored as the Checkpoint's data, the pod
replaced on the other node with kube32.io/restore-from. Its request count
and clock continue there. Also: a freeze and thaw through the node proxy,
blocked checkpoints, a failed restore that starts the pod afresh,
interpreter speed annotations, and a worker leaving the cluster on SIGINT,
its pod moving to the worker that stays.
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

    def _events(self, cp, reason):
        _, _, body = http("GET", f"{cp.api_url}/api/v1/namespaces/default/events")
        return [e.get("message", "") for e in json.loads(body).get("items", []) if e.get("reason") == reason]

    def _checkpoint(self, cp, name):
        status, _, body = http("GET", f"{cp.api_url}/apis/kube32.io/v1/namespaces/default/checkpoints/{name}")
        return json.loads(body) if status == 200 else None

    def test_move_continues_state(self):
        """echo moves from one worker to another through a Move Checkpoint; its count and clock continue"""
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

        # Frozen through the node proxy: 503 with Retry-After, not Ready, until a thaw
        status, headers, snap = http("POST", f"{api}/api/v1/nodes/{a.name}/proxy/checkpoint/default/echo/echo?leave=frozen&timeoutMs=5000")
        self.assertEqual(status, 200, snap[:200])
        self.assertEqual(snap[:4], b"K32S")
        self.assertEqual(int(headers["Content-Length"]), len(snap))
        self.assertIn("X-Kube32-Snapshot-Sha256", headers)
        status, h = self._requests(a, "echo")
        self.assertEqual(status, 503)
        self.assertEqual(h.get("Retry-After"), "1")
        self.wait_for_condition(lambda: self._ready(cp, "echo") == "False", timeout=8.0, description="echo not Ready while frozen")
        status, _, _ = http("POST", f"{api}/api/v1/nodes/{a.name}/proxy/thaw/default/echo/echo")
        self.assertEqual(status, 200)
        self.assertEqual(self._requests(a, "echo"), (200, 4))
        uid = self._pod(cp, "echo")["metadata"]["uid"]

        # A Move Checkpoint: the Checkpoint controller takes the pod to worker b
        t0 = time.time()
        status, _, _ = http("POST", f"{api}/apis/kube32.io/v1/namespaces/default/checkpoints",
                            {"apiVersion": "kube32.io/v1", "kind": "Checkpoint", "metadata": {"name": "echo-move"},
                             "spec": {"podName": "echo", "action": "Move", "target": {"nodeName": b.name}}})
        self.assertEqual(status, 201)
        ckpt = self.wait_for_condition(lambda: (lambda c: c if c and c.get("status", {}).get("phase") in ("Completed", "Failed") else None)(
            self._checkpoint(cp, "echo-move")), timeout=20.0, description="echo-move finished")
        st = ckpt["status"]
        self.assertEqual(st["phase"], "Completed", json.dumps(st))
        self.assertEqual((st["nodeName"], st["targetNode"], st["newPodName"]), (a.name, b.name, "echo"))
        self.assertTrue(st["sizeBytes"] > 0 and len(st["snapshotSha256"]) == 64)
        moved = self.wait_for_condition(lambda: self._running(cp, "echo"), timeout=15.0, description="echo Running on worker b")
        self.assertEqual(moved["spec"]["nodeName"], b.name)
        self.assertTrue(moved["metadata"]["uid"] != uid)
        status, count = self._requests(b, "echo")
        hop_ms = int((time.time() - t0) * 1000)
        self.assertEqual((status, count), (200, 5))
        self.assertTrue(any("from node worker-move-a to node worker-move-b" in m for m in self._events(cp, "Moved")))
        # Its clock went on from the snapshot's
        _, _, body = http("GET", f"http://127.0.0.1:{b.pod_port}/count", headers={"X-Kube32-Pod": "default/echo"})
        uptime = json.loads(body)["uptime_ms"]
        self.assertTrue(st["podTimeMs"] <= uptime < st["podTimeMs"] + 30000, f"pod clock {uptime} after a snapshot at {st['podTimeMs']}")
        status, _, table = http("GET", f"{api}/apis/kube32.io/v1/namespaces/default/checkpoints",
                                headers={"Accept": "application/json;as=Table;v=v1;g=meta.k8s.io"})
        names = [c["name"] for c in json.loads(table)["columnDefinitions"]]
        for col in ("NAME", "POD", "ACTION", "PHASE", "NODE", "SIZE", "AGE"):
            self.assertIn(col, names)
        print(f"        moved in {hop_ms} ms (Checkpoint created to the first answer on the new node), {st['sizeBytes']} byte snapshot")

        # A pod that cannot move is refused with the reason
        status, _, _ = http("POST", f"{api}/api/v1/namespaces/default/pods",
                            echo_pod("pinned", a.name, {"kube32.io/migration": "Never"}))
        self.assertEqual(status, 201)
        self.wait_for_condition(lambda: self._running(cp, "pinned"), timeout=15.0, description="pinned Running")
        status, _, body = http("POST", f"{api}/api/v1/nodes/{a.name}/proxy/checkpoint/default/pinned/echo")
        self.assertEqual(status, 409)
        self.assertEqual(json.loads(body), {"reason": "NotMovable", "blockers": ["annotation kube32.io/migration: Never"]})
        self.assertEqual(self._requests(a, "pinned"), (200, 1))

        # A restore without data: RestoreError, then the pod starts afresh
        http("POST", f"{api}/apis/kube32.io/v1/namespaces/default/checkpoints",
             {"apiVersion": "kube32.io/v1", "kind": "Checkpoint", "metadata": {"name": "empty"}, "spec": {"podName": "lost"}})
        status, _, _ = http("POST", f"{api}/api/v1/namespaces/default/pods",
                            echo_pod("lost", b.name, {"kube32.io/restore-from": "empty"}))
        self.assertEqual(status, 201)
        self.wait_for_condition(lambda: any("has no data" in m and "starts afresh" in m for m in self._events(cp, "MoveFailed")),
                                timeout=15.0, description="MoveFailed for the restore without data")
        fresh = self.wait_for_condition(lambda: self._running(cp, "lost"), timeout=15.0, description="lost Running afresh")
        self.assertNotIn("kube32.io/restore-from", fresh["metadata"].get("annotations", {}))
        self.assertEqual(self._requests(b, "lost"), (200, 1))

    def test_worker_leaves_on_sigint(self):
        """A worker cordons itself on SIGINT and asks for evictions: its pod moves to the other worker with its state"""
        cp = self.harness.start_control_plane(name="cp-leave", lease_interval_ms=500, reconcile_interval_ms=200)
        w = self.harness.start_worker("worker-leave", 1, cp, lease_interval_ms=500, pod_port=17941)
        stay = self.harness.start_worker("worker-stay", 2, cp, lease_interval_ms=500, pod_port=17942)
        api = cp.api_url
        http("POST", f"{api}/api/v1/namespaces/default/pods", echo_pod("echo", w.name))
        self.wait_for_condition(lambda: self._running(cp, "echo"), timeout=15.0, description="echo Running")
        for n in (1, 2):
            self.assertEqual(self._requests(w, "echo"), (200, n))

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
        moved = self.wait_for_condition(lambda: (lambda p: p if p and p["spec"].get("nodeName") == stay.name else None)(self._running(cp, "echo")),
                                        timeout=15.0, description="echo Running on the worker that stays")
        self.assertTrue(moved)
        self.assertEqual(self._requests(stay, "echo"), (200, 3))

        # Started again, it is schedulable
        self.harness.restart_node(w.name, cp, lease_interval_ms=500)
        def schedulable():
            _, _, body = http("GET", f"{api}/api/v1/nodes/{w.name}")
            return not json.loads(body).get("spec", {}).get("unschedulable")
        self.wait_for_condition(schedulable, timeout=10.0, description="node schedulable after restart")
