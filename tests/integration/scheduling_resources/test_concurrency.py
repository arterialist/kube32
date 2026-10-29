"""
Scheduling and Resource Tests: Concurrent API Operations & Thread Safety
Authoritative sources:
- Kube32 Concurrency & State Store Thread-Safety Contract (PROJECT.md § 4.1)
- In-Memory Radix State Store with Thread-Safe Locking (PROJECT.md § 1)
"""

import threading
import queue
from tests.integration.framework import BaseTestCase


class TestConcurrency(BaseTestCase):
    def test_concurrent_pod_creations(self):
        """
        Verify multiple parallel threads can create independent Pods simultaneously
        without race conditions, socket hangs, or state corruption.
        """
        concurrency = 6
        results_queue = queue.Queue()

        def worker(thread_idx: int):
            pod_name = self.unique_name(f"conc-pod-{thread_idx}")
            manifest = {
                "apiVersion": "v1",
                "kind": "Pod",
                "metadata": {"name": pod_name, "namespace": "default"},
                "spec": {
                    "containers": [
                        {"name": "worker", "image": "wasm://embedded/echo", "resources": {"requests": {"memory": "16Ki"}}}
                    ]
                }
            }
            try:
                resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
                results_queue.put((pod_name, resp.status_code, None))
            except Exception as e:
                results_queue.put((pod_name, -1, str(e)))

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(concurrency)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=10.0)

        created_pods = []
        while not results_queue.empty():
            pname, status, err = results_queue.get()
            self.assertIsNone(err, f"Thread error: {err}")
            self.assertEqual(status, 201, f"Expected 201 for {pname}, got {status}")
            created_pods.append(pname)
            self.register_cleanup("pod", "default", pname)

        self.assertEqual(len(created_pods), concurrency)

    def test_concurrent_read_during_write(self):
        """
        Verify readers observing /api/v1/pods while concurrent writers are creating pods
        receive valid, non-corrupted JSON responses.
        """
        stop_event = threading.Event()
        read_errors = []

        def reader():
            while not stop_event.is_set():
                try:
                    resp = self.http.get("/api/v1/pods")
                    if resp.status_code != 200:
                        read_errors.append(f"Bad status: {resp.status_code}")
                    data = resp.json()
                    if not isinstance(data.get("items"), list):
                        read_errors.append("items not a list")
                except Exception as e:
                    read_errors.append(str(e))

        r_thread = threading.Thread(target=reader)
        r_thread.start()

        # Perform 5 rapid writes
        for i in range(5):
            pname = self.unique_name(f"rw-pod-{i}")
            self.register_cleanup("pod", "default", pname)
            manifest = {
                "apiVersion": "v1",
                "kind": "Pod",
                "metadata": {"name": pname, "namespace": "default"},
                "spec": {"containers": [{"name": "app", "image": "wasm://embedded/echo"}]}
            }
            resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
            self.assertEqual(resp.status_code, 201)

        stop_event.set()
        r_thread.join(timeout=5.0)
        self.assertEqual(len(read_errors), 0, f"Concurrent read errors: {read_errors}")

    def test_concurrent_delete_and_get(self):
        """
        Verify simultaneous DELETE and GET on the same pod handles atomic transition cleanly.
        """
        pod_name = self.unique_name("conc-del-get")
        self.register_cleanup("pod", "default", pod_name)
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "app", "image": "wasm://embedded/echo"}]}
        }
        resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(resp.status_code, 201)

        get_res = []
        del_res = []

        def do_get():
            r = self.http.get(f"/api/v1/namespaces/default/pods/{pod_name}")
            get_res.append(r.status_code)

        def do_del():
            r = self.http.delete(f"/api/v1/namespaces/default/pods/{pod_name}")
            del_res.append(r.status_code)

        t1 = threading.Thread(target=do_get)
        t2 = threading.Thread(target=do_del)
        t1.start()
        t2.start()
        t1.join()
        t2.join()

        # Delete should succeed (200)
        self.assertEqual(del_res[0], 200)
        # Get should either see 200 (before delete) or 404 (after delete)
        self.assertIn(get_res[0], [200, 404])

    def test_concurrent_status_updates(self):
        """
        Verify concurrent status updates on distinct pods execute cleanly.
        """
        pod_names = [self.unique_name(f"stat-pod-{i}") for i in range(4)]
        for pname in pod_names:
            self.register_cleanup("pod", "default", pname)
            manifest = {
                "apiVersion": "v1",
                "kind": "Pod",
                "metadata": {"name": pname, "namespace": "default"},
                "spec": {"containers": [{"name": "app", "image": "wasm://embedded/echo"}]}
            }
            self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        update_errors = []

        def updater(pname: str, phase: str):
            try:
                body = {"status": {"phase": phase}}
                r = self.http.put(f"/api/v1/namespaces/default/pods/{pname}/status", body=body)
                if r.status_code != 200:
                    update_errors.append(f"{pname} status error {r.status_code}")
            except Exception as e:
                update_errors.append(str(e))

        threads = [
            threading.Thread(target=updater, args=(pod_names[i], f"Phase-{i}"))
            for i in range(4)
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=5.0)

        self.assertEqual(len(update_errors), 0, f"Status update errors: {update_errors}")

    def test_rapid_create_delete_churn(self):
        """
        Verify rapid succession of pod create and delete cycles does not degrade system responsiveness.
        """
        iterations = 5
        for i in range(iterations):
            pname = self.unique_name(f"churn-pod-{i}")
            self.register_cleanup("pod", "default", pname)   # teardown waits until the pod is gone
            manifest = {
                "apiVersion": "v1",
                "kind": "Pod",
                "metadata": {"name": pname, "namespace": "default"},
                "spec": {"containers": [{"name": "app", "image": "wasm://embedded/echo"}]}
            }
            # Create
            c_resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
            self.assertEqual(c_resp.status_code, 201)
            # Delete
            d_resp = self.http.delete(f"/api/v1/namespaces/default/pods/{pname}")
            self.assertEqual(d_resp.status_code, 200)

        # Verify cluster is responsive after churn
        nodes_resp = self.http.get("/api/v1/nodes")
        self.assertEqual(nodes_resp.status_code, 200)
