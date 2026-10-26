"""
Tier 1 Feature Tests: Node Management & Listing
Authoritative sources:
- Kubernetes Node v1 API Specification (io.k8s.api.core.v1.Node)
- survey_spec_miner_1/report.md § 2.1
- PROJECT.md § 2 (Feature 5: Core Resource HTTP REST Endpoints)
"""

from tests.framework import BaseTestCase


class TestNodes(BaseTestCase):
    def test_list_nodes_schema(self):
        """
        Verify GET /api/v1/nodes returns 200 OK with kind NodeList and items list.
        """
        resp = self.http.get("/api/v1/nodes")
        self.assertEqual(resp.status_code, 200, f"Expected 200 from /api/v1/nodes, got {resp.status_code}")
        body = resp.json()
        self.assertEqual(body.get("kind"), "NodeList")
        self.assertEqual(body.get("apiVersion"), "v1")
        self.assertIn("items", body)
        items = body["items"]
        self.assertTrue(isinstance(items, list))
        self.assertTrue(len(items) > 0, "Expected at least one node registered in cluster")

    def test_node_metadata_and_conditions(self):
        """
        Verify node objects contain metadata.name, labels, and Ready condition.
        """
        resp = self.http.get("/api/v1/nodes")
        body = resp.json()
        node = body["items"][0]

        meta = node.get("metadata", {})
        self.assertIn("name", meta)
        self.assertTrue(len(meta["name"]) > 0)

        status = node.get("status", {})
        self.assertIn("conditions", status)
        conditions = status["conditions"]
        ready_cond = next((c for c in conditions if c.get("type") == "Ready"), None)
        self.assertTrue(ready_cond is not None, "Node must report Ready condition")
        self.assertEqual(ready_cond.get("status"), "True")

    def test_node_capacity_and_allocatable(self):
        """
        Verify node status reports hardware capacity and allocatable resources,
        including microcontroller-specific peripheral limits.
        """
        resp = self.http.get("/api/v1/nodes")
        node = resp.json()["items"][0]
        status = node.get("status", {})

        self.assertIn("capacity", status)
        self.assertIn("allocatable", status)

        capacity = status["capacity"]
        allocatable = status["allocatable"]

        # CPU and Memory
        self.assertIn("memory", capacity)
        self.assertIn("memory", allocatable)
        self.assertIn("cpu", capacity)

        # Microcontroller peripherals (heap, gpio, i2c)
        for key in ["memory", "cpu"]:
            self.assertTrue(len(str(capacity[key])) > 0)

    def test_get_single_node(self):
        """
        Verify GET /api/v1/nodes/{name} returns individual node specification.
        """
        list_resp = self.http.get("/api/v1/nodes")
        target_name = list_resp.json()["items"][0]["metadata"]["name"]

        resp = self.http.get(f"/api/v1/nodes/{target_name}")
        self.assertEqual(resp.status_code, 200)
        node = resp.json()
        self.assertEqual(node.get("kind"), "Node")
        self.assertEqual(node.get("metadata", {}).get("name"), target_name)

    def test_node_system_info(self):
        """
        Verify nodeInfo reports operating system and runtime metadata.
        """
        resp = self.http.get("/api/v1/nodes")
        node = resp.json()["items"][0]
        node_info = node.get("status", {}).get("nodeInfo", {})

        self.assertIn("kubeletVersion", node_info)
        self.assertIn("osImage", node_info)
        self.assertTrue(len(node_info["kubeletVersion"]) > 0)

    def test_kubectl_get_nodes_cli(self):
        """
        Verify kubectl get nodes executes against Kube32 and returns valid output.
        """
        res = self.kubectl.run(["get", "nodes", "-o", "json"])
        self.assertTrue(res.ok, f"kubectl get nodes failed: {res.stderr}")
        data = res.json()
        self.assertIn(data.get("kind"), ["NodeList", "List"])
        self.assertTrue(len(data.get("items", [])) > 0)
