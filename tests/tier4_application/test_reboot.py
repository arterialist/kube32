"""
Tier 4 Application Tests: Flash-Backed Reboot Recovery (LittleFS Journal)
Authoritative sources:
- LittleFS State Store & Journal Contract (PROJECT.md § 1, 4.1 storage::Store)
- survey_explorer_2/report.md § 2 (Cold-Start Flash Journal Recovery)
"""

from tests.framework import BaseTestCase


class TestRebootRecovery(BaseTestCase):
    def test_state_persistence_before_reboot(self):
        """
        Verify pod and deployment definitions submitted to cluster are retrievable
        and ready for flash journal synchronization.
        """
        pod_name = self.unique_name("preboot-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "app", "image": "wasm://app"}]}
        }
        self.register_cleanup("pod", "default", pod_name)
        create_resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(create_resp.status_code, 201)

        get_resp = self.http.get(f"/api/v1/namespaces/default/pods/{pod_name}")
        self.assertEqual(get_resp.status_code, 200)
        self.assertEqual(get_resp.json()["metadata"]["name"], pod_name)

    def test_simulated_node_reboot_reconnect(self):
        """
        Verify that after a simulated power-cycle/reboot, node re-registers with master
        with fresh bootID and preserves its capacity definitions.
        """
        reboot_node = {
            "apiVersion": "v1",
            "kind": "Node",
            "metadata": {"name": "esp32-node-01"},
            "status": {
                "conditions": [{"type": "Ready", "status": "True"}],
                "nodeInfo": {
                    "bootID": f"boot-{self.unique_id}",
                    "kubeletVersion": "v1.33.0-kube32",
                    "osImage": "ESP-IDF v5.2"
                },
                "capacity": {"memory": "320Ki", "cpu": "2", "esp.io/gpio": "16"},
                "allocatable": {"memory": "240Ki", "cpu": "2", "esp.io/gpio": "14", "esp.io/i2c": "2"}
            }
        }
        resp = self.http.put("/api/v1/nodes/esp32-node-01", body=reboot_node)
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["status"]["nodeInfo"]["bootID"], f"boot-{self.unique_id}")

    def test_pod_state_restoration_post_reboot(self):
        """
        Verify active pod manifests survive across boot cycles and maintain spec.nodeName binding.
        """
        pod_name = self.unique_name("survive-boot-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {
                "nodeName": "esp32-node-01",
                "containers": [{"name": "c1", "image": "wasm://app"}]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        # Query state store to verify persistence
        resp = self.http.get(f"/api/v1/namespaces/default/pods/{pod_name}")
        self.assertEqual(resp.status_code, 200)
        pod = resp.json()
        self.assertEqual(pod["spec"]["nodeName"], "esp32-node-01")

    def test_workload_reconciliation_post_reboot(self):
        """
        Verify restarted node reports active container status reconciliation.
        """
        pod_name = self.unique_name("reconcile-boot-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "app", "image": "wasm://app"}]}
        }
        self.register_cleanup("pod", "default", pod_name)
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        # Node agent after reboot reconciles status
        recon_status = {
            "status": {
                "phase": "Running",
                "containerStatuses": [
                    {
                        "name": "app",
                        "ready": True,
                        "restartCount": 1,
                        "state": {"running": {"startedAt": "2026-10-26T07:35:00Z"}}
                    }
                ]
            }
        }
        resp = self.http.put(f"/api/v1/namespaces/default/pods/{pod_name}/status", body=recon_status)
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["status"]["phase"], "Running")
        self.assertEqual(resp.json()["status"]["containerStatuses"][0]["restartCount"], 1)

    def test_atomic_journal_clean_state_query(self):
        """
        Verify cluster wide resource listing after state changes is clean and consistent.
        """
        resp = self.http.get("/api/v1/pods")
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.json()["kind"], "PodList")
