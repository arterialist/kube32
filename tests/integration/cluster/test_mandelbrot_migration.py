"""
Kube32 Cluster Tests: an image rendered across nodes
The mandelbrot example (apps/mandelbrot) renders a row at a time and keeps
the image in its linear memory. It moves twice, worker a to b to c, through
Move Checkpoints while it renders. Its rows keep growing across the moves,
no row is rendered twice or lost, each row keeps the node that rendered it,
and the pod reads its node's architecture and OS from the API server. The
Service and Ingress follow the pod to each node.
"""

import base64
import json
import os
import struct
import time
import urllib.error
import urllib.request

from tests.integration.framework import BaseTestCase
from tests.integration.cluster_harness import ClusterHarness, PROJECT_ROOT

WASM = os.path.join(PROJECT_ROOT, "images", "sd", "mandelbrot.wasm")
WIDTH, HEIGHT = 64, 64
INGRESS_PORT = 18180


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


def mandelbrot_objects(node):
    """The Pod, Service and Ingress of examples/mandelbrot.yaml, small and fast"""
    with open(WASM, "rb") as f:
        module = base64.b64encode(f.read()).decode()
    env = [{"name": "API_URL", "value": "http://$(KUBERNETES_SERVICE_HOST):$(KUBERNETES_SERVICE_PORT)"},
           {"name": "POD_NAME", "valueFrom": {"fieldRef": {"fieldPath": "metadata.name"}}},
           {"name": "WIDTH", "value": str(WIDTH)}, {"name": "HEIGHT", "value": str(HEIGHT)},
           {"name": "ROWS_PER_TICK", "value": "1"}, {"name": "TICK_MS", "value": "150"},
           {"name": "MAX_ITER", "value": "64"}]
    pod = {
        "apiVersion": "v1", "kind": "Pod",
        "metadata": {"name": "mandelbrot", "namespace": "default", "labels": {"app": "mandelbrot"},
                     "annotations": {"kube32.io/wasm-base64": module, "kube32.io/migration": "OnDrain",
                                     "kube32.io/tick-interval": "100ms", "kube32.io/wasm-stack": "1Ki"}},
        "spec": {"nodeName": node, "restartPolicy": "Always",
                 "containers": [{"name": "mandelbrot", "image": "wasm://inline/mandelbrot",
                                 "ports": [{"containerPort": 7000, "name": "http"}], "env": env,
                                 "resources": {"requests": {"memory": "32Ki"}, "limits": {"memory": "32Ki"}},
                                 "securityContext": {"capabilities": {"add": ["SERVICE_CALL"]}}}]},
    }
    service = {
        "apiVersion": "v1", "kind": "Service", "metadata": {"name": "mandelbrot", "namespace": "default"},
        "spec": {"selector": {"app": "mandelbrot"}, "ports": [{"name": "http", "port": 80, "targetPort": 7000}]},
    }
    ingress = {
        "apiVersion": "networking.k8s.io/v1", "kind": "Ingress", "metadata": {"name": "mandelbrot", "namespace": "default"},
        "spec": {"rules": [{"http": {"paths": [{"path": "/mandelbrot", "pathType": "Prefix", "backend": {
            "service": {"name": "mandelbrot", "port": {"number": 80}}}}]}}]},
    }
    return pod, service, ingress


class TestMandelbrotMigration(BaseTestCase):
    def setUp(self):
        super().setUp()
        self.harness = ClusterHarness(base_api_port=17043, base_ipc_port=18100)

    def tearDown(self):
        self.harness.teardown()
        super().tearDown()

    def _pod(self, cp):
        status, _, body = http("GET", f"{cp.api_url}/api/v1/namespaces/default/pods/mandelbrot")
        return json.loads(body) if status == 200 else None

    def _running_on(self, cp, node):
        pod = self._pod(cp) or {}
        return pod if pod.get("spec", {}).get("nodeName") == node and pod.get("status", {}).get("phase") == "Running" else None

    def _status(self, worker):
        """The pod's /status, from its node's pod server"""
        status, _, body = http("GET", f"http://127.0.0.1:{worker.pod_port}/status", headers={"X-Kube32-Pod": "default/mandelbrot"})
        return json.loads(body) if status == 200 else None

    def _status_via_ingress(self):
        status, _, body = http("GET", f"http://127.0.0.1:{INGRESS_PORT}/mandelbrot/status")
        return json.loads(body) if status == 200 else None

    def _rows_at_least(self, worker, n):
        s = self._status(worker)
        return s if s and s["rows_done"] >= n else None

    def _move(self, cp, name, target):
        status, _, body = http("POST", f"{cp.api_url}/apis/kube32.io/v1/namespaces/default/checkpoints",
                               {"apiVersion": "kube32.io/v1", "kind": "Checkpoint", "metadata": {"name": name},
                                "spec": {"podName": "mandelbrot", "action": "Move", "target": {"nodeName": target}}})
        self.assertEqual(status, 201, body[:200])

        def finished():
            st, _, b = http("GET", f"{cp.api_url}/apis/kube32.io/v1/namespaces/default/checkpoints/{name}")
            c = json.loads(b) if st == 200 else {}
            return c if c.get("status", {}).get("phase") in ("Completed", "Failed") else None
        ckpt = self.wait_for_condition(finished, timeout=30.0, description=f"{name} finished")
        self.assertEqual(ckpt["status"]["phase"], "Completed", json.dumps(ckpt["status"]))
        self.wait_for_condition(lambda: self._running_on(cp, target), timeout=15.0, description=f"mandelbrot Running on {target}")

    def _node_index(self, status, node):
        names = [n["name"] for n in status["nodes"]]
        self.assertIn(node, names, json.dumps(status["nodes"]))
        i = names.index(node)
        return "0123456789abcdefghijklmnopqrstuvwxyz"[i]

    def _assert_consistent(self, before, after):
        """after continues before: no row lost, none rendered again, earlier rows keep their node"""
        self.assertGreaterEqual(after["rows_done"], before["rows_done"])
        self.assertEqual(after["rows"][:before["rows_done"]], before["rows"][:before["rows_done"]])
        self.assertEqual(after["renders"], after["rows_done"], "a row rendered twice")
        done = after["rows"][:after["rows_done"]]
        self.assertNotIn(".", done)
        self.assertEqual(after["rows"][after["rows_done"]:], "." * (after["height"] - after["rows_done"]))

    def test_image_spans_three_nodes(self):
        """mandelbrot moves a to b to c while it renders; the image keeps every row and the node that drew it"""
        self.assertTrue(os.path.exists(WASM), f"{WASM} missing: make -C apps/mandelbrot")
        cp = self.harness.start_control_plane(name="cp-mb", lease_interval_ms=500, reconcile_interval_ms=200,
                                              ingress_port=INGRESS_PORT)
        a = self.harness.start_worker("mb-worker-a", 1, cp, lease_interval_ms=500, pod_port=18131)
        b = self.harness.start_worker("mb-worker-b", 2, cp, lease_interval_ms=500, pod_port=18132)
        c = self.harness.start_worker("mb-worker-c", 3, cp, lease_interval_ms=500, pod_port=18133)
        api = cp.api_url

        pod, service, ingress = mandelbrot_objects(a.name)
        for path, obj in (("/api/v1/namespaces/default/services", service),
                          ("/apis/networking.k8s.io/v1/namespaces/default/ingresses", ingress),
                          ("/api/v1/namespaces/default/pods", pod)):
            status, _, body = http("POST", api + path, obj)
            self.assertEqual(status, 201, body[:200])
        self.wait_for_condition(lambda: self._running_on(cp, a.name), timeout=15.0, description="mandelbrot Running on worker a")

        # The pod found its node and the node's platform through the API server
        s0 = self.wait_for_condition(lambda: self._rows_at_least(a, 6), timeout=20.0, description="6 rows on worker a")
        self.assertEqual((s0["width"], s0["height"], s0["rows_total"]), (WIDTH, HEIGHT, HEIGHT))
        self.assertTrue(s0["identified"])
        self.assertEqual(s0["node"], a.name)
        _, _, body = http("GET", f"{api}/api/v1/nodes/{a.name}")
        info = json.loads(body)["status"]["nodeInfo"]
        self.assertEqual((s0["arch"], s0["os"]), (info["architecture"], info["operatingSystem"]))
        self.assertTrue(s0["arch"] and s0["os"])
        self.assertEqual(s0["moves"], 0)
        self.assertNotIn(".", s0["rows"][:s0["rows_done"]])
        self.assertEqual(set(s0["rows"][:s0["rows_done"]]), {self._node_index(s0, a.name)})

        # The Ingress reaches it on worker a, and serves the page and the image
        self.wait_for_condition(lambda: (self._status_via_ingress() or {}).get("node") == a.name, timeout=10.0,
                                description="the Ingress routes to worker a")
        status, headers, page = http("GET", f"http://127.0.0.1:{INGRESS_PORT}/mandelbrot")
        self.assertEqual(status, 200)
        self.assertIn(b'<base href="/mandelbrot/">', page)

        # Move 1: a to b, while it renders
        before = self._status(a)
        self._move(cp, "mb-move-1", b.name)
        s1 = self.wait_for_condition(lambda: self._rows_at_least(b, before["rows_done"] + 6), timeout=20.0,
                                     description="6 more rows on worker b")
        self._assert_consistent(before, s1)
        self.assertEqual((s1["node"], s1["moves"], s1["restores"]), (b.name, 1, 1))
        on_b = s1["rows"][before["rows_done"]:s1["rows_done"]]
        self.assertIn(self._node_index(s1, b.name), on_b)
        self.wait_for_condition(lambda: (self._status_via_ingress() or {}).get("node") == b.name, timeout=10.0,
                                description="the Ingress follows the pod to worker b")

        # Move 2: b to c
        before = self._status(b)
        self._move(cp, "mb-move-2", c.name)
        s2 = self.wait_for_condition(lambda: self._rows_at_least(c, before["rows_done"] + 1), timeout=20.0,
                                     description="rows on worker c")
        self._assert_consistent(before, s2)
        self.assertEqual((s2["node"], s2["moves"], s2["restores"]), (c.name, 2, 2))

        # The finished image: three runs of rows, a then b then c, each row once
        done = self.wait_for_condition(lambda: (lambda s: s if s and s["done"] else None)(self._status(c)),
                                       timeout=30.0, description="the image finished on worker c")
        self._assert_consistent(s2, done)
        self.assertEqual(done["rows_done"], HEIGHT)
        self.assertEqual(done["renders"], HEIGHT)
        ia, ib, ic = (self._node_index(done, n.name) for n in (a, b, c))
        rows = done["rows"]
        runs = [rows[0]] + [rows[i] for i in range(1, len(rows)) if rows[i] != rows[i - 1]]
        self.assertEqual(runs, [ia, ib, ic], rows)
        counts = {n["name"]: n["rows"] for n in done["nodes"]}
        self.assertEqual(counts, {a.name: rows.count(ia), b.name: rows.count(ib), c.name: rows.count(ic)})
        self.assertTrue(all(counts[n.name] > 0 for n in (a, b, c)), counts)
        for n in done["nodes"]:
            self.assertEqual((n["arch"], n["os"]), (done["arch"], done["os"]))

        # The image: an 8-bit top-down BMP; each row's colours are its node's platform slot
        status, headers, bmp = http("GET", f"http://127.0.0.1:{INGRESS_PORT}/mandelbrot/image.bmp")
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("Content-Type"), "image/bmp")
        self.assertEqual(bmp[:2], b"BM")
        size, off = struct.unpack_from("<I", bmp, 2)[0], struct.unpack_from("<I", bmp, 10)[0]
        w, h, planes, bits = struct.unpack_from("<iiHH", bmp, 18)
        self.assertEqual((size, len(bmp), w, h, planes, bits), (len(bmp), off + WIDTH * HEIGHT, WIDTH, -HEIGHT, 1, 8))
        slots = {bmp[off + y * WIDTH + x] >> 4 for y in range(HEIGHT) for x in range(WIDTH)}
        self.assertEqual(len(slots), 1, "every node here shares one platform")
        self.assertNotIn(0, slots, "no row is left waiting")

        # Each move was recorded with its time
        _, _, body = http("GET", f"{api}/api/v1/namespaces/default/events")
        moved = [e.get("message", "") for e in json.loads(body).get("items", []) if e.get("reason") == "Moved"]
        self.assertTrue(any(f"from node {a.name} to node {b.name}" in m for m in moved), moved)
        self.assertTrue(any(f"from node {b.name} to node {c.name}" in m for m in moved), moved)

        # /restart starts the image again where the pod runs
        status, _, body = http("GET", f"http://127.0.0.1:{c.pod_port}/restart", headers={"X-Kube32-Pod": "default/mandelbrot"})
        self.assertEqual(status, 200)
        restarted = json.loads(body)
        self.assertEqual((restarted["rows_done"], restarted["renders"], restarted["done"]), (0, 0, False))
        self.assertEqual(restarted["rows"], "." * HEIGHT)
