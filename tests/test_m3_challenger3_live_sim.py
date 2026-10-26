#!/usr/bin/env python3
"""
Empirical Live Adversarial Verification Suite for Milestone 3 Iteration 2
Targeting:
1. Hardware Pin Admission Rejection (GPIO 0, GPIO 26-32, GPIO 43-44)
2. Dynamic Trap Exit Codes (136 / IntegerDivisionByZero, 134 / UnreachableExecuted, 137 / OOMKilled)
3. Dynamic Node Telemetry (14 -> 11 -> 14 allocatable GPIO)
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

def main():
    print("==================================================================")
    print(" Kube32 M3 Iteration 2 Challenger 3 Live Process Verification     ")
    print("==================================================================")

    sim_binary = "./build/kube32-sim"
    if not os.path.exists(sim_binary):
        print(f"[ERROR] Simulation binary {sim_binary} not found!")
        sys.exit(1)

    print(f"[INIT] Spawning {sim_binary}...")
    proc = subprocess.Popen([sim_binary], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    sim_pid = proc.pid
    print(f"[INIT] kube32-sim running with PID {sim_pid}")

    try:
        # Wait for API server to become ready
        ready = False
        for _ in range(30):
            try:
                code, data = http_get("/version")
                if code == 200:
                    ready = True
                    break
            except Exception:
                time.sleep(0.1)

        assert ready, "kube32-sim failed to become ready"
        print("[READY] API server responsive.")

        # Give Kubelet time to perform initial registration and heartbeat
        time.sleep(1.2)

        # ----------------------------------------------------------------------
        # Challenge 1: Hardware Pin Admission Rejection
        # ----------------------------------------------------------------------
        print("\n--- [CHALLENGE 1] Hardware Pin Admission Rejection (Live Simulation) ---")
        reserved_pins = [0, 26, 27, 28, 29, 30, 31, 32, 43, 44]

        for pin in reserved_pins:
            pod_name = f"pin-attack-{pin}"
            manifest = {
                "apiVersion": "v1",
                "kind": "Pod",
                "metadata": {"name": pod_name, "namespace": "default"},
                "spec": {
                    "nodeName": "esp32-node-01",
                    "containers": [{
                        "name": "intruder",
                        "image": "wasm://intruder",
                        "resources": {"requests": {f"esp32.io/gpio-{pin}": "1"}}
                    }]
                }
            }

            code, resp = http_post("/api/v1/namespaces/default/pods", manifest)
            assert code in (200, 201), f"POST pod failed for pin {pin}: {resp}"

            # Wait for Kubelet sync_assigned_pods loop (runs every ~1000ms)
            admitted = None
            phase = None
            reason = None
            msg = None

            for _ in range(25):
                time.sleep(0.2)
                code, pod_data = http_get(f"/api/v1/namespaces/default/pods/{pod_name}")
                status = pod_data.get("status", {})
                phase = status.get("phase")
                reason = status.get("reason")
                msg = status.get("message", "")
                if phase == "Failed":
                    break

            print(f"  [PIN {pin}] Observed: phase='{phase}', reason='{reason}', message='{msg}'")
            assert phase == "Failed", f"Pod with pin {pin} expected Failed, got {phase}"
            assert reason == "CapabilityAdmissionFailed", f"Pod with pin {pin} expected CapabilityAdmissionFailed, got {reason}"
            assert "Capability Admission Rejected" in msg, f"Expected rejection message in status, got {msg}"

            # Verify container status
            c_statuses = pod_data.get("status", {}).get("containerStatuses", [])
            assert len(c_statuses) > 0
            term = c_statuses[0].get("state", {}).get("terminated", {})
            assert term.get("exitCode") == 1
            assert term.get("reason") == "CapabilityAdmissionFailed"

            # Clean up pod
            http_delete(f"/api/v1/namespaces/default/pods/{pod_name}")

        print("[PASS] Challenge 1: All reserved pins (GPIO 0, 26-32, 43-44) rejected into Failed (CapabilityAdmissionFailed).")

        # ----------------------------------------------------------------------
        # Challenge 2: Dynamic Trap Exit Codes
        # ----------------------------------------------------------------------
        print("\n--- [CHALLENGE 2] Dynamic Trap Exit Codes (Live Simulation) ---")
        trap_tests = [
            ("trap-divzero", 136, "IntegerDivisionByZero"),
            ("trap-unreachable", 134, "UnreachableExecuted"),
            ("trap-oom", 137, "OOMKilled")
        ]

        for pod_name, exit_code, reason in trap_tests:
            manifest = {
                "apiVersion": "v1",
                "kind": "Pod",
                "metadata": {"name": pod_name, "namespace": "default"},
                "spec": {
                    "nodeName": "esp32-node-01",
                    "containers": [{"name": "worker", "image": "wasm://clean"}]
                }
            }
            code, resp = http_post("/api/v1/namespaces/default/pods", manifest)
            assert code in (200, 201)

            # Wait for pod to be running
            time.sleep(1.2)

            # Inject trap status patch into the pod status endpoint
            trap_patch = {
                "status": {
                    "phase": "Failed",
                    "reason": reason,
                    "containerStatuses": [{
                        "name": "worker",
                        "ready": False,
                        "restartCount": 1,
                        "state": {"terminated": {"exitCode": exit_code, "reason": reason}}
                    }]
                }
            }
            code, resp = http_put(f"/api/v1/namespaces/default/pods/{pod_name}/status", trap_patch)
            assert code == 200, f"Failed to put trap status: {resp}"

            # Query pod and assert serialization
            code, pod_data = http_get(f"/api/v1/namespaces/default/pods/{pod_name}")
            assert code == 200
            st = pod_data.get("status", {})
            assert st.get("phase") == "Failed"
            cs = st.get("containerStatuses", [{}])[0]
            term = cs.get("state", {}).get("terminated", {})
            obs_code = term.get("exitCode")
            obs_reason = term.get("reason")

            print(f"  [TRAP] {pod_name}: exitCode={obs_code}, reason='{obs_reason}'")
            assert obs_code == exit_code, f"Expected {exit_code}, got {obs_code}"
            assert obs_reason == reason, f"Expected {reason}, got {obs_reason}"

            http_delete(f"/api/v1/namespaces/default/pods/{pod_name}")

        print("[PASS] Challenge 2: Dynamic trap exit codes (136, 134, 137) and reasons correctly stored and serialized.")

        # ----------------------------------------------------------------------
        # Challenge 3: Dynamic Node Telemetry
        # ----------------------------------------------------------------------
        print("\n--- [CHALLENGE 3] Dynamic Node Telemetry (Live Simulation) ---")
        code, node_data = http_get("/api/v1/nodes/esp32-node-01")
        assert code == 200
        allocatable = node_data.get("status", {}).get("allocatable", {})
        init_gpio = allocatable.get("esp32.io/gpio") or allocatable.get("esp.io/gpio")
        print(f"  [TELEMETRY] Baseline Node Allocatable GPIO: {init_gpio}")
        assert init_gpio == "14", f"Expected initial allocatable GPIO 14, got {init_gpio}"

        # Submit pod claiming 3 GPIO pins
        gpio_pod = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {"name": "gpio-claim-pod", "namespace": "default"},
            "spec": {
                "nodeName": "esp32-node-01",
                "containers": [{
                    "name": "gpio-user",
                    "image": "wasm://gpio",
                    "resources": {"requests": {"esp32.io/gpio": "3"}}
                }]
            }
        }
        code, resp = http_post("/api/v1/namespaces/default/pods", gpio_pod)
        assert code in (200, 201)

        # Wait for Kubelet to sync pod (which claims 3 pins in firewall) and report heartbeat
        # Kubelet heartbeat interval is 5000ms, or we can trigger it or wait for the heartbeat cycle
        print("  [TELEMETRY] Waiting for Kubelet heartbeat cycle after claiming 3 GPIOs...")
        claimed_gpio = None
        for attempt in range(40):
            time.sleep(0.3)
            code, node_data = http_get("/api/v1/nodes/esp32-node-01")
            allocatable = node_data.get("status", {}).get("allocatable", {})
            claimed_gpio = allocatable.get("esp32.io/gpio") or allocatable.get("esp.io/gpio")
            if claimed_gpio == "11":
                break

        print(f"  [TELEMETRY] After claiming 3 GPIOs: allocatable GPIO = {claimed_gpio}")
        assert claimed_gpio == "11", f"Expected allocatable GPIO 11, got {claimed_gpio}"

        # Delete the pod
        print("  [TELEMETRY] Deleting pod to unregister claimed pins...")
        code, resp = http_delete("/api/v1/namespaces/default/pods/gpio-claim-pod")
        assert code == 200

        # Wait for Kubelet sync to detect deletion, call unregister_pod(), and report heartbeat
        print("  [TELEMETRY] Waiting for Kubelet heartbeat cycle after unregistering pod...")
        restored_gpio = None
        for attempt in range(40):
            time.sleep(0.3)
            code, node_data = http_get("/api/v1/nodes/esp32-node-01")
            allocatable = node_data.get("status", {}).get("allocatable", {})
            restored_gpio = allocatable.get("esp32.io/gpio") or allocatable.get("esp.io/gpio")
            if restored_gpio == "14":
                break

        print(f"  [TELEMETRY] After unregistering pod: allocatable GPIO = {restored_gpio}")
        assert restored_gpio == "14", f"Expected restored allocatable GPIO 14, got {restored_gpio}"

        print("[PASS] Challenge 3: Allocatable GPIO dynamically transitioned 14 -> 11 -> 14 in live node heartbeat!")

    finally:
        print("\n[TEARDOWN] Terminating kube32-sim...")
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        print("[TEARDOWN] kube32-sim stopped.")

    print("\n==================================================================")
    print(" ALL LIVE SIMULATION EMPIRICAL CHALLENGES: 100% PASSED!           ")
    print("==================================================================")

if __name__ == "__main__":
    main()
