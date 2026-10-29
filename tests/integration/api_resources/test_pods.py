"""
Tier 1 Feature Tests: Pod Creation, Retrieval, Listing & Deletion
Authoritative sources:
- Kubernetes Pod v1 API Specification (io.k8s.api.core.v1.Pod)
- survey_spec_miner_1/report.md § 2.2
- PROJECT.md § 2 (Feature 5: Core Resource HTTP REST Endpoints)
"""

from tests.framework import BaseTestCase


class TestPods(BaseTestCase):
    def test_create_pod_happy_path(self):
        """
        Verify POST /api/v1/namespaces/{ns}/pods creates a Pod and returns 201 Created
        with valid ObjectMeta and assigned Pod status.
        """
        pod_name = self.unique_name("pod-create")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {
                "name": pod_name,
                "namespace": "default",
                "labels": {"app": "e2e-worker"}
            },
            "spec": {
                "containers": [
                    {
                        "name": "worker",
                        "image": "wasm://sample-worker:v1",
                        "resources": {
                            "limits": {"memory": "64Ki"}
                        }
                    }
                ]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(resp.status_code, 201, f"Expected 201 Created, got {resp.status_code}: {resp.text}")

        pod = resp.json()
        self.assertEqual(pod.get("kind"), "Pod")
        self.assertEqual(pod.get("apiVersion"), "v1")
        self.assertEqual(pod.get("metadata", {}).get("name"), pod_name)
        self.assertEqual(pod.get("metadata", {}).get("namespace"), "default")
        self.assertIn("spec", pod)
        self.assertIn("containers", pod["spec"])
        self.assertEqual(len(pod["spec"]["containers"]), 1)

    def test_get_single_pod(self):
        """
        Verify GET /api/v1/namespaces/{ns}/pods/{name} retrieves specific pod.
        """
        pod_name = self.unique_name("pod-get")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {
                "containers": [{"name": "c1", "image": "wasm://app"}]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        create_resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(create_resp.status_code, 201)

        get_resp = self.http.get(f"/api/v1/namespaces/default/pods/{pod_name}")
        self.assertEqual(get_resp.status_code, 200)
        pod = get_resp.json()
        self.assertEqual(pod.get("metadata", {}).get("name"), pod_name)
        self.assertEqual(pod.get("kind"), "Pod")

    def test_list_pods_in_namespace(self):
        """
        Verify GET /api/v1/namespaces/{ns}/pods returns PodList containing the created pod.
        """
        pod_name = self.unique_name("pod-list-ns")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "c1", "image": "wasm://app"}]}
        }
        self.register_cleanup("pod", "default", pod_name)
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        resp = self.http.get("/api/v1/namespaces/default/pods")
        self.assertEqual(resp.status_code, 200)
        body = resp.json()
        self.assertEqual(body.get("kind"), "PodList")
        pod_names = [p.get("metadata", {}).get("name") for p in body.get("items", [])]
        self.assertIn(pod_name, pod_names)

    def test_list_pods_cluster_wide(self):
        """
        Verify GET /api/v1/pods returns cluster-wide PodList across namespaces.
        """
        pod_name = self.unique_name("pod-cluster-list")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "c1", "image": "wasm://app"}]}
        }
        self.register_cleanup("pod", "default", pod_name)
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        resp = self.http.get("/api/v1/pods")
        self.assertEqual(resp.status_code, 200)
        body = resp.json()
        self.assertEqual(body.get("kind"), "PodList")
        pod_names = [p.get("metadata", {}).get("name") for p in body.get("items", [])]
        self.assertIn(pod_name, pod_names)

    def test_delete_pod_happy_path(self):
        """
        Verify DELETE /api/v1/namespaces/{ns}/pods/{name} deletes the pod
        and returns 200 OK with Status Success.
        """
        pod_name = self.unique_name("pod-delete")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "c1", "image": "wasm://app"}]}
        }
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        del_resp = self.http.delete(f"/api/v1/namespaces/default/pods/{pod_name}")
        self.assertEqual(del_resp.status_code, 200)
        body = del_resp.json()
        self.assertEqual(body.get("kind"), "Status")
        self.assertEqual(body.get("status"), "Success")

        # Verify pod is no longer returned
        get_resp = self.http.get(f"/api/v1/namespaces/default/pods/{pod_name}")
        self.assertEqual(get_resp.status_code, 404)

    def test_kubectl_get_pods_cli(self):
        """
        Verify kubectl get pods executes against Kube32 API server and returns PodList.
        """
        pod_name = self.unique_name("pod-kubectl")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "c1", "image": "wasm://app"}]}
        }
        self.register_cleanup("pod", "default", pod_name)
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        res = self.kubectl.run(["get", "pods", "-o", "json"])
        self.assertTrue(res.ok, f"kubectl get pods failed: {res.stderr}")
        data = res.json()
        self.assertIn(data.get("kind"), ["PodList", "List"])
        names = [p.get("metadata", {}).get("name") for p in data.get("items", [])]
        self.assertIn(pod_name, names)
