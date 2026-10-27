#!/usr/bin/env python3
"""
Kube32 Kubernetes Mesh Controller
Synchronizes physical ESP32 microcontroller cluster state with OrbStack Kubernetes on macOS.
- Mirrors bare-metal ESP32 pods and node conditions into OrbStack k8s namespace 'kube32-cluster'
- Allows managing microcontroller workloads via standard kubectl --context orbstack
"""

import sys
import os
import json
import time
import subprocess
import urllib.request
import urllib.error

KUBE32_API = "http://127.0.0.1:6443"
K8S_CONTEXT = "orbstack"
NAMESPACE = "kube32-cluster"

def run_kubectl(args, input_data=None):
    cmd = ["kubectl", f"--context={K8S_CONTEXT}"] + args
    res = subprocess.run(
        cmd,
        input=input_data,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    return res.returncode, res.stdout, res.stderr

def fetch_kube32(path):
    try:
        url = f"{KUBE32_API}{path}"
        req = urllib.request.Request(url, headers={"Accept": "application/json"})
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            data = resp.read().decode("utf-8")
            return json.loads(data)
    except Exception as e:
        return None

def post_kube32(path, payload):
    try:
        url = f"{KUBE32_API}{path}"
        data = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            return resp.status, resp.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8")
    except Exception as e:
        return 500, str(e)

def init_k8s_namespace():
    print(f"[*] Ensuring namespace '{NAMESPACE}' exists in OrbStack Kubernetes...")
    rc, out, err = run_kubectl(["create", "namespace", NAMESPACE, "--dry-run=client", "-o", "yaml"])
    if rc == 0:
        run_kubectl(["apply", "-f", "-"], input_data=out)
    print(f"[+] Namespace '{NAMESPACE}' ready.")

def sync_nodes_and_pods():
    # 1. Fetch nodes from Kube32
    nodes_doc = fetch_kube32("/api/v1/nodes")
    if not nodes_doc or "items" not in nodes_doc:
        return False
    
    # 2. Fetch pods from Kube32
    pods_doc = fetch_kube32("/api/v1/pods")
    if not pods_doc:
        pods_doc = {"items": []}
    
    pods = pods_doc.get("items", [])
    
    # 3. For each pod on Kube32, reflect into OrbStack Kubernetes
    for pod in pods:
        name = pod["metadata"]["name"]
        node_name = pod["spec"].get("nodeName", "esp32-node-01")
        phase = pod.get("status", {}).get("phase", "Running")
        
        # Build synthetic K8s pod manifest
        k8s_pod = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {
                "name": name,
                "namespace": NAMESPACE,
                "labels": pod["metadata"].get("labels", {}),
                "annotations": {
                    "kube32.io/runtime": "wasm3",
                    "kube32.io/hardware-target": "esp32-s3",
                    "kube32.io/node": node_name,
                    "kube32.io/synced-at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
                }
            },
            "spec": {
                "nodeName": "orbstack",
                "containers": [
                    {
                        "name": c.get("name", "app"),
                        "image": c.get("image", "wasm://embedded/workload:v1"),
                        "resources": c.get("resources", {})
                    }
                    for c in pod.get("spec", {}).get("containers", [])
                ],
                "restartPolicy": "Never"
            }
        }
        
        manifest_yaml = json.dumps(k8s_pod)
        rc, out, err = run_kubectl(["apply", "-f", "-"], input_data=manifest_yaml)
        if rc == 0:
            # Update status to match Kube32 physical silicon status
            status_patch = json.dumps({
                "status": {
                    "phase": phase,
                    "conditions": [
                        {"type": "Ready", "status": "True" if phase == "Running" else "False"},
                        {"type": "PodScheduled", "status": "True"}
                    ],
                    "containerStatuses": [
                        {
                            "name": c.get("name", "app"),
                            "ready": phase == "Running",
                            "state": {"running": {"startedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}} if phase == "Running" else {"waiting": {"reason": phase}}
                        }
                        for c in pod.get("spec", {}).get("containers", [])
                    ]
                }
            })
            run_kubectl(["patch", "pod", name, "-n", NAMESPACE, "--subresource=status", "--type=merge", "-p", status_patch])
    
    return True

def main():
    print("=" * 65)
    print("  Kube32 <-> OrbStack Kubernetes Mesh Controller")
    print(f"  Target Context:   {K8S_CONTEXT}")
    print(f"  Target Namespace: {NAMESPACE}")
    print(f"  Kube32 Gateway:   {KUBE32_API}")
    print("=" * 65)
    
    init_k8s_namespace()
    
    if len(sys.argv) > 1 and sys.argv[1] == "--once":
        ok = sync_nodes_and_pods()
        print(f"Sync complete. Status: {'SUCCESS' if ok else 'FAILED'}")
        sys.exit(0 if ok else 1)
    
    print("[*] Starting continuous synchronization loop (2s interval)...")
    while True:
        try:
            sync_nodes_and_pods()
        except Exception as e:
            print(f"[!] Sync error: {e}")
        time.sleep(2.0)

if __name__ == "__main__":
    main()
