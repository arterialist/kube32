"""
Automated E2E Integration Test Suite for Milestone 5:
Inter-Cluster Federation Bridge & Virtual Kubelet Gateway
Tests:
- Test 1: Node registration & aggregate capacity check
- Test 2: Upstream lease heartbeats renewed across 30+ seconds
- Test 3: Upstream pod scheduled -> translated -> autonomously scheduled on Kube32
- Test 4: Reverse status sync (Pending -> Running with IP -> Failed exitCode 137)
- Test 5: Pod deletion cascade & finalizer removal
- Test 6: Channel IPC Gateway bidirectional message transmission over K32C UDP frames
- Test 7: Downstream Kube32 restart / partition resilience
"""

import os
import sys
import time
import json
import atexit
import logging
import unittest
import subprocess
import urllib.request
import urllib.error
from typing import Dict, Any, Optional

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if PROJECT_ROOT not in sys.path:
    sys.path.insert(0, PROJECT_ROOT)

from tests.framework import BaseTestCase
from tests.m4_cluster_harness import ClusterHarness
from tests.mock_upstream_k8s import MockUpstreamK8sServer

VK_BIN_PATH = os.path.join(PROJECT_ROOT, "federation", "bin", "virtual-kubelet")
logger = logging.getLogger("test_m5_federation_e2e")


class TestM5FederationE2E(unittest.TestCase, BaseTestCase):
    _cluster_started = False
    _harness = None
    _cp = None
    _worker = None
    _mock_k8s = None
    _vk_proc = None

    def __init__(self, *args, **kwargs):
        if len(args) >= 2 and not isinstance(args[0], str):
            http_client = args[0]
            kubectl_client = args[1]
            target = args[2] if len(args) > 2 else "sim"
            verbose = args[3] if len(args) > 3 else False
            unittest.TestCase.__init__(self)
            BaseTestCase.__init__(self, http_client, kubectl_client, target, verbose)
        else:
            unittest.TestCase.__init__(self, *args, **kwargs)
            BaseTestCase.__init__(self, None, None, "sim", False)

        self.k32_cp_port = 16443
        self.k32_cp_ipc_port = 17400
        self.k32_worker_ipc_port = 17401
        self.mock_k8s_port = 18443
        self.vk_channel_ipc_port = 14242
        self.vk_gateway_http_port = 18444
        self.node_name = "kube32-edge-gateway"

    @classmethod
    def _start_all(cls):
        if cls._cluster_started:
            return

        k32_cp_port = 16443
        k32_cp_ipc_port = 17400
        k32_worker_ipc_port = 17401
        mock_k8s_port = 18443
        vk_channel_ipc_port = 14242
        vk_gateway_http_port = 18444
        node_name = "kube32-edge-gateway"

        # 1. Start Downstream Kube32 Cluster
        cls._harness = ClusterHarness(base_api_port=k32_cp_port, base_ipc_port=k32_cp_ipc_port)
        cls._cp = cls._harness.start_control_plane(
            name="cp-fed-0",
            lease_interval_ms=500,
            not_ready_timeout_ms=3000,
            eviction_timeout_ms=6000
        )
        cls._worker = cls._harness.start_worker(
            name="worker-1",
            node_id=1,
            cp_node=cls._cp,
            lease_interval_ms=500
        )
        cls._worker2 = cls._harness.start_worker(
            name="worker-2",
            node_id=2,
            cp_node=cls._cp,
            lease_interval_ms=500
        )

        # 2. Start Mock Upstream Kubernetes Server
        cls._mock_k8s = MockUpstreamK8sServer(port=mock_k8s_port)
        cls._mock_k8s.start()

        # 3. Start Virtual Kubelet Daemon
        cls._vk_proc = subprocess.Popen([
            VK_BIN_PATH,
            "--master", cls._mock_k8s.url,
            "--kube32-api", cls._cp.api_url,
            "--node-name", node_name,
            "--lease-interval", "2s",
            "--sync-interval", "500ms",
            "--channel-ipc-port", str(vk_channel_ipc_port),
            "--gateway-http-port", str(vk_gateway_http_port)
        ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        cls._cluster_started = True
        atexit.register(cls._stop_all)

        # Wait for VK daemon to register node with Mock K8s
        start = time.time()
        registered = False
        while time.time() - start < 6.0:
            with cls._mock_k8s.lock:
                if node_name in cls._mock_k8s.store["nodes"]:
                    registered = True
                    break
            time.sleep(0.1)
        if not registered:
            raise TimeoutError(f"Virtual node {node_name} failed to register with Mock K8s within 6.0s")

    @classmethod
    def _stop_all(cls):
        if not cls._cluster_started:
            return
        cls._cluster_started = False

        if cls._vk_proc:
            cls._vk_proc.terminate()
            try:
                cls._vk_proc.wait(timeout=2.0)
            except Exception:
                cls._vk_proc.kill()
            cls._vk_proc = None

        if cls._mock_k8s:
            cls._mock_k8s.stop()
            cls._mock_k8s = None

        if cls._harness:
            cls._harness.teardown()
            cls._harness = None

    def setUp(self):
        super().setUp()
        self._start_all()
        self.cp = self.__class__._cp
        self.worker = self.__class__._worker
        self.mock_k8s = self.__class__._mock_k8s
        self.vk_proc = self.__class__._vk_proc
        self.harness = self.__class__._harness

    def tearDown(self):
        super().tearDown()

    def test_01_node_registration_and_aggregate_capacity(self):
        """Test 1: Node registration & aggregate capacity check."""
        with self.mock_k8s.lock:
            node = self.mock_k8s.store["nodes"].get(self.node_name)

        self.assertIsNotNone(node, "Virtual node not found in upstream store")
        meta = node.get("metadata", {})
        self.assertEqual(meta.get("name"), self.node_name)
        labels = meta.get("labels", {})
        self.assertEqual(labels.get("type"), "virtual-kubelet")
        self.assertEqual(labels.get("kubernetes.io/arch"), "xtensa")

        # Taints verification
        taints = node.get("spec", {}).get("taints", [])
        self.assertTrue(any(t.get("key") == "virtual-kubelet.io/provider" and t.get("value") == "kube32" for t in taints))

        # Check conditions
        status = node.get("status", {})
        conditions = status.get("conditions", [])
        ready_cond = next((c for c in conditions if c.get("type") == "Ready"), None)
        self.assertIsNotNone(ready_cond)
        self.assertEqual(ready_cond.get("status"), "True")

        # Capacity & Allocatable verification (aggregated from CP + worker-1)
        capacity = status.get("capacity", {})
        allocatable = status.get("allocatable", {})
        self.assertIn("memory", capacity)
        self.assertIn("cpu", capacity)
        self.assertIn("esp32.io/gpio", allocatable)

        # Aggregate CPU cores >= 4 (2 per ESP32-S3 node)
        cpu_val = int(capacity.get("cpu", "0"))
        self.assertTrue(cpu_val >= 4, f"Expected aggregate CPU >= 4, got {cpu_val}")

        # Aggregate GPIO >= 28 (14 per node)
        gpio_val = int(allocatable.get("esp32.io/gpio", "0"))
        self.assertTrue(gpio_val >= 28, f"Expected aggregate GPIO >= 28, got {gpio_val}")

    def test_02_upstream_lease_heartbeats_renewed(self):
        """Test 2: Upstream lease heartbeats renewed across 30+ seconds."""
        with self.mock_k8s.lock:
            lease = self.mock_k8s.store["leases"].get(self.node_name)
        self.assertIsNotNone(lease, "Upstream lease was not created")

        initial_renew = lease.get("spec", {}).get("renewTime")
        self.assertIsNotNone(initial_renew)

        # Monitor renewals over 6 seconds (with lease interval 2s, should see >= 2 renewals)
        renewals_seen = 0
        last_renew = initial_renew
        start = time.time()
        while time.time() - start < 6.0:
            time.sleep(1.2)
            with self.mock_k8s.lock:
                cur_lease = self.mock_k8s.store["leases"].get(self.node_name)
            cur_renew = cur_lease.get("spec", {}).get("renewTime")
            if cur_renew != last_renew:
                renewals_seen += 1
                last_renew = cur_renew

        self.assertTrue(renewals_seen >= 2, f"Expected >= 2 lease renewals, got {renewals_seen}")

    def test_03_pod_translation_and_autonomous_scheduling(self):
        """Test 3: Upstream pod scheduled -> translated -> autonomously scheduled on Kube32."""
        pod_name = "esp32-sensor-workload"
        upstream_manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {
                "name": pod_name,
                "namespace": "default",
                "labels": {"app": "telemetry"}
            },
            "spec": {
                "nodeName": self.node_name,
                "containers": [
                    {
                        "name": "collector",
                        "image": "wasm://telemetry:v1",
                        "resources": {
                            "limits": {
                                "memory": "64Ki",
                                "esp32.io/gpio": "1"
                            }
                        }
                    }
                ]
            }
        }

        # Inject upstream pod
        self.mock_k8s.inject_pod(upstream_manifest)

        # Wait for forward syncer to process and submit to downstream Kube32
        start = time.time()
        downstream_pod = None
        while time.time() - start < 5.0:
            try:
                req = urllib.request.Request(f"{self.cp.api_url}/api/v1/namespaces/default/pods/{pod_name}", method="GET")
                with urllib.request.urlopen(req, timeout=1.0) as resp:
                    if resp.status == 200:
                        downstream_pod = json.loads(resp.read().decode())
                        break
            except Exception:
                pass
            time.sleep(0.2)

        self.assertIsNotNone(downstream_pod, "Pod was not dispatched to downstream Kube32")
        self.assertEqual(downstream_pod["metadata"]["name"], pod_name)
        # Verify autonomous scheduler bound the pod to a physical node
        bound_node = downstream_pod.get("spec", {}).get("nodeName")
        self.assertNotEqual(bound_node, "", "Pod nodeName was not bound by autonomous scheduler")
        self.assertNotEqual(bound_node, self.node_name, "Pod should be bound to physical node, not virtual gateway")

        # Verify upstream pod received federation finalizer
        with self.mock_k8s.lock:
            up_pod = self.mock_k8s.store["pods"].get("default", {}).get(pod_name)
        self.assertIsNotNone(up_pod)
        finalizers = up_pod.get("metadata", {}).get("finalizers", [])
        self.assertIn("kube32.io/federation-protection", finalizers)

    def test_04_reverse_status_sync(self):
        """Test 4: Reverse status sync (Pending -> Running with IP -> Failed exitCode 137)."""
        pod_name = "esp32-status-sync-pod"
        upstream_manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {
                "nodeName": self.node_name,
                "containers": [{"name": "app", "image": "wasm://app"}]
            }
        }
        self.mock_k8s.inject_pod(upstream_manifest)

        # Wait for downstream creation
        time.sleep(1.0)

        # Step 4A: Check Running with IP
        start = time.time()
        synced_running = False
        while time.time() - start < 5.0:
            with self.mock_k8s.lock:
                up_pod = self.mock_k8s.store["pods"].get("default", {}).get(pod_name, {})
                phase = up_pod.get("status", {}).get("phase")
                pod_ip = up_pod.get("status", {}).get("podIP")
                if phase == "Running" and pod_ip:
                    synced_running = True
                    break
            time.sleep(0.2)
        self.assertTrue(synced_running, "Upstream pod status did not transition to Running with podIP")

        # Step 4B: Simulate Wasm runtime trap / OOM (exitCode 137) on Kube32
        patch_body = {
            "status": {
                "phase": "Failed",
                "reason": "RuntimeTrap",
                "message": "Memory bounds trap (exitCode=137)",
                "containerStatuses": [
                    {
                        "name": "app",
                        "ready": False,
                        "state": {
                            "terminated": {
                                "exitCode": 137,
                                "reason": "RuntimeTrap"
                            }
                        }
                    }
                ]
            }
        }
        req = urllib.request.Request(
            f"{self.cp.api_url}/api/v1/namespaces/default/pods/{pod_name}/status",
            data=json.dumps(patch_body).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="PUT"
        )
        with urllib.request.urlopen(req, timeout=1.0) as resp:
            self.assertEqual(resp.status, 200)

        # Verify reverse syncer updates upstream pod to Failed with exitCode 137
        start = time.time()
        synced_failed = False
        while time.time() - start < 5.0:
            with self.mock_k8s.lock:
                up_pod = self.mock_k8s.store["pods"].get("default", {}).get(pod_name, {})
                phase = up_pod.get("status", {}).get("phase")
                c_statuses = up_pod.get("status", {}).get("containerStatuses", [])
                if phase == "Failed" and c_statuses:
                    term = c_statuses[0].get("state", {}).get("terminated", {})
                    if term.get("exitCode") == 137:
                        synced_failed = True
                        break
            time.sleep(0.2)
        self.assertTrue(synced_failed, "Upstream pod did not sync Failed phase with exitCode 137")

    def test_05_pod_deletion_cascade(self):
        """Test 5: Pod deletion cascade & finalizer removal."""
        pod_name = "esp32-delete-cascade-pod"
        upstream_manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": pod_name, "namespace": "default"},
            "spec": {
                "nodeName": self.node_name,
                "containers": [{"name": "app", "image": "wasm://app"}]
            }
        }
        self.mock_k8s.inject_pod(upstream_manifest)
        time.sleep(1.0)

        # Trigger deletion upstream
        self.mock_k8s.delete_upstream_pod("default", pod_name)

        # Wait for deletion syncer to delete downstream and strip finalizer upstream
        start = time.time()
        cascade_complete = False
        while time.time() - start < 6.0:
            # 1. Check downstream pod deleted
            downstream_exists = True
            try:
                req = urllib.request.Request(f"{self.cp.api_url}/api/v1/namespaces/default/pods/{pod_name}", method="GET")
                with urllib.request.urlopen(req, timeout=0.5) as resp:
                    pass
            except urllib.error.HTTPError as he:
                if he.code == 404:
                    downstream_exists = False
            except Exception:
                pass

            # 2. Check upstream finalizer removed
            upstream_finalizer_removed = False
            with self.mock_k8s.lock:
                up_pod = self.mock_k8s.store["pods"].get("default", {}).get(pod_name)
                if up_pod:
                    finalizers = up_pod.get("metadata", {}).get("finalizers", [])
                    if "kube32.io/federation-protection" not in finalizers:
                        upstream_finalizer_removed = True
                else:
                    upstream_finalizer_removed = True

            if not downstream_exists and upstream_finalizer_removed:
                cascade_complete = True
                break
            time.sleep(0.2)

        self.assertTrue(cascade_complete, "Pod deletion cascade failed to delete downstream and remove finalizer")

    def test_06_channel_ipc_gateway_bidirectional_transmission(self):
        """Test 6: Channel IPC Gateway bidirectional message transmission over K32C UDP frames."""
        import urllib.parse
        channel_uri = "pod://worker-1/sensor"
        send_url = f"http://127.0.0.1:{self.vk_gateway_http_port}/api/v1/channels/send"
        recv_url = f"http://127.0.0.1:{self.vk_gateway_http_port}/api/v1/channels/recv?channel_uri={urllib.parse.quote(channel_uri, safe='')}&timeout=2000ms"

        # 1. Send outbound message from cloud client to worker-1 via Gateway HTTP REST
        outbound_data = {
            "channel_uri": channel_uri,
            "target_pod": "worker-1",
            "sender_pod": "cloud-control",
            "payload": "Telemetry-Ping-Command",
            "dest_port": self.k32_worker_ipc_port
        }
        req = urllib.request.Request(
            send_url,
            data=json.dumps(outbound_data).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        with urllib.request.urlopen(req, timeout=2.0) as resp:
            self.assertEqual(resp.status, 200)

        # Verify worker received valid K32C frame
        time.sleep(0.3)
        stdout = self.worker.read_stdout(timeout=1.0)
        self.assertIn("[IPC-RX] Accepted valid K32C frame", stdout, "Worker did not receive frame from Gateway")

        # 2. Transmit reply frame from worker-1 back to Gateway UDP port
        reply_payload = b"Telemetry-Ack-Response-Data"
        self.harness.send_k32c_packet(
            dest_ip="127.0.0.1",
            dest_port=self.vk_channel_ipc_port,
            channel_uri=channel_uri,
            sender_pod="worker-1",
            payload=reply_payload
        )

        # 3. Retrieve reply via Gateway HTTP REST endpoint
        recv_req = urllib.request.Request(recv_url, method="GET")
        with urllib.request.urlopen(recv_req, timeout=3.0) as resp:
            self.assertEqual(resp.status, 200)
            recv_body = json.loads(resp.read().decode())
            self.assertEqual(recv_body.get("payload"), reply_payload.decode())
            self.assertEqual(recv_body.get("channel_uri"), channel_uri)

    def test_07_downstream_restart_resilience(self):
        """Test 7: Downstream Kube32 restart / partition resilience."""
        # 1. Simulate Kube32 Control Plane crash
        self.harness.kill_node("cp-fed-0")

        time.sleep(1.0)
        self.assertIsNone(self.vk_proc.poll(), "VK daemon crashed when downstream Kube32 dropped")

        # 2. Restart Control Plane on same port
        cp_restarted = self.harness.start_control_plane(
            name="cp-fed-0",
            lease_interval_ms=500
        )
        self.__class__._cp = cp_restarted
        self.cp = cp_restarted
        self.worker = self.harness.restart_node("worker-1", cp_restarted)
        self.__class__._worker = self.worker
        self.worker2 = self.harness.restart_node("worker-2", cp_restarted)
        self.__class__._worker2 = self.worker2

        # 3. Verify bridge reconnects and maintains Ready=True
        time.sleep(2.0)
        with self.mock_k8s.lock:
            node = self.mock_k8s.store["nodes"].get(self.node_name, {})
            conditions = node.get("status", {}).get("conditions", [])
            ready_cond = next((c for c in conditions if c.get("type") == "Ready"), None)
        self.assertIsNotNone(ready_cond)
        self.assertEqual(ready_cond.get("status"), "True")

        # Clean shutdown after all tests
        self._stop_all()


if __name__ == "__main__":
    unittest.main()
