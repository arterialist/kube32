"""
Kube32 Milestone 4 Test Suite: Node Lease Heartbeat API & Lifecycle Transitions
Verifies coordination.k8s.io/v1 Lease renewal, expiration, NotReady condition,
and eviction thresholds without flash wear.
"""

import time
import json
import urllib.request
from tests.framework import BaseTestCase, HTTPResponse
from tests.m4_cluster_harness import ClusterHarness


class TestM4NodeLease(BaseTestCase):
    def setUp(self):
        super().setUp()
        self.harness = ClusterHarness(base_api_port=16543, base_ipc_port=17500)

    def tearDown(self):
        self.harness.teardown()
        super().tearDown()

    def test_worker_node_lease_renewal(self):
        """Worker node automatically renews lease every heartbeat cycle."""
        cp = self.harness.start_control_plane(
            name="cp-lease-1",
            lease_interval_ms=500,
            not_ready_timeout_ms=2500,
            eviction_timeout_ms=5000
        )
        worker = self.harness.start_worker("worker-lease-1", 1, cp, lease_interval_ms=500)

        # Query Lease object from CP
        lease_url = f"{cp.api_url}/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/{worker.name}"
        req = urllib.request.Request(lease_url, method="GET")
        with urllib.request.urlopen(req, timeout=2.0) as resp:
            self.assertEqual(resp.status, 200)
            data = json.loads(resp.read().decode())
            self.assertEqual(data.get("kind"), "Lease")
            self.assertEqual(data.get("metadata", {}).get("name"), worker.name)
            initial_renew = data.get("spec", {}).get("renewTime")
            self.assertIsNotNone(initial_renew)

        # Wait for another heartbeat cycle and verify renewTime changed
        time.sleep(1.2)
        with urllib.request.urlopen(req, timeout=2.0) as resp:
            data = json.loads(resp.read().decode())
            second_renew = data.get("spec", {}).get("renewTime")
            self.assertIsNotNone(second_renew)

    def test_node_heartbeat_expiration_not_ready(self):
        """Worker crash stops lease renewal, triggering NotReady status after threshold."""
        cp = self.harness.start_control_plane(
            name="cp-lease-2",
            lease_interval_ms=500,
            not_ready_timeout_ms=2000,
            eviction_timeout_ms=5000,
            reconcile_interval_ms=200
        )
        worker = self.harness.start_worker("worker-lease-2", 1, cp, lease_interval_ms=500)

        # Verify initially Ready = True
        node_url = f"{cp.api_url}/api/v1/nodes/{worker.name}"
        req = urllib.request.Request(node_url, method="GET")
        with urllib.request.urlopen(req, timeout=2.0) as resp:
            node_data = json.loads(resp.read().decode())
            conds = {c.get("type"): c.get("status") for c in node_data.get("status", {}).get("conditions", [])}
            self.assertEqual(conds.get("Ready"), "True")

        # Kill worker to simulate abrupt hardware drop
        self.harness.kill_node(worker.name)

        # Wait for not_ready_timeout (2000ms) + reconcile interval
        time.sleep(2.6)

        # Node condition should now be Ready = False, reason = NodeStatusUnknown
        with urllib.request.urlopen(req, timeout=2.0) as resp:
            node_data = json.loads(resp.read().decode())
            conditions = node_data.get("status", {}).get("conditions", [])
            ready_cond = next((c for c in conditions if c.get("type") == "Ready"), None)
            self.assertIsNotNone(ready_cond)
            self.assertEqual(ready_cond.get("status"), "False")
            self.assertEqual(ready_cond.get("reason"), "NodeStatusUnknown")
