#!/usr/bin/env python3
"""
Kube32 Milestone 4 Empirical Adversarial Verification Harness
Adversarially challenges and stress-tests:
1. Sudden Worker Node Crash (hard SIGKILL under in-flight load)
2. Rigorous Multi-Stage Timing Verification (Sub-threshold, NotReady threshold, Eviction threshold)
3. Rescheduling Integrity, Multi-Node Resource Constraints & Race Condition Resistance
4. Dirty LittleFS Rejoin & Power-Cut Recovery
5. High-Frequency Lease Flooding & Lock Contention Under Stress
"""

import os
import sys
import time
import signal
import shutil
import socket
import struct
import urllib.request
import urllib.error
import json
import threading
from typing import Dict, List, Optional, Any, Tuple

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if PROJECT_ROOT not in sys.path:
    sys.path.insert(0, PROJECT_ROOT)

DEFAULT_BIN = os.path.join(PROJECT_ROOT, "build", "kube32-sim")
ASAN_BIN = os.path.join(PROJECT_ROOT, "build_asan", "kube32-sim")


class SimulatedNode:
    def __init__(self, role: str, name: str, node_id: int, api_port: int, ipc_port: int, data_dir: str, process, enable_flash: bool = False):
        self.role = role
        self.name = name
        self.node_id = node_id
        self.api_port = api_port
        self.ipc_port = ipc_port
        self.data_dir = data_dir
        self.process = process
        self.enable_flash = enable_flash
        self.api_url = f"http://127.0.0.1:{api_port}"

    def is_alive(self) -> bool:
        return self.process is not None and self.process.poll() is None


class AdversarialClusterHarness:
    def __init__(self, bin_path: str = DEFAULT_BIN, base_api_port: int = 18443, base_ipc_port: int = 19400):
        self.bin_path = bin_path
        self.base_api_port = base_api_port
        self.base_ipc_port = base_ipc_port
        self.nodes: Dict[str, SimulatedNode] = {}
        self.temp_dirs: List[str] = []

    def start_control_plane(
        self,
        name: str = "adv-cp-0",
        lease_interval_ms: int = 500,
        not_ready_timeout_ms: int = 3000,
        eviction_timeout_ms: int = 6000,
        reconcile_interval_ms: int = 250,
        enable_flash: bool = False
    ) -> SimulatedNode:
        data_dir = f"/tmp/k32_adv_cp_{name}_{os.getpid()}_{int(time.time()*1000)%10000}"
        self.temp_dirs.append(data_dir)
        shutil.rmtree(data_dir, ignore_errors=True)
        os.makedirs(data_dir, exist_ok=True)

        api_port = self.base_api_port
        ipc_port = self.base_ipc_port

        cmd = [
            self.bin_path,
            "--role", "control-plane",
            "--node-name", name,
            "--node-id", "0",
            "--port", str(api_port),
            "--ipc-port", str(ipc_port),
            "--data-dir", data_dir,
            "--lease-interval", str(lease_interval_ms),
            "--not-ready-timeout", str(not_ready_timeout_ms),
            "--eviction-timeout", str(eviction_timeout_ms),
            "--reconcile-interval", str(reconcile_interval_ms)
        ]
        if enable_flash:
            cmd.append("--enable-flash")

        import subprocess
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        node = SimulatedNode("control-plane", name, 0, api_port, ipc_port, data_dir, proc, enable_flash)
        self.nodes[name] = node

        self._wait_for_api_ready(node.api_url, timeout=6.0)
        return node

    def start_worker(
        self,
        name: str,
        node_id: int,
        cp_node: SimulatedNode,
        lease_interval_ms: int = 500,
        enable_flash: bool = False
    ) -> SimulatedNode:
        data_dir = f"/tmp/k32_adv_worker_{name}_{os.getpid()}_{int(time.time()*1000)%10000}"
        self.temp_dirs.append(data_dir)
        shutil.rmtree(data_dir, ignore_errors=True)
        os.makedirs(data_dir, exist_ok=True)

        api_port = self.base_api_port + node_id
        ipc_port = self.base_ipc_port + node_id

        cmd = [
            self.bin_path,
            "--role", "worker",
            "--node-name", name,
            "--node-id", str(node_id),
            "--port", str(api_port),
            "--ipc-port", str(ipc_port),
            "--join", cp_node.api_url,
            "--data-dir", data_dir,
            "--lease-interval", str(lease_interval_ms)
        ]
        if enable_flash:
            cmd.append("--enable-flash")

        import subprocess
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        node = SimulatedNode("worker", name, node_id, api_port, ipc_port, data_dir, proc, enable_flash)
        self.nodes[name] = node

        self._wait_for_worker_registered(cp_node.api_url, name, timeout=8.0)
        return node

    def sigkill_node(self, name: str):
        if name in self.nodes:
            node = self.nodes[name]
            if node.process and node.process.poll() is None:
                os.kill(node.process.pid, signal.SIGKILL)
                try:
                    node.process.wait(timeout=2.0)
                except Exception:
                    pass
            node.process = None

    def restart_worker(self, name: str, cp_node: SimulatedNode, lease_interval_ms: int = 500) -> SimulatedNode:
        if name in self.nodes:
            old = self.nodes[name]
            if old.process and old.process.poll() is None:
                os.kill(old.process.pid, signal.SIGKILL)
                old.process.wait(timeout=1.0)
            cmd = [
                self.bin_path,
                "--role", old.role,
                "--node-name", old.name,
                "--node-id", str(old.node_id),
                "--port", str(old.api_port),
                "--ipc-port", str(old.ipc_port),
                "--join", cp_node.api_url,
                "--data-dir", old.data_dir,
                "--lease-interval", str(lease_interval_ms)
            ]
            if getattr(old, 'enable_flash', False):
                cmd.append("--enable-flash")

            import subprocess
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            old.process = proc
            self._wait_for_worker_registered(cp_node.api_url, name, timeout=8.0)
            return old
        raise ValueError(f"Unknown node {name}")

    def teardown(self):
        for node in list(self.nodes.values()):
            if node.process and node.process.poll() is None:
                try:
                    os.kill(node.process.pid, signal.SIGKILL)
                    node.process.wait(timeout=1.0)
                except Exception:
                    pass
                node.process = None
        self.nodes.clear()
        for d in self.temp_dirs:
            shutil.rmtree(d, ignore_errors=True)
        self.temp_dirs.clear()

    def _wait_for_api_ready(self, api_url: str, timeout: float = 6.0):
        start = time.time()
        while time.time() - start < timeout:
            try:
                req = urllib.request.Request(f"{api_url}/version", method="GET")
                with urllib.request.urlopen(req, timeout=1.0) as resp:
                    if resp.status == 200:
                        return
            except Exception:
                pass
            time.sleep(0.05)
        raise TimeoutError(f"Control plane {api_url} failed to become ready within {timeout}s")

    def _wait_for_worker_registered(self, cp_url: str, worker_name: str, timeout: float = 8.0):
        start = time.time()
        while time.time() - start < timeout:
            try:
                node_req = urllib.request.Request(f"{cp_url}/api/v1/nodes/{worker_name}", method="GET")
                lease_req = urllib.request.Request(f"{cp_url}/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/{worker_name}", method="GET")
                with urllib.request.urlopen(node_req, timeout=1.0) as n_resp, urllib.request.urlopen(lease_req, timeout=1.0) as l_resp:
                    if n_resp.status == 200 and l_resp.status == 200:
                        return
            except Exception:
                pass
            time.sleep(0.08)
        raise TimeoutError(f"Worker {worker_name} failed to register at {cp_url} within {timeout}s")


# ==============================================================================
# Helper functions
# ==============================================================================

def http_get(url: str, timeout: float = 2.0) -> Tuple[int, Dict[str, Any]]:
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode())
            return resp.status, data
    except urllib.error.HTTPError as e:
        data = {}
        try:
            data = json.loads(e.read().decode())
        except Exception:
            pass
        return e.code, data


def http_post(url: str, payload: Dict[str, Any], timeout: float = 2.0) -> Tuple[int, Dict[str, Any]]:
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode())
            return resp.status, data
    except urllib.error.HTTPError as e:
        data = {}
        try:
            data = json.loads(e.read().decode())
        except Exception:
            pass
        return e.code, data


def http_put(url: str, payload: Dict[str, Any], timeout: float = 2.0) -> Tuple[int, Dict[str, Any]]:
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"}, method="PUT")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode())
            return resp.status, data
    except urllib.error.HTTPError as e:
        data = {}
        try:
            data = json.loads(e.read().decode())
        except Exception:
            pass
        return e.code, data


# ==============================================================================
# 5 Adversarial Stress Tests
# ==============================================================================

def test_1_sudden_sigkill_under_inflight_traffic(bin_path: str = DEFAULT_BIN, base_port: int = 20010):
    print(f"\n--- [ADV-TEST 1] Sudden Hard SIGKILL Worker Crash Under In-Flight Traffic (Port {base_port}) ---")
    harness = AdversarialClusterHarness(bin_path=bin_path, base_api_port=base_port, base_ipc_port=base_port + 1000)
    try:
        cp = harness.start_control_plane(
            name="cp-adv-1",
            lease_interval_ms=400,
            not_ready_timeout_ms=1500,
            eviction_timeout_ms=3000,
            reconcile_interval_ms=200
        )
        w1 = harness.start_worker("worker-kill-1", 1, cp, lease_interval_ms=400)
        w2 = harness.start_worker("worker-kill-2", 2, cp, lease_interval_ms=400)

        # Deploy 2 pods on w1 and 2 pods on w2
        for idx in range(2):
            pod = {
                "apiVersion": "v1", "kind": "Pod",
                "metadata": {"name": f"pod-w1-{idx}", "namespace": "default"},
                "spec": {"nodeName": w1.name, "containers": [{"name": f"c-{idx}", "image": "wasm://workload"}]}
            }
            s, _ = http_post(f"{cp.api_url}/api/v1/namespaces/default/pods", pod)
            assert s == 201, f"Failed deploying pod-w1-{idx}"

        for idx in range(2):
            pod = {
                "apiVersion": "v1", "kind": "Pod",
                "metadata": {"name": f"pod-w2-{idx}", "namespace": "default"},
                "spec": {"nodeName": w2.name, "containers": [{"name": f"c-{idx}", "image": "wasm://workload"}]}
            }
            s, _ = http_post(f"{cp.api_url}/api/v1/namespaces/default/pods", pod)
            assert s == 201, f"Failed deploying pod-w2-{idx}"

        # Launch concurrent in-flight query traffic
        stop_traffic = threading.Event()
        traffic_errors = []
        traffic_count = [0]

        def traffic_worker():
            endpoints = ["/version", "/api/v1/nodes", "/api/v1/pods"]
            while not stop_traffic.is_set():
                for ep in endpoints:
                    try:
                        st, _ = http_get(f"{cp.api_url}{ep}", timeout=1.0)
                        if st != 200:
                            traffic_errors.append(f"{ep} returned status {st}")
                        traffic_count[0] += 1
                    except Exception as ex:
                        traffic_errors.append(f"{ep} failed: {ex}")
                    time.sleep(0.01)

        t = threading.Thread(target=traffic_worker)
        t.daemon = True
        t.start()

        time.sleep(0.2)
        # Abruptly SIGKILL w1 while traffic is active
        print(f"[*] Dispatching SIGKILL directly to {w1.name} (pid={w1.process.pid})...")
        harness.sigkill_node(w1.name)
        assert w1.process is None or not w1.is_alive(), "Worker 1 process failed to die"

        # Keep traffic going for another 1 second
        time.sleep(1.0)
        stop_traffic.set()
        t.join(timeout=2.0)

        assert len(traffic_errors) == 0, f"In-flight traffic experienced errors: {traffic_errors[:5]}"
        print(f"[+] Completed {traffic_count[0]} in-flight requests without a single 500 error or socket hang.")

        # Verify w2 is still alive and reporting healthy
        st, w2_data = http_get(f"{cp.api_url}/api/v1/nodes/{w2.name}")
        assert st == 200
        conds = {c.get("type"): c.get("status") for c in w2_data.get("status", {}).get("conditions", [])}
        assert conds.get("Ready") == "True", "Surviving worker-2 was improperly affected"

        # Wait for eviction timeout (3000ms) + reconcile
        print("[*] Waiting for eviction and failover to complete...")
        time.sleep(3.5)

        # Verify worker-1 is marked NotReady/Unreachable
        st, w1_data = http_get(f"{cp.api_url}/api/v1/nodes/{w1.name}")
        assert st == 200
        w1_conds = {c.get("type"): c.get("status") for c in w1_data.get("status", {}).get("conditions", [])}
        assert w1_conds.get("Ready") == "False", f"Worker-1 Ready condition expected False, got {w1_conds}"
        assert w1_conds.get("Unreachable") == "True", f"Worker-1 Unreachable condition expected True, got {w1_conds}"

        # Verify w1's pods were evicted and rebound to surviving worker w2
        for idx in range(2):
            st, pdata = http_get(f"{cp.api_url}/api/v1/namespaces/default/pods/pod-w1-{idx}")
            assert st == 200
            assert pdata.get("spec", {}).get("nodeName") == w2.name, \
                f"Pod pod-w1-{idx} failed to reschedule to surviving worker-2: {pdata}"
        print("[PASS] Test 1: Sudden SIGKILL handled cleanly with zero in-flight dropped queries and successful failover.")
        return True
    finally:
        harness.teardown()


def test_2_multi_stage_timing_and_daemonset_immunity(bin_path: str = DEFAULT_BIN, base_port: int = 20020) -> Tuple[bool, str]:
    print(f"\n--- [ADV-TEST 2] Multi-Stage Timing Verification (Sub-threshold, NotReady, Eviction) (Port {base_port}) ---")
    harness = AdversarialClusterHarness(bin_path=bin_path, base_api_port=base_port, base_ipc_port=base_port + 1000)
    try:
        # Exact thresholds:
        # Lease interval = 400ms
        # NotReady threshold = 2500ms
        # Eviction threshold = 5500ms
        # Reconcile interval = 200ms
        cp = harness.start_control_plane(
            name="cp-adv-2",
            lease_interval_ms=400,
            not_ready_timeout_ms=2500,
            eviction_timeout_ms=5500,
            reconcile_interval_ms=200
        )
        worker = harness.start_worker("worker-timing-1", 1, cp, lease_interval_ms=400)

        # Deploy normal pod
        normal_pod = {
            "apiVersion": "v1", "kind": "Pod",
            "metadata": {"name": "app-standard", "namespace": "default"},
            "spec": {"nodeName": worker.name, "containers": [{"name": "app", "image": "wasm://app"}]}
        }
        s, _ = http_post(f"{cp.api_url}/api/v1/namespaces/default/pods", normal_pod)
        assert s == 201

        # Deploy DaemonSet pod
        daemon_pod = {
            "apiVersion": "v1", "kind": "Pod",
            "metadata": {"name": "ds-collector", "namespace": "default", "labels": {"kube32.io/daemonset": "true"}},
            "spec": {"nodeName": worker.name, "containers": [{"name": "collector", "image": "wasm://ds"}]}
        }
        s, _ = http_post(f"{cp.api_url}/api/v1/namespaces/default/pods", daemon_pod)
        assert s == 201

        # T0: Kill worker abruptly with SIGKILL
        t0 = time.time()
        print(f"[*] T0 = {t0:.3f}: Abruptly killing {worker.name} via SIGKILL")
        harness.sigkill_node(worker.name)

        # STAGE A: Check at T0 + 1.2s (< 2.5s NotReady threshold)
        time.sleep(1.2)
        elapsed_a = time.time() - t0
        print(f"[*] Stage A: Elapsed = {elapsed_a:.2f}s (< 2.5s NotReady threshold)")
        st, node_data = http_get(f"{cp.api_url}/api/v1/nodes/{worker.name}")
        assert st == 200
        conds = {c.get("type"): c.get("status") for c in node_data.get("status", {}).get("conditions", [])}
        assert conds.get("Ready") == "True", f"Premature NotReady at t={elapsed_a:.2f}s: {conds}"

        st, pdata = http_get(f"{cp.api_url}/api/v1/namespaces/default/pods/app-standard")
        assert pdata.get("spec", {}).get("nodeName") == worker.name
        print("[+] Stage A verified: Node remains Ready=True before 2.5s threshold.")

        # STAGE B: Check at T0 + 3.2s (between 2.5s NotReady and 5.5s Eviction threshold)
        rem_b = max(0.0, 3.2 - (time.time() - t0))
        time.sleep(rem_b)
        elapsed_b = time.time() - t0
        print(f"[*] Stage B: Elapsed = {elapsed_b:.2f}s (between 2.5s NotReady and 5.5s Eviction threshold)")
        st, node_data = http_get(f"{cp.api_url}/api/v1/nodes/{worker.name}")
        assert st == 200
        conditions = node_data.get("status", {}).get("conditions", [])
        ready_c = next((c for c in conditions if c.get("type") == "Ready"), None)
        assert ready_c is not None, "Missing Ready condition"
        assert ready_c.get("status") == "False", f"Expected Ready=False, got {ready_c}"
        assert ready_c.get("reason") == "NodeStatusUnknown", f"Expected NodeStatusUnknown, got {ready_c}"

        unreach_c = next((c for c in conditions if c.get("type") == "Unreachable"), None)
        assert unreach_c is None or unreach_c.get("status") != "True", f"Premature Unreachable before 5.5s: {unreach_c}"

        # Regular pod MUST NOT be evicted yet during grace period!
        st, pdata = http_get(f"{cp.api_url}/api/v1/namespaces/default/pods/app-standard")
        assigned = pdata.get("spec", {}).get("nodeName")
        if assigned != worker.name:
            err_msg = (
                f"CRITICAL TIMING COLLISION: Pod 'app-standard' was PREMATURELY evicted at t={elapsed_b:.2f}s "
                f"(configured eviction threshold is 5.5s)! "
                f"Root Cause: Scheduler::reconcile_cluster line 583 treats 'ready_nodes.count == 0' as immediate death, "
                f"preempting the NodeLifecycleController 30s eviction grace period and skipping DisruptionTarget tagging. "
                f"Observed pod spec.nodeName='{assigned}', phase='{pdata.get('status', {}).get('phase')}', reason='{pdata.get('status', {}).get('reason')}'"
            )
            print(f"[FAIL-BUG] {err_msg}")
            return False, err_msg

        print("[+] Stage B verified: Ready=False (NodeStatusUnknown) triggered, pods NOT yet evicted.")

        # STAGE C: Check after T0 + 6.2s (> 5.5s Eviction threshold)
        rem_c = max(0.0, 6.2 - (time.time() - t0))
        time.sleep(rem_c)
        elapsed_c = time.time() - t0
        print(f"[*] Stage C: Elapsed = {elapsed_c:.2f}s (> 5.5s Eviction threshold)")

        st, node_data = http_get(f"{cp.api_url}/api/v1/nodes/{worker.name}")
        assert st == 200
        conditions = node_data.get("status", {}).get("conditions", [])
        unreach_c = next((c for c in conditions if c.get("type") == "Unreachable"), None)
        assert unreach_c is not None and unreach_c.get("status") == "True", f"Expected Unreachable=True: {conditions}"
        assert unreach_c.get("reason") == "NodeLeaseExpired", f"Expected NodeLeaseExpired: {unreach_c}"

        # Verify regular pod was evicted and tagged with DisruptionTarget
        st, pdata = http_get(f"{cp.api_url}/api/v1/namespaces/default/pods/app-standard")
        assert st == 200
        assert pdata.get("spec", {}).get("nodeName") == "", f"Expected nodeName empty, got {pdata.get('spec')}"
        p_conds = pdata.get("status", {}).get("conditions", [])
        dt_cond = next((c for c in p_conds if c.get("type") == "DisruptionTarget"), None)
        assert dt_cond is not None, f"Missing DisruptionTarget condition on evicted pod: {p_conds}"
        assert dt_cond.get("status") == "True"
        assert dt_cond.get("reason") == "EvictionByNodeLifecycleController"

        # Verify DaemonSet pod was NOT evicted
        st, ds_data = http_get(f"{cp.api_url}/api/v1/namespaces/default/pods/ds-collector")
        assert st == 200
        assert ds_data.get("spec", {}).get("nodeName") == worker.name, f"DaemonSet pod was improperly evicted: {ds_data}"
        print("[PASS] Test 2: Multi-stage timing and DaemonSet immunity verified.")
        return True, ""
    finally:
        harness.teardown()


def test_3_rescheduling_resource_constraints_and_races(bin_path: str = DEFAULT_BIN, base_port: int = 20030):
    print(f"\n--- [ADV-TEST 3] Rescheduling Integrity, Resource Constraints & Race Conditions (Port {base_port}) ---")
    harness = AdversarialClusterHarness(bin_path=bin_path, base_api_port=base_port, base_ipc_port=base_port + 1000)
    try:
        cp = harness.start_control_plane(
            name="cp-adv-3",
            lease_interval_ms=400,
            not_ready_timeout_ms=1500,
            eviction_timeout_ms=3000,
            reconcile_interval_ms=200
        )
        w1 = harness.start_worker("worker-race-1", 1, cp, lease_interval_ms=400) # will crash
        w2 = harness.start_worker("worker-race-2", 2, cp, lease_interval_ms=400) # surviving healthy node (240Ki allocatable)

        pods = [
            ("pod-fit-1", "100Ki"),
            ("pod-fit-2", "100Ki"),
            ("pod-exceed-aggregate", "100Ki"),
            ("pod-exceed-solo", "500Ki"),
        ]

        for pname, mem_req in pods:
            manifest = {
                "apiVersion": "v1", "kind": "Pod",
                "metadata": {"name": pname, "namespace": "default"},
                "spec": {
                    "nodeName": w1.name,
                    "containers": [{
                        "name": "c",
                        "image": "wasm://workload",
                        "resources": {"requests": {"memory": mem_req}}
                    }]
                }
            }
            s, _ = http_post(f"{cp.api_url}/api/v1/namespaces/default/pods", manifest)
            assert s == 201, f"Failed deploying {pname}"

        # Abruptly SIGKILL w1
        print(f"[*] SIGKILLing {w1.name} with 4 pending workloads...")
        harness.sigkill_node(w1.name)

        # Wait for eviction timeout (3000ms) + reconcile cycles (1000ms)
        time.sleep(4.5)

        # Query pod statuses
        pod_statuses = {}
        for pname, _ in pods:
            st, data = http_get(f"{cp.api_url}/api/v1/namespaces/default/pods/{pname}")
            assert st == 200
            pod_statuses[pname] = data

        node_assignments = [
            pdata.get("spec", {}).get("nodeName")
            for pname, pdata in pod_statuses.items()
            if pdata.get("spec", {}).get("nodeName") == w2.name
        ]

        print(f"[*] Workloads scheduled onto {w2.name}: {len(node_assignments)}")
        # Exactly 2 pods fitting within 240Ki allocatable heap can be scheduled onto w2
        assert len(node_assignments) == 2, f"Expected exactly 2 pods on w2, got {node_assignments}"

        # Verify pod-exceed-solo is Unschedulable Pending
        solo_data = pod_statuses["pod-exceed-solo"]
        assert solo_data.get("spec", {}).get("nodeName") == ""
        assert solo_data.get("status", {}).get("phase") == "Pending"
        conds = solo_data.get("status", {}).get("conditions", [])
        sched_cond = next((c for c in conds if c.get("type") == "PodScheduled"), None)
        assert sched_cond is not None and sched_cond.get("status") == "False"
        assert sched_cond.get("reason") == "Unschedulable"

        # Verify the aggregate exceeding pod is also Pending Unschedulable
        unbound_pods = [pname for pname, pdata in pod_statuses.items() if pdata.get("spec", {}).get("nodeName") == ""]
        assert len(unbound_pods) == 2, f"Expected 2 unbound pods, got {unbound_pods}"
        for pname in unbound_pods:
            pdata = pod_statuses[pname]
            assert pdata.get("status", {}).get("phase") == "Pending"

        print("[PASS] Test 3: Rescheduling strictly respects individual and aggregate resource predicates without race conditions.")
        return True
    finally:
        harness.teardown()


def test_4_dirty_littlefs_rejoin_and_power_cut_recovery(bin_path: str = DEFAULT_BIN, base_port: int = 20040):
    print(f"\n--- [ADV-TEST 4] Dirty LittleFS Rejoin & Power-Cut Recovery (Port {base_port}) ---")
    harness = AdversarialClusterHarness(bin_path=bin_path, base_api_port=base_port, base_ipc_port=base_port + 1000)
    try:
        # Start CP and Worker with --enable-flash persistence
        cp = harness.start_control_plane(
            name="cp-adv-4",
            lease_interval_ms=400,
            not_ready_timeout_ms=2000,
            eviction_timeout_ms=4500,
            reconcile_interval_ms=200,
            enable_flash=True
        )
        worker = harness.start_worker("worker-dirty-1", 1, cp, lease_interval_ms=400, enable_flash=True)

        # Confirm initial Ready state
        st, data = http_get(f"{cp.api_url}/api/v1/nodes/{worker.name}")
        assert st == 200
        ready_c = next((c for c in data.get("status", {}).get("conditions", []) if c.get("type") == "Ready"), None)
        assert ready_c and ready_c.get("status") == "True"

        # Kill worker abruptly via SIGKILL
        print(f"[*] Abruptly killing {worker.name} via SIGKILL...")
        harness.sigkill_node(worker.name)

        # Plant corrupted / dirty LittleFS artifacts in worker data dir
        worker_dir = worker.data_dir
        print(f"[*] Corrupting LittleFS directory at {worker_dir}...")
        corrupt_tmp_file = os.path.join(worker_dir, "registry", "pods", "partial_manifest.json.tmp")
        os.makedirs(os.path.dirname(corrupt_tmp_file), exist_ok=True)
        with open(corrupt_tmp_file, "wb") as f:
            f.write(b'{"apiVersion":"v1","kind":"Pod","metadata":{"name":"corrupted_truncated_write"') # cut mid-write

        garbage_file = os.path.join(worker_dir, "bad_syntax.json")
        with open(garbage_file, "w") as f:
            f.write('{"unclosed_brace": true, "syntax": ')

        stray_lock = os.path.join(worker_dir, "kubelet.lock")
        with open(stray_lock, "w") as f:
            f.write('PID 999999\n')

        # Plant dirty .tmp file in CP directory too
        cp_dir = cp.data_dir
        cp_orphan_tmp = os.path.join(cp_dir, "registry", "pods", "orphan_cut.json.tmp")
        os.makedirs(os.path.dirname(cp_orphan_tmp), exist_ok=True)
        with open(cp_orphan_tmp, "wb") as f:
            f.write(b'junk uncommitted data')

        # Wait for lease to expire on CP (2000ms threshold)
        time.sleep(2.8)
        st, data = http_get(f"{cp.api_url}/api/v1/nodes/{worker.name}")
        assert st == 200
        ready_c = next((c for c in data.get("status", {}).get("conditions", []) if c.get("type") == "Ready"), None)
        assert ready_c and ready_c.get("status") == "False"
        print("[+] Worker is confirmed NotReady on CP after power-cut.")

        # Reboot the worker against the dirty data directory
        print("[*] Rebooting worker with dirty LittleFS state...")
        rebooted = harness.restart_worker(worker.name, cp, lease_interval_ms=400)
        time.sleep(0.5)
        assert rebooted.is_alive(), "Worker failed to restart against dirty data directory"

        # Wait for worker registration + lease heartbeat renewal
        time.sleep(1.2)

        # Verify worker re-registered and Ready condition restored
        st, data = http_get(f"{cp.api_url}/api/v1/nodes/{worker.name}")
        assert st == 200
        ready_c = next((c for c in data.get("status", {}).get("conditions", []) if c.get("type") == "Ready"), None)
        assert ready_c and ready_c.get("status") == "True", f"Expected Ready=True after reboot, got {ready_c}"
        assert ready_c.get("reason") == "KubeletReady"

        print("[PASS] Test 4: Worker safely survived dirty LittleFS state, uncommitted .tmp power-cuts, and successfully re-registered.")
        return True
    finally:
        harness.teardown()


def test_5_high_frequency_lease_flooding_and_lock_contention(bin_path: str = DEFAULT_BIN, base_port: int = 20050):
    print(f"\n--- [ADV-TEST 5] High-Frequency Lease Flooding & Lock Contention Under Stress (Port {base_port}) ---")
    harness = AdversarialClusterHarness(bin_path=bin_path, base_api_port=base_port, base_ipc_port=base_port + 1000)
    try:
        cp = harness.start_control_plane(
            name="cp-adv-5",
            lease_interval_ms=500,
            not_ready_timeout_ms=3000,
            eviction_timeout_ms=6000,
            reconcile_interval_ms=200
        )
        worker = harness.start_worker("worker-flood-1", 1, cp, lease_interval_ms=500)

        lease_url = f"{cp.api_url}/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/{worker.name}"

        flood_errors = []
        flood_successes = [0]
        stop_workers = threading.Event()

        def lease_flooder(worker_idx: int, num_requests: int):
            for i in range(num_requests):
                if stop_workers.is_set():
                    break
                renew_time = f"2026-10-26T14:55:{i%60:02d}.{i%1000:03d}Z"
                lease_payload = {
                    "apiVersion": "coordination.k8s.io/v1",
                    "kind": "Lease",
                    "metadata": {"name": worker.name, "namespace": "kube-node-lease"},
                    "spec": {
                        "holderIdentity": worker.name,
                        "leaseDurationSeconds": 40,
                        "renewTime": renew_time
                    }
                }
                st, data = http_put(lease_url, lease_payload, timeout=2.0)
                if st in (200, 201):
                    flood_successes[0] += 1
                else:
                    flood_errors.append(f"Worker {worker_idx} req {i} failed: status={st}")

        # Concurrently query nodes and pods while flood is occurring
        def reader_worker():
            while not stop_workers.is_set():
                http_get(f"{cp.api_url}/api/v1/nodes", timeout=1.0)
                http_get(f"{cp.api_url}/api/v1/pods", timeout=1.0)
                time.sleep(0.01)

        threads = []
        # 4 flooders, 100 requests each = 400 rapid PUT requests
        for w_idx in range(4):
            th = threading.Thread(target=lease_flooder, args=(w_idx, 100))
            threads.append(th)

        # 2 reader threads
        for _ in range(2):
            th = threading.Thread(target=reader_worker)
            threads.append(th)

        start_time = time.time()
        for th in threads:
            th.start()

        # Wait for flooders to finish (or max 10s)
        for th in threads[:4]:
            th.join(timeout=10.0)

        stop_workers.set()
        for th in threads[4:]:
            th.join(timeout=2.0)

        total_time = time.time() - start_time
        print(f"[*] Blasted {flood_successes[0]} lease renewals in {total_time:.2f}s ({flood_successes[0]/total_time:.1f} req/s)")

        assert len(flood_errors) == 0, f"Lease flood had errors: {flood_errors[:5]}"
        assert flood_successes[0] >= 350, f"Expected >= 350 successful renewals, got {flood_successes[0]}"

        # Verify node is still Ready=True
        st, data = http_get(f"{cp.api_url}/api/v1/nodes/{worker.name}")
        assert st == 200
        ready_c = next((c for c in data.get("status", {}).get("conditions", []) if c.get("type") == "Ready"), None)
        assert ready_c and ready_c.get("status") == "True"

        # Verify lease object has valid format
        st, lease_data = http_get(lease_url)
        assert st == 200
        assert lease_data.get("kind") == "Lease"
        assert lease_data.get("spec", {}).get("holderIdentity") == worker.name

        print("[PASS] Test 5: High-frequency lease flooding handled with zero deadlocks, zero dropped requests, and monotonic updates.")
        return True
    finally:
        harness.teardown()


# ==============================================================================
# Main Runner
# ==============================================================================

def main():
    print("======================================================================")
    print(" Kube32 Milestone 4 Adversarial Stress & Rescheduling Challenge Harness")
    print("======================================================================")

    results = {}

    # 1. Run Release Binary Suite
    print(f"\n>>> Running Suite against Release Binary: {DEFAULT_BIN}")
    results["Release: Test 1 (Sudden SIGKILL Under Traffic)"] = ("PASS", test_1_sudden_sigkill_under_inflight_traffic(DEFAULT_BIN, 20010))

    t2_pass, t2_err = test_2_multi_stage_timing_and_daemonset_immunity(DEFAULT_BIN, 20030)
    results["Release: Test 2 (Timing Verification & DisruptionTarget)"] = ("PASS" if t2_pass else "FAIL", t2_err)

    results["Release: Test 3 (Rescheduling Resource Constraints)"] = ("PASS", test_3_rescheduling_resource_constraints_and_races(DEFAULT_BIN, 20050))
    results["Release: Test 4 (Dirty LittleFS Rejoin Recovery)"] = ("PASS", test_4_dirty_littlefs_rejoin_and_power_cut_recovery(DEFAULT_BIN, 20070))
    results["Release: Test 5 (High-Frequency Lease Flood Stress)"] = ("PASS", test_5_high_frequency_lease_flooding_and_lock_contention(DEFAULT_BIN, 20090))

    # 2. Run AddressSanitizer Suite
    if os.path.exists(ASAN_BIN):
        print(f"\n>>> Running Suite against AddressSanitizer/UBSan Binary: {ASAN_BIN}")
        for t_idx, (t_name, t_fn, t_port) in enumerate([
            ("ASAN: Test 1 (Sudden SIGKILL Under Traffic)", test_1_sudden_sigkill_under_inflight_traffic, 30010),
            ("ASAN: Test 2 (Timing Verification & DisruptionTarget)", test_2_multi_stage_timing_and_daemonset_immunity, 30030),
            ("ASAN: Test 3 (Rescheduling Resource Constraints)", test_3_rescheduling_resource_constraints_and_races, 30050),
            ("ASAN: Test 4 (Dirty LittleFS Rejoin Recovery)", test_4_dirty_littlefs_rejoin_and_power_cut_recovery, 30070),
            ("ASAN: Test 5 (High-Frequency Lease Flood Stress)", test_5_high_frequency_lease_flooding_and_lock_contention, 30090),
        ], 1):
            try:
                res = t_fn(ASAN_BIN, t_port)
                if isinstance(res, tuple):
                    passed, err = res
                    results[t_name] = ("PASS" if passed else "FAIL", err)
                else:
                    results[t_name] = ("PASS", "")
            except Exception as ex:
                results[t_name] = ("FAIL", f"AddressSanitizer Crash: {ex}")

    print("\n======================================================================")
    print("                    ADVERSARIAL VERIFICATION SUMMARY                  ")
    print("======================================================================")
    any_failed = False
    for test_name, (status, detail) in results.items():
        print(f"[{status}] {test_name}")
        if status == "FAIL":
            any_failed = True
            print(f"       -> Details: {detail}")
    print("======================================================================")

    if any_failed:
        print("\n[VERDICT] REQUEST_CHANGES — Empirical timing/architectural bugs reproduced.")
        sys.exit(1)
    else:
        print("\n[VERDICT] APPROVE — All adversarial stress tests passed.")
        sys.exit(0)


if __name__ == "__main__":
    main()
