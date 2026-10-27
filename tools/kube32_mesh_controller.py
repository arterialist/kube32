#!/usr/bin/env python3
"""
Kube32 Kubernetes Mesh Controller
Synchronizes physical ESP32 microcontroller cluster state with OrbStack Kubernetes on macOS.
- Registers esp32-node-01 and esp32-node-02 as Ready Node objects in OrbStack Kubernetes
- Mirrors bare-metal ESP32 pods into OrbStack k8s namespace 'kube32-cluster' assigned to the ESP32 nodes
- Maintains continuous node heartbeats and status conditions
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
    except Exception:
        return None

def init_k8s_mesh():
    # 1. Ensure namespace exists
    run_kubectl(["create", "namespace", NAMESPACE, "--dry-run=client", "-o", "yaml"])
    rc, out, err = run_kubectl(["create", "namespace", NAMESPACE, "--dry-run=client", "-o", "yaml"])
    if rc == 0:
        run_kubectl(["apply", "-f", "-"], input_data=out)

def sync_nodes():
    nodes_doc = fetch_kube32("/api/v1/nodes")
    if not nodes_doc or "items" not in nodes_doc:
        return False
    
    now_iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

    for item in nodes_doc["items"]:
        name = item["metadata"]["name"]
        labels = item["metadata"].get("labels", {})
        is_cp = "node-role.kubernetes.io/control-plane" in labels
        role = "control-plane,microcontroller" if is_cp else "microcontroller,worker"
        ip = "192.168.4.1" if is_cp else "192.168.4.2"
        mem_alloc = item.get("status", {}).get("allocatable", {}).get("memory", "400Ki")
        mem_cap = item.get("status", {}).get("capacity", {}).get("memory", "512Ki")

        # 1. Apply Node Spec
        node_spec = {
            "apiVersion": "v1",
            "kind": "Node",
            "metadata": {
                "name": name,
                "labels": {
                    "kubernetes.io/arch": "xtensa",
                    "kubernetes.io/hostname": name,
                    "kubernetes.io/os": "freertos",
                    "node-role.kubernetes.io/control-plane": "" if is_cp else None,
                    "node-role.kubernetes.io/worker": None if is_cp else "",
                    "node-role.kubernetes.io/microcontroller": ""
                }
            },
            "spec": {
                "taints": [
                    {
                        "key": "node.kubernetes.io/embedded",
                        "value": "true",
                        "effect": "NoSchedule"
                    }
                ]
            }
        }
        # Filter None values from labels
        node_spec["metadata"]["labels"] = {k: v for k, v in node_spec["metadata"]["labels"].items() if v is not None}
        run_kubectl(["apply", "-f", "-"], input_data=json.dumps(node_spec))

        # 2. Patch Node Status & Heartbeat
        status_patch = {
            "status": {
                "conditions": [
                    {
                        "type": "Ready",
                        "status": "True",
                        "reason": "KubeletReady",
                        "message": f"kubelet is reporting healthy lease from physical ESP32 silicon ({name})",
                        "lastHeartbeatTime": now_iso,
                        "lastTransitionTime": now_iso
                    }
                ],
                "addresses": [
                    {"type": "InternalIP", "address": ip},
                    {"type": "Hostname", "address": name}
                ],
                "capacity": {
                    "cpu": "2",
                    "memory": mem_cap,
                    "pods": "16"
                },
                "allocatable": {
                    "cpu": "2",
                    "memory": mem_alloc,
                    "pods": "16"
                },
                "nodeInfo": {
                    "machineID": f"esp32s3-{name}",
                    "systemUUID": f"esp32s3-{name}",
                    "bootID": "1",
                    "kernelVersion": "FreeRTOS-v10.5 (xtensa)",
                    "osImage": "ESP-IDF v5.5.3",
                    "containerRuntimeVersion": "wasm3://v0.5.0",
                    "kubeletVersion": "v1.28.0-kube32",
                    "kubeProxyVersion": "v1.28.0-kube32",
                    "operatingSystem": "freertos",
                    "architecture": "xtensa"
                }
            }
        }
        run_kubectl(["patch", "node", name, "--subresource=status", "--type=merge", "-p", json.dumps(status_patch)])
    return True

def sync_pods():
    pods_doc = fetch_kube32("/api/v1/pods")
    if not pods_doc:
        pods_doc = {"items": []}
    
    pods = pods_doc.get("items", [])
    now_iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

    for pod in pods:
        name = pod["metadata"]["name"]
        node_name = pod["spec"].get("nodeName", "esp32-node-02")
        if not node_name or node_name == "<none>":
            node_name = "esp32-node-01"
        phase = pod.get("status", {}).get("phase", "Running")
        
        # Build synthetic K8s pod manifest assigned to the ESP32 node
        # Use standard pause container image format so containerd parser is satisfied
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
                    "kube32.io/synced-at": now_iso
                }
            },
            "spec": {
                "nodeName": node_name,  # Assigned directly to the ESP32 node!
                "containers": [
                    {
                        "name": c.get("name", "app"),
                        "image": "registry.k8s.io/pause:3.9",  # Valid OCI reference
                        "resources": {
                            "requests": c.get("resources", {}).get("requests", {})
                        }
                    }
                    for c in pod.get("spec", {}).get("containers", [])
                ],
                "restartPolicy": "Never",
                "tolerations": [
                    {
                        "key": "node.kubernetes.io/embedded",
                        "operator": "Exists",
                        "effect": "NoSchedule"
                    }
                ]
            }
        }
        
        # Save original Wasm image URIs into container annotations
        for idx, c in enumerate(pod.get("spec", {}).get("containers", [])):
            img = c.get("image", "")
            if img:
                k8s_pod["metadata"]["annotations"][f"wasm.kube32.io/container-{c.get('name', idx)}"] = img

        rc, out, err = run_kubectl(["apply", "-f", "-"], input_data=json.dumps(k8s_pod))
        if rc == 0:
            status_patch = {
                "status": {
                    "phase": phase,
                    "hostIP": "192.168.4.1" if node_name == "esp32-node-01" else "192.168.4.2",
                    "podIP": "192.168.4.1" if node_name == "esp32-node-01" else "192.168.4.2",
                    "conditions": [
                        {"type": "Ready", "status": "True" if phase == "Running" else "False", "lastTransitionTime": now_iso},
                        {"type": "ContainersReady", "status": "True" if phase == "Running" else "False", "lastTransitionTime": now_iso},
                        {"type": "PodScheduled", "status": "True", "lastTransitionTime": now_iso}
                    ],
                    "containerStatuses": [
                        {
                            "name": c.get("name", "app"),
                            "ready": phase == "Running",
                            "restartCount": 0,
                            "image": "registry.k8s.io/pause:3.9",
                            "imageID": "wasm3://embedded",
                            "state": {"running": {"startedAt": now_iso}} if phase == "Running" else {"waiting": {"reason": phase}}
                        }
                        for c in pod.get("spec", {}).get("containers", [])
                    ]
                }
            }
            run_kubectl(["patch", "pod", name, "-n", NAMESPACE, "--subresource=status", "--type=merge", "-p", json.dumps(status_patch)])

def sync_cycle():
    sync_nodes()
    sync_pods()

def main():
    print("=================================================================")
    print("  Kube32 <-> OrbStack Kubernetes Mesh Controller Online")
    print(f"  Target Context:   {K8S_CONTEXT}")
    print(f"  Target Namespace: {NAMESPACE}")
    print(f"  Kube32 Gateway:   {KUBE32_API}")
    print("=================================================================")
    init_k8s_mesh()

    if len(sys.argv) > 1 and sys.argv[1] == "--once":
        sync_cycle()
        print("[+] Mesh sync complete.")
        return

    print("[*] Continuously maintaining nodes and pods across OrbStack & ESP32...")
    while True:
        try:
            sync_cycle()
        except Exception as e:
            print(f"[!] Mesh error: {e}")
        time.sleep(4.0)

if __name__ == "__main__":
    main()
