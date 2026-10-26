#!/usr/bin/env python3
"""
Deep inspection test script to verify exact Stage B and Stage C JSON payloads
for both Release and ASan binaries.
"""
import os
import sys
import time
import json
from adversarial_m4_failover import AdversarialClusterHarness, http_get, http_post, DEFAULT_BIN, ASAN_BIN

def run_deep_inspection(bin_name: str, bin_path: str, base_port: int):
    print(f"\n=======================================================")
    print(f" Deep Inspection: {bin_name} ({bin_path}) on Port {base_port}")
    print(f"=======================================================")
    harness = AdversarialClusterHarness(bin_path=bin_path, base_api_port=base_port, base_ipc_port=base_port + 1000)
    try:
        cp = harness.start_control_plane(
            name=f"cp-{base_port}",
            lease_interval_ms=400,
            not_ready_timeout_ms=2500,
            eviction_timeout_ms=5500,
            reconcile_interval_ms=200
        )
        worker = harness.start_worker(f"w-{base_port}", 1, cp, lease_interval_ms=400)

        # Standard pod
        pod_body = {
            "apiVersion": "v1", "kind": "Pod",
            "metadata": {"name": "app-standard", "namespace": "default"},
            "spec": {"nodeName": worker.name, "containers": [{"name": "app", "image": "wasm://app"}]}
        }
        s, _ = http_post(f"{cp.api_url}/api/v1/namespaces/default/pods", pod_body)
        assert s == 201

        # DaemonSet pod
        ds_body = {
            "apiVersion": "v1", "kind": "Pod",
            "metadata": {"name": "ds-collector", "namespace": "default", "labels": {"kube32.io/daemonset": "true"}},
            "spec": {"nodeName": worker.name, "containers": [{"name": "ds", "image": "wasm://ds"}]}
        }
        s, _ = http_post(f"{cp.api_url}/api/v1/namespaces/default/pods", ds_body)
        assert s == 201

        t0 = time.time()
        print(f"[*] T0 = {t0:.3f}: SIGKILL worker {worker.name}")
        harness.sigkill_node(worker.name)

        # Stage B inspection at t = 3.2s
        time.sleep(3.2)
        elapsed_b = time.time() - t0
        st_node_b, node_data_b = http_get(f"{cp.api_url}/api/v1/nodes/{worker.name}")
        st_pod_b, pod_data_b = http_get(f"{cp.api_url}/api/v1/namespaces/default/pods/app-standard")
        print(f"\n--- [STAGE B: Elapsed {elapsed_b:.2f}s] ---")
        print(f"Node Status Conditions: {json.dumps(node_data_b.get('status', {}).get('conditions', []), indent=2)}")
        print(f"Pod Spec nodeName: '{pod_data_b.get('spec', {}).get('nodeName')}'")
        print(f"Pod Status Phase: '{pod_data_b.get('status', {}).get('phase')}'")

        # Stage C inspection at t = 6.2s
        rem = max(0.0, 6.2 - (time.time() - t0))
        time.sleep(rem)
        elapsed_c = time.time() - t0
        st_node_c, node_data_c = http_get(f"{cp.api_url}/api/v1/nodes/{worker.name}")
        st_pod_c, pod_data_c = http_get(f"{cp.api_url}/api/v1/namespaces/default/pods/app-standard")
        st_ds_c, ds_data_c = http_get(f"{cp.api_url}/api/v1/namespaces/default/pods/ds-collector")
        print(f"\n--- [STAGE C: Elapsed {elapsed_c:.2f}s] ---")
        print(f"Node Status Conditions: {json.dumps(node_data_c.get('status', {}).get('conditions', []), indent=2)}")
        print(f"Pod Spec nodeName: '{pod_data_c.get('spec', {}).get('nodeName')}'")
        print(f"Pod Status: {json.dumps(pod_data_c.get('status', {}), indent=2)}")
        print(f"DaemonSet Pod Spec nodeName: '{ds_data_c.get('spec', {}).get('nodeName')}'")

        # Assertions
        assert pod_data_b.get("spec", {}).get("nodeName") == worker.name, "Stage B: Pod was evicted prematurely"
        assert pod_data_c.get("spec", {}).get("nodeName") == "", "Stage C: Pod was NOT evicted"
        conds_c = pod_data_c.get("status", {}).get("conditions", [])
        dt_c = next((c for c in conds_c if c.get("type") == "DisruptionTarget"), None)
        assert dt_c is not None, "Stage C: Missing DisruptionTarget"
        assert dt_c.get("status") == "True"
        assert dt_c.get("reason") == "EvictionByNodeLifecycleController"
        assert ds_data_c.get("spec", {}).get("nodeName") == worker.name, "Stage C: DaemonSet was evicted improperly"
        print(f"\n[CONFIRMED] {bin_name} passed all Stage B and Stage C assertions!")
    finally:
        harness.teardown()

if __name__ == "__main__":
    run_deep_inspection("Release", DEFAULT_BIN, 21030)
    run_deep_inspection("AddressSanitizer", ASAN_BIN, 31030)
