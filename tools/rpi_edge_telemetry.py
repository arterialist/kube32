#!/usr/bin/env python3
"""
Kube32 Raspberry Pi Edge Telemetry Exporter
Exposes real-time ESP32 cluster metrics formatted for Prometheus scraping on port 9132.
Enables cross-tier observability across Mac, Raspberry Pi, and bare-metal ESP32 nodes.
"""

import sys
import time
import json
import urllib.request
from http.server import HTTPServer, BaseHTTPRequestHandler

KUBE32_API = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:6443"
METRICS_PORT = 9132

def fetch_json(url):
    try:
        req = urllib.request.Request(url, headers={"Accept": "application/json"})
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except Exception:
        return None

def generate_prometheus_metrics():
    lines = [
        "# HELP kube32_cluster_info Static cluster topology and metadata",
        "# TYPE kube32_cluster_info gauge",
        'kube32_cluster_info{version="1.0.0",arch="xtensa-lx7",rtos="freertos"} 1'
    ]
    
    # 1. Scrape Nodes
    nodes = fetch_json(f"{KUBE32_API}/api/v1/nodes")
    ready_count = 0
    total_nodes = 0
    if nodes and "items" in nodes:
        total_nodes = len(nodes["items"])
        lines.append("# HELP kube32_node_ready Node readiness status (1=Ready, 0=NotReady)")
        lines.append("# TYPE kube32_node_ready gauge")
        lines.append("# HELP kube32_node_allocatable_memory_bytes Total allocatable RAM per node")
        lines.append("# TYPE kube32_node_allocatable_memory_bytes gauge")
        
        for item in nodes["items"]:
            m = item.get("metadata", {})
            name = m.get("name", "unknown")
            labels = m.get("labels", {})
            role = "control-plane" if "node-role.kubernetes.io/control-plane" in labels else "worker"
            
            # Check ready condition
            is_ready = 1
            for cond in item.get("status", {}).get("conditions", []):
                if cond.get("type") == "Ready" and cond.get("status") != "True":
                    is_ready = 0
            if is_ready:
                ready_count += 1
            
            lines.append(f'kube32_node_ready{{node="{name}",role="{role}"}} {is_ready}')
            
            alloc_mem_str = item.get("status", {}).get("allocatable", {}).get("memory", "0Ki")
            if alloc_mem_str.endswith("Ki"):
                mem_bytes = int(alloc_mem_str[:-2]) * 1024
            else:
                mem_bytes = 0
            lines.append(f'kube32_node_allocatable_memory_bytes{{node="{name}",role="{role}"}} {mem_bytes}')
            
    # 2. Scrape Pods
    pods = fetch_json(f"{KUBE32_API}/api/v1/pods")
    running_pods = 0
    total_pods = 0
    if pods and "items" in pods:
        total_pods = len(pods["items"])
        lines.append("# HELP kube32_pod_status Pod execution status (1=Running, 0=Pending/Failed)")
        lines.append("# TYPE kube32_pod_status gauge")
        for p in pods["items"]:
            pname = p.get("metadata", {}).get("name", "unknown")
            pns = p.get("metadata", {}).get("namespace", "default")
            phase = p.get("status", {}).get("phase", "Pending")
            val = 1 if phase == "Running" else 0
            if val == 1:
                running_pods += 1
            lines.append(f'kube32_pod_status{{pod="{pname}",namespace="{pns}",phase="{phase}"}} {val}')
            
    lines.append(f"kube32_nodes_total {total_nodes}")
    lines.append(f"kube32_nodes_ready {ready_count}")
    lines.append(f"kube32_pods_total {total_pods}")
    lines.append(f"kube32_pods_running {running_pods}")
    
    return "\n".join(lines) + "\n"

class MetricsHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/metrics" or self.path == "/":
            body = generate_prometheus_metrics().encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        pass

def main():
    print(f"[*] Starting Kube32 Prometheus Exporter on :9132 (bridging {KUBE32_API})...")
    server = HTTPServer(("0.0.0.0", METRICS_PORT), MetricsHandler)
    server.serve_forever()

if __name__ == "__main__":
    main()
