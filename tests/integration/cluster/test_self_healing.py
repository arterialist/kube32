"""
Kube32 Milestone 4 Test Suite: Autonomous Self-Healing and Multi-Node Failover
Verifies node lease eviction, DisruptionTarget tagging, and rescheduling
of orphaned workloads to surviving healthy cluster nodes.
"""

import time
import json
import urllib.request
from tests.framework import BaseTestCase
from tests.m4_cluster_harness import ClusterHarness


class TestM4SelfHealing(BaseTestCase):
    def setUp(self):
        super().setUp()
        self.harness = ClusterHarness(base_api_port=16643, base_ipc_port=17600)

    def tearDown(self):
        self.harness.teardown()
        super().tearDown()

    def test_eviction_and_autonomous_rescheduling(self):
        """When worker-1 fails, its workloads are evicted and rebound to healthy worker-2."""
        cp = self.harness.start_control_plane(
            name="cp-heal-0",
            lease_interval_ms=400,
            not_ready_timeout_ms=1500,
            eviction_timeout_ms=3000,
            reconcile_interval_ms=200
        )
        worker1 = self.harness.start_worker("worker-heal-1", 1, cp, lease_interval_ms=400)
        worker2 = self.harness.start_worker("worker-heal-2", 2, cp, lease_interval_ms=400)

        # Deploy a regular workload pod explicitly scheduled on worker-1
        pod_manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {
                "name": "resilient-sensor-app",
                "namespace": "default"
            },
            "spec": {
                "nodeName": worker1.name,
                "containers": [{
                    "name": "sensor",
                    "image": "wasm://sensor-firmware",
                    "resources": {
                        "requests": {
                            "memory": "32Ki"
                        }
                    }
                }]
            }
        }

        post_req = urllib.request.Request(
            f"{cp.api_url}/api/v1/namespaces/default/pods",
            data=json.dumps(pod_manifest).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        with urllib.request.urlopen(post_req, timeout=2.0) as resp:
            self.assertEqual(resp.status, 201)

        # Deploy a DaemonSet pod on worker-1 (should be immune to eviction)
        daemon_manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {
                "name": "node-telemetry-daemon",
                "namespace": "default",
                "labels": {
                    "kube32.io/daemonset": "true"
                }
            },
            "spec": {
                "nodeName": worker1.name,
                "containers": [{
                    "name": "daemon",
                    "image": "wasm://telemetry"
                }]
            }
        }
        post_daemon = urllib.request.Request(
            f"{cp.api_url}/api/v1/namespaces/default/pods",
            data=json.dumps(daemon_manifest).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        with urllib.request.urlopen(post_daemon, timeout=2.0) as resp:
            self.assertEqual(resp.status, 201)

        # Kill worker-1
        self.harness.kill_node(worker1.name)

        # Wait for eviction timeout (3000ms) + controller reconcile (500ms) + scheduler cycle
        time.sleep(4.0)

        # Query resilient pod: should be evicted and rescheduled to worker-2
        pod_url = f"{cp.api_url}/api/v1/namespaces/default/pods/resilient-sensor-app"
        with urllib.request.urlopen(pod_url, timeout=2.0) as resp:
            pod_data = json.loads(resp.read().decode())
            # Self-healing should reassign to surviving healthy worker-2
            self.assertEqual(pod_data.get("spec", {}).get("nodeName"), worker2.name)

        # Query daemon pod: should NOT be evicted because of daemonset exemption
        daemon_url = f"{cp.api_url}/api/v1/namespaces/default/pods/node-telemetry-daemon"
        with urllib.request.urlopen(daemon_url, timeout=2.0) as resp:
            d_data = json.loads(resp.read().decode())
            self.assertEqual(d_data.get("spec", {}).get("nodeName"), worker1.name)
