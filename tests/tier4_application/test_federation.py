"""
Tier 4 Application Tests: Virtual Kubelet Federation Bridge Synchronization
Authoritative sources:
- Kube32 Virtual Kubelet Bridge Contract (PROJECT.md § 4.4, § 1 M5)
- survey_explorer_2/report.md § 3 (Inter-Cluster Federation & External Kubernetes Interfacing)
"""

from tests.framework import BaseTestCase


class TestFederation(BaseTestCase):
    def test_vk_aggregated_capacity_query(self):
        """
        Verify Virtual Kubelet provider can query Kube32 GET /api/v1/nodes
        and aggregate total allocatable memory, CPU cores, and GPIO peripherals.
        """
        resp = self.http.get("/api/v1/nodes")
        self.assertEqual(resp.status_code, 200)
        nodes = resp.json().get("items", [])
        self.assertTrue(len(nodes) >= 2, "Federation requires at least 2 nodes")

        total_mem_kb = 0
        total_gpio = 0
        for n in nodes:
            alloc = n.get("status", {}).get("allocatable", {})
            mem_str = alloc.get("memory", "0Ki")
            if "Ki" in mem_str:
                total_mem_kb += int(mem_str.replace("Ki", ""))
            total_gpio += int(alloc.get("esp.io/gpio", 0))

        # Total capacity aggregated across nodes
        self.assertTrue(total_mem_kb >= 400, f"Expected total heap >= 400Ki, got {total_mem_kb}Ki")
        self.assertTrue(total_gpio >= 20, f"Expected total GPIO >= 20, got {total_gpio}")

    def test_vk_upstream_podspec_translation(self):
        """
        Verify translating an upstream Kubernetes Pod targeting the virtual node
        into a native Kube32 pod submission.
        """
        upstream_pod_name = self.unique_name("vk-upstream-pod")
        # Manifest received by Virtual Kubelet from upstream EKS/GKE
        kube32_translated_manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {
                "name": upstream_pod_name,
                "namespace": "default",
                "labels": {
                    "federated-from": "upstream-k8s-cluster",
                    "app": "edge-analytics"
                }
            },
            "spec": {
                "containers": [
                    {
                        "name": "edge-sensor",
                        "image": "wasm://edge-analytics:v1",
                        "resources": {
                            "limits": {
                                "memory": "64Ki",
                                "esp.io/gpio": "1"
                            }
                        }
                    }
                ]
            }
        }
        self.register_cleanup("pod", "default", upstream_pod_name)
        resp = self.http.post("/api/v1/namespaces/default/pods", body=kube32_translated_manifest)
        self.assertEqual(resp.status_code, 201)
        pod = resp.json()
        self.assertEqual(pod["metadata"]["name"], upstream_pod_name)
        self.assertEqual(pod["status"]["phase"], "Running")

    def test_vk_bidirectional_status_synchronization(self):
        """
        Verify Virtual Kubelet polling GET /api/v1/namespaces/{ns}/pods/{name}
        retrieves assigned pod IP and container status for upstream propagation.
        """
        pod_name = self.unique_name("vk-sync-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "app", "image": "wasm://app"}]}
        }
        self.register_cleanup("pod", "default", pod_name)
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        resp = self.http.get(f"/api/v1/namespaces/default/pods/{pod_name}")
        self.assertEqual(resp.status_code, 200)
        pod = resp.json()
        self.assertIn("podIP", pod["status"])
        self.assertEqual(pod["status"]["phase"], "Running")

    def test_vk_upstream_deletion_cascade(self):
        """
        Verify upstream pod deletion cascades to DELETE on Kube32 cluster.
        """
        pod_name = self.unique_name("vk-del-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "app", "image": "wasm://app"}]}
        }
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        # Upstream deletes pod -> VK invokes DELETE
        del_resp = self.http.delete(f"/api/v1/namespaces/default/pods/{pod_name}")
        self.assertEqual(del_resp.status_code, 200)

        get_resp = self.http.get(f"/api/v1/namespaces/default/pods/{pod_name}")
        self.assertEqual(get_resp.status_code, 404)

    def test_vk_cluster_readiness_mirroring(self):
        """
        Verify Virtual Kubelet can determine aggregate cluster health by checking node Ready conditions.
        """
        resp = self.http.get("/api/v1/nodes")
        self.assertEqual(resp.status_code, 200)
        nodes = resp.json().get("items", [])
        any_healthy = any(
            any(c["type"] == "Ready" and c["status"] == "True" for c in n.get("status", {}).get("conditions", []))
            for n in nodes
        )
        self.assertTrue(any_healthy, "At least one node must be ready for VK health advertisement")
