"""
Tier 1 Feature Tests: Deployment Lifecycle & Scale Subresource
Authoritative sources:
- Kubernetes Deployment apps/v1 API Specification (io.k8s.api.apps.v1.Deployment)
- survey_spec_miner_1/report.md § 2.3
- PROJECT.md § 2 (Feature 5: Core Resource HTTP REST Endpoints)
"""

from tests.framework import BaseTestCase


class TestDeployments(BaseTestCase):
    def test_create_deployment(self):
        """
        Verify POST /apis/apps/v1/namespaces/{ns}/deployments creates a deployment
        and returns 201 Created.
        """
        deploy_name = self.unique_name("deploy-create")
        manifest = {
            "apiVersion": "apps/v1",
            "kind": "Deployment",
            "metadata": {
                "name": deploy_name,
                "namespace": "default",
                "labels": {"app": "collector"}
            },
            "spec": {
                "replicas": 2,
                "selector": {
                    "matchLabels": {"app": "collector"}
                },
                "template": {
                    "metadata": {"labels": {"app": "collector"}},
                    "spec": {
                        "containers": [
                            {
                                "name": "collector",
                                "image": "wasm://collector:v1",
                                "resources": {"limits": {"memory": "32Ki"}}
                            }
                        ]
                    }
                }
            }
        }
        self.register_cleanup("deployment", "default", deploy_name)
        resp = self.http.post("/apis/apps/v1/namespaces/default/deployments", body=manifest)
        self.assertEqual(resp.status_code, 201, f"Expected 201 Created, got {resp.status_code}: {resp.text}")

        deploy = resp.json()
        self.assertEqual(deploy.get("kind"), "Deployment")
        self.assertEqual(deploy.get("metadata", {}).get("name"), deploy_name)
        self.assertEqual(deploy.get("spec", {}).get("replicas"), 2)

    def test_list_deployments(self):
        """
        Verify GET /apis/apps/v1/namespaces/{ns}/deployments lists active deployments.
        """
        deploy_name = self.unique_name("deploy-list")
        manifest = {
            "apiVersion": "apps/v1",
            "kind": "Deployment",
            "metadata": {"name": deploy_name, "namespace": "default"},
            "spec": {
                "replicas": 1,
                "selector": {"matchLabels": {"app": "test"}},
                "template": {
                    "metadata": {"labels": {"app": "test"}},
                    "spec": {"containers": [{"name": "app", "image": "wasm://app"}]}
                }
            }
        }
        self.register_cleanup("deployment", "default", deploy_name)
        self.http.post("/apis/apps/v1/namespaces/default/deployments", body=manifest)

        resp = self.http.get("/apis/apps/v1/namespaces/default/deployments")
        self.assertEqual(resp.status_code, 200)
        body = resp.json()
        self.assertEqual(body.get("kind"), "DeploymentList")
        names = [d.get("metadata", {}).get("name") for d in body.get("items", [])]
        self.assertIn(deploy_name, names)

    def test_get_single_deployment(self):
        """
        Verify GET /apis/apps/v1/namespaces/{ns}/deployments/{name} retrieves specific deployment.
        """
        deploy_name = self.unique_name("deploy-get")
        manifest = {
            "apiVersion": "apps/v1",
            "kind": "Deployment",
            "metadata": {"name": deploy_name, "namespace": "default"},
            "spec": {
                "replicas": 3,
                "selector": {"matchLabels": {"app": "test"}},
                "template": {
                    "metadata": {"labels": {"app": "test"}},
                    "spec": {"containers": [{"name": "app", "image": "wasm://app"}]}
                }
            }
        }
        self.register_cleanup("deployment", "default", deploy_name)
        self.http.post("/apis/apps/v1/namespaces/default/deployments", body=manifest)

        resp = self.http.get(f"/apis/apps/v1/namespaces/default/deployments/{deploy_name}")
        self.assertEqual(resp.status_code, 200)
        deploy = resp.json()
        self.assertEqual(deploy.get("metadata", {}).get("name"), deploy_name)
        self.assertEqual(deploy.get("spec", {}).get("replicas"), 3)

    def test_deployment_scale_subresource(self):
        """
        Verify GET /apis/apps/v1/namespaces/{ns}/deployments/{name}/scale returns Scale subresource.
        """
        deploy_name = self.unique_name("deploy-scale")
        manifest = {
            "apiVersion": "apps/v1",
            "kind": "Deployment",
            "metadata": {"name": deploy_name, "namespace": "default"},
            "spec": {
                "replicas": 4,
                "selector": {"matchLabels": {"app": "test"}},
                "template": {
                    "metadata": {"labels": {"app": "test"}},
                    "spec": {"containers": [{"name": "app", "image": "wasm://app"}]}
                }
            }
        }
        self.register_cleanup("deployment", "default", deploy_name)
        self.http.post("/apis/apps/v1/namespaces/default/deployments", body=manifest)

        resp = self.http.get(f"/apis/apps/v1/namespaces/default/deployments/{deploy_name}/scale")
        self.assertEqual(resp.status_code, 200)
        scale = resp.json()
        self.assertEqual(scale.get("kind"), "Scale")
        self.assertEqual(scale.get("spec", {}).get("replicas"), 4)

    def test_deployment_status_replicas(self):
        """
        Verify deployment status reports replica count and availability.
        """
        deploy_name = self.unique_name("deploy-status")
        manifest = {
            "apiVersion": "apps/v1",
            "kind": "Deployment",
            "metadata": {"name": deploy_name, "namespace": "default"},
            "spec": {
                "replicas": 2,
                "selector": {"matchLabels": {"app": "test"}},
                "template": {
                    "metadata": {"labels": {"app": "test"}},
                    "spec": {"containers": [{"name": "app", "image": "wasm://app"}]}
                }
            }
        }
        self.register_cleanup("deployment", "default", deploy_name)
        self.http.post("/apis/apps/v1/namespaces/default/deployments", body=manifest)

        resp = self.http.get(f"/apis/apps/v1/namespaces/default/deployments/{deploy_name}")
        deploy = resp.json()
        self.assertIn("status", deploy)
        status = deploy["status"]
        self.assertIn("replicas", status)
        self.assertIn("readyReplicas", status)
        self.assertIn("availableReplicas", status)

    def test_delete_deployment(self):
        """
        Verify DELETE /apis/apps/v1/namespaces/{ns}/deployments/{name} deletes deployment.
        """
        deploy_name = self.unique_name("deploy-del")
        manifest = {
            "apiVersion": "apps/v1",
            "kind": "Deployment",
            "metadata": {"name": deploy_name, "namespace": "default"},
            "spec": {
                "replicas": 1,
                "selector": {"matchLabels": {"app": "test"}},
                "template": {
                    "metadata": {"labels": {"app": "test"}},
                    "spec": {"containers": [{"name": "app", "image": "wasm://app"}]}
                }
            }
        }
        self.http.post("/apis/apps/v1/namespaces/default/deployments", body=manifest)

        del_resp = self.http.delete(f"/apis/apps/v1/namespaces/default/deployments/{deploy_name}")
        self.assertEqual(del_resp.status_code, 200)
        self.assertEqual(del_resp.json().get("kind"), "Status")

        get_resp = self.http.get(f"/apis/apps/v1/namespaces/default/deployments/{deploy_name}")
        self.assertEqual(get_resp.status_code, 404)
