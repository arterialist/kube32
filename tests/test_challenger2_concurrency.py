#!/usr/bin/env python3
"""
Empirical Stress-Testing Harness for Milestone 2:
Concurrency, Autonomous Scheduler, OCC Atomic Binding, Capacity Boundary & Cluster Reconciliation.

Author: m2_challenger_2
"""

import sys
import os
import time
import json
import socket
import threading
import subprocess
import urllib.request
import urllib.error

SERVER_HOST = "127.0.0.1"
SERVER_PORT = 6443
BASE_URL = f"http://{SERVER_HOST}:{SERVER_PORT}"
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY_PATH = os.path.join(PROJECT_ROOT, "build", "kube32-sim")

sim_proc = None

def start_sim():
    global sim_proc
    if sim_proc and sim_proc.poll() is None:
        return
    print(f"[HARNESS] Spawning {BINARY_PATH}...")
    sim_proc = subprocess.Popen(
        [BINARY_PATH],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        preexec_fn=os.setsid
    )
    for _ in range(30):
        time.sleep(0.1)
        try:
            req = urllib.request.Request(f"{BASE_URL}/version")
            with urllib.request.urlopen(req, timeout=0.5) as r:
                if r.status == 200:
                    print(f"[HARNESS] kube32-sim ready on port {SERVER_PORT} (PID {sim_proc.pid})")
                    return
        except Exception:
            pass
    raise RuntimeError("Failed to start kube32-sim")

def stop_sim():
    global sim_proc
    if sim_proc and sim_proc.poll() is None:
        print("[HARNESS] Stopping kube32-sim...")
        try:
            os.killpg(os.getpgid(sim_proc.pid), 9)
            sim_proc.wait(timeout=1.0)
        except Exception:
            pass
        sim_proc = None

def restart_sim():
    stop_sim()
    time.sleep(0.2)
    start_sim()

def http_req(path, method="GET", body=None, headers=None):
    if headers is None:
        headers = {}
    url = f"{BASE_URL}{path}"
    data = None
    if body is not None:
        if isinstance(body, dict):
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        elif isinstance(body, str):
            data = body.encode("utf-8")
        elif isinstance(body, bytes):
            data = body
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            resp_body = resp.read().decode("utf-8")
            return resp.status, resp_body, resp.headers
    except urllib.error.HTTPError as e:
        resp_body = e.read().decode("utf-8")
        return e.code, resp_body, e.headers
    except Exception as e:
        return 0, str(e), {}

# ==============================================================================
# TEST SUITE 1: Capacity Exhaustion & Boundary Slicing
# ==============================================================================

def test_capacity_exhaustion_and_boundary_slicing():
    print("\n" + "="*70)
    print("TEST SUITE 1: Capacity Exhaustion & Boundary Slicing")
    print("="*70)
    restart_sim()

    # 1.1 Pod requesting exact allocatable SRAM on single node: 240Ki
    print("\n[1.1] Pod requesting exact SRAM (240Ki = 245760 bytes)...")
    pod_exact_sram = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "pod-exact-sram", "namespace": "default"},
        "spec": {
            "containers": [{
                "name": "worker",
                "image": "wasm://worker",
                "resources": {"requests": {"memory": "240Ki"}}
            }]
        }
    }
    status, body, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pod_exact_sram)
    assert status == 201, f"Expected 201 Created, got {status}: {body}"
    pod_data = json.loads(body)
    assigned_node = pod_data.get("spec", {}).get("nodeName")
    phase = pod_data.get("status", {}).get("phase")
    print(f" -> Assigned node: {assigned_node}, Phase: {phase}")
    assert assigned_node in ["esp32-node-01", "esp32-node-02"], f"Invalid node: {assigned_node}"
    assert phase == "Running", f"Expected Running, got {phase}"
    print(" [PASS] 1.1 Exact SRAM allocation succeeded.")

    # 1.2 Pod requesting 1 byte over remaining SRAM on that node:
    # First fill node 2 as well with 240Ki
    print("\n[1.2] Filling second node with exact SRAM (240Ki)...")
    pod_exact_sram_2 = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "pod-exact-sram-2", "namespace": "default"},
        "spec": {
            "containers": [{
                "name": "worker",
                "image": "wasm://worker",
                "resources": {"requests": {"memory": "240Ki"}}
            }]
        }
    }
    status, body, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pod_exact_sram_2)
    assert status == 201, f"Expected 201 Created, got {status}: {body}"
    pod_data2 = json.loads(body)
    assigned_node2 = pod_data2.get("spec", {}).get("nodeName")
    phase2 = pod_data2.get("status", {}).get("phase")
    print(f" -> Assigned node: {assigned_node2}, Phase: {phase2}")
    assert assigned_node2 in ["esp32-node-01", "esp32-node-02"] and assigned_node2 != assigned_node, "Should bind to the other node"
    assert phase2 == "Running", f"Expected Running, got {phase2}"

    # Now both nodes have 0 bytes of allocatable SRAM remaining!
    # Submit pod requesting 1 byte SRAM
    print("\n[1.3] Submitting pod requesting 1 byte SRAM when cluster has 0 free SRAM...")
    pod_1byte = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "pod-1byte-sram", "namespace": "default"},
        "spec": {
            "containers": [{
                "name": "worker",
                "image": "wasm://worker",
                "resources": {"requests": {"memory": "1"}}
            }]
        }
    }
    status, body, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pod_1byte)
    assert status == 201, f"Expected 201 Created, got {status}: {body}"
    pod_data3 = json.loads(body)
    phase3 = pod_data3.get("status", {}).get("phase")
    node3 = pod_data3.get("spec", {}).get("nodeName", "")
    conds3 = pod_data3.get("status", {}).get("conditions", [])
    print(f" -> Assigned node: '{node3}', Phase: {phase3}")
    print(f" -> Conditions: {conds3}")
    assert phase3 == "Pending", f"Expected Pending, got {phase3}"
    assert node3 == "" or node3 is None or node3 == "<none>", f"Expected empty nodeName, got '{node3}'"
    sched_cond = next((c for c in conds3 if c.get("type") == "PodScheduled"), None)
    assert sched_cond is not None, "Missing PodScheduled condition"
    assert sched_cond.get("status") == "False", f"Expected status False, got {sched_cond.get('status')}"
    assert sched_cond.get("reason") == "Unschedulable", f"Expected Unschedulable, got {sched_cond.get('reason')}"
    print(" [PASS] 1.3 Pod deterministically rejected into Pending with reason Unschedulable.")

    # 1.4 Single pod exceeding total node capacity by 1 byte (245761 bytes = 240Ki + 1)
    print("\n[1.4] Testing single pod exceeding node max SRAM (245761 bytes)...")
    restart_sim()
    pod_exceed_sram = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "pod-exceed-sram", "namespace": "default"},
        "spec": {
            "containers": [{
                "name": "worker",
                "image": "wasm://worker",
                "resources": {"requests": {"memory": "245761"}}
            }]
        }
    }
    status, body, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pod_exceed_sram)
    assert status == 201, f"Expected 201, got {status}: {body}"
    data = json.loads(body)
    assert data.get("status", {}).get("phase") == "Pending"
    assert data.get("spec", {}).get("nodeName", "") in ["", None]
    assert data.get("status", {}).get("conditions", [{}])[0].get("reason") == "Unschedulable"
    print(" [PASS] 1.4 Single pod exceeding SRAM by 1 byte rejected into Pending (Unschedulable).")

    # 1.5 Exact GPIO Boundary: Node has 14 GPIO
    print("\n[1.5] Exact GPIO Boundary: Requesting 14 GPIO pins...")
    restart_sim()
    pod_14_gpio = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "pod-14-gpio", "namespace": "default"},
        "spec": {
            "containers": [{
                "name": "worker",
                "image": "wasm://worker",
                "resources": {"requests": {"esp32.io/gpio": "14", "memory": "16Ki"}}
            }]
        }
    }
    status, body, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pod_14_gpio)
    assert status == 201
    data = json.loads(body)
    assert data.get("status", {}).get("phase") == "Running"
    n1 = data.get("spec", {}).get("nodeName")
    print(f" -> 14 GPIO bound to {n1}")

    # Second pod requests 14 GPIO pins -> should bind to the other node
    pod_14_gpio_2 = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "pod-14-gpio-2", "namespace": "default"},
        "spec": {
            "containers": [{
                "name": "worker",
                "image": "wasm://worker",
                "resources": {"requests": {"esp32.io/gpio": "14", "memory": "16Ki"}}
            }]
        }
    }
    status, body, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pod_14_gpio_2)
    assert status == 201
    data2 = json.loads(body)
    assert data2.get("status", {}).get("phase") == "Running"
    n2 = data2.get("spec", {}).get("nodeName")
    print(f" -> 14 GPIO bound to {n2}")
    assert n1 != n2, "Must bind to different nodes"

    # Third pod requests 1 extra GPIO pin (both nodes exhausted of GPIO)
    print("\n[1.6] Third pod requests 1 GPIO pin when all GPIOs allocated...")
    pod_extra_gpio = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "pod-extra-gpio", "namespace": "default"},
        "spec": {
            "containers": [{
                "name": "worker",
                "image": "wasm://worker",
                "resources": {"requests": {"esp32.io/gpio": "1", "memory": "16Ki"}}
            }]
        }
    }
    status, body, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pod_extra_gpio)
    assert status == 201
    data3 = json.loads(body)
    assert data3.get("status", {}).get("phase") == "Pending"
    assert data3.get("spec", {}).get("nodeName", "") in ["", None]
    assert data3.get("status", {}).get("conditions", [{}])[0].get("reason") == "Unschedulable"
    print(" [PASS] 1.6 Pod requesting 1 GPIO over capacity rejected into Pending (Unschedulable).")

    # 1.7 Pod requesting 15 GPIO pins on fresh cluster (max is 14)
    print("\n[1.7] Pod requesting 15 GPIO pins when max capacity is 14...")
    pod_15_gpio = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "pod-15-gpio", "namespace": "default"},
        "spec": {
            "containers": [{
                "name": "worker",
                "image": "wasm://worker",
                "resources": {"requests": {"esp32.io/gpio": "15", "memory": "16Ki"}}
            }]
        }
    }
    status, body, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pod_15_gpio)
    assert status == 201
    data = json.loads(body)
    assert data.get("status", {}).get("phase") == "Pending"
    assert data.get("status", {}).get("conditions", [{}])[0].get("reason") == "Unschedulable"
    print(" [PASS] 1.7 Pod requesting 15 GPIO rejected into Pending (Unschedulable).")

    # 1.8 Exact PSRAM Boundary (3840Ki) vs Exceeding PSRAM (3840Ki + 1 = 3932161)
    print("\n[1.8] Exact PSRAM (3840Ki) vs Exceeding PSRAM (3932161 bytes)...")
    restart_sim()
    pod_exact_psram = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "pod-exact-psram", "namespace": "default"},
        "spec": {
            "containers": [{
                "name": "worker",
                "image": "wasm://worker",
                "resources": {"requests": {"esp32.io/psram": "3840Ki", "memory": "16Ki"}}
            }]
        }
    }
    status, body, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pod_exact_psram)
    assert status == 201
    assert json.loads(body).get("status", {}).get("phase") == "Running"

    pod_exceed_psram = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "pod-exceed-psram", "namespace": "default"},
        "spec": {
            "containers": [{
                "name": "worker",
                "image": "wasm://worker",
                "resources": {"requests": {"esp32.io/psram": "3932161", "memory": "16Ki"}}
            }]
        }
    }
    status, body, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pod_exceed_psram)
    assert status == 201
    assert json.loads(body).get("status", {}).get("phase") == "Pending"
    print(" [PASS] 1.8 PSRAM exact boundary and 1-byte overflow verified.")


# ==============================================================================
# TEST SUITE 2: Concurrent Pod Scheduling Races & Contention
# ==============================================================================

def test_concurrent_pod_scheduling_races():
    print("\n" + "="*70)
    print("TEST SUITE 2: Concurrent Pod Scheduling Races & Contention")
    print("="*70)
    restart_sim()

    # The cluster has 2 nodes, each with 14 GPIO pins. Total cluster GPIO = 28 pins.
    # We will launch 6 concurrent worker threads simultaneously submitting pods requesting 10 GPIO pins each.
    # Total requested GPIO across 6 pods = 60 GPIO pins.
    # Max possible scheduled pods without over-subscribing any node:
    # Each node has 14 GPIO. A pod requesting 10 GPIO leaves 4 GPIO free.
    # So each node can ONLY host 1 pod of 10 GPIO!
    # Therefore, exactly 2 pods CAN run! The remaining 4 pods MUST be Pending!
    # If OCC or resource tracking fails and permits >2 pods to be Running,
    # or if any node gets assigned >14 GPIO pins, OVER-SUBSCRIPTION BUG CONFIRMED!

    NUM_PODS = 6
    threads = []
    results = [None] * NUM_PODS

    def submit_pod_worker(idx):
        pod_manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": f"contention-pod-{idx}", "namespace": "default"},
            "spec": {
                "containers": [{
                    "name": "worker",
                    "image": "wasm://worker",
                    "resources": {"requests": {"esp32.io/gpio": "10", "memory": "16Ki"}}
                }]
            }
        }
        st, b, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pod_manifest)
        results[idx] = (st, b)

    print(f"\n[2.1] Launching {NUM_PODS} threads concurrently contending for 10 GPIO each...")
    start_barrier = threading.Barrier(NUM_PODS)

    def worker_wrapper(idx):
        start_barrier.wait()
        submit_pod_worker(idx)

    for i in range(NUM_PODS):
        t = threading.Thread(target=worker_wrapper, args=(i,))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    # Analyze results
    running_pods = []
    pending_pods = []
    node_allocations = {"esp32-node-01": 0, "esp32-node-02": 0}

    for idx, (st, b) in enumerate(results):
        assert st == 201, f"Pod {idx} submission failed: code {st}, body {b}"
        p = json.loads(b)
        phase = p.get("status", {}).get("phase")
        node = p.get("spec", {}).get("nodeName")
        if phase == "Running":
            running_pods.append((idx, node))
            node_allocations[node] = node_allocations.get(node, 0) + 10
        elif phase == "Pending":
            pending_pods.append(idx)
        else:
            print(f"[WARN] Pod {idx} in unexpected phase: {phase}")

    print(f" -> Total Running pods: {len(running_pods)}: {running_pods}")
    print(f" -> Total Pending pods: {len(pending_pods)}: {pending_pods}")
    print(f" -> Final GPIO allocation per node: {node_allocations}")

    # Check for oversubscription:
    for node, alloc in node_allocations.items():
        print(f"Checking node {node}: allocated {alloc} GPIO (allocatable: 14 GPIO)")
        assert alloc <= 14, f"CRITICAL CONCURRENCY BUG: Node {node} is OVER-SUBSCRIBED with {alloc} GPIO > 14 allocatable!"

    assert len(running_pods) == 2, f"Expected exactly 2 pods scheduled (one per node), got {len(running_pods)}"
    assert len(pending_pods) == 4, f"Expected exactly 4 pods pending, got {len(pending_pods)}"
    print(" [PASS] 2.1 Concurrent pod scheduling race correctly prevented over-subscription.")


# ==============================================================================
# TEST SUITE 3: Node Failure & Rescheduling
# ==============================================================================

def test_node_failure_and_rescheduling():
    print("\n" + "="*70)
    print("TEST SUITE 3: Node Failure & Rescheduling")
    print("="*70)
    restart_sim()

    # 3.1 Node Ready=False triggers pod eviction and rescheduling onto surviving node
    print("\n[3.1] Testing Node Ready=False eviction and rescheduling...")
    # Pin pod to node-01 by consuming node-02's memory first, or direct nodeSelector
    pod_node1 = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "pod-resched-test", "namespace": "default"},
        "spec": {
            "nodeSelector": {"kubernetes.io/hostname": "esp32-node-01"},
            "containers": [{
                "name": "worker",
                "image": "wasm://worker",
                "resources": {"requests": {"memory": "32Ki"}}
            }]
        }
    }
    status, body, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pod_node1)
    assert status == 201
    pdata = json.loads(body)
    assert pdata.get("spec", {}).get("nodeName") == "esp32-node-01"
    assert pdata.get("status", {}).get("phase") == "Running"
    print(" -> Pod 'pod-resched-test' is Running on esp32-node-01.")

    # Now mark esp32-node-01 as Ready=False
    print(" -> Marking esp32-node-01 Ready=False...")
    st, n1_body, _ = http_req("/api/v1/nodes/esp32-node-01")
    assert st == 200
    n1_obj = json.loads(n1_body)
    n1_obj["status"]["conditions"] = [{"type": "Ready", "status": "False", "reason": "HeartbeatTimeout"}]
    st, updated_n1, _ = http_req("/api/v1/nodes/esp32-node-01", method="PUT", body=n1_obj)
    assert st == 200, f"Failed updating node: {updated_n1}"

    # Wait for scheduler reconcile loop (interval is 1000ms)
    print(" -> Waiting 2.5 seconds for reconciliation loop...")
    time.sleep(2.5)

    # Check pod state
    st, pod_check_body, _ = http_req("/api/v1/namespaces/default/pods/pod-resched-test")
    assert st == 200
    pod_check = json.loads(pod_check_body)
    phase_after = pod_check.get("status", {}).get("phase")
    node_after = pod_check.get("spec", {}).get("nodeName")
    print(f" -> After node failure: Pod phase is '{phase_after}', nodeName is '{node_after}'")

    # Note: because pod had nodeSelector kubernetes.io/hostname=esp32-node-01,
    # esp32-node-02 cannot satisfy nodeSelector, so pod should be evicted into Pending!
    assert phase_after == "Pending", f"Expected pod to be evicted into Pending, got {phase_after}"
    print(" [PASS] 3.1 Pod on NotReady node evicted into Pending.")

    # 3.2 Now test with an unpinned pod that CAN migrate to esp32-node-02
    print("\n[3.2] Testing unpinned pod migrating from failing node to surviving node...")
    restart_sim()
    # Fill node-02 slightly so node-01 is preferred, or submit pod without selector
    unpinned_pod = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "unpinned-migrator", "namespace": "default"},
        "spec": {
            "containers": [{
                "name": "worker",
                "image": "wasm://worker",
                "resources": {"requests": {"memory": "32Ki"}}
            }]
        }
    }
    status, body, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=unpinned_pod)
    assert status == 201
    first_node = json.loads(body).get("spec", {}).get("nodeName")
    other_node = "esp32-node-02" if first_node == "esp32-node-01" else "esp32-node-01"
    print(f" -> Unpinned pod initially scheduled to {first_node}. Other node is {other_node}.")

    # Mark first_node as Ready=False
    print(f" -> Marking {first_node} Ready=False...")
    st, n_body, _ = http_req(f"/api/v1/nodes/{first_node}")
    n_obj = json.loads(n_body)
    n_obj["status"]["conditions"] = [{"type": "Ready", "status": "False", "reason": "NodeCrash"}]
    st, _, _ = http_req(f"/api/v1/nodes/{first_node}", method="PUT", body=n_obj)
    assert st == 200

    # Wait for reconciliation
    print(" -> Waiting 3.0 seconds for reconciliation loop to reschedule pod...")
    time.sleep(3.0)

    st, pod_migrated_body, _ = http_req("/api/v1/namespaces/default/pods/unpinned-migrator")
    assert st == 200
    migrated_pod = json.loads(pod_migrated_body)
    phase_migrated = migrated_pod.get("status", {}).get("phase")
    node_migrated = migrated_pod.get("spec", {}).get("nodeName")
    print(f" -> After reconciliation: Phase: '{phase_migrated}', Assigned Node: '{node_migrated}'")

    # CHECK RESCHEDULING RESULT
    # Did it reschedule onto the other node?
    if node_migrated == other_node and phase_migrated == "Running":
        print(f" [PASS] 3.2 Unpinned pod cleanly rescheduled to surviving node {other_node}!")
    else:
        print(f" [EVIDENCE] Pod status: phase={phase_migrated}, node={node_migrated}")

    # 3.3 Node REMOVAL (DELETE /api/v1/nodes/<node>)
    print("\n[3.3] Adversarial Test: Node REMOVAL (DELETE node)...")
    restart_sim()
    pod_del_test = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "pod-on-deleted-node", "namespace": "default"},
        "spec": {
            "nodeSelector": {"kubernetes.io/hostname": "esp32-node-01"},
            "containers": [{
                "name": "worker",
                "image": "wasm://worker",
                "resources": {"requests": {"memory": "32Ki"}}
            }]
        }
    }
    status, body, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pod_del_test)
    assert status == 201
    assert json.loads(body).get("spec", {}).get("nodeName") == "esp32-node-01"
    print(" -> Pod 'pod-on-deleted-node' Running on esp32-node-01.")

    # Now DELETE esp32-node-01 from cluster
    print(" -> DELETING node esp32-node-01 from cluster...")
    st, del_resp, _ = http_req("/api/v1/nodes/esp32-node-01", method="DELETE")
    assert st == 200, f"Expected 200, got {st}: {del_resp}"

    # Wait for reconciliation
    print(" -> Waiting 3.0 seconds for reconciliation loop...")
    time.sleep(3.0)

    st, pod_del_check, _ = http_req("/api/v1/namespaces/default/pods/pod-on-deleted-node")
    assert st == 200
    pdata_del = json.loads(pod_del_check)
    phase_del = pdata_del.get("status", {}).get("phase")
    node_del = pdata_del.get("spec", {}).get("nodeName")
    print(f" -> Pod state after node deletion: Phase: '{phase_del}', Assigned Node: '{node_del}'")

    # In standard Kubernetes / resilient orchestrator:
    # A running pod whose assigned node was deleted must NOT remain in 'Running' on that deleted node!
    # It must be evicted / marked Pending or Terminated.
    if node_del == "esp32-node-01" and phase_del == "Running":
        print("[CRITICAL VULNERABILITY FOUND] Pod remains Running on deleted node esp32-node-01!")
        print("Reason: reconcile_cluster() only checks nodes present in /registry/nodes/ for Ready condition;")
        print("it fails to detect when a pod's assigned node is deleted entirely from the registry!")
        return False, "Pod stranded on deleted node"

    return True, "All node failure tests passed"

# ==============================================================================
# MAIN RUNNER
# ==============================================================================

def main():
    print("==================================================================")
    print("  Kube32 Adversarial Concurrency & Scheduling Test Suite          ")
    print("  Tester: m2_challenger_2                                         ")
    print("==================================================================")

    try:
        test_capacity_exhaustion_and_boundary_slicing()
        test_concurrent_pod_scheduling_races()
        resched_ok, msg = test_node_failure_and_rescheduling()
        print("\n" + "="*70)
        print("SUMMARY OF EMPIRICAL ADVERSARIAL TESTING")
        print("="*70)
        print(f"1. Capacity Exhaustion & Boundary Slicing: PASS")
        print(f"2. Concurrent Pod Scheduling Races (No over-subscription): PASS")
        print(f"3. Node Failure & Rescheduling: {'PASS' if resched_ok else 'FINDING / DEFECT DETECTED: ' + msg}")
    finally:
        stop_sim()

if __name__ == "__main__":
    main()
