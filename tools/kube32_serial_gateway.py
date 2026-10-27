#!/usr/bin/env python3
"""
Kube32 USB-Serial HTTP API Gateway
Bridges standard Kubernetes HTTP API requests from kubectl/k9s on localhost:6443
directly to the physical ESP32-S3 control plane over USB-Serial/JTAG.
Includes dynamic Table conversion for full kubectl and k9s UI compatibility.
"""

import sys
import time
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
import json
import serial

SERIAL_PORT = "/dev/cu.usbmodem101"
BAUD_RATE = 115200
HTTP_PORT = 6443

ser_lock = threading.Lock()
ser = None

def get_serial():
    global ser
    if ser is None or not ser.is_open:
        ser = serial.Serial()
        ser.port = SERIAL_PORT
        ser.baudrate = BAUD_RATE
        ser.timeout = 4.0
        ser.dtr = False
        ser.rts = False
        ser.open()
        time.sleep(0.05)
    return ser

def send_k32_request(method, path, body=""):
    with ser_lock:
        s = get_serial()
        # Flush any stale input
        s.reset_input_buffer()
        
        # Minify body to a single line so it doesn't break serial line protocol
        clean_body = ""
        if body:
            try:
                clean_body = json.dumps(json.loads(body))
            except Exception:
                clean_body = body.replace("\r", " ").replace("\n", " ").strip()
        
        req_line = f"K32_REQ:{method}:{path}"
        if clean_body:
            req_line += f":{clean_body}"
        req_line += "\n"
        
        s.write(req_line.encode("utf-8"))
        s.flush()
        
        # Read response delimited by [K32-RESP-START] ... [K32-RESP-END]
        status_code = 500
        content_type = "application/json"
        response_body = []
        in_resp = False
        start_time = time.time()
        
        while time.time() - start_time < 5.0:
            raw_line = s.readline()
            if not raw_line:
                continue
            line = raw_line.decode("utf-8", errors="replace").strip()
            
            if "[K32-RESP-START]:" in line:
                in_resp = True
                parts = line.split(":")
                if len(parts) >= 2:
                    try:
                        status_code = int(parts[1])
                    except ValueError:
                        status_code = 200
                continue
            elif line == "[K32-RESP-END]":
                break
            elif in_resp:
                response_body.append(line)
        
        body_text = "\n".join(response_body) if response_body else "{}"
        return status_code, content_type, body_text

def enrich_nodes(nodes_doc):
    for item in nodes_doc.get("items", []):
        meta = item.setdefault("metadata", {})
        if "creationTimestamp" not in meta:
            meta["creationTimestamp"] = "2026-10-27T00:00:00Z"
        status = item.setdefault("status", {})
        status.setdefault("nodeInfo", {
            "machineID": f"esp32s3-{meta.get('name', 'node')}",
            "systemUUID": f"esp32s3-{meta.get('name', 'node')}",
            "bootID": "1",
            "kernelVersion": "FreeRTOS-v10.5 (xtensa)",
            "osImage": "ESP-IDF v5.5.3",
            "containerRuntimeVersion": "wasm3://v0.5.0",
            "kubeletVersion": "v1.28.0-kube32",
            "kubeProxyVersion": "v1.28.0-kube32",
            "operatingSystem": "freertos",
            "architecture": "xtensa"
        })
        # Ensure Ready condition is True for physical hardware
        conds = status.setdefault("conditions", [])
        has_ready = False
        for c in conds:
            if c.get("type") == "Ready":
                c["status"] = "True"
                c["reason"] = "KubeletReady"
                c["message"] = "kubelet is operational and reporting healthy lease"
                has_ready = True
                break
        if not has_ready:
            conds.append({
                "type": "Ready",
                "status": "True",
                "reason": "KubeletReady",
                "message": "kubelet is operational and reporting healthy lease"
            })
    return nodes_doc

def node_to_table(node_doc):
    enrich_nodes(node_doc)
    rows = []
    for item in node_doc.get("items", []):
        meta = item.get("metadata", {})
        name = meta.get("name", "esp32-node")
        labels = meta.get("labels", {})
        is_cp = "node-role.kubernetes.io/control-plane" in labels or "node-role.kubernetes.io/master" in labels
        roles = "control-plane,master" if is_cp else "worker"
        
        status = "Ready"
        for c in item.get("status", {}).get("conditions", []):
            if c.get("type") == "Ready":
                status = "Ready" if c.get("status") == "True" else "NotReady"
        
        ip = "192.168.4.1" if is_cp else "192.168.4.2"
        for addr in item.get("status", {}).get("addresses", []):
            if addr.get("type") == "InternalIP":
                ip = addr.get("address", ip)
        
        cells = [
            name,
            status,
            roles,
            "1h",
            "v1.28.0-kube32",
            ip,
            "<none>",
            "ESP-IDF v5.5.3",
            "FreeRTOS-v10.5 (xtensa)",
            "wasm3://v0.5.0"
        ]
        rows.append({
            "cells": cells,
            "object": item
        })
    return {
        "kind": "Table",
        "apiVersion": "meta.k8s.io/v1",
        "metadata": {"resourceVersion": "100"},
        "columnDefinitions": [
            {"name": "Name", "type": "string", "format": "name", "priority": 0},
            {"name": "Status", "type": "string", "format": "", "priority": 0},
            {"name": "Roles", "type": "string", "format": "", "priority": 0},
            {"name": "Age", "type": "string", "format": "", "priority": 0},
            {"name": "Version", "type": "string", "format": "", "priority": 0},
            {"name": "Internal-IP", "type": "string", "format": "", "priority": 1},
            {"name": "External-IP", "type": "string", "format": "", "priority": 1},
            {"name": "OS-Image", "type": "string", "format": "", "priority": 1},
            {"name": "Kernel-Version", "type": "string", "format": "", "priority": 1},
            {"name": "Container-Runtime", "type": "string", "format": "", "priority": 1}
        ],
        "rows": rows
    }

def pod_to_table(pod_doc):
    rows = []
    for item in pod_doc.get("items", []):
        meta = item.setdefault("metadata", {})
        if "creationTimestamp" not in meta:
            meta["creationTimestamp"] = "2026-10-27T00:00:00Z"
        name = meta.get("name", "pod")
        ns = meta.get("namespace", "default")
        spec = item.get("spec", {})
        status_obj = item.get("status", {})
        phase = status_obj.get("phase", "Running")
        node_name = spec.get("nodeName", "<none>")
        ip = status_obj.get("podIP", "192.168.4.2" if "02" in node_name else "192.168.4.1")
        ready = "1/1" if phase == "Running" else "0/1"
        restarts = 0
        age = "5m"
        
        cells = [
            ns,
            name,
            ready,
            phase,
            restarts,
            age,
            ip,
            node_name if node_name else "<none>",
            "<none>",
            "<none>"
        ]
        rows.append({
            "cells": cells,
            "object": item
        })
    return {
        "kind": "Table",
        "apiVersion": "meta.k8s.io/v1",
        "metadata": {"resourceVersion": "100"},
        "columnDefinitions": [
            {"name": "Namespace", "type": "string", "format": "", "priority": 0},
            {"name": "Name", "type": "string", "format": "name", "priority": 0},
            {"name": "Ready", "type": "string", "format": "", "priority": 0},
            {"name": "Status", "type": "string", "format": "", "priority": 0},
            {"name": "Restarts", "type": "integer", "format": "", "priority": 0},
            {"name": "Age", "type": "string", "format": "", "priority": 0},
            {"name": "IP", "type": "string", "format": "", "priority": 1},
            {"name": "Node", "type": "string", "format": "", "priority": 1},
            {"name": "Nominated Node", "type": "string", "format": "", "priority": 1},
            {"name": "Readiness Gates", "type": "string", "format": "", "priority": 1}
        ],
        "rows": rows
    }

def namespace_to_table(ns_doc):
    rows = []
    for item in ns_doc.get("items", []):
        meta = item.setdefault("metadata", {})
        if "creationTimestamp" not in meta:
            meta["creationTimestamp"] = "2026-10-27T00:00:00Z"
        name = meta.get("name", "default")
        phase = item.get("status", {}).get("phase", "Active")
        rows.append({
            "cells": [name, phase, "1h"],
            "object": item
        })
    return {
        "kind": "Table",
        "apiVersion": "meta.k8s.io/v1",
        "metadata": {"resourceVersion": "100"},
        "columnDefinitions": [
            {"name": "Name", "type": "string", "format": "name", "priority": 0},
            {"name": "Status", "type": "string", "format": "", "priority": 0},
            {"name": "Age", "type": "string", "format": "", "priority": 0}
        ],
        "rows": rows
    }

class KubeGatewayHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.handle_proxy("GET")

    def do_POST(self):
        self.handle_proxy("POST")

    def do_PUT(self):
        self.handle_proxy("PUT")

    def do_DELETE(self):
        self.handle_proxy("DELETE")

    def send_json(self, status_code, obj):
        resp_bytes = json.dumps(obj).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(resp_bytes)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(resp_bytes)

    def handle_proxy(self, method):
        accept = self.headers.get("Accept", "")

        if self.path == "/status" or self.path == "/telemetry":
            with ser_lock:
                s = get_serial()
                s.reset_input_buffer()
                s.write(b"K32_CMD:STATUS\n")
                s.flush()
                out = []
                t_end = time.time() + 1.5
                while time.time() < t_end:
                    b = s.readline()
                    if b:
                        out.append(b.decode("utf-8", errors="replace"))
                resp_bytes = "".join(out).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(resp_bytes)))
                self.end_headers()
                self.wfile.write(resp_bytes)
                return
        elif self.path == "/pipeline/transmit":
            with ser_lock:
                s = get_serial()
                s.reset_input_buffer()
                s.write(b"K32_CMD:PIPELINE_TRANSMIT\n")
                s.flush()
                out = []
                t_end = time.time() + 2.0
                while time.time() < t_end:
                    b = s.readline()
                    if b:
                        out.append(b.decode("utf-8", errors="replace"))
                resp_bytes = "".join(out).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(resp_bytes)))
                self.end_headers()
                self.wfile.write(resp_bytes)
                return
        elif self.path == "/apis" or self.path.startswith("/apis?"):
            self.send_json(200, {
                "kind": "APIGroupList",
                "apiVersion": "v1",
                "groups": [
                    {
                        "name": "authorization.k8s.io",
                        "versions": [{"groupVersion": "authorization.k8s.io/v1", "version": "v1"}],
                        "preferredVersion": {"groupVersion": "authorization.k8s.io/v1", "version": "v1"}
                    },
                    {
                        "name": "apps",
                        "versions": [{"groupVersion": "apps/v1", "version": "v1"}],
                        "preferredVersion": {"groupVersion": "apps/v1", "version": "v1"}
                    },
                    {
                        "name": "coordination.k8s.io",
                        "versions": [{"groupVersion": "coordination.k8s.io/v1", "version": "v1"}],
                        "preferredVersion": {"groupVersion": "coordination.k8s.io/v1", "version": "v1"}
                    },
                    {
                        "name": "apiextensions.k8s.io",
                        "versions": [{"groupVersion": "apiextensions.k8s.io/v1", "version": "v1"}],
                        "preferredVersion": {"groupVersion": "apiextensions.k8s.io/v1", "version": "v1"}
                    }
                ]
            })
            return
        elif "/selfsubjectaccessreviews" in self.path:
            self.send_json(201, {
                "kind": "SelfSubjectAccessReview",
                "apiVersion": "authorization.k8s.io/v1",
                "status": {
                    "allowed": True,
                    "reason": "Kube32 cluster authorization is open"
                }
            })
            return
        elif "/selfsubjectrulesreviews" in self.path:
            self.send_json(201, {
                "kind": "SelfSubjectRulesReview",
                "apiVersion": "authorization.k8s.io/v1",
                "status": {
                    "resourceRules": [
                        {"verbs": ["*"], "apiGroups": ["*"], "resources": ["*"]}
                    ],
                    "nonResourceRules": [
                        {"verbs": ["*"], "nonResourceURLs": ["*"]}
                    ],
                    "incomplete": False
                }
            })
            return
        elif self.path == "/apis/authorization.k8s.io" or self.path.startswith("/apis/authorization.k8s.io/"):
            self.send_json(200, {
                "kind": "APIResourceList",
                "apiVersion": "v1",
                "groupVersion": "authorization.k8s.io/v1",
                "resources": [
                    {
                        "name": "selfsubjectaccessreviews",
                        "singularName": "selfsubjectaccessreview",
                        "namespaced": False,
                        "kind": "SelfSubjectAccessReview",
                        "verbs": ["create"]
                    },
                    {
                        "name": "selfsubjectrulesreviews",
                        "singularName": "selfsubjectrulesreview",
                        "namespaced": False,
                        "kind": "SelfSubjectRulesReview",
                        "verbs": ["create"]
                    }
                ]
            })
            return
        elif self.path == "/apis/apiextensions.k8s.io" or self.path.startswith("/apis/apiextensions.k8s.io/"):
            if "customresourcedefinitions" in self.path:
                self.send_json(200, {
                    "kind": "CustomResourceDefinitionList",
                    "apiVersion": "apiextensions.k8s.io/v1",
                    "metadata": {},
                    "items": []
                })
            else:
                self.send_json(200, {
                    "kind": "APIResourceList",
                    "apiVersion": "v1",
                    "groupVersion": "apiextensions.k8s.io/v1",
                    "resources": [
                        {
                            "name": "customresourcedefinitions",
                            "singularName": "customresourcedefinition",
                            "namespaced": False,
                            "kind": "CustomResourceDefinition",
                            "verbs": ["get", "list", "watch"]
                        }
                    ]
                })
            return
        elif self.path == "/apis/coordination.k8s.io" or self.path.startswith("/apis/coordination.k8s.io/"):
            if "leases" in self.path:
                self.send_json(200, {
                    "kind": "LeaseList",
                    "apiVersion": "coordination.k8s.io/v1",
                    "metadata": {},
                    "items": []
                })
            else:
                self.send_json(200, {
                    "kind": "APIResourceList",
                    "apiVersion": "v1",
                    "groupVersion": "coordination.k8s.io/v1",
                    "resources": [
                        {
                            "name": "leases",
                            "singularName": "lease",
                            "namespaced": True,
                            "kind": "Lease",
                            "verbs": ["get", "list", "watch", "create", "update", "delete"]
                        }
                    ]
                })
            return
        elif self.path == "/apis/apps" or self.path.startswith("/apis/apps/"):
            self.send_json(200, {
                "kind": "APIResourceList",
                "apiVersion": "v1",
                "groupVersion": "apps/v1",
                "resources": [
                    {"name": "deployments", "singularName": "deployment", "namespaced": True, "kind": "Deployment", "verbs": ["get", "list", "watch", "create", "update", "delete"]},
                    {"name": "statefulsets", "singularName": "statefulset", "namespaced": True, "kind": "StatefulSet", "verbs": ["get", "list", "watch", "create", "update", "delete"]},
                    {"name": "daemonsets", "singularName": "daemonset", "namespaced": True, "kind": "DaemonSet", "verbs": ["get", "list", "watch", "create", "update", "delete"]},
                    {"name": "replicasets", "singularName": "replicaset", "namespaced": True, "kind": "ReplicaSet", "verbs": ["get", "list", "watch", "create", "update", "delete"]}
                ]
            })
            return
        elif self.path == "/api/v1/namespaces" or self.path.startswith("/api/v1/namespaces?"):
            ns_doc = {
                "kind": "NamespaceList",
                "apiVersion": "v1",
                "metadata": {"resourceVersion": "1"},
                "items": [
                    {"apiVersion": "v1", "kind": "Namespace", "metadata": {"name": "default", "creationTimestamp": "2026-10-27T00:00:00Z"}, "status": {"phase": "Active"}},
                    {"apiVersion": "v1", "kind": "Namespace", "metadata": {"name": "kube-system", "creationTimestamp": "2026-10-27T00:00:00Z"}, "status": {"phase": "Active"}},
                    {"apiVersion": "v1", "kind": "Namespace", "metadata": {"name": "kube32-cluster", "creationTimestamp": "2026-10-27T00:00:00Z"}, "status": {"phase": "Active"}}
                ]
            }
            if "as=Table" in accept:
                self.send_json(200, namespace_to_table(ns_doc))
            else:
                self.send_json(200, ns_doc)
            return
        elif self.path.startswith("/openapi/v2") or self.path.startswith("/openapi/v3"):
            # Returning 404 cleanly tells kubectl to skip OpenAPI protobuf validation
            self.send_response(404)
            self.send_header("Content-Type", "application/json")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b'{"message": "OpenAPI not implemented"}')
            return

        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length).decode("utf-8", errors="replace") if content_length > 0 else ""
        
        status_code, content_type, resp_body = send_k32_request(method, self.path, body)
        
        # Intercept and enrich Node and Pod responses for k9s & kubectl Table rendering
        if status_code == 200 and "application/json" in content_type:
            try:
                doc = json.loads(resp_body)
                if doc.get("kind") == "NodeList":
                    enrich_nodes(doc)
                    if "as=Table" in accept:
                        self.send_json(200, node_to_table(doc))
                        return
                    else:
                        self.send_json(200, doc)
                        return
                elif doc.get("kind") == "PodList":
                    if "as=Table" in accept:
                        self.send_json(200, pod_to_table(doc))
                        return
                    else:
                        for p in doc.get("items", []):
                            p.setdefault("metadata", {}).setdefault("creationTimestamp", "2026-10-27T00:00:00Z")
                        self.send_json(200, doc)
                        return
            except Exception:
                pass

        resp_bytes = resp_body.encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(resp_bytes)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(resp_bytes)

    def log_message(self, format, *args):
        # Concise logging
        print(f"[GATEWAY] {self.command} {self.path} -> {args[1] if len(args) > 1 else ''}")

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else HTTP_PORT
    server = HTTPServer(("127.0.0.1", port), KubeGatewayHandler)
    print(f"=================================================================")
    print(f"  Kube32 USB-Serial Kubernetes HTTP Gateway Online")
    print(f"  Listening on http://127.0.0.1:{port}")
    print(f"  Bridging to ESP32 Control Plane at {SERIAL_PORT}")
    print(f"=================================================================")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        if ser:
            ser.close()

if __name__ == "__main__":
    main()
