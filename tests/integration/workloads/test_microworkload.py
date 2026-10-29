"""
Workload Tests: Micro-Workload Runtime & Lifecycle (Wasm3 Sandbox)
Authoritative sources:
- Kube32 Micro-Workload Runtime Contract (PROJECT.md § 4.3 runtime::Runtime)
- survey_explorer_2/report.md § 1.1 - 1.4 (Wasm3 Sandboxed Execution, Traps & Health Probes)
"""

from tests.integration.framework import BaseTestCase


class TestMicroWorkload(BaseTestCase):
    def test_deploy_wasm_workload_reaches_running(self):
        """
        Verify deploying a micro-workload manifest with Wasm binary/URI
        successfully transitions pod to Running phase with containerStatuses.
        """
        pod_name = self.unique_name("wasm-app")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {
                "name": pod_name,
                "namespace": "default",
                "labels": {"app": "temp-sensor"}
            },
            "spec": {
                "restartPolicy": "Always",
                "containers": [
                    {
                        "name": "sensor-reader",
                        "image": "wasm://embedded/echo",
                        "resources": {
                            "limits": {"memory": "64Ki"}
                        },
                        "env": [
                            {"name": "SAMPLE_RATE", "value": "10"}
                        ]
                    }
                ]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(resp.status_code, 201)

        pod = self.wait_for_phase(pod_name, "Running")
        self.assertTrue(len(pod["status"]["podIP"]) > 0)
        c_statuses = pod["status"].get("containerStatuses", [])
        self.assertEqual(len(c_statuses), 1)
        self.assertTrue(c_statuses[0].get("ready"))
        self.assertIn("running", c_statuses[0].get("state", {}))

    def test_workload_execution_telemetry(self):
        """
        Verify pod status reflects runtime execution metrics (node IP, containerID, restartCount).
        """
        pod_name = self.unique_name("telemetry-app")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {
                "containers": [{"name": "telemetry", "image": "wasm://embedded/echo"}]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        pod = self.wait_for_phase(pod_name, "Running")
        status = pod.get("status", {})
        self.assertIn("hostIP", status)
        self.assertIn("podIP", status)
        self.assertIn("startTime", status)
        c_status = status.get("containerStatuses", [{}])[0]
        self.assertEqual(c_status.get("restartCount"), 0)

    def test_workload_liveness_probe_metadata(self):
        """
        Verify pod manifest with liveness probe is accepted and preserved in Spec.
        """
        pod_name = self.unique_name("probe-app")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {
                "containers": [
                    {
                        "name": "worker",
                        "image": "wasm://health:v1",
                        "livenessProbe": {
                            "httpGet": {"path": "/healthz", "port": 8080},
                            "initialDelaySeconds": 5,
                            "periodSeconds": 10
                        }
                    }
                ]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(resp.status_code, 201)
        pod = resp.json()
        probe = pod["spec"]["containers"][0].get("livenessProbe")
        self.assertTrue(probe is not None)
        self.assertEqual(probe.get("initialDelaySeconds"), 5)

    def test_clean_container_termination(self):
        """
        Verify graceful pod deletion removes workload from cluster.
        """
        pod_name = self.unique_name("term-app")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "app", "image": "wasm://embedded/echo"}]}
        }
        self.register_cleanup("pod", "default", pod_name)
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        del_resp = self.http.delete(f"/api/v1/namespaces/default/pods/{pod_name}")
        self.assertEqual(del_resp.status_code, 200)

        self.wait_for_condition(
            lambda: self.http.get(f"/api/v1/namespaces/default/pods/{pod_name}").status_code == 404,
            timeout=10.0, description=f"pod {pod_name} to disappear")
