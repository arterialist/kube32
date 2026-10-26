"""
Tier 2 Boundary Tests: Empty Payloads
Authoritative sources:
- Kubernetes API Server Validation Protocol (io.k8s.apimachinery.pkg.apis.meta.v1.Status)
- survey_spec_miner_1/report.md § 4.1 - 4.2
"""

from tests.framework import BaseTestCase


class TestEmptyPayloads(BaseTestCase):
    def test_post_pod_empty_string_body(self):
        """
        Verify POST /api/v1/namespaces/{ns}/pods with empty string body returns 400 BadRequest.
        """
        resp = self.http.post("/api/v1/namespaces/default/pods", body="")
        self.assert_k8s_status(resp, 400, expected_reason="BadRequest")

    def test_post_deployment_empty_string_body(self):
        """
        Verify POST /apis/apps/v1/namespaces/{ns}/deployments with empty string body returns 400 BadRequest.
        """
        resp = self.http.post("/apis/apps/v1/namespaces/default/deployments", body="")
        self.assert_k8s_status(resp, 400, expected_reason="BadRequest")

    def test_post_pod_empty_json_object(self):
        """
        Verify POST /api/v1/namespaces/{ns}/pods with empty JSON object '{}' returns 400 BadRequest
        due to missing metadata.name and spec.
        """
        resp = self.http.post("/api/v1/namespaces/default/pods", body={})
        self.assert_k8s_status(resp, 400, expected_reason="BadRequest")

    def test_post_pod_missing_containers_field(self):
        """
        Verify POST /api/v1/namespaces/{ns}/pods with metadata but empty spec returns 400 BadRequest.
        """
        resp = self.http.post("/api/v1/namespaces/default/pods", body={
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": "invalid-pod"},
            "spec": {}
        })
        self.assert_k8s_status(resp, 400, expected_reason="BadRequest")

    def test_put_pod_empty_body(self):
        """
        Verify PUT /api/v1/namespaces/{ns}/pods/{name} with empty body returns 400 BadRequest.
        """
        resp = self.http.put("/api/v1/namespaces/default/pods/any-pod", body="")
        self.assert_k8s_status(resp, 400, expected_reason="BadRequest")

    def test_post_node_empty_body(self):
        """
        Verify POST /api/v1/nodes with empty body returns 400 BadRequest.
        """
        resp = self.http.post("/api/v1/nodes", body="")
        self.assert_k8s_status(resp, 400, expected_reason="BadRequest")
