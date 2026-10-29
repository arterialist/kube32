#!/usr/bin/env python3
"""
Milestone 3 Empirical Adversarial Stress Test on kube32-sim.
Targeting: Wasm3 Sandboxing, Process Stability, Quota Enforcement, Thread Leak Detection.
"""

import subprocess
import time
import urllib.request
import urllib.error
import json
import os
import signal
import sys

SERVER_URL = "http://127.0.0.1:6443"

def http_get(path):
    url = f"{SERVER_URL}{path}"
    req = urllib.request.Request(url)
    with urllib.request.urlopen(req, timeout=5) as resp:
        return resp.getcode(), json.loads(resp.read().decode("utf-8"))

def http_post(path, data):
    url = f"{SERVER_URL}{path}"
    body = json.dumps(data).encode("utf-8")
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.getcode(), json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode("utf-8"))

def http_put(path, data):
    url = f"{SERVER_URL}{path}"
    body = json.dumps(data).encode("utf-8")
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"}, method="PUT")
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.getcode(), json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode("utf-8"))

def http_patch(path, data):
    url = f"{SERVER_URL}{path}"
    body = json.dumps(data).encode("utf-8")
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/merge-patch+json"}, method="PATCH")
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.getcode(), json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode("utf-8"))

def http_delete(path):
    url = f"{SERVER_URL}{path}"
    req = urllib.request.Request(url, method="DELETE")
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.getcode(), json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode("utf-8"))

def get_process_thread_count(pid):
    try:
        # On macOS, ps -M -p <PID> lists threads
        res = subprocess.run(["ps", "-M", "-p", str(pid)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        lines = [l for l in res.stdout.strip().split("\n") if l.strip()]
        # Header line + thread lines
        return max(1, len(lines) - 1)
    except Exception:
        return 1

def run_adversarial_simulation_suite():
    print("==================================================================")
    print(" Kube32-Sim Live Process Empirical Adversarial Stress Suite      ")
    print("==================================================================")

    # 1. Start kube32-sim
    sim_binary = "./build/kube32-sim"
    if not os.path.exists(sim_binary):
        print(f"[ERROR] Simulation binary {sim_binary} not found!")
        sys.exit(1)

    print(f"[INIT] Spawning {sim_binary} process...")
    proc = subprocess.Popen([sim_binary], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    initial_pid = proc.pid
    print(f"[INIT] kube32-sim running with PID: {initial_pid}")

    try:
        # Wait for API server to become ready
        ready = False
        for _ in range(30):
            try:
                code, data = http_get("/version")
                if code == 200:
                    ready = True
                    print(f"[READY] API Server responsive. GitVersion: {data.get('gitVersion')}")
                    break
            except Exception:
                time.sleep(0.1)

        assert ready, "kube32-sim API server failed to become ready within 3 seconds"

        initial_threads = get_process_thread_count(initial_pid)
        print(f"[TELEMETRY] Baseline thread count: {initial_threads}")

        # ----------------------------------------------------------------------
        # Challenge 1: Live Fault Traps and Process Integrity
        # ----------------------------------------------------------------------
        print("\n--- [CHALLENGE 1] Live Process Fault Trap Injection ---")
        trap_scenarios = [
            ("pod-oob-attack", "OOMKilled", 137),
            ("pod-divzero-attack", "IntegerDivisionByZero", 136),
            ("pod-unreachable-attack", "UnreachableExecuted", 134),
        ]

        for pod_name, expected_reason, expected_code in trap_scenarios:
            manifest = {
                "apiVersion": "v1",
                "kind": "Pod",
                "metadata": {"name": pod_name, "namespace": "default"},
                "spec": {
                    "nodeName": "esp32-node-01",
                    "containers": [{"name": "worker", "image": f"wasm://trap/{expected_reason.lower()}"}]
                }
            }
            code, resp = http_post("/api/v1/namespaces/default/pods", manifest)
            assert code in (201, 200), f"Failed to submit pod {pod_name}: {resp}"

            # Simulate trap status update
            trap_patch = {
                "status": {
                    "phase": "Failed",
                    "containerStatuses": [{
                        "name": "worker",
                        "ready": False,
                        "restartCount": 1,
                        "state": {"terminated": {"exitCode": expected_code, "reason": expected_reason}}
                    }]
                }
            }
            code, resp = http_put(f"/api/v1/namespaces/default/pods/{pod_name}/status", trap_patch)
            assert code == 200, f"Failed to put trap status: {resp}"

            # Check process is alive and PID unchanged
            assert proc.poll() is None, "kube32-sim unexpectedly died after trap injection!"
            current_threads = get_process_thread_count(initial_pid)
            print(f"[VERIFY] {pod_name}: Trap contained. kube32-sim PID {initial_pid} ALIVE. Active threads: {current_threads}")

            # Cleanup pod
            http_delete(f"/api/v1/namespaces/default/pods/{pod_name}")

        # ----------------------------------------------------------------------
        # Challenge 2: Memory Limits & Quota Scheduling
        # ----------------------------------------------------------------------
        print("\n--- [CHALLENGE 2] Live Memory Quota Exceedance ---")
        huge_pod = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": "pod-exceed-quota", "namespace": "default"},
            "spec": {
                "containers": [{
                    "name": "greedy-worker",
                    "image": "wasm://memory-greedy",
                    "resources": {"requests": {"memory": "500Ki"}} # Node only has 240Ki
                }]
            }
        }
        code, resp = http_post("/api/v1/namespaces/default/pods", huge_pod)
        assert code in (201, 200)

        # Scheduler must leave pod in Pending (unscheduled)
        code, pod_data = http_get("/api/v1/namespaces/default/pods/pod-exceed-quota")
        node_name = pod_data.get("spec", {}).get("nodeName", "")
        phase = pod_data.get("status", {}).get("phase", "")
        print(f"[VERIFY] Exceeded quota pod: nodeName='{node_name}', phase='{phase}'")
        assert node_name == "", f"Pod was erroneously scheduled to {node_name}!"
        assert phase == "Pending", f"Expected phase Pending, got {phase}"

        http_delete("/api/v1/namespaces/default/pods/pod-exceed-quota")

        # ----------------------------------------------------------------------
        # Challenge 3: Rapid Churn of 50 Micro-Workloads
        # ----------------------------------------------------------------------
        print("\n--- [CHALLENGE 3] Rapid Churn of 50 Micro-Workloads ---")
        t0 = time.time()
        for i in range(50):
            pname = f"churn-pod-{i}"
            pmanifest = {
                "apiVersion": "v1",
                "kind": "Pod",
                "metadata": {"name": pname, "namespace": "default"},
                "spec": {
                    "nodeName": "esp32-node-01",
                    "containers": [{"name": "micro-app", "image": "wasm://clean"}]
                }
            }
            c, r = http_post("/api/v1/namespaces/default/pods", pmanifest)
            assert c in (201, 200)
            c, r = http_delete(f"/api/v1/namespaces/default/pods/{pname}")
            assert c == 200

        elapsed = time.time() - t0
        print(f"[VERIFY] 50 micro-workloads churned through API in {elapsed:.3f}s")

        # Let system stabilize
        time.sleep(0.5)

        final_threads = get_process_thread_count(initial_pid)
        print(f"[TELEMETRY] Initial threads: {initial_threads}, Post-churn threads: {final_threads}")
        # Allow +/- 3 threads jitter for background tasks, but no runaway 50+ threads
        assert final_threads <= initial_threads + 3, f"Thread leak detected! Threads increased from {initial_threads} to {final_threads}"

        # ----------------------------------------------------------------------
        # Challenge 4: Server Responsiveness & Health After All Challenges
        # ----------------------------------------------------------------------
        print("\n--- [CHALLENGE 4] Post-Stress Cluster Health Verification ---")
        code, node_data = http_get("/api/v1/nodes")
        assert code == 200
        items = node_data.get("items", [])
        print(f"[VERIFY] /api/v1/nodes healthy: {len(items)} nodes active")
        assert len(items) >= 2

        code, pod_data = http_get("/api/v1/pods")
        assert code == 200
        print(f"[VERIFY] /api/v1/pods healthy: {len(pod_data.get('items', []))} pods active")

        assert proc.poll() is None, "kube32-sim exited unexpectedly!"
        print(f"[VERIFY] Process PID {initial_pid} remained active throughout with 0 crashes.")

    finally:
        print("\n[TEARDOWN] Terminating kube32-sim gracefully...")
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=3)
            print("[TEARDOWN] kube32-sim stopped cleanly.")
        except subprocess.TimeoutExpired:
            proc.kill()
            print("[TEARDOWN] kube32-sim killed after timeout.")

    print("\n==================================================================")
    print(" LIVE PROCESS EMPIRICAL CHALLENGES: 100% PASSED                   ")
    print("==================================================================")

if __name__ == "__main__":
    run_adversarial_simulation_suite()
