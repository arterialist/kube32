"""
Tier 2 Boundary Tests: Malformed & Syntactically Invalid JSON
Authoritative sources:
- Kubernetes API Server Serialization Protocol
- survey_spec_miner_1/report.md § 4.1 - 4.2
"""

from tests.framework import BaseTestCase


class TestMalformedJSON(BaseTestCase):
    def test_truncated_json_string(self):
        """
        Verify POST with truncated JSON returns 400 BadRequest with Status schema.
        """
        resp = self.http.post("/api/v1/namespaces/default/pods", body='{"kind": "Pod", "metadata": {"name": "trun')
        self.assert_k8s_status(resp, 400, expected_reason="BadRequest")

    def test_invalid_json_syntax(self):
        """
        Verify POST with invalid JSON syntax returns 400 BadRequest.
        """
        resp = self.http.post("/api/v1/namespaces/default/pods", body='{invalid_key: true, "unquoted"}')
        self.assert_k8s_status(resp, 400, expected_reason="BadRequest")

    def test_unclosed_brace_json(self):
        """
        Verify POST to deployments with unclosed bracket syntax returns 400 BadRequest.
        """
        resp = self.http.post("/apis/apps/v1/namespaces/default/deployments", body='{"kind": "Deployment", "spec": [')
        self.assert_k8s_status(resp, 400, expected_reason="BadRequest")

    def test_non_json_plain_text(self):
        """
        Verify PUT with plain text non-JSON body returns 400 BadRequest.
        """
        resp = self.http.put("/api/v1/namespaces/default/pods/any-pod", body='PLAIN_TEXT_NOT_JSON')
        self.assert_k8s_status(resp, 400, expected_reason="BadRequest")

    def test_trailing_comma_json_syntax(self):
        """
        Verify PATCH with trailing comma syntax error returns 400 BadRequest.
        """
        resp = self.http.patch("/api/v1/namespaces/default/pods/any-pod", body='{"metadata": {"labels": {"a": "b",}}}')
        self.assert_k8s_status(resp, 400, expected_reason="BadRequest")

    def test_invalid_type_for_containers_field(self):
        """
        Verify POST where spec.containers is a string instead of a list returns 400 BadRequest.
        """
        resp = self.http.post("/api/v1/namespaces/default/pods", body={
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": "bad-type-pod"},
            "spec": {"containers": "not-a-list"}
        })
        self.assert_k8s_status(resp, 400, expected_reason="BadRequest")
