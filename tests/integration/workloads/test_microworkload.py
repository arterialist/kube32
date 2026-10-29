"""
Tier 4 Application Tests: Micro-Workload Runtime & Lifecycle (Wasm3 Sandbox)
Authoritative sources:
- Kube32 Micro-Workload Runtime Contract (PROJECT.md § 4.3 runtime::Runtime)
- survey_explorer_2/report.md § 1.1 - 1.4 (Wasm3 Sandboxed Execution, Traps & Health Probes)
"""

from tests.framework import BaseTestCase


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
                        "image": "wasm://temperature-sensor:latest",
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
        pod = resp.json()

        self.assertEqual(pod["status"]["phase"], "Running")
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
                "containers": [{"name": "telemetry", "image": "wasm://telemetry:v1"}]
            }
        }
        self.register_cleanup("pod", "default", pod_name)
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        resp = self.http.get(f"/api/v1/namespaces/default/pods/{pod_name}")
        self.assertEqual(resp.status_code, 200)
        pod = resp.json()

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

    def test_sandboxed_fault_containment_status(self):
        """
        Verify updating pod status to CrashLoopBackOff/Failed simulates
        sandboxed trap containment without affecting cluster responsiveness.
        """
        pod_name = self.unique_name("trap-app")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "fragile-worker", "image": "wasm://crash"}]}
        }
        self.register_cleanup("pod", "default", pod_name)
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        # Update status to simulate runtime trap catch
        trap_status = {
            "status": {
                "phase": "Failed",
                "containerStatuses": [
                    {
                        "name": "fragile-worker",
                        "ready": False,
                        "restartCount": 1,
                        "state": {"terminated": {"exitCode": 137, "reason": "OOMKilled"}}
                    }
                ]
            }
        }
        resp = self.http.put(f"/api/v1/namespaces/default/pods/{pod_name}/status", body=trap_status)
        self.assertEqual(resp.status_code, 200)

        # Verify cluster nodes remain healthy and unaffected
        nodes_resp = self.http.get("/api/v1/nodes")
        self.assertEqual(nodes_resp.status_code, 200)
        ready_nodes = [n for n in nodes_resp.json()["items"] if any(c["type"] == "Ready" and c["status"] == "True" for c in n["status"]["conditions"])]
        self.assertTrue(len(ready_nodes) > 0)

    def test_clean_container_termination(self):
        """
        Verify graceful pod deletion removes workload from cluster.
        """
        pod_name = self.unique_name("term-app")
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {"containers": [{"name": "app", "image": "wasm://app"}]}
        }
        self.http.post("/api/v1/namespaces/default/pods", body=manifest)

        del_resp = self.http.delete(f"/api/v1/namespaces/default/pods/{pod_name}")
        self.assertEqual(del_resp.status_code, 200)

        get_resp = self.http.get(f"/api/v1/namespaces/default/pods/{pod_name}")
        self.assertEqual(get_resp.status_code, 404)
