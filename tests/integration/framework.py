"""
Kube32 E2E Test Framework
Provides HTTPClient, KubectlClient and BaseTestCase for opaque-box testing of the
integration suites against simulated and physical Kube32 clusters.
"""

import os
import sys
import json
import time
import logging
import subprocess
import urllib.request
import urllib.error
import urllib.parse
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
        pods = []
        for res_type, ns, name in reversed(self.cleanup_tasks):
            try:
                if res_type == "pod":
                    self.http.delete(f"/api/v1/namespaces/{ns}/pods/{name}")
                    pods.append(f"/api/v1/namespaces/{ns}/pods/{name}")
                elif res_type == "deployment":
                    self.http.delete(f"/apis/apps/v1/namespaces/{ns}/deployments/{name}")
                elif res_type == "node":
                    self.http.delete(f"/api/v1/nodes/{name}")
            except Exception:
                pass
        # Pod deletion is graceful: wait until the pods are gone so the next test sees free node resources
        deadline = time.time() + 10.0
        for path in pods:
            try:
                while time.time() < deadline and self.http.get(path).status_code != 404:
                    time.sleep(0.1)
            except Exception:
                pass
        self.cleanup_tasks.clear()

    def register_cleanup(self, resource_type: str, namespace: str, name: str):
        self.cleanup_tasks.append((resource_type, namespace, name))

    def create_pod(self, name: str, requests: Optional[Dict[str, str]] = None, image: str = "wasm://embedded/echo") -> Dict[str, Any]:
        """POST a one-container pod, register its cleanup and return the created object."""
        container: Dict[str, Any] = {"name": "w", "image": image}
        if requests:
            container["resources"] = {"requests": requests}
        manifest = {
            "apiVersion": "v1", "kind": "Pod",
            "metadata": {"name": name, "namespace": "default"},
            "spec": {"containers": [container]},
        }
        self.register_cleanup("pod", "default", name)
        resp = self.http.post("/api/v1/namespaces/default/pods", body=manifest)
        self.assertEqual(resp.status_code, 201)
        return resp.json()

    def wait_for_phase(self, name: str, phase: str = "Running", timeout: float = 10.0) -> Dict[str, Any]:
        """Poll a default-namespace pod until status.phase equals `phase`; returns the pod."""
        def check():
            pod = self.http.get(f"/api/v1/namespaces/default/pods/{name}").json()
            return pod if pod.get("status", {}).get("phase") == phase else None
        return self.wait_for_condition(check, timeout=timeout, description=f"pod {name} phase {phase}")

    def first_node(self) -> Dict[str, Any]:
        """The first node of the cluster."""
        items = self.http.get("/api/v1/nodes").json().get("items", [])
        self.assertTrue(len(items) > 0, "Expected at least one node registered in cluster")
        return items[0]

    def wait_for_node_memory(self, min_ki: int = 90, timeout: float = 20.0) -> None:
        """Wait until the first node reports at least `min_ki` Ki allocatable memory. The node reports
        its free heap with each heartbeat, so it recovers a few seconds after a pod has stopped."""
        def enough():
            mem = self.first_node()["status"]["allocatable"]["memory"]
            return mem.endswith("Ki") and int(mem[:-2]) >= min_ki
        self.wait_for_condition(enough, timeout=timeout, interval=0.5, description=f"node memory >= {min_ki}Ki")

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
