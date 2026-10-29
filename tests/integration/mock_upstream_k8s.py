"""
Mock Upstream Kubernetes API Server for Federation Testing
Implements Node registration, Lease heartbeats, and HTTP chunked Pod watch streams.
Uses only Python standard library.
"""

import json
import time
import queue
import logging
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from typing import Dict, Any, List, Optional
from urllib.parse import urlparse, parse_qs

logger = logging.getLogger("mock_upstream_k8s")


class MockK8sHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        # Suppress routine HTTP logs during test execution
        pass

    @property
    def server_store(self):
        return self.server.store

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)

        self.server.record_request("GET", self.path)

        # 1. Discovery Endpoints
        if path == "/version":
            self.send_json(200, {
                "major": "1",
                "minor": "31",
                "gitVersion": "v1.31.0",
                "platform": "linux/amd64"
            })
            return

        if path == "/api":
            self.send_json(200, {"versions": ["v1"]})
            return

        if path == "/apis":
            self.send_json(200, {
                "groups": [
                    {
                        "name": "coordination.k8s.io",
                        "versions": [{"groupVersion": "coordination.k8s.io/v1", "version": "v1"}],
                        "preferredVersion": {"groupVersion": "coordination.k8s.io/v1", "version": "v1"}
                    }
                ]
            })
            return

        if path == "/apis/coordination.k8s.io" or path == "/apis/coordination.k8s.io/v1":
            self.send_json(200, {
                "kind": "APIResourceList",
                "apiVersion": "v1",
                "groupVersion": "coordination.k8s.io/v1",
                "resources": [
                    {"name": "leases", "singularName": "lease", "namespaced": True, "kind": "Lease"}
                ]
            })
            return

        # 2. Node Leases: /apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/{name}
        if path.startswith("/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/"):
            lease_name = path.split("/")[-1]
            with self.server.lock:
                lease = self.server_store["leases"].get(lease_name)
            if lease:
                self.send_json(200, lease)
            else:
                self.send_status(404, "NotFound", f'leases "{lease_name}" not found')
            return

        # 3. Nodes: /api/v1/nodes or /api/v1/nodes/{name}
        if path == "/api/v1/nodes":
            with self.server.lock:
                nodes = list(self.server_store["nodes"].values())
            self.send_json(200, {"kind": "NodeList", "apiVersion": "v1", "metadata": {}, "items": nodes})
            return

        if path.startswith("/api/v1/nodes/"):
            parts = path.split("/")
            node_name = parts[4]
            with self.server.lock:
                node = self.server_store["nodes"].get(node_name)
            if node:
                self.send_json(200, node)
            else:
                self.send_status(404, "NotFound", f'nodes "{node_name}" not found')
            return

        # 4. Pods: /api/v1/pods or /api/v1/namespaces/{ns}/pods
        if path in ("/api/v1/pods", "/api/v1/namespaces/default/pods") or (path.startswith("/api/v1/namespaces/") and path.endswith("/pods")):
            if query.get("watch", ["false"])[0].lower() in ("true", "1"):
                self.handle_pod_watch()
                return
            else:
                with self.server.lock:
                    pods = []
                    for ns_pods in self.server_store["pods"].values():
                        pods.extend(ns_pods.values())
                self.send_json(200, {"kind": "PodList", "apiVersion": "v1", "metadata": {}, "items": pods})
                return

        # Specific Pod: /api/v1/namespaces/{ns}/pods/{name}
        if path.startswith("/api/v1/namespaces/") and "/pods/" in path:
            parts = path.split("/")
            ns = parts[4]
            name = parts[6]
            with self.server.lock:
                pod = self.server_store["pods"].get(ns, {}).get(name)
            if pod:
                self.send_json(200, pod)
            else:
                self.send_status(404, "NotFound", f'pods "{name}" not found')
            return

        self.send_status(404, "NotFound", f"Endpoint {path} not found")

    def handle_pod_watch(self):
        """Streams chunked JSON watch events for Pod lifecycle changes."""
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "keep-alive")
        self.end_headers()

        event_queue = queue.Queue()
        self.server.register_watch_queue(event_queue)

        try:
            # Emit initial snapshot of currently existing pods
            with self.server.lock:
                for ns, pmap in self.server_store["pods"].items():
                    for p in pmap.values():
                        chunk = json.dumps({"type": "ADDED", "object": p}).encode("utf-8") + b"\n"
                        self.wfile.write(f"{len(chunk):X}\r\n".encode("utf-8") + chunk + b"\r\n")
            self.wfile.flush()

            # Stream dynamic events as they are produced
            while not self.server.stopped:
                try:
                    event = event_queue.get(timeout=0.2)
                    chunk = json.dumps(event).encode("utf-8") + b"\n"
                    self.wfile.write(f"{len(chunk):X}\r\n".encode("utf-8") + chunk + b"\r\n")
                    self.wfile.flush()
                except queue.Empty:
                    continue
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            self.server.unregister_watch_queue(event_queue)

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path
        body = self.read_body()
        self.server.record_request("POST", self.path, body)

        # 1. Create Node: POST /api/v1/nodes
        if path == "/api/v1/nodes":
            node_name = body.get("metadata", {}).get("name")
            if not node_name:
                self.send_status(400, "BadRequest", "metadata.name required")
                return
            with self.server.lock:
                body.setdefault("metadata", {}).setdefault("resourceVersion", str(int(time.time() * 1000)))
                self.server_store["nodes"][node_name] = body
            self.send_json(201, body)
            return

        # 2. Create Lease: POST /apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases
        if path == "/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases":
            lease_name = body.get("metadata", {}).get("name")
            with self.server.lock:
                body.setdefault("metadata", {}).setdefault("resourceVersion", str(int(time.time() * 1000)))
                self.server_store["leases"][lease_name] = body
            self.send_json(201, body)
            return

        # 3. Create Pod: POST /api/v1/namespaces/{ns}/pods
        if path.startswith("/api/v1/namespaces/") and path.endswith("/pods"):
            ns = path.split("/")[4]
            pod_name = body.get("metadata", {}).get("name")
            body.setdefault("metadata", {})["namespace"] = ns
            body["metadata"].setdefault("resourceVersion", str(int(time.time() * 1000)))
            body.setdefault("status", {"phase": "Pending"})
            with self.server.lock:
                self.server_store["pods"].setdefault(ns, {})[pod_name] = body
            self.server.broadcast_pod_event("ADDED", body)
            self.send_json(201, body)
            return

        self.send_status(404, "NotFound", f"Endpoint {path} not found")

    def do_PUT(self):
        parsed = urlparse(self.path)
        path = parsed.path
        body = self.read_body()
        self.server.record_request("PUT", self.path, body)

        # 1. Update Lease: PUT /apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/{name}
        if path.startswith("/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/"):
            lease_name = path.split("/")[-1]
            with self.server.lock:
                body.setdefault("metadata", {})["resourceVersion"] = str(int(time.time() * 1000))
                self.server_store["leases"][lease_name] = body
            self.send_json(200, body)
            return

        # 2. Update Node Status: PUT /api/v1/nodes/{name}/status
        if path.startswith("/api/v1/nodes/") and path.endswith("/status"):
            node_name = path.split("/")[4]
            with self.server.lock:
                if node_name in self.server_store["nodes"]:
                    node = self.server_store["nodes"][node_name]
                    node["status"] = body.get("status", {})
                    node["metadata"]["resourceVersion"] = str(int(time.time() * 1000))
                    self.send_json(200, node)
                    return
            self.send_status(404, "NotFound", f'nodes "{node_name}" not found')
            return

        # 3. Update Pod: PUT /api/v1/namespaces/{ns}/pods/{name}
        if path.startswith("/api/v1/namespaces/") and "/pods/" in path and not path.endswith("/status"):
            parts = path.split("/")
            ns = parts[4]
            name = parts[6]
            with self.server.lock:
                if ns in self.server_store["pods"] and name in self.server_store["pods"][ns]:
                    existing = self.server_store["pods"][ns][name]
                    existing.update(body)
                    existing.setdefault("metadata", {})["resourceVersion"] = str(int(time.time() * 1000))
                    self.server.broadcast_pod_event("MODIFIED", existing)
                    self.send_json(200, existing)
                    return
            self.send_status(404, "NotFound", f'pods "{name}" not found')
            return

        # 4. Update Pod Status: PUT /api/v1/namespaces/{ns}/pods/{name}/status
        if path.startswith("/api/v1/namespaces/") and "/pods/" in path and path.endswith("/status"):
            parts = path.split("/")
            ns = parts[4]
            name = parts[6]
            with self.server.lock:
                pod = self.server_store["pods"].get(ns, {}).get(name)
                if not pod:
                    self.send_status(404, "NotFound", f'pods "{name}" not found')
                    return
                pod["status"] = body.get("status", {})
                pod.setdefault("metadata", {})["resourceVersion"] = str(int(time.time() * 1000))
                self.server.broadcast_pod_event("MODIFIED", pod)
                self.send_json(200, pod)
                return

        self.send_status(404, "NotFound", f"Endpoint {path} not found")

    def do_PATCH(self):
        parsed = urlparse(self.path)
        path = parsed.path
        body = self.read_body()
        self.server.record_request("PATCH", self.path, body)

        # Patch Pod Status: PATCH /api/v1/namespaces/{ns}/pods/{name}/status
        if path.startswith("/api/v1/namespaces/") and "/pods/" in path and path.endswith("/status"):
            parts = path.split("/")
            ns = parts[4]
            name = parts[6]
            with self.server.lock:
                pod = self.server_store["pods"].get(ns, {}).get(name)
                if not pod:
                    self.send_status(404, "NotFound", f'pods "{name}" not found')
                    return
                pod.setdefault("status", {})
                pod["status"].update(body.get("status", {}))
                pod.setdefault("metadata", {})["resourceVersion"] = str(int(time.time() * 1000))
                self.server.broadcast_pod_event("MODIFIED", pod)
                self.send_json(200, pod)
                return

        self.send_status(404, "NotFound", f"Endpoint {path} not found")

    def do_DELETE(self):
        parsed = urlparse(self.path)
        path = parsed.path
        self.server.record_request("DELETE", self.path)

        # Delete Pod: DELETE /api/v1/namespaces/{ns}/pods/{name}
        if path.startswith("/api/v1/namespaces/") and "/pods/" in path:
            parts = path.split("/")
            ns = parts[4]
            name = parts[6]
            with self.server.lock:
                pod = self.server_store["pods"].get(ns, {}).get(name)
                if pod:
                    # If pod has finalizers, mark deletionTimestamp and broadcast MODIFIED
                    finalizers = pod.get("metadata", {}).get("finalizers", [])
                    if finalizers:
                        pod["metadata"]["deletionTimestamp"] = "2026-10-27T00:00:00Z"
                        pod["metadata"]["deletionGracePeriodSeconds"] = 30
                        self.server.broadcast_pod_event("MODIFIED", pod)
                        self.send_json(200, pod)
                        return
                    else:
                        del self.server_store["pods"][ns][name]
                        self.server.broadcast_pod_event("DELETED", pod)
                        self.send_json(200, {"kind": "Status", "status": "Success"})
                        return
            self.send_status(404, "NotFound", f'pods "{name}" not found')
            return

        self.send_status(404, "NotFound", f"Endpoint {path} not found")

    def read_body(self) -> Dict[str, Any]:
        content_len = int(self.headers.get("Content-Length", 0))
        if content_len == 0:
            return {}
        raw = self.rfile.read(content_len)
        try:
            return json.loads(raw.decode("utf-8"))
        except Exception:
            return {}

    def send_json(self, status: int, data: Any):
        body = json.dumps(data).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_status(self, code: int, reason: str, message: str):
        self.send_json(code, {
            "kind": "Status",
            "apiVersion": "v1",
            "status": "Failure",
            "message": message,
            "reason": reason,
            "code": code
        })


import socketserver


class ReusableHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    allow_reuse_address = True
    daemon_threads = True


class MockUpstreamK8sServer:
    def __init__(self, port: int = 18443):
        self.port = port
        self.server: Optional[HTTPServer] = None
        self.thread: Optional[threading.Thread] = None
        self.stopped = False
        self.lock = threading.RLock()
        self.watch_queues: List[queue.Queue] = []
        self.requests_log: List[Dict[str, Any]] = []

        self.store = {
            "nodes": {},
            "leases": {},
            "pods": {}
        }

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.port}"

    def start(self):
        self.stopped = False
        self.server = ReusableHTTPServer(("127.0.0.1", self.port), MockK8sHandler)
        self.server.store = self.store
        self.server.lock = self.lock
        self.server.stopped = False
        self.server.record_request = self.record_request
        self.server.register_watch_queue = self.register_watch_queue
        self.server.unregister_watch_queue = self.unregister_watch_queue
        self.server.broadcast_pod_event = self.broadcast_pod_event

        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def stop(self):
        self.stopped = True
        if self.server:
            self.server.stopped = True
            try:
                self.server.shutdown()
            except Exception:
                pass
            try:
                self.server.server_close()
            except Exception:
                pass
        if self.thread and self.thread.is_alive():
            self.thread.join(timeout=1.0)

    def record_request(self, method: str, path: str, body: Optional[Dict[str, Any]] = None):
        with self.lock:
            self.requests_log.append({
                "timestamp": time.time(),
                "method": method,
                "path": path,
                "body": body
            })

    def register_watch_queue(self, q: queue.Queue):
        with self.lock:
            self.watch_queues.append(q)

    def unregister_watch_queue(self, q: queue.Queue):
        with self.lock:
            if q in self.watch_queues:
                self.watch_queues.remove(q)

    def broadcast_pod_event(self, event_type: str, pod: Dict[str, Any]):
        event = {"type": event_type, "object": pod}
        with self.lock:
            for q in self.watch_queues:
                q.put(event)

    def inject_pod(self, pod_manifest: Dict[str, Any]):
        ns = pod_manifest.get("metadata", {}).get("namespace", "default")
        name = pod_manifest.get("metadata", {}).get("name")
        with self.lock:
            self.store["pods"].setdefault(ns, {})[name] = pod_manifest
        self.broadcast_pod_event("ADDED", pod_manifest)

    def delete_upstream_pod(self, ns: str, name: str):
        with self.lock:
            pod = self.store["pods"].get(ns, {}).get(name)
            if pod:
                pod.setdefault("metadata", {})["deletionTimestamp"] = "2026-10-27T00:00:00Z"
                pod["metadata"]["deletionGracePeriodSeconds"] = 30
                self.broadcast_pod_event("MODIFIED", pod)
