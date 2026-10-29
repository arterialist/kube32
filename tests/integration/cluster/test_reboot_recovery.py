"""
Kube32 Milestone 4 Test Suite: Node Reboot and Rejoin Recovery
Verifies ephemeral lease re-establishment, state recovery, and cluster rejoin
after an abrupt node reboot or power outage.
"""

import time
import json
import urllib.request
from tests.framework import BaseTestCase
from tests.m4_cluster_harness import ClusterHarness


class TestM4RebootRecovery(BaseTestCase):
    def setUp(self):
        super().setUp()
        self.harness = ClusterHarness(base_api_port=16843, base_ipc_port=17800)

    def tearDown(self):
        self.harness.teardown()
        super().tearDown()

    def test_worker_node_reboot_and_rejoin(self):
        """Node abruptly rebooted re-registers, renews lease, and restores Ready=True condition."""
        cp = self.harness.start_control_plane(
            name="cp-reboot-0",
            lease_interval_ms=500,
            not_ready_timeout_ms=2000,
            eviction_timeout_ms=5000,
            reconcile_interval_ms=200
        )
        worker = self.harness.start_worker("worker-reboot-1", 1, cp, lease_interval_ms=500)

        # Confirm initial Ready condition
        node_url = f"{cp.api_url}/api/v1/nodes/{worker.name}"
        with urllib.request.urlopen(node_url, timeout=2.0) as resp:
            data = json.loads(resp.read().decode())
            ready_cond = next((c for c in data.get("status", {}).get("conditions", []) if c.get("type") == "Ready"), None)
            self.assertIsNotNone(ready_cond)
            self.assertEqual(ready_cond.get("status"), "True")

        # Simulate abrupt reboot: kill worker
        self.harness.kill_node(worker.name)

        # Wait for lease expiration (2000ms threshold)
        time.sleep(2.4)

        with urllib.request.urlopen(node_url, timeout=2.0) as resp:
            data = json.loads(resp.read().decode())
            ready_cond = next((c for c in data.get("status", {}).get("conditions", []) if c.get("type") == "Ready"), None)
            self.assertEqual(ready_cond.get("status"), "False")

        # Reboot / restart the worker
        rebooted = self.harness.restart_node(worker.name, cp, lease_interval_ms=500)
        self.assertTrue(rebooted.is_alive())

        # Wait for kubelet heartbeat to renew lease and restore Ready=True
        time.sleep(1.2)

        with urllib.request.urlopen(node_url, timeout=2.0) as resp:
            data = json.loads(resp.read().decode())
            ready_cond = next((c for c in data.get("status", {}).get("conditions", []) if c.get("type") == "Ready"), None)
            self.assertEqual(ready_cond.get("status"), "True")
            self.assertEqual(ready_cond.get("reason"), "KubeletReady")
