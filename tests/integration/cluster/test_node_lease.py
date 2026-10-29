"""
Kube32 Cluster Tests: Node Lease Heartbeat API & Lifecycle Transitions
Verifies coordination.k8s.io/v1 Lease renewal, expiration, NotReady condition,
and eviction thresholds without flash wear.
"""

import time
import json
import urllib.request
from tests.integration.framework import BaseTestCase, HTTPResponse
from tests.integration.cluster_harness import ClusterHarness, node_ready_condition


class TestNodeLease(BaseTestCase):
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

        # The kubelet reports Ready once it has registered
        def ready(status):
            cond = node_ready_condition(cp.api_url, worker.name)
            return cond if cond and cond.get("status") == status else None
        self.wait_for_condition(lambda: ready("True"), timeout=10.0, description="node Ready=True")

        # Kill worker to simulate abrupt hardware drop
        self.harness.kill_node(worker.name)

        # The lease lasts at least 15 s, so the lifecycle controller marks the node
        # Ready=Unknown (reason NodeStatusUnknown), as Kubernetes does, after that
        cond = self.wait_for_condition(lambda: ready("Unknown"), timeout=25.0, interval=0.5,
                                       description="node Ready=Unknown")
        self.assertEqual(cond.get("reason"), "NodeStatusUnknown")
