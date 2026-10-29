"""
Kube32 Cluster Tests: Node Reboot and Rejoin Recovery
Verifies ephemeral lease re-establishment, state recovery, and cluster rejoin
after an abrupt node reboot or power outage.
"""

import time
import json
import urllib.request
from tests.integration.framework import BaseTestCase
from tests.integration.cluster_harness import ClusterHarness, node_ready_condition


class TestRebootRecovery(BaseTestCase):
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
        def ready(status, reason=None):
            cond = node_ready_condition(cp.api_url, worker.name)
            ok = cond and cond.get("status") == status and (reason is None or cond.get("reason") == reason)
            return cond if ok else None
        self.wait_for_condition(lambda: ready("True"), timeout=10.0, description="node Ready=True")

        # Simulate abrupt reboot: kill worker
        self.harness.kill_node(worker.name)

        # Lease expiry (at least 15 s) turns the condition to Unknown
        self.wait_for_condition(lambda: ready("Unknown"), timeout=25.0, interval=0.5, description="node Ready=Unknown")

        # Reboot / restart the worker
        rebooted = self.harness.restart_node(worker.name, cp, lease_interval_ms=500)
        self.assertTrue(rebooted.is_alive())

        # The kubelet heartbeat renews the lease and restores Ready=True
        self.wait_for_condition(lambda: ready("True", "KubeletReady"), timeout=15.0, description="node Ready=True after restart")
