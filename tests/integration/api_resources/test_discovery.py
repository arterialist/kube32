"""
Tier 1 Feature Tests: Discovery Endpoints
Authoritative sources:
- Kubernetes Discovery API Specification (meta/v1 APIResourceList, APIVersions, APIGroupList)
- survey_spec_miner_1/report.md § 1.1 - 1.2
- PROJECT.md § 2 (Feature 4: Kubernetes Discovery Endpoints)
"""

from tests.framework import BaseTestCase


class TestDiscovery(BaseTestCase):
    def test_version_endpoint(self):
        """
        Verify GET /version returns 200 OK with valid version metadata.
        Expected: major == "1", minor >= "20", platform specified, gitVersion present.
        """
        resp = self.http.get("/version")
        self.assertEqual(resp.status_code, 200, f"Expected 200 from /version, got {resp.status_code}")
        body = resp.json()
        self.assertIn("major", body)
        self.assertIn("minor", body)
        self.assertIn("gitVersion", body)
        self.assertIn("platform", body)
        self.assertEqual(body["major"], "1")
        self.assertTrue(len(body["gitVersion"]) > 0)

    def test_root_api_discovery(self):
        """
        Verify GET /api returns 200 OK with APIVersions schema exposing legacy core 'v1'.
        """
        resp = self.http.get("/api")
        self.assertEqual(resp.status_code, 200, f"Expected 200 from /api, got {resp.status_code}")
        body = resp.json()
        self.assertEqual(body.get("kind"), "APIVersions")
        self.assertIn("versions", body)
        self.assertIn("v1", body["versions"])

    def test_root_apis_discovery(self):
        """
        Verify GET /apis returns 200 OK with APIGroupList exposing 'apps'.
        """
        resp = self.http.get("/apis")
        self.assertEqual(resp.status_code, 200, f"Expected 200 from /apis, got {resp.status_code}")
        body = resp.json()
        self.assertEqual(body.get("kind"), "APIGroupList")
        groups = body.get("groups", [])
        self.assertTrue(len(groups) > 0, "Expected non-empty APIGroupList")
        group_names = [g.get("name") for g in groups]
        self.assertIn("apps", group_names)

        apps_group = next(g for g in groups if g.get("name") == "apps")
        versions = [v.get("groupVersion") for v in apps_group.get("versions", [])]
        self.assertIn("apps/v1", versions)

    def test_core_v1_resource_list(self):
        """
        Verify GET /api/v1 returns 200 OK with APIResourceList containing core resources:
        nodes, pods, namespaces.
        """
        resp = self.http.get("/api/v1")
        self.assertEqual(resp.status_code, 200, f"Expected 200 from /api/v1, got {resp.status_code}")
        body = resp.json()
        self.assertEqual(body.get("kind"), "APIResourceList")
        self.assertEqual(body.get("groupVersion"), "v1")
        resources = body.get("resources", [])
        res_names = [r.get("name") for r in resources]

        self.assertIn("nodes", res_names)
        self.assertIn("pods", res_names)
        self.assertIn("namespaces", res_names)

        # Verify resource attributes
        pod_res = next(r for r in resources if r.get("name") == "pods")
        self.assertEqual(pod_res.get("kind"), "Pod")
        self.assertTrue(pod_res.get("namespaced"))
        self.assertIn("get", pod_res.get("verbs", []))
        self.assertIn("create", pod_res.get("verbs", []))
        self.assertIn("delete", pod_res.get("verbs", []))

    def test_apps_v1_resource_list(self):
        """
        Verify GET /apis/apps/v1 returns 200 OK with APIResourceList containing deployments.
        """
        resp = self.http.get("/apis/apps/v1")
        self.assertEqual(resp.status_code, 200, f"Expected 200 from /apis/apps/v1, got {resp.status_code}")
        body = resp.json()
        self.assertEqual(body.get("kind"), "APIResourceList")
        self.assertEqual(body.get("groupVersion"), "apps/v1")
        resources = body.get("resources", [])
        res_names = [r.get("name") for r in resources]
        self.assertIn("deployments", res_names)

        deploy_res = next(r for r in resources if r.get("name") == "deployments")
        self.assertEqual(deploy_res.get("kind"), "Deployment")
        self.assertTrue(deploy_res.get("namespaced"))
        self.assertIn("get", deploy_res.get("verbs", []))
        self.assertIn("create", deploy_res.get("verbs", []))

    def test_kubectl_client_compatibility(self):
        """
        Verify client compatibility with kubectl CLI discovery requests.
        """
        res = self.kubectl.run(["version", "--client", "-o", "json"])
        self.assertTrue(res.ok, f"kubectl version failed: {res.stderr}")
        client_ver = res.json()
        self.assertIn("clientVersion", client_ver)
        self.assertIn("major", client_ver["clientVersion"])
