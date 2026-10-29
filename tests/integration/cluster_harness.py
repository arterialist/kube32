"""
Kube32 Multi-Node Cluster Simulation Harness
Manages multiple kube32-sim node instances (Control Plane + Worker Nodes)
over loopback networking, mDNS, and UDP Virtual Channel IPC.
"""

import os
import sys
import time
import socket
import struct
import shutil
import logging
import subprocess
import urllib.request
import urllib.error
import json
from typing import Dict, List, Optional, Any, Tuple

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN_PATH = os.path.join(PROJECT_ROOT, "build", "kube32-sim")

logger = logging.getLogger("kube32_cluster")


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= (b << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def fnv1a_32(text: str) -> int:
    h = 0x811C9DC5
    for b in text.encode("utf-8"):
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


class SimulatedNode:
    def __init__(self, role: str, name: str, node_id: int, api_port: int, ipc_port: int, data_dir: str):
        self.role = role
        self.name = name
        self.node_id = node_id
        self.api_port = api_port
        self.ipc_port = ipc_port
        self.data_dir = data_dir
        self.process: Optional[subprocess.Popen] = None

    @property
    def api_url(self) -> str:
        return f"http://127.0.0.1:{self.api_port}"

    def is_alive(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def read_stdout(self, timeout: float = 0.5) -> str:
        if not self.process or not self.process.stdout:
            return ""
        import fcntl
        fd = self.process.stdout.fileno()
        fl = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        chunks = []
        end_time = time.time() + timeout
        while time.time() < end_time:
            try:
                data = self.process.stdout.read()
                if data:
                    chunks.append(data.decode("utf-8", errors="replace"))
            except Exception:
                pass
            if chunks:
                break
            time.sleep(0.05)
        return "".join(chunks)

    def terminate(self):
        if self.process and self.is_alive():
            self.process.terminate()
            try:
                self.process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=1.0)
        self.process = None


class ClusterHarness:
    def __init__(self, base_api_port: int = 16443, base_ipc_port: int = 17400):
        self.base_api_port = base_api_port
        self.base_ipc_port = base_ipc_port
        self.nodes: Dict[str, SimulatedNode] = {}
        self.temp_dirs: List[str] = []

    def start_control_plane(
        self,
        name: str = "cp-0",
        lease_interval_ms: int = 1000,
        not_ready_timeout_ms: int = 3000,
        eviction_timeout_ms: int = 6000,
        reconcile_interval_ms: int = 500,
        ingress_port: int = 0
    ) -> SimulatedNode:
        data_dir = f"/tmp/k32_cluster_test_{name}_{os.getpid()}_{int(time.time()*1000)%10000}"
        self.temp_dirs.append(data_dir)
        shutil.rmtree(data_dir, ignore_errors=True)
        os.makedirs(data_dir, exist_ok=True)

        api_port = self.base_api_port
        ipc_port = self.base_ipc_port

        cmd = [
            BIN_PATH,
            "--role", "control-plane",
            "--node-name", name,
            "--node-id", "0",
            "--port", str(api_port),
            "--ipc-port", str(ipc_port),
            "--data-dir", data_dir,
            "--lease-interval", str(lease_interval_ms),
            "--not-ready-timeout", str(not_ready_timeout_ms),
            "--eviction-timeout", str(eviction_timeout_ms),
            "--reconcile-interval", str(reconcile_interval_ms)
        ]
        if ingress_port:
            cmd += ["--ingress-port", str(ingress_port)]

        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        node = SimulatedNode("control-plane", name, 0, api_port, ipc_port, data_dir)
        node.process = proc
        self.nodes[name] = node

        # Wait for API server ready
        self._wait_for_api_ready(node.api_url, timeout=5.0)
        return node

    def start_worker(
        self,
        name: str,
        node_id: int,
        cp_node: SimulatedNode,
        lease_interval_ms: int = 1000
    ) -> SimulatedNode:
        data_dir = f"/tmp/k32_cluster_test_{name}_{os.getpid()}_{int(time.time()*1000)%10000}"
        self.temp_dirs.append(data_dir)
        shutil.rmtree(data_dir, ignore_errors=True)
        os.makedirs(data_dir, exist_ok=True)

        api_port = self.base_api_port + node_id
        ipc_port = self.base_ipc_port + node_id

        cmd = [
            BIN_PATH,
            "--role", "worker",
            "--node-name", name,
            "--node-id", str(node_id),
            "--port", str(api_port),
            "--ipc-port", str(ipc_port),
            "--join", cp_node.api_url,
            "--data-dir", data_dir,
            "--lease-interval", str(lease_interval_ms)
        ]

        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        node = SimulatedNode("worker", name, node_id, api_port, ipc_port, data_dir)
        node.process = proc
        self.nodes[name] = node

        # Wait for worker lease renewal to appear on CP
        self._wait_for_worker_registered(cp_node.api_url, name, timeout=6.0)
        return node

    def kill_node(self, name: str):
        if name in self.nodes:
            self.nodes[name].terminate()

    def restart_node(self, name: str, cp_node: SimulatedNode, lease_interval_ms: int = 1000) -> SimulatedNode:
        if name in self.nodes:
            old = self.nodes[name]
            old.terminate()
            # Reuse parameters
            cmd = [
                BIN_PATH,
                "--role", old.role,
                "--node-name", old.name,
                "--node-id", str(old.node_id),
                "--port", str(old.api_port),
                "--ipc-port", str(old.ipc_port),
                "--join", cp_node.api_url,
                "--data-dir", old.data_dir,
                "--lease-interval", str(lease_interval_ms)
            ]
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            old.process = proc
            self._wait_for_worker_registered(cp_node.api_url, name, timeout=6.0)
            return old
        raise ValueError(f"Unknown node {name}")

    def teardown(self):
        for node in self.nodes.values():
            node.terminate()
        self.nodes.clear()
        for d in self.temp_dirs:
            shutil.rmtree(d, ignore_errors=True)
        self.temp_dirs.clear()

    def _wait_for_api_ready(self, api_url: str, timeout: float = 5.0):
        start = time.time()
        while time.time() - start < timeout:
            try:
                req = urllib.request.Request(f"{api_url}/version", method="GET")
                with urllib.request.urlopen(req, timeout=1.0) as resp:
                    if resp.status == 200:
                        return
            except Exception:
                pass
            time.sleep(0.1)
        raise TimeoutError(f"Control plane {api_url} failed to become ready within {timeout}s")

    def _wait_for_worker_registered(self, cp_url: str, worker_name: str, timeout: float = 6.0):
        start = time.time()
        while time.time() - start < timeout:
            try:
                node_req = urllib.request.Request(f"{cp_url}/api/v1/nodes/{worker_name}", method="GET")
                lease_req = urllib.request.Request(f"{cp_url}/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/{worker_name}", method="GET")
                with urllib.request.urlopen(node_req, timeout=1.0) as n_resp, urllib.request.urlopen(lease_req, timeout=1.0) as l_resp:
                    if n_resp.status == 200 and l_resp.status == 200:
                        return
            except Exception:
                pass
            time.sleep(0.15)
        raise TimeoutError(f"Worker {worker_name} failed to register at {cp_url} within {timeout}s")

    @staticmethod
    def send_k32c_packet(dest_ip: str, dest_port: int, channel_uri: str, sender_pod: str, payload: bytes):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        magic = 0x4332334B # "K32C" little-endian
        version = 1
        flags = 0
        seq = 1
        chan_hash = fnv1a_32(channel_uri)
        send_hash = fnv1a_32(sender_pod)
        target_hash = 0
        frag_idx = 0
        frag_cnt = 1
        payload_len = len(payload)

        # 22-byte header
        header = struct.pack(
            "<IBBHIIIBBH",
            magic,
            version,
            flags,
            seq,
            chan_hash,
            send_hash,
            target_hash,
            frag_idx,
            frag_cnt,
            payload_len
        )
        packet_no_crc = header + payload
        crc = crc16_ccitt(packet_no_crc)
        full_packet = packet_no_crc + struct.pack("<H", crc)
        sock.sendto(full_packet, (dest_ip, dest_port))
        sock.close()


def enable_link_stats(cp_node: SimulatedNode) -> None:
    """Expose the control plane's channel counters on its Ingress port, GET /api/cluster (link.*),
    through an Ingress with the kube32.io/cluster-telemetry annotation."""
    import urllib.request
    ingress = {
        "apiVersion": "networking.k8s.io/v1", "kind": "Ingress",
        "metadata": {"name": "link-stats", "namespace": "default",
                     "annotations": {"kube32.io/cluster-telemetry": "/api/cluster"}},
        "spec": {"rules": [{"http": {"paths": []}}]},
    }
    req = urllib.request.Request(
        f"{cp_node.api_url}/apis/networking.k8s.io/v1/namespaces/default/ingresses",
        data=json.dumps(ingress).encode(), headers={"Content-Type": "application/json"}, method="POST")
    urllib.request.urlopen(req, timeout=2.0).read()


def link_stats(ingress_port: int) -> Dict:
    """The "link" counters (frames_rx, crc_errors, ...) the control plane's Ingress reports."""
    import urllib.request
    with urllib.request.urlopen(f"http://127.0.0.1:{ingress_port}/api/cluster", timeout=2.0) as resp:
        return json.loads(resp.read().decode()).get("link", {})


def node_ready_condition(api_url: str, name: str) -> Optional[Dict]:
    """The Ready condition of a node, or None while the node is unknown or the API is unreachable."""
    import urllib.request
    try:
        with urllib.request.urlopen(f"{api_url}/api/v1/nodes/{name}", timeout=2.0) as resp:
            conds = json.loads(resp.read().decode()).get("status", {}).get("conditions", [])
    except Exception:
        return None
    return next((c for c in conds if c.get("type") == "Ready"), None)
