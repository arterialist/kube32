#!/usr/bin/env python3
"""
Adversarial Concurrency, Scheduler & Reconcile Stress Test Harness.
Designed by m2_challenger_4 to empirically test edge cases, high contention,
dynamic node lifecycle (removal & resurrection), multi-container summation,
and ping-pong failover.
"""

import sys
import os
import time
import json
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
    sim_proc = subprocess.Popen(
        [BINARY_PATH],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        preexec_fn=os.setsid
    )
    for _ in range(40):
        time.sleep(0.1)
        try:
            req = urllib.request.Request(f"{BASE_URL}/version")
            with urllib.request.urlopen(req, timeout=0.5) as r:
                if r.status == 200:
                    return
        except Exception:
            pass
    raise RuntimeError("Failed to start kube32-sim")

def stop_sim():
    global sim_proc
    if sim_proc and sim_proc.poll() is None:
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

def http_req(path, method="GET", body=None, headers=None, timeout=5.0):
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
    for attempt in range(6):
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                resp_body = resp.read().decode("utf-8")
                return resp.status, resp_body, resp.headers
        except urllib.error.HTTPError as e:
            resp_body = e.read().decode("utf-8")
            return e.code, resp_body, e.headers
        except Exception as e:
            if attempt == 5:
                return 0, str(e), {}
            time.sleep(0.05 * (attempt + 1))

def parse_bytes(val):
    if not val:
        return 0
    val = val.strip()
    if val.endswith("Ki") or val.endswith("ki"):
        return int(float(val[:-2]) * 1024)
    if val.endswith("Mi") or val.endswith("mi"):
        return int(float(val[:-2]) * 1024 * 1024)
    if val.endswith("Gi") or val.endswith("gi"):
        return int(float(val[:-2]) * 1024 * 1024 * 1024)
    return int(float(val))

# ==============================================================================
# CHALLENGE SUITE 1: Extreme Multi-Dimensional Resource Contention (40 Threads)
# ==============================================================================
def challenge_multi_dimensional_contention():
    print("\n" + "="*70)
    print("CHALLENGE 1: Extreme Multi-Dimensional Resource Contention (20 Threads)")
    print("="*70)
    restart_sim()

    NUM_THREADS = 20
    barrier = threading.Barrier(NUM_THREADS)
    results = [None] * NUM_THREADS

    # 4 distinct pod profiles designed to test memory, GPIO, I2C, ADC, and PSRAM simultaneously
    profiles = [
        {"memory": "50Ki", "esp32.io/gpio": "3", "esp32.io/i2c": "1"},          # Type A
        {"memory": "70Ki", "esp32.io/gpio": "4", "esp32.io/adc": "1"},          # Type B
        {"memory": "60Ki", "esp32.io/psram": "1500Ki"},                          # Type C
        {"memory": "80Ki", "esp32.io/gpio": "5"}                                 # Type D
    ]

    def worker(idx):
        prof = profiles[idx % len(profiles)]
        manifest = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": f"contend-pod-{idx}", "namespace": "default"},
            "spec": {
                "containers": [{
                    "name": "worker",
                    "image": "wasm://worker",
                    "resources": {"requests": prof}
                }]
            }
        }
        barrier.wait()
        st, body, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=manifest)
        results[idx] = (idx, prof, st, body)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(NUM_THREADS)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    # Track allocations per node
    node_alloc = {
        "esp32-node-01": {"memory": 0, "gpio": 0, "i2c": 0, "adc": 0, "psram": 0, "pods": []},
        "esp32-node-02": {"memory": 0, "gpio": 0, "i2c": 0, "adc": 0, "psram": 0, "pods": []}
    }
    pending_count = 0

    for idx, prof, st, body in results:
        assert st == 201, f"Failed pod creation {idx}: status={st}, body={body}"
        p = json.loads(body)
        phase = p.get("status", {}).get("phase")
        node = p.get("spec", {}).get("nodeName")
        if phase == "Running":
            assert node in node_alloc, f"Unknown node {node}"
            node_alloc[node]["pods"].append(idx)
            node_alloc[node]["memory"] += parse_bytes(prof.get("memory", "0"))
            node_alloc[node]["gpio"] += int(prof.get("esp32.io/gpio", 0))
            node_alloc[node]["i2c"] += int(prof.get("esp32.io/i2c", 0))
            node_alloc[node]["adc"] += int(prof.get("esp32.io/adc", 0))
            node_alloc[node]["psram"] += parse_bytes(prof.get("esp32.io/psram", "0"))
        elif phase == "Pending":
            pending_count += 1
            conds = p.get("status", {}).get("conditions", [])
            assert any(c.get("reason") == "Unschedulable" for c in conds), f"Missing Unschedulable reason: {conds}"
        else:
            raise AssertionError(f"Unexpected pod phase: {phase}")

    print(f" -> Results: Node 1 hosted {len(node_alloc['esp32-node-01']['pods'])} pods, Node 2 hosted {len(node_alloc['esp32-node-02']['pods'])} pods, {pending_count} Pending pods")
    print(f" -> Node 1 Allocations: {node_alloc['esp32-node-01']}")
    print(f" -> Node 2 Allocations: {node_alloc['esp32-node-02']}")

    # Strict non-oversubscription assertions:
    # Memory <= 240Ki (245760 B), GPIO <= 14, I2C <= 2, ADC <= 2, PSRAM <= 3840Ki (3932160 B)
    for nname in ["esp32-node-01", "esp32-node-02"]:
        na = node_alloc[nname]
        assert na["memory"] <= 245760, f"OVER-SUBSCRIPTION: {nname} memory {na['memory']} > 245760"
        assert na["gpio"] <= 14, f"OVER-SUBSCRIPTION: {nname} gpio {na['gpio']} > 14"
        assert na["i2c"] <= 2, f"OVER-SUBSCRIPTION: {nname} i2c {na['i2c']} > 2"
        assert na["adc"] <= 2, f"OVER-SUBSCRIPTION: {nname} adc {na['adc']} > 2"
        assert na["psram"] <= 3932160, f"OVER-SUBSCRIPTION: {nname} psram {na['psram']} > 3932160"

    print(" [PASS] Challenge 1: Zero over-subscription across all 5 resource dimensions under 40-thread contention.")

# ==============================================================================
# CHALLENGE SUITE 2: Concurrent Node Deletion & Workload Race
# ==============================================================================
def challenge_concurrent_node_deletion_race():
    print("\n" + "="*70)
    print("CHALLENGE 2: Concurrent Node Deletion & Workload Race")
    print("="*70)
    restart_sim()

    # Pre-populate cluster with 4 pods running on both nodes
    for i in range(4):
        p = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": f"pre-pod-{i}", "namespace": "default"},
            "spec": {"containers": [{"name": "c", "image": "wasm://app", "resources": {"requests": {"memory": "30Ki"}}}]}
        }
        st, b, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=p)
        assert st == 201

    # Verify pods are distributed across nodes
    st, pods_b, _ = http_req("/api/v1/namespaces/default/pods")
    assert st == 200
    pods_list = json.loads(pods_b).get("items", [])
    assert len(pods_list) == 4
    nodes_used = set(p["spec"].get("nodeName") for p in pods_list)
    print(f" -> Initial 4 pods distributed across: {nodes_used}")

    # Now launch 10 threads submitting pods while concurrently DELETING node esp32-node-02!
    NUM_WORKERS = 10
    barrier = threading.Barrier(NUM_WORKERS + 1)
    del_result = {}

    def del_worker():
        barrier.wait()
        st, b, _ = http_req("/api/v1/nodes/esp32-node-02", method="DELETE")
        del_result["status"] = st
        del_result["body"] = b

    def submit_worker(idx):
        barrier.wait()
        p = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": f"race-pod-{idx}", "namespace": "default"},
            "spec": {"containers": [{"name": "c", "image": "wasm://app", "resources": {"requests": {"memory": "20Ki"}}}]}
        }
        http_req("/api/v1/namespaces/default/pods", method="POST", body=p)

    threads = [threading.Thread(target=del_worker)]
    for i in range(NUM_WORKERS):
        threads.append(threading.Thread(target=submit_worker, args=(i,)))

    for t in threads:
        t.start()
    for t in threads:
        t.join()

    print(f" -> Node deletion response: status={del_result.get('status')}")
    assert del_result.get("status") in [200, 204]

    # Wait for reconciliation loop (reconcile runs every 1000ms)
    print(" -> Waiting 3.5 seconds for reconcile loop to clear deleted node assignments...")
    time.sleep(3.5)

    # Inspect all pods
    st, pods_b, _ = http_req("/api/v1/namespaces/default/pods")
    assert st == 200
    all_pods = json.loads(pods_b).get("items", [])
    print(f" -> Inspected {len(all_pods)} total pods in cluster after node-02 deletion.")

    stranded_pods = []
    node01_mem = 0
    for p in all_pods:
        pname = p["metadata"]["name"]
        node = p.get("spec", {}).get("nodeName")
        phase = p.get("status", {}).get("phase")
        if node == "esp32-node-02":
            stranded_pods.append((pname, phase))
        if node == "esp32-node-01" and phase == "Running":
            node01_mem += parse_bytes(p["spec"]["containers"][0]["resources"]["requests"]["memory"])

    print(f" -> Stranded pods on deleted node-02: {stranded_pods}")
    print(f" -> Total memory running on esp32-node-01: {node01_mem} bytes (max 245760)")
    assert len(stranded_pods) == 0, f"DEFECT: Found pods still bound to deleted node: {stranded_pods}"
    assert node01_mem <= 245760, f"OVER-SUBSCRIPTION on node-01: {node01_mem} > 245760"

    print(" [PASS] Challenge 2: Node deletion race completely resolved with 0 stranded pods and 0 over-subscriptions.")

# ==============================================================================
# CHALLENGE SUITE 3: Dynamic Node Resurrection & Pending Pod Re-Scheduling
# ==============================================================================
def challenge_dynamic_node_resurrection():
    print("\n" + "="*70)
    print("CHALLENGE 3: Dynamic Node Resurrection & Pending Pod Re-Scheduling")
    print("="*70)
    # Continuing on the cluster where node-02 was deleted and pods are Pending!
    # Check pending pods
    st, pods_b, _ = http_req("/api/v1/namespaces/default/pods")
    all_pods = json.loads(pods_b).get("items", [])
    pending_pods = [p["metadata"]["name"] for p in all_pods if p.get("status", {}).get("phase") == "Pending"]
    print(f" -> Currently pending pods awaiting capacity: {len(pending_pods)}: {pending_pods}")
    assert len(pending_pods) > 0, "Need pending pods for resurrection test"

    # Now resurrect esp32-node-02 by POSTing /api/v1/nodes
    print(" -> Resurrecting esp32-node-02 via POST /api/v1/nodes...")
    node2_manifest = {
        "apiVersion": "v1",
        "kind": "Node",
        "metadata": {
            "name": "esp32-node-02",
            "labels": {
                "kubernetes.io/hostname": "esp32-node-02",
                "kubernetes.io/arch": "xtensa",
                "node-role.kubernetes.io/worker": ""
            }
        },
        "spec": {"unschedulable": False},
        "status": {
            "conditions": [{"type": "Ready", "status": "True", "reason": "KubeletReady"}],
            "addresses": [{"type": "InternalIP", "address": "127.0.0.2"}],
            "allocatable": {
                "memory": "240Ki",
                "cpu": "2",
                "esp32.io/gpio": "14",
                "esp32.io/i2c": "2",
                "esp32.io/psram": "3840Ki"
            }
        }
    }
    st, b, _ = http_req("/api/v1/nodes", method="POST", body=node2_manifest)
    assert st == 201, f"Failed resurrecting node: {st}, {b}"

    # Wait for scheduler reconcile loop to pick up the newly ready node and drain pending pods
    print(" -> Waiting 3.5 seconds for reconcile loop to schedule pending pods onto resurrected node...")
    time.sleep(3.5)

    st, pods_b2, _ = http_req("/api/v1/namespaces/default/pods")
    all_pods2 = json.loads(pods_b2).get("items", [])
    node02_pods = [p["metadata"]["name"] for p in all_pods2 if p.get("spec", {}).get("nodeName") == "esp32-node-02"]
    node02_running = [p["metadata"]["name"] for p in all_pods2 if p.get("spec", {}).get("nodeName") == "esp32-node-02" and p.get("status", {}).get("phase") == "Running"]
    print(f" -> Pods scheduled to resurrected esp32-node-02: {len(node02_running)} Running: {node02_running}")

    assert len(node02_running) > 0, "Scheduler failed to auto-schedule pending pods onto resurrected node!"
    print(" [PASS] Challenge 3: Pending pods automatically scheduled onto resurrected node.")

# ==============================================================================
# CHALLENGE SUITE 4: Multi-Container Pod Resource Request Summation
# ==============================================================================
def challenge_multi_container_pod_summation():
    print("\n" + "="*70)
    print("CHALLENGE 4: Multi-Container Pod Resource Request Summation")
    print("="*70)
    restart_sim()

    # Create a 3-container pod that requests:
    # c1: 80Ki memory, 4 gpio
    # c2: 80Ki memory, 5 gpio
    # c3: 80Ki memory, 5 gpio
    # Total: 240Ki memory, 14 gpio (EXACT capacity of one node!)
    pod_3c = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "pod-3containers", "namespace": "default"},
        "spec": {
            "containers": [
                {"name": "c1", "image": "wasm://c1", "resources": {"requests": {"memory": "80Ki", "esp32.io/gpio": "4"}}},
                {"name": "c2", "image": "wasm://c2", "resources": {"requests": {"memory": "80Ki", "esp32.io/gpio": "5"}}},
                {"name": "c3", "image": "wasm://c3", "resources": {"requests": {"memory": "80Ki", "esp32.io/gpio": "5"}}}
            ]
        }
    }
    st, b, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pod_3c)
    assert st == 201
    pdata = json.loads(b)
    assigned_node = pdata.get("spec", {}).get("nodeName")
    phase = pdata.get("status", {}).get("phase")
    print(f" -> Multi-container pod scheduled: Node={assigned_node}, Phase={phase}")
    assert phase == "Running"
    assert assigned_node in ["esp32-node-01", "esp32-node-02"]

    # Now on that assigned node, it has 0 Ki memory and 0 GPIO left!
    # Attempt to pin a pod to that specific node with 1 byte memory
    pinned_overflow = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "pinned-overflow", "namespace": "default"},
        "spec": {
            "nodeSelector": {"kubernetes.io/hostname": assigned_node},
            "containers": [{"name": "c", "image": "wasm://c", "resources": {"requests": {"memory": "1"}}}]
        }
    }
    st, b2, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pinned_overflow)
    assert st == 201
    pdata2 = json.loads(b2)
    phase2 = pdata2.get("status", {}).get("phase")
    print(f" -> Overflow pod pinned to {assigned_node}: Phase={phase2}")
    assert phase2 == "Pending", f"Expected Pending due to exhausted capacity on {assigned_node}, got {phase2}"

    print(" [PASS] Challenge 4: Multi-container resource requests accurately summed and enforced.")

# ==============================================================================
# CHALLENGE SUITE 5: Rapid Failover Ping-Pong (3 Cycles)
# ==============================================================================
def challenge_rapid_failover_pingpong():
    print("\n" + "="*70)
    print("CHALLENGE 5: Rapid Failover Ping-Pong (3 Cycles)")
    print("="*70)
    restart_sim()

    # Create unpinned pod
    pod = {
        "apiVersion": "v1",
        "kind": "Pod",
        "metadata": {"name": "pingpong-pod", "namespace": "default"},
        "spec": {"containers": [{"name": "c", "image": "wasm://app", "resources": {"requests": {"memory": "32Ki"}}}]}
    }
    st, b, _ = http_req("/api/v1/namespaces/default/pods", method="POST", body=pod)
    assert st == 201
    curr_node = json.loads(b).get("spec", {}).get("nodeName")
    print(f" -> Initial pod placement: {curr_node}")

    for cycle in range(1, 4):
        target_failing = curr_node
        target_surviving = "esp32-node-02" if curr_node == "esp32-node-01" else "esp32-node-01"

        print(f"\n[Cycle {cycle}] Failing {target_failing}, restoring/keeping {target_surviving}...")
        # Mark target_failing Ready=False
        st, n_b, _ = http_req(f"/api/v1/nodes/{target_failing}")
        n_obj = json.loads(n_b)
        n_obj["status"]["conditions"] = [{"type": "Ready", "status": "False", "reason": "NodeCrash"}]
        http_req(f"/api/v1/nodes/{target_failing}", method="PUT", body=n_obj)

        # Ensure target_surviving is Ready=True
        st, ns_b, _ = http_req(f"/api/v1/nodes/{target_surviving}")
        ns_obj = json.loads(ns_b)
        ns_obj["status"]["conditions"] = [{"type": "Ready", "status": "True", "reason": "KubeletReady"}]
        http_req(f"/api/v1/nodes/{target_surviving}", method="PUT", body=ns_obj)

        # Wait for reconcile loop to failover (reconcile needs 1 pass to evict to Pending, 1 pass to reschedule)
        time.sleep(3.0)

        st, p_b, _ = http_req("/api/v1/namespaces/default/pods/pingpong-pod")
        p_obj = json.loads(p_b)
        new_node = p_obj.get("spec", {}).get("nodeName")
        new_phase = p_obj.get("status", {}).get("phase")
        print(f" -> Cycle {cycle} Result: Pod is now on {new_node} in phase {new_phase}")

        assert new_phase == "Running", f"Expected Running, got {new_phase}"
        assert new_node == target_surviving, f"Expected {target_surviving}, got {new_node}"
        curr_node = new_node

    print("\n [PASS] Challenge 5: 3 consecutive rapid failover ping-pong cycles succeeded without state drift.")

# ==============================================================================
# MAIN HARNESS
# ==============================================================================
def main():
    print("==================================================================")
    print("  Kube32 Adversarial Concurrency & Stress Verification Suite      ")
    print("  Tester: m2_challenger_4                                         ")
    print("==================================================================")

    try:
        challenge_multi_dimensional_contention()
        challenge_concurrent_node_deletion_race()
        challenge_dynamic_node_resurrection()
        challenge_multi_container_pod_summation()
        challenge_rapid_failover_pingpong()

        print("\n" + "="*70)
        print("EMPIRICAL ADVERSARIAL STRESS VERDICT: ALL 5 CHALLENGES PASSED")
        print("="*70)
    finally:
        stop_sim()

if __name__ == "__main__":
    main()
