"""
Tier 2 Boundary Tests: 409 Conflict on Duplicate Creation
Authoritative sources:
- Kubernetes Status Schema (code: 409, reason: "AlreadyExists")
- survey_spec_miner_1/report.md § 4.1 - 4.2
"""

from tests.framework import BaseTestCase


class TestConflict(BaseTestCase):
    def test_duplicate_pod_creation_returns_409(self):
        """
        Verify creating a pod with an already existing name returns 409 Conflict.
        """
        pod_name = self.unique_name("dup-pod")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "c1", "image": "wasm://app"}]}
        }
        self.register_cleanup("pod", "default", pod_name)
        first_resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(first_resp.status_code, 201)

        # Attempt duplicate creation
        second_resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assert_k8s_status(second_resp, 409, expected_reason="AlreadyExists")
        self.assertIn("already exists", second_resp.json().get("message", "").lower())

    def test_duplicate_deployment_creation_returns_409(self):
        """
        Verify creating a deployment with duplicate name returns 409 Conflict.
        """
        deploy_name = self.unique_name("dup-deploy")
        manifest = {
            "apiVersion": "apps/v1",
            "kind": "Deployment",
            "metadata": {"name": deploy_name, "namespace": "default"},
            "spec": {
                "replicas": 1,
                "selector": {"matchLabels": {"app": "dup"}},
                "template": {
                    "metadata": {"labels": {"app": "dup"}},
                    "spec": {"containers": [{"name": "c1", "image": "wasm://app"}]}
                }
            }
        }
        self.register_cleanup("deployment", "default", deploy_name)
        first_resp = self.http.post("/apis/apps/v1/namespaces/default/deployments", body=manifest)
        self.assertEqual(first_resp.status_code, 201)

        second_resp = self.http.post("/apis/apps/v1/namespaces/default/deployments", body=manifest)
        self.assert_k8s_status(second_resp, 409, expected_reason="AlreadyExists")

    def test_duplicate_node_creation_returns_409(self):
        """
        Verify creating a node with duplicate name returns 409 Conflict.
        """
        node_name = self.unique_name("dup-node")
        manifest = {
            "apiVersion": "v1",
            "kind": "Node",
            "metadata": {"name": node_name}
        }
        self.register_cleanup("node", "", node_name)
        first_resp = self.http.post("/api/v1/nodes", body=manifest)
        self.assertEqual(first_resp.status_code, 201)

        second_resp = self.http.post("/api/v1/nodes", body=manifest)
        self.assert_k8s_status(second_resp, 409, expected_reason="AlreadyExists")

    def test_conflict_details_contain_name_and_kind(self):
        """
        Verify 409 response payload details block contains accurate name and kind.
        """
        pod_name = self.unique_name("dup-details")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "c1", "image": "wasm://app"}]}
        }
        self.register_cleanup("pod", "default", pod_name)
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        dup_resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        details = dup_resp.json().get("details", {})
        self.assertEqual(details.get("name"), pod_name)
        self.assertEqual(details.get("kind"), "pods")

    def test_kubectl_conflict_error_output(self):
        """
        Verify kubectl CLI reports Error from server (AlreadyExists) on duplicate resource.
        """
        pod_name = self.unique_name("dup-cli")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "c1", "image": "wasm://app"}]}
        }
        self.register_cleanup("pod", "default", pod_name)
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        # Attempt create with kubectl
        import json
        res = self.kubectl.run(["create", "--validate=false", "-f", "-"], input_text=json.dumps(manifest))
        self.assertFalse(res.ok, "Expected kubectl create to fail on duplicate")
        self.assertIn("alreadyexists", res.stderr.lower())
