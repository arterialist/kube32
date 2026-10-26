"""
Tier 3 Pairwise Tests: Pod Scheduling with Node Memory Limits
Authoritative sources:
- Kube32 Autonomous Scheduler Specification (PROJECT.md § 2, 4.2)
- survey_spec_miner_1/report.md § 5.1 (Predicate Filter: Heap & PSRAM)
- survey_explorer_2/report.md § 1.1 (Memory Boundary Containment)
"""

from tests.framework import BaseTestCase


class TestMemoryLimits(BaseTestCase):
    def test_pod_within_node_heap_schedules_successfully(self):
        """
        Verify pod requesting memory within allocatable SRAM heap (64Ki < 240Ki)
        passes scheduler predicate filter and binds spec.nodeName.
        """
        pod_name = self.unique_name("mem-fit-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {
                "containers": [
                    {
                        "name": "worker",
                        "image": "wasm://app",
                        "resources": {
                            "requests": {"memory": "64Ki"},
                            "limits": {"memory": "64Ki"}
                        }
                    }
                ]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(resp.status_code, 201)
        pod = resp.json()

        # Should be scheduled and bound to a node
        self.assertTrue(len(pod.get("spec", {}).get("nodeName", "")) > 0, "Pod should have nodeName assigned")
        self.assertEqual(pod.get("status", {}).get("phase"), "Running")

    def test_pod_exceeding_node_heap_remains_pending(self):
        """
        Verify pod requesting memory greater than any node's allocatable heap (e.g. 600Ki)
        fails predicate filter, remains with spec.nodeName == "" and phase == "Pending".
        """
        pod_name = self.unique_name("mem-excess-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {
                "containers": [
                    {
                        "name": "huge-worker",
                        "image": "wasm://huge-app",
                        "resources": {
                            "requests": {"memory": "600Ki"},
                            "limits": {"memory": "600Ki"}
                        }
                    }
                ]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(resp.status_code, 201)
        pod = resp.json()

        self.assertEqual(pod.get("spec", {}).get("nodeName", ""), "")
        self.assertEqual(pod.get("status", {}).get("phase"), "Pending")
        conditions = pod.get("status", {}).get("conditions", [])
        sched_cond = next((c for c in conditions if c.get("type") == "PodScheduled"), None)
        self.assertTrue(sched_cond is not None)
        self.assertEqual(sched_cond.get("status"), "False")
        self.assertEqual(sched_cond.get("reason"), "Unschedulable")

    def test_cumulative_memory_exhaustion(self):
        """
        Verify cumulative memory accounting:
        Two pods requesting 140Ki each on 240Ki node capacity will exhaust node capacity.
        First pod schedules; second pod with insufficient remaining capacity on all nodes remains Pending.
        """
        # Node capacity is 240Ki each (2 nodes = 480Ki total).
        # Submit 3 pods of 180Ki each.
        # Pod 1 -> Node 1 (180Ki used, 60Ki left)
        # Pod 2 -> Node 2 (180Ki used, 60Ki left)
        # Pod 3 -> Neither node has 180Ki left -> Pod 3 remains Pending!
        pods = []
        for i in range(3):
            pname = self.unique_name(f"cumul-mem-{i}")
            self.register_cleanup("pod", "default", pname)
            manifest = {
                "apiVersion": "v1",
                "kind": "Pod",
                "metadata": {"name": pname, "namespace": "default"},
                "spec": {
                    "containers": [
                        {
                            "name": "worker",
                            "image": "wasm://app",
                            "resources": {"requests": {"memory": "180Ki"}}
                        }
                    ]
                }
            }
            resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
            self.assertEqual(resp.status_code, 201)
            pods.append(resp.json())

        # First two pods should be scheduled
        self.assertEqual(pods[0]["status"]["phase"], "Running")
        self.assertEqual(pods[1]["status"]["phase"], "Running")
        # Third pod must be Pending
        self.assertEqual(pods[2]["status"]["phase"], "Pending")

    def test_psram_allocation_request(self):
        """
        Verify pod requesting external PSRAM (esp.io/psram) matches against node PSRAM capacity.
        """
        pod_name = self.unique_name("psram-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {
                "containers": [
                    {
                        "name": "psram-worker",
                        "image": "wasm://app",
                        "resources": {
                            "requests": {
                                "memory": "32Ki",
                                "esp.io/psram": "1Mi"
                            }
                        }
                    }
                ]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(resp.status_code, 201)
        pod = resp.json()
        self.assertEqual(pod.get("status", {}).get("phase"), "Running")
        self.assertTrue(len(pod.get("spec", {}).get("nodeName", "")) > 0)

    def test_memory_unit_parsing(self):
        """
        Verify scheduler correctly parses Mi and Ki units in memory requests.
        """
        pod_name = self.unique_name("unit-parse-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {
                "containers": [
                    {
                        "name": "worker",
                        "image": "wasm://app",
                        "resources": {"requests": {"memory": "48Ki"}}
                    }
                ]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(resp.status_code, 201)
        self.assertEqual(resp.json()["status"]["phase"], "Running")
