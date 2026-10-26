"""
Tier 2 Boundary Tests: 404 Not Found on Missing Resources
Authoritative sources:
- Kubernetes Status Schema (code: 404, reason: "NotFound")
- survey_spec_miner_1/report.md § 4.1 - 4.2
"""

from tests.framework import BaseTestCase


class TestNotFound(BaseTestCase):
    def test_get_missing_pod_returns_404(self):
        """
        Verify GET on non-existent pod returns 404 Not Found with Status schema.
        """
        resp = self.http.get("/api/v1/namespaces/default/pods/does-not-exist")
        self.assert_k8s_status(resp, 404, expected_reason="NotFound")
        body = resp.json()
        self.assertIn("pods", body.get("message", "").lower())

    def test_get_missing_node_returns_404(self):
        """
        Verify GET on non-existent node returns 404 Not Found.
        """
        resp = self.http.get("/api/v1/nodes/non-existent-esp32-node")
        self.assert_k8s_status(resp, 404, expected_reason="NotFound")

    def test_get_missing_deployment_returns_404(self):
        """
        Verify GET on non-existent deployment returns 404 Not Found.
        """
        resp = self.http.get("/apis/apps/v1/namespaces/default/deployments/no-such-deployment")
        self.assert_k8s_status(resp, 404, expected_reason="NotFound")

    def test_delete_missing_pod_returns_404(self):
        """
        Verify DELETE on non-existent pod returns 404 Not Found.
        """
        resp = self.http.delete("/api/v1/namespaces/default/pods/ghost-pod")
        self.assert_k8s_status(resp, 404, expected_reason="NotFound")

    def test_get_missing_scale_subresource_returns_404(self):
        """
        Verify GET /scale on non-existent deployment returns 404 Not Found.
        """
        resp = self.http.get("/apis/apps/v1/namespaces/default/deployments/ghost-app/scale")
        self.assert_k8s_status(resp, 404, expected_reason="NotFound")

    def test_unknown_api_endpoint_returns_404(self):
        """
        Verify requesting an unrecognized API endpoint returns 404 Not Found.
        """
        resp = self.http.get("/api/v1/namespaces/default/unsupported-resources")
        self.assert_k8s_status(resp, 404, expected_reason="NotFound")
