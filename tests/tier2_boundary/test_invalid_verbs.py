"""
Tier 2 Boundary Tests: 405 Method Not Allowed for Invalid HTTP Verbs
Authoritative sources:
- HTTP/1.1 Specification (RFC 7231 § 6.5.5)
- Kubernetes REST API Convention
- survey_spec_miner_1/report.md § 3.1
"""

from tests.framework import BaseTestCase


class TestInvalidVerbs(BaseTestCase):
    def test_post_on_version_discovery(self):
        """
        Verify POST on discovery endpoint /version returns 405 Method Not Allowed.
        """
        resp = self.http.post("/version", body={"foo": "bar"})
        self.assert_k8s_status(resp, 405, expected_reason="MethodNotAllowed")

    def test_delete_on_api_discovery(self):
        """
        Verify DELETE on discovery endpoint /api/v1 returns 405 Method Not Allowed.
        """
        resp = self.http.delete("/api/v1")
        self.assert_k8s_status(resp, 405, expected_reason="MethodNotAllowed")

    def test_put_on_node_collection(self):
        """
        Verify PUT on collection endpoint /api/v1/nodes returns 405 Method Not Allowed.
        """
        resp = self.http.put("/api/v1/nodes", body={"kind": "NodeList"})
        self.assert_k8s_status(resp, 405, expected_reason="MethodNotAllowed")

    def test_post_on_single_node_item(self):
        """
        Verify POST on resource item /api/v1/nodes/{name} returns 405 Method Not Allowed.
        """
        resp = self.http.post("/api/v1/nodes/esp32-node-01", body={"metadata": {"name": "test"}})
        self.assert_k8s_status(resp, 405, expected_reason="MethodNotAllowed")

    def test_delete_on_apis_group_list(self):
        """
        Verify DELETE on root APIGroupList /apis returns 405 Method Not Allowed.
        """
        resp = self.http.delete("/apis")
        self.assert_k8s_status(resp, 405, expected_reason="MethodNotAllowed")

    def test_put_on_deployments_collection(self):
        """
        Verify PUT on deployments collection returns 405 Method Not Allowed.
        """
        resp = self.http.put("/apis/apps/v1/namespaces/default/deployments", body={"kind": "DeploymentList"})
        self.assert_k8s_status(resp, 405, expected_reason="MethodNotAllowed")
