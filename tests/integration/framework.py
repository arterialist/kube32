"""
Kube32 E2E Test Framework
Provides HTTPClient, KubectlClient, BaseTestCase, and Reference Mock Server
for opaque-box testing against simulated and physical Kube32 clusters.
"""

import os
import sys
import json
import time
import socket
import logging
import threading
import subprocess
import urllib.request
import urllib.error
import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler
from typing import Dict, Any, Optional, List, Tuple

logger = logging.getLogger("kube32_test")


class HTTPResponse:
    def __init__(self, status_code: int, headers: Dict[str, str], body: bytes):
        self.status_code = status_code
        self.headers = headers
        self.raw_body = body
        self._text: Optional[str] = None
        self._json: Optional[Any] = None

    @property
    def text(self) -> str:
        if self._text is None:
            self._text = self.raw_body.decode("utf-8", errors="replace")
        return self._text

    def json(self) -> Any:
        if self._json is None:
            self._json = json.loads(self.text)
        return self._json

    def __repr__(self) -> str:
        return f"<HTTPResponse status={self.status_code} bytes={len(self.raw_body)}>"


class HTTPClient:
    def __init__(self, base_url: str, default_timeout: float = 10.0, verbose: bool = False):
        self.base_url = base_url.rstrip("/")
        self.default_timeout = default_timeout
        self.verbose = verbose

    def request(
        self,
        method: str,
        path: str,
        headers: Optional[Dict[str, str]] = None,
        body: Optional[Any] = None,
        timeout: Optional[float] = None
    ) -> HTTPResponse:
        url = f"{self.base_url}{path}" if path.startswith("/") else f"{self.base_url}/{path}"
        req_headers = {
            "Accept": "application/json",
            "User-Agent": "kube32-e2e-runner/1.0"
        }
        if headers:
            req_headers.update(headers)

        data = None
        if body is not None:
            if isinstance(body, (dict, list)):
                data = json.dumps(body).encode("utf-8")
                if "Content-Type" not in req_headers:
                    req_headers["Content-Type"] = "application/json"
            elif isinstance(body, str):
                data = body.encode("utf-8")
                if "Content-Type" not in req_headers:
                    req_headers["Content-Type"] = "application/json"
            elif isinstance(body, bytes):
                data = body

        req = urllib.request.Request(url, data=data, headers=req_headers, method=method)
        req_timeout = timeout if timeout is not None else self.default_timeout

        if self.verbose:
            logger.debug(f"[HTTP] {method} {url} Headers={req_headers} BodyLen={len(data) if data else 0}")

        try:
            with urllib.request.urlopen(req, timeout=req_timeout) as response:
                resp_body = response.read()
                resp_headers = dict(response.headers)
                status_code = response.status
                if self.verbose:
                    logger.debug(f"[HTTP] Response {status_code} ({len(resp_body)} bytes)")
                return HTTPResponse(status_code, resp_headers, resp_body)
        except urllib.error.HTTPError as e:
            resp_body = e.read()
            resp_headers = dict(e.headers)
            status_code = e.code
            if self.verbose:
                logger.debug(f"[HTTP] Error Response {status_code}: {resp_body.decode('utf-8', errors='replace')}")
            return HTTPResponse(status_code, resp_headers, resp_body)
        except urllib.error.URLError as e:
            raise ConnectionError(f"Failed to connect to Kube32 at {url}: {e.reason}") from e

    def get(self, path: str, headers: Optional[Dict[str, str]] = None, **kwargs) -> HTTPResponse:
        return self.request("GET", path, headers=headers, **kwargs)

    def post(self, path: str, body: Optional[Any] = None, headers: Optional[Dict[str, str]] = None, **kwargs) -> HTTPResponse:
        return self.request("POST", path, headers=headers, body=body, **kwargs)

    def put(self, path: str, body: Optional[Any] = None, headers: Optional[Dict[str, str]] = None, **kwargs) -> HTTPResponse:
        return self.request("PUT", path, headers=headers, body=body, **kwargs)

    def patch(self, path: str, body: Optional[Any] = None, headers: Optional[Dict[str, str]] = None, **kwargs) -> HTTPResponse:
        return self.request("PATCH", path, headers=headers, body=body, **kwargs)

    def delete(self, path: str, headers: Optional[Dict[str, str]] = None, **kwargs) -> HTTPResponse:
        return self.request("DELETE", path, headers=headers, **kwargs)


class KubectlResult:
    def __init__(self, returncode: int, stdout: str, stderr: str):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr

    @property
    def ok(self) -> bool:
        return self.returncode == 0

    def json(self) -> Any:
        return json.loads(self.stdout)

    def __repr__(self) -> str:
        return f"<KubectlResult code={self.returncode} stdout_len={len(self.stdout)}>"


class KubectlClient:
    def __init__(self, server_url: str, kubectl_binary: str = "kubectl", verbose: bool = False):
        self.server_url = server_url
        self.kubectl_binary = self._resolve_kubectl(kubectl_binary)
        self.verbose = verbose

    def _resolve_kubectl(self, binary: str) -> str:
        if os.path.isabs(binary) and os.path.exists(binary):
            return binary
        # Check standard locations
        for candidate in ["/usr/local/bin/kubectl", "/usr/bin/kubectl", "/opt/homebrew/bin/kubectl", binary]:
            if os.path.exists(candidate) and os.access(candidate, os.X_OK):
                return candidate
        return binary

    def run(self, args: List[str], input_text: Optional[str] = None, timeout: float = 15.0) -> KubectlResult:
        cmd = [self.kubectl_binary, f"--server={self.server_url}", "--insecure-skip-tls-verify=true"] + args
        if self.verbose:
            logger.debug(f"[KUBECTL] Running: {' '.join(cmd)}")

        proc = subprocess.run(
            cmd,
            input=input_text,
            capture_output=True,
            text=True,
            timeout=timeout
        )
        if self.verbose:
            logger.debug(f"[KUBECTL] Exit {proc.returncode}\nStdout: {proc.stdout}\nStderr: {proc.stderr}")
        return KubectlResult(proc.returncode, proc.stdout, proc.stderr)


class BaseTestCase:
    """
    Standard base class for all Kube32 test cases.
    """
    def __init__(self, http_client: HTTPClient, kubectl_client: KubectlClient, target: str = "sim", verbose: bool = False):
        self.http = http_client
        self.kubectl = kubectl_client
        self.target = target
        self.verbose = verbose
        self.cleanup_tasks: List[Tuple[str, str, str]] = []  # (resource_type, namespace, name)
        self.unique_id = f"{int(time.time() * 1000) % 1000000}"

    def setUp(self):
        self.cleanup_tasks.clear()

    def tearDown(self):
        # Execute cleanup in reverse order
        for res_type, ns, name in reversed(self.cleanup_tasks):
            try:
                if res_type == "pod":
                    self.http.delete(f"/api/v1/namespaces/{ns}/pods/{name}")
                elif res_type == "deployment":
                    self.http.delete(f"/apis/apps/v1/namespaces/{ns}/deployments/{name}")
                elif res_type == "node":
                    self.http.delete(f"/api/v1/nodes/{name}")
            except Exception:
                pass
        self.cleanup_tasks.clear()

    def register_cleanup(self, resource_type: str, namespace: str, name: str):
        self.cleanup_tasks.append((resource_type, namespace, name))

    def unique_name(self, prefix: str = "test") -> str:
        return f"{prefix}-{self.unique_id}-{os.urandom(2).hex()}"

    def assertEqual(self, first: Any, second: Any, msg: Optional[str] = None):
        if first != second:
            raise AssertionError(msg or f"Expected {second!r}, got {first!r}")

    def assertTrue(self, expr: bool, msg: Optional[str] = None):
        if not expr:
            raise AssertionError(msg or "Expected True, got False")

    def assertFalse(self, expr: bool, msg: Optional[str] = None):
        if expr:
            raise AssertionError(msg or "Expected False, got True")

    def assertIn(self, member: Any, container: Any, msg: Optional[str] = None):
        if member not in container:
            raise AssertionError(msg or f"Expected {member!r} in {container!r}")

    def assertNotIn(self, member: Any, container: Any, msg: Optional[str] = None):
        if member in container:
            raise AssertionError(msg or f"Expected {member!r} not in {container!r}")

    def assertIsNone(self, obj: Any, msg: Optional[str] = None):
        if obj is not None:
            raise AssertionError(msg or f"Expected None, got {obj!r}")

    def assertIsNotNone(self, obj: Any, msg: Optional[str] = None):
        if obj is None:
            raise AssertionError(msg or "Expected not None, got None")

    def assert_k8s_status(self, resp: HTTPResponse, expected_code: int, expected_reason: Optional[str] = None):
        self.assertEqual(resp.status_code, expected_code, f"Expected HTTP status {expected_code}, got {resp.status_code}")
        body = resp.json()
        self.assertEqual(body.get("kind"), "Status", f"Expected kind Status, got {body.get('kind')}")
        self.assertEqual(body.get("status"), "Failure" if expected_code >= 400 else "Success")
        self.assertEqual(body.get("code"), expected_code)
        if expected_reason:
            self.assertEqual(body.get("reason"), expected_reason)

    def wait_for_condition(self, predicate, timeout: float = 10.0, interval: float = 0.25, description: str = "condition") -> Any:
        start = time.time()
        last_val = None
        while time.time() - start < timeout:
            try:
                res = predicate()
                if res:
                    return res
                last_val = res
            except Exception as e:
                last_val = e
            time.sleep(interval)
        raise TimeoutError(f"Timed out after {timeout}s waiting for {description}. Last check: {last_val}")


class MockKube32Handler(BaseHTTPRequestHandler):
    """
    Authoritative reference HTTP server handler implementing the Kube32 REST API specifications.
    Provides discovery endpoints, core resources, meta.k8s.io Table formatting,
    scheduler predicates, and Kubernetes Status error formatting.
    """
    server_version = "Kube32-Mock/1.0"

    def log_message(self, format, *args):
        # Suppress default server logs unless verbose
        pass

    @property
    def store(self):
        return self.server.store

    def _send_json(self, status_code: int, data: Any, content_type: str = "application/json"):
        body = json.dumps(data, indent=2).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _send_status(self, status_code: int, reason: str, message: str, details: Optional[Dict[str, Any]] = None):
        status_payload = {
            "kind": "Status",
            "apiVersion": "v1",
            "metadata": {},
            "status": "Failure" if status_code >= 400 else "Success",
            "message": message,
            "reason": reason,
            "details": details or {},
            "code": status_code
        }
        self._send_json(status_code, status_payload)

    def _read_body_json(self) -> Tuple[Optional[Any], Optional[str]]:
        content_len = int(self.headers.get("Content-Length", 0))
        if content_len == 0:
            return None, "empty body"
        raw = self.rfile.read(content_len).decode("utf-8", errors="replace")
        try:
            return json.loads(raw), None
        except Exception as e:
            return None, f"malformed JSON: {e}"

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path.rstrip("/")
        if not path:
            path = "/"

        accept = self.headers.get("Accept", "")
        as_table = "as=Table" in accept

        # 1. Discovery Endpoints
        if path == "/version":
            self._send_json(200, {
                "major": "1",
                "minor": "33",
                "gitVersion": "v1.33.0-kube32",
                "gitCommit": "esp32-kube32-idf",
                "gitTreeState": "clean",
                "buildDate": "2026-10-26T00:00:00Z",
                "goVersion": "go1.22.0",
                "compiler": "gcc-xtensa",
                "platform": "freertos/xtensa-esp32"
            })
            return

        if path == "/api":
            self._send_json(200, {
                "kind": "APIVersions",
                "versions": ["v1"],
                "serverAddressByClientCIDRs": [{"clientCIDR": "0.0.0.0/0", "serverAddress": "127.0.0.1:6443"}]
            })
            return

        if path == "/apis":
            self._send_json(200, {
                "kind": "APIGroupList",
                "apiVersion": "v1",
                "groups": [
                    {
                        "name": "apps",
                        "versions": [{"groupVersion": "apps/v1", "version": "v1"}],
                        "preferredVersion": {"groupVersion": "apps/v1", "version": "v1"}
                    },
                    {
                        "name": "events.k8s.io",
                        "versions": [{"groupVersion": "events.k8s.io/v1", "version": "v1"}],
                        "preferredVersion": {"groupVersion": "events.k8s.io/v1", "version": "v1"}
                    }
                ]
            })
            return

        if path == "/api/v1":
            self._send_json(200, {
                "kind": "APIResourceList",
                "apiVersion": "v1",
                "groupVersion": "v1",
                "resources": [
                    {
                        "name": "nodes",
                        "singularName": "node",
                        "namespaced": False,
                        "kind": "Node",
                        "verbs": ["get", "list", "watch", "create", "update", "patch", "delete"],
                        "shortNames": ["no"]
                    },
                    {
                        "name": "nodes/status",
                        "singularName": "node",
                        "namespaced": False,
                        "kind": "Node",
                        "verbs": ["get", "patch", "update"]
                    },
                    {
                        "name": "pods",
                        "singularName": "pod",
                        "namespaced": True,
                        "kind": "Pod",
                        "verbs": ["get", "list", "watch", "create", "update", "patch", "delete"],
                        "shortNames": ["po"],
                        "categories": ["all"]
                    },
                    {
                        "name": "pods/status",
                        "singularName": "pod",
                        "namespaced": True,
                        "kind": "Pod",
                        "verbs": ["get", "patch", "update"]
                    },
                    {
                        "name": "namespaces",
                        "singularName": "namespace",
                        "namespaced": False,
                        "kind": "Namespace",
                        "verbs": ["get", "list", "create", "delete"],
                        "shortNames": ["ns"]
                    },
                    {
                        "name": "events",
                        "singularName": "event",
                        "namespaced": True,
                        "kind": "Event",
                        "verbs": ["get", "list"]
                    }
                ]
            })
            return

        if path == "/apis/apps/v1":
            self._send_json(200, {
                "kind": "APIResourceList",
                "apiVersion": "v1",
                "groupVersion": "apps/v1",
                "resources": [
                    {
                        "name": "deployments",
                        "singularName": "deployment",
                        "namespaced": True,
                        "kind": "Deployment",
                        "verbs": ["get", "list", "watch", "create", "update", "patch", "delete"],
                        "shortNames": ["deploy"],
                        "categories": ["all"]
                    },
                    {
                        "name": "deployments/status",
                        "singularName": "deployment",
                        "namespaced": True,
                        "kind": "Deployment",
                        "verbs": ["get", "patch", "update"]
                    },
                    {
                        "name": "deployments/scale",
                        "singularName": "deployment",
                        "namespaced": True,
                        "group": "autoscaling",
                        "version": "v1",
                        "kind": "Scale",
                        "verbs": ["get", "patch", "update"]
                    }
                ]
            })
            return

        if path == "/openapi/v3":
            self._send_json(200, {"paths": {}})
            return

        # 2. Node Resources
        if path == "/api/v1/nodes":
            nodes = list(self.store["nodes"].values())
            if as_table:
                # Return meta.k8s.io/v1 Table
                table = {
                    "kind": "Table",
                    "apiVersion": "meta.k8s.io/v1",
                    "metadata": {"resourceVersion": "100"},
                    "columnDefinitions": [
                        {"name": "NAME", "type": "string", "priority": 0},
                        {"name": "STATUS", "type": "string", "priority": 0},
                        {"name": "ROLES", "type": "string", "priority": 0},
                        {"name": "AGE", "type": "string", "priority": 0},
                        {"name": "VERSION", "type": "string", "priority": 0},
                        {"name": "INTERNAL-IP", "type": "string", "priority": 1},
                        {"name": "EXTERNAL-IP", "type": "string", "priority": 1},
                        {"name": "OS-IMAGE", "type": "string", "priority": 1},
                        {"name": "KERNEL-VERSION", "type": "string", "priority": 1},
                        {"name": "CONTAINER-RUNTIME", "type": "string", "priority": 1}
                    ],
                    "rows": [
                        {
                            "cells": [
                                n["metadata"]["name"],
                                "Ready" if any(c.get("type") == "Ready" and c.get("status") == "True" for c in n.get("status", {}).get("conditions", [])) else "NotReady",
                                "worker",
                                "1h",
                                n.get("status", {}).get("nodeInfo", {}).get("kubeletVersion", "v1.33.0-kube32"),
                                (n.get("status", {}).get("addresses", [{}])[0].get("address", "127.0.0.1")),
                                "<none>",
                                n.get("status", {}).get("nodeInfo", {}).get("osImage", "ESP-IDF v5.2"),
                                n.get("status", {}).get("nodeInfo", {}).get("kernelVersion", "FreeRTOS-v10.4"),
                                n.get("status", {}).get("nodeInfo", {}).get("containerRuntimeVersion", "kube32-runtime://0.1.0")
                            ],
                            "object": {
                                "kind": "PartialObjectMetadata",
                                "apiVersion": "meta.k8s.io/v1",
                                "metadata": n["metadata"]
                            }
                        } for n in nodes
                    ]
                }
                self._send_json(200, table)
            else:
                self._send_json(200, {
                    "kind": "NodeList",
                    "apiVersion": "v1",
                    "metadata": {"resourceVersion": "100"},
                    "items": nodes
                })
            return

        if path.startswith("/api/v1/nodes/"):
            name = path.split("/")[-1]
            if name in self.store["nodes"]:
                self._send_json(200, self.store["nodes"][name])
            else:
                self._send_status(404, "NotFound", f'nodes "{name}" not found', {"name": name, "kind": "nodes"})
            return

        # 3. Pod Resources
        if path == "/api/v1/pods":
            pods = []
            for ns_pods in self.store["pods"].values():
                pods.extend(ns_pods.values())
            self._send_json(200, {"kind": "PodList", "apiVersion": "v1", "metadata": {}, "items": pods})
            return

        # Namespaced pods list: /api/v1/namespaces/{ns}/pods
        if path.startswith("/api/v1/namespaces/") and path.endswith("/pods"):
            parts = path.split("/")
            ns = parts[4]
            pods = list(self.store["pods"].get(ns, {}).values())
            if as_table:
                table = {
                    "kind": "Table",
                    "apiVersion": "meta.k8s.io/v1",
                    "metadata": {},
                    "columnDefinitions": [
                        {"name": "NAME", "type": "string", "priority": 0},
                        {"name": "READY", "type": "string", "priority": 0},
                        {"name": "STATUS", "type": "string", "priority": 0},
                        {"name": "RESTARTS", "type": "string", "priority": 0},
                        {"name": "AGE", "type": "string", "priority": 0},
                        {"name": "IP", "type": "string", "priority": 1},
                        {"name": "NODE", "type": "string", "priority": 1},
                        {"name": "NOMINATED NODE", "type": "string", "priority": 1},
                        {"name": "READINESS GATES", "type": "string", "priority": 1}
                    ],
                    "rows": [
                        {
                            "cells": [
                                p["metadata"]["name"],
                                "1/1" if p.get("status", {}).get("phase") == "Running" else "0/1",
                                p.get("status", {}).get("phase", "Pending"),
                                str(p.get("status", {}).get("containerStatuses", [{}])[0].get("restartCount", 0)),
                                "5m",
                                p.get("status", {}).get("podIP", "127.0.0.1"),
                                p.get("spec", {}).get("nodeName", "<none>"),
                                "<none>",
                                "<none>"
                            ],
                            "object": {
                                "kind": "PartialObjectMetadata",
                                "apiVersion": "meta.k8s.io/v1",
                                "metadata": p["metadata"]
                            }
                        } for p in pods
                    ]
                }
                self._send_json(200, table)
            else:
                self._send_json(200, {"kind": "PodList", "apiVersion": "v1", "metadata": {}, "items": pods})
            return

        # Namespaced single pod: /api/v1/namespaces/{ns}/pods/{name}
        if path.startswith("/api/v1/namespaces/") and "/pods/" in path:
            parts = path.split("/")
            ns = parts[4]
            name = parts[6]
            if name in self.store["pods"].get(ns, {}):
                self._send_json(200, self.store["pods"][ns][name])
            else:
                self._send_status(404, "NotFound", f'pods "{name}" not found', {"name": name, "kind": "pods"})
            return

        # 4. Deployments: /apis/apps/v1/namespaces/{ns}/deployments
        if path.startswith("/apis/apps/v1/namespaces/") and path.endswith("/deployments"):
            parts = path.split("/")
            ns = parts[5]
            deploys = list(self.store["deployments"].get(ns, {}).values())
            if as_table:
                table = {
                    "kind": "Table",
                    "apiVersion": "meta.k8s.io/v1",
                    "metadata": {},
                    "columnDefinitions": [
                        {"name": "NAME", "type": "string", "priority": 0},
                        {"name": "READY", "type": "string", "priority": 0},
                        {"name": "UP-TO-DATE", "type": "string", "priority": 0},
                        {"name": "AVAILABLE", "type": "string", "priority": 0},
                        {"name": "AGE", "type": "string", "priority": 0},
                        {"name": "CONTAINERS", "type": "string", "priority": 1},
                        {"name": "IMAGES", "type": "string", "priority": 1},
                        {"name": "SELECTOR", "type": "string", "priority": 1}
                    ],
                    "rows": [
                        {
                            "cells": [
                                d["metadata"]["name"],
                                f"{d.get('status', {}).get('readyReplicas', 0)}/{d.get('spec', {}).get('replicas', 1)}",
                                str(d.get("status", {}).get("updatedReplicas", 0)),
                                str(d.get("status", {}).get("availableReplicas", 0)),
                                "10m",
                                d.get("spec", {}).get("template", {}).get("spec", {}).get("containers", [{}])[0].get("name", "app"),
                                d.get("spec", {}).get("template", {}).get("spec", {}).get("containers", [{}])[0].get("image", "wasm://app"),
                                "app=test"
                            ],
                            "object": {
                                "kind": "PartialObjectMetadata",
                                "apiVersion": "meta.k8s.io/v1",
                                "metadata": d["metadata"]
                            }
                        } for d in deploys
                    ]
                }
                self._send_json(200, table)
            else:
                self._send_json(200, {"kind": "DeploymentList", "apiVersion": "apps/v1", "metadata": {}, "items": deploys})
            return

        # Deployment scale: /apis/apps/v1/namespaces/{ns}/deployments/{name}/scale
        if path.startswith("/apis/apps/v1/namespaces/") and path.endswith("/scale"):
            parts = path.split("/")
            ns = parts[5]
            name = parts[7]
            deploy = self.store["deployments"].get(ns, {}).get(name)
            if not deploy:
                self._send_status(404, "NotFound", f'deployments.apps "{name}" not found', {"name": name, "kind": "deployments"})
                return
            self._send_json(200, {
                "kind": "Scale",
                "apiVersion": "autoscaling/v1",
                "metadata": {"name": name, "namespace": ns},
                "spec": {"replicas": deploy.get("spec", {}).get("replicas", 1)},
                "status": {"replicas": deploy.get("status", {}).get("replicas", 1)}
            })
            return

        # Single deployment: /apis/apps/v1/namespaces/{ns}/deployments/{name}
        if path.startswith("/apis/apps/v1/namespaces/") and "/deployments/" in path:
            parts = path.split("/")
            ns = parts[5]
            name = parts[7]
            if name in self.store["deployments"].get(ns, {}):
                self._send_json(200, self.store["deployments"][ns][name])
            else:
                self._send_status(404, "NotFound", f'deployments.apps "{name}" not found', {"name": name, "kind": "deployments"})
            return

        self._send_status(404, "NotFound", f"Endpoint not found: {path}")

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path.rstrip("/")

        # Check invalid endpoint verbs
        if path in ["/version", "/api", "/apis", "/api/v1", "/apis/apps/v1"]:
            self._send_status(405, "MethodNotAllowed", f"POST not allowed on discovery endpoint {path}")
            return

        # Check for POST on a specific resource item instead of collection
        if path.startswith("/api/v1/nodes/") and len(path.split("/")) > 4:
            self._send_status(405, "MethodNotAllowed", f"POST not allowed on resource instance {path}")
            return

        body, err = self._read_body_json()
        if err:
            self._send_status(400, "BadRequest", f"Failed to parse request body: {err}")
            return

        # Node Creation: POST /api/v1/nodes
        if path == "/api/v1/nodes":
            if not isinstance(body, dict) or "metadata" not in body or not body.get("metadata", {}).get("name"):
                self._send_status(400, "BadRequest", "Node is invalid: metadata.name is required")
                return
            name = body["metadata"]["name"]
            if name in self.store["nodes"]:
                self._send_status(409, "AlreadyExists", f'nodes "{name}" already exists', {"name": name, "kind": "nodes"})
                return
            self.store["nodes"][name] = body
            self._send_json(201, body)
            return

        # Pod Creation: POST /api/v1/namespaces/{ns}/pods
        if path.startswith("/api/v1/namespaces/") and path.endswith("/pods"):
            parts = path.split("/")
            ns = parts[4]
            if not isinstance(body, dict) or "metadata" not in body or not body.get("metadata", {}).get("name"):
                self._send_status(400, "BadRequest", "Pod is invalid: metadata.name is required")
                return
            if "spec" not in body or not isinstance(body.get("spec", {}).get("containers"), list):
                self._send_status(400, "BadRequest", 'Pod is invalid: spec.containers: Required value')
                return

            name = body["metadata"]["name"]
            if ns not in self.store["pods"]:
                self.store["pods"][ns] = {}

            if name in self.store["pods"][ns]:
                self._send_status(409, "AlreadyExists", f'pods "{name}" already exists', {"name": name, "kind": "pods"})
                return

            # Autonomous Scheduler Mock Logic:
            pod = dict(body)
            pod["metadata"]["namespace"] = ns
            pod.setdefault("status", {})
            self._schedule_pod(pod)

            self.store["pods"][ns][name] = pod
            self._send_json(201, pod)
            return

        # Deployment Creation: POST /apis/apps/v1/namespaces/{ns}/deployments
        if path.startswith("/apis/apps/v1/namespaces/") and path.endswith("/deployments"):
            parts = path.split("/")
            ns = parts[5]
            if not isinstance(body, dict) or "metadata" not in body or not body.get("metadata", {}).get("name"):
                self._send_status(400, "BadRequest", "Deployment is invalid: metadata.name is required")
                return
            name = body["metadata"]["name"]
            if ns not in self.store["deployments"]:
                self.store["deployments"][ns] = {}

            if name in self.store["deployments"][ns]:
                self._send_status(409, "AlreadyExists", f'deployments.apps "{name}" already exists', {"name": name, "kind": "deployments"})
                return

            replicas = body.get("spec", {}).get("replicas", 1)
            body.setdefault("status", {
                "replicas": replicas,
                "readyReplicas": replicas,
                "availableReplicas": replicas,
                "updatedReplicas": replicas
            })
            self.store["deployments"][ns][name] = body
            self._send_json(201, body)
            return

        self._send_status(404, "NotFound", f"Endpoint not found: {path}")

    def do_PUT(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path.rstrip("/")

        # Disallow PUT on collections
        if path in ["/api/v1/nodes", "/api/v1/pods", "/apis/apps/v1/namespaces/default/deployments"]:
            self._send_status(405, "MethodNotAllowed", f"PUT not allowed on collection endpoint {path}")
            return

        body, err = self._read_body_json()
        if err:
            self._send_status(400, "BadRequest", f"Failed to parse request body: {err}")
            return

        # Update Pod Status: /api/v1/namespaces/{ns}/pods/{name}/status
        if path.startswith("/api/v1/namespaces/") and "/pods/" in path and path.endswith("/status"):
            parts = path.split("/")
            ns = parts[4]
            name = parts[6]
            if name not in self.store["pods"].get(ns, {}):
                self._send_status(404, "NotFound", f'pods "{name}" not found')
                return
            pod = self.store["pods"][ns][name]
            pod["status"] = body.get("status", {})
            self._send_json(200, pod)
            return

        # Update Node: /api/v1/nodes/{name}
        if path.startswith("/api/v1/nodes/"):
            name = path.split("/")[-1]
            if name not in self.store["nodes"]:
                self._send_status(404, "NotFound", f'nodes "{name}" not found')
                return
            self.store["nodes"][name] = body
            self._send_json(200, body)
            return

        self._send_status(404, "NotFound", f"Endpoint not found: {path}")

    def do_PATCH(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path.rstrip("/")

        body, err = self._read_body_json()
        if err:
            self._send_status(400, "BadRequest", f"Failed to parse request body: {err}")
            return

        if path.startswith("/api/v1/namespaces/") and "/pods/" in path:
            parts = path.split("/")
            ns = parts[4]
            name = parts[6]
            if name not in self.store["pods"].get(ns, {}):
                self._send_status(404, "NotFound", f'pods "{name}" not found')
                return
            pod = self.store["pods"][ns][name]
            if isinstance(body, dict):
                # Simple recursive dict merge for patch
                def merge_dict(target, source):
                    for k, v in source.items():
                        if isinstance(v, dict) and k in target and isinstance(target[k], dict):
                            merge_dict(target[k], v)
                        else:
                            target[k] = v
                merge_dict(pod, body)
            self._send_json(200, pod)
            return

        if path.startswith("/api/v1/nodes/"):
            parts = path.split("/")
            name = parts[4]
            if name not in self.store["nodes"]:
                self._send_status(404, "NotFound", f'nodes "{name}" not found')
                return
            node = self.store["nodes"][name]
            if isinstance(body, dict):
                def merge_dict(target, source):
                    for k, v in source.items():
                        if isinstance(v, dict) and k in target and isinstance(target[k], dict):
                            merge_dict(target[k], v)
                        else:
                            target[k] = v
                merge_dict(node, body)
            self._send_json(200, node)
            return

        self._send_status(404, "NotFound", f"Endpoint not found: {path}")

    def do_DELETE(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path.rstrip("/")

        if path in ["/version", "/api", "/apis", "/api/v1", "/apis/apps/v1"]:
            self._send_status(405, "MethodNotAllowed", f"DELETE not allowed on discovery endpoint {path}")
            return

        # Delete Pod: /api/v1/namespaces/{ns}/pods/{name}
        if path.startswith("/api/v1/namespaces/") and "/pods/" in path:
            parts = path.split("/")
            ns = parts[4]
            name = parts[6]
            if name in self.store["pods"].get(ns, {}):
                del self.store["pods"][ns][name]
                self._send_status(200, "Success", f'pods "{name}" deleted')
            else:
                self._send_status(404, "NotFound", f'pods "{name}" not found')
            return

        # Delete Deployment: /apis/apps/v1/namespaces/{ns}/deployments/{name}
        if path.startswith("/apis/apps/v1/namespaces/") and "/deployments/" in path:
            parts = path.split("/")
            ns = parts[5]
            name = parts[7]
            if name in self.store["deployments"].get(ns, {}):
                del self.store["deployments"][ns][name]
                self._send_status(200, "Success", f'deployments.apps "{name}" deleted')
            else:
                self._send_status(404, "NotFound", f'deployments.apps "{name}" not found')
            return

        # Delete Node: /api/v1/nodes/{name}
        if path.startswith("/api/v1/nodes/"):
            name = path.split("/")[-1]
            if name in self.store["nodes"]:
                del self.store["nodes"][name]
                self._send_status(200, "Success", f'nodes "{name}" deleted')
            else:
                self._send_status(404, "NotFound", f'nodes "{name}" not found')
            return

        self._send_status(404, "NotFound", f"Endpoint not found: {path}")

    def _schedule_pod(self, pod: Dict[str, Any]):
        """
        Emulates the Kube32 Autonomous Scheduler:
        Filters nodes based on allocatable heap and peripherals,
        binds target nodeName, and sets phase to Running or Pending.
        """
        containers = pod.get("spec", {}).get("containers", [])
        total_mem_req = 0
        req_gpio = 0
        req_i2c = 0

        for c in containers:
            resources = c.get("resources", {})
            requests = resources.get("requests", resources.get("limits", {}))
            mem_str = requests.get("memory", "0Ki")
            if "Ki" in mem_str:
                total_mem_req += int(mem_str.replace("Ki", ""))
            elif "Mi" in mem_str:
                total_mem_req += int(mem_str.replace("Mi", "")) * 1024
            elif mem_str.isdigit():
                total_mem_req += int(mem_str) // 1024

            req_gpio += int(requests.get("esp.io/gpio", 0))
            req_i2c += int(requests.get("esp.io/i2c", 0))

        # Filter candidate nodes
        best_node = None
        for node_name, node in self.store["nodes"].items():
            # Check if Ready
            is_ready = any(c.get("type") == "Ready" and c.get("status") == "True" for c in node.get("status", {}).get("conditions", []))
            if not is_ready:
                continue

            allocatable = node.get("status", {}).get("allocatable", {})
            alloc_mem_str = allocatable.get("memory", "0Ki")
            alloc_mem_kb = int(alloc_mem_str.replace("Ki", "")) if "Ki" in alloc_mem_str else 240
            alloc_gpio = int(allocatable.get("esp.io/gpio", 16))
            alloc_i2c = int(allocatable.get("esp.io/i2c", 2))

            # Calculate currently allocated memory on this node across all scheduled pods
            used_mem_kb = 0
            used_gpio = 0
            for ns_pods in self.store["pods"].values():
                for p in ns_pods.values():
                    if p.get("spec", {}).get("nodeName") == node_name and p.get("status", {}).get("phase") in ["Running", "Pending"]:
                        for pc in p.get("spec", {}).get("containers", []):
                            pres = pc.get("resources", {})
                            preq = pres.get("requests", pres.get("limits", {}))
                            pmem = preq.get("memory", "0Ki")
                            if "Ki" in pmem:
                                used_mem_kb += int(pmem.replace("Ki", ""))
                            used_gpio += int(preq.get("esp.io/gpio", 0))

            if (alloc_mem_kb - used_mem_kb >= total_mem_req) and (alloc_gpio - used_gpio >= req_gpio) and (alloc_i2c >= req_i2c):
                best_node = node_name
                break

        if best_node:
            pod["spec"]["nodeName"] = best_node
            pod["status"] = {
                "phase": "Running",
                "hostIP": "127.0.0.1",
                "podIP": "127.0.0.1",
                "startTime": "2026-10-26T07:00:00Z",
                "conditions": [
                    {"type": "PodScheduled", "status": "True"},
                    {"type": "Initialized", "status": "True"},
                    {"type": "Ready", "status": "True"},
                    {"type": "ContainersReady", "status": "True"}
                ],
                "containerStatuses": [
                    {
                        "name": containers[0]["name"] if containers else "c1",
                        "ready": True,
                        "restartCount": 0,
                        "image": containers[0].get("image", "wasm://app") if containers else "wasm://app",
                        "state": {"running": {"startedAt": "2026-10-26T07:00:00Z"}}
                    }
                ]
            }
        else:
            pod["spec"]["nodeName"] = ""
            pod["status"] = {
                "phase": "Pending",
                "conditions": [
                    {
                        "type": "PodScheduled",
                        "status": "False",
                        "reason": "Unschedulable",
                        "message": "0/1 nodes are available: insufficient heap memory or peripheral resources"
                    }
                ]
            }


class MockKube32Server:
    """
    In-process reference Kube32 HTTP Server for self-testing and mock validation.
    """
    def __init__(self, host: str = "127.0.0.1", port: int = 0):
        self.server = HTTPServer((host, port), MockKube32Handler)
        self.server.store = {
            "nodes": {},
            "pods": {},
            "deployments": {}
        }
        self.thread: Optional[threading.Thread] = None
        self._init_default_cluster_state()

    def _init_default_cluster_state(self):
        # Seed 2 default nodes
        self.server.store["nodes"]["esp32-node-01"] = {
            "kind": "Node",
            "apiVersion": "v1",
            "metadata": {
                "name": "esp32-node-01",
                "labels": {
                    "kubernetes.io/hostname": "esp32-node-01",
                    "kubernetes.io/arch": "xtensa",
                    "node-role.kubernetes.io/worker": ""
                }
            },
            "spec": {"unschedulable": False},
            "status": {
                "conditions": [{"type": "Ready", "status": "True", "reason": "KubeletReady"}],
                "addresses": [{"type": "InternalIP", "address": "127.0.0.1"}],
                "nodeInfo": {
                    "kubeletVersion": "v1.33.0-kube32",
                    "osImage": "ESP-IDF v5.2",
                    "kernelVersion": "FreeRTOS-v10.4",
                    "containerRuntimeVersion": "kube32-runtime://0.1.0"
                },
                "capacity": {"memory": "320Ki", "cpu": "2", "esp.io/gpio": "16", "esp.io/i2c": "2", "esp.io/psram": "4Mi"},
                "allocatable": {"memory": "240Ki", "cpu": "2", "esp.io/gpio": "14", "esp.io/i2c": "2", "esp.io/psram": "3840Ki"}
            }
        }
        self.server.store["nodes"]["esp32-node-02"] = {
            "kind": "Node",
            "apiVersion": "v1",
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
                "nodeInfo": {
                    "kubeletVersion": "v1.33.0-kube32",
                    "osImage": "ESP-IDF v5.2",
                    "kernelVersion": "FreeRTOS-v10.4",
                    "containerRuntimeVersion": "kube32-runtime://0.1.0"
                },
                "capacity": {"memory": "320Ki", "cpu": "2", "esp.io/gpio": "16", "esp.io/i2c": "2", "esp.io/psram": "4Mi"},
                "allocatable": {"memory": "240Ki", "cpu": "2", "esp.io/gpio": "14", "esp.io/i2c": "2", "esp.io/psram": "3840Ki"}
            }
        }

    @property
    def url(self) -> str:
        host, port = self.server.server_address
        return f"http://{host}:{port}"

    def start(self):
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def stop(self):
        if self.server:
            self.server.shutdown()
            self.server.server_close()
        if self.thread and self.thread.is_alive():
            self.thread.join(timeout=2.0)
