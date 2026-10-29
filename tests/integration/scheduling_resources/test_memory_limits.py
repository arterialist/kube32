"""
Scheduling and Resource Tests: Pod Scheduling with Node Memory Limits
Authoritative sources:
- Kube32 Autonomous Scheduler Specification (PROJECT.md § 2, 4.2)
- survey_spec_miner_1/report.md § 5.1 (Predicate Filter: Heap & PSRAM)
- survey_explorer_2/report.md § 1.1 (Memory Boundary Containment)
"""

from tests.integration.framework import BaseTestCase


class TestMemoryLimits(BaseTestCase):
    def setUp(self):
        super().setUp()
        self.wait_for_node_memory()

    def test_pod_within_node_heap_schedules_successfully(self):
        """
        Verify a pod requesting memory within the node's allocatable heap passes the
        scheduler predicate filter, binds spec.nodeName and starts.
        """
        pod = self.create_pod(self.unique_name("mem-fit-pod"), {"memory": "64Ki"})
        self.assertTrue(len(pod.get("spec", {}).get("nodeName", "")) > 0, "Pod should have nodeName assigned")
        self.wait_for_phase(pod["metadata"]["name"], "Running")

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
        Verify cumulative memory accounting: two pods each requesting 60% of the node's
        allocatable memory cannot both fit. The first schedules, the second stays Pending.
        """
        alloc = self.first_node()["status"]["allocatable"]["memory"]
        self.assertTrue(alloc.endswith("Ki"), "allocatable memory in Ki: %s" % alloc)
        each = "%dKi" % (int(alloc[:-2]) * 6 // 10)
        first = self.create_pod(self.unique_name("cumul-mem-0"), {"memory": each})
        self.assertTrue(len(first["spec"].get("nodeName", "")) > 0)
        second = self.create_pod(self.unique_name("cumul-mem-1"), {"memory": each})
        self.assertEqual(second["spec"].get("nodeName", ""), "")
        self.assertEqual(second["status"]["phase"], "Pending")

    def test_memory_unit_parsing(self):
        """
        Verify the scheduler parses Ki and Mi units in memory requests: 48Ki fits the node,
        1Mi exceeds it.
        """
        fits = self.create_pod(self.unique_name("unit-parse-ki"), {"memory": "48Ki"})
        self.assertTrue(len(fits["spec"].get("nodeName", "")) > 0)
        too_big = self.create_pod(self.unique_name("unit-parse-mi"), {"memory": "1Mi"})
        self.assertEqual(too_big["spec"].get("nodeName", ""), "")

    def test_psram_allocation_request(self):
        """
        Verify a pod requesting external PSRAM (esp32.io/psram) only schedules onto a node
        that reports that much PSRAM: a node without PSRAM leaves it Pending.
        """
        alloc = self.first_node()["status"]["allocatable"]
        have = alloc.get("esp32.io/psram") or alloc.get("esp.io/psram")
        pod = self.create_pod(self.unique_name("psram-pod"), {"memory": "32Ki", "esp32.io/psram": "1Mi"})
        if have:
            self.assertTrue(len(pod["spec"].get("nodeName", "")) > 0)
        else:
            self.assertEqual(pod["spec"].get("nodeName", ""), "")
            self.assertEqual(pod["status"]["phase"], "Pending")
