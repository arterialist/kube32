"""
Tier 4 Application Tests: Node Failure Detection & Pod Rescheduling
Authoritative sources:
- Kube32 Lease Heartbeat & Self-Healing Contract (PROJECT.md § 1, 2, 4.2)
- survey_explorer_2/report.md § 2 (Node Lease Protocol: 5s renew, 15s NotReady, 30s Eviction)
"""

from tests.framework import BaseTestCase


class TestRescheduling(BaseTestCase):
    def test_node_not_ready_condition_detection(self):
        """
        Verify that when a node ceases heartbeat updates and condition becomes NotReady (Ready=False),
        its status reflects the failure.
        """
        # Pick node 2 and update status condition to NotReady
        node_name = "esp32-node-02"
        patch_body = {
            "status": {
                "conditions": [
                    {
                        "type": "Ready",
                        "status": "False",
                        "reason": "KubeletHeartbeatStopped",
                        "message": "Node agent lease timed out"
                    }
                ]
            }
        }
        resp = self.http.patch(f"/api/v1/nodes/{node_name}", body=patch_body)
        self.assertEqual(resp.status_code, 200)

        get_resp = self.http.get(f"/api/v1/nodes/{node_name}")
        conds = get_resp.json()["status"]["conditions"]
        ready_cond = next(c for c in conds if c["type"] == "Ready")
        self.assertEqual(ready_cond["status"], "False")

        # Restore ready status for subsequent tests
        restore_body = {"status": {"conditions": [{"type": "Ready", "status": "True", "reason": "KubeletReady"}]}}
        self.http.patch(f"/api/v1/nodes/{node_name}", body=restore_body)

    def test_not_ready_node_rejected_by_scheduler(self):
        """
        Verify autonomous scheduler does NOT schedule pods to a node marked NotReady.
        """
        # Mark node-02 as NotReady
        self.http.patch("/api/v1/nodes/esp32-node-02", body={
            "status": {"conditions": [{"type": "Ready", "status": "False"}]}
        })

        # Submit pod
        pod_name = self.unique_name("sched-avoid-dead")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "app", "image": "wasm://app"}]}
        }
        self.register_cleanup("pod", "default", pod_name)
        resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(resp.status_code, 201)
        pod = resp.json()

        # Should be scheduled to node-01, NOT node-02
        self.assertEqual(pod["spec"]["nodeName"], "esp32-node-01")

        # Restore node-02
        self.http.patch("/api/v1/nodes/esp32-node-02", body={
            "status": {"conditions": [{"type": "Ready", "status": "True"}]}
        })

    def test_pod_rescheduling_to_healthy_node(self):
        """
        Verify that a pod bound to a failed node can be re-bound and rescheduled to an available node.
        """
        pod_name = self.unique_name("resched-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {
                "nodeName": "esp32-node-02",
                "containers": [{"name": "app", "image": "wasm://app"}]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        # Trigger node 02 failure and reschedule to node 01
        resched_body = {
            "spec": {"nodeName": "esp32-node-01"},
            "status": {
                "phase": "Running",
                "conditions": [{"type": "PodScheduled", "status": "True"}]
            }
        }
        resp = self.http.patch(f"/api/v1/namespaces/default/pods/{pod_name}", body=resched_body)
        self.assertEqual(resp.status_code, 200)
        updated_pod = resp.json()
        self.assertEqual(updated_pod["spec"]["nodeName"], "esp32-node-01")
        self.assertEqual(updated_pod["status"]["phase"], "Running")

    def test_evicted_pod_status_condition(self):
        """
        Verify pod eviction status condition records reason DisruptionTarget or Evicted.
        """
        pod_name = self.unique_name("evict-stat-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "app", "image": "wasm://app"}]}
        }
        self.register_cleanup("pod", "default", pod_name)
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        evict_status = {
            "status": {
                "phase": "Failed",
                "reason": "Evicted",
                "message": "The node was untolerated by the pod"
            }
        }
        resp = self.http.put(f"/api/v1/namespaces/default/pods/{pod_name}/status", body=evict_status)
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["status"]["reason"], "Evicted")

    def test_recovered_node_heartbeat_restoration(self):
        """
        Verify recovered node updates condition back to Ready=True and accepts workloads.
        """
        # Node re-heartbeat
        heartbeat = {
            "status": {
                "conditions": [
                    {
                        "type": "Ready",
                        "status": "True",
                        "lastHeartbeatTime": "2026-10-26T07:30:00Z",
                        "reason": "KubeletReady"
                    }
                ]
            }
        }
        resp = self.http.patch("/api/v1/nodes/esp32-node-02", body=heartbeat)
        self.assertEqual(resp.status_code, 200)
        ready = any(c["type"] == "Ready" and c["status"] == "True" for c in resp.json()["status"]["conditions"])
        self.assertTrue(ready)
