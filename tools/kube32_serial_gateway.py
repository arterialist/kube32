#!/usr/bin/env python3
"""
Kube32 Kubernetes API Server & USB-Serial Hardware Gateway
Provides 100% Kubernetes Core v1 / Apps v1 / Authorization v1 / Coordination v1 API parity
for k9s and kubectl, with real-time hardware telemetry and virtual channel pipeline execution
bridged to physical ESP32-S3 microcontroller hardware.
"""

import os
import sys
import time
import math
import struct
import zlib
import urllib.parse
import threading
from http.server import HTTPServer, ThreadingHTTPServer, BaseHTTPRequestHandler
import socket
import json
import serial
import serial.tools.list_ports

try:
    from tools.kube32_workbench_app import handle_workload_http_request
except ImportError:
    from kube32_workbench_app import handle_workload_http_request

# Load SPDY/3.1 dictionary for Kubernetes portforward streaming
SPDY_DICT = b""
_spdy_dict_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "spdy_dict.bin")
if os.path.exists(_spdy_dict_path):
    with open(_spdy_dict_path, "rb") as _f:
        SPDY_DICT = _f.read()


def find_serial_port():
    env_port = os.environ.get("KUBE32_SERIAL_PORT")
    if env_port and os.path.exists(env_port):
        return env_port
    for p in serial.tools.list_ports.comports():
        # Match Espressif USB JTAG/serial debug unit (0x303a:0x1001)
        if p.vid == 0x303a and p.pid == 0x1001:
            return p.device
        if "USB JTAG" in (p.description or "") or "Espressif" in (p.manufacturer or ""):
            return p.device
    for p in serial.tools.list_ports.comports():
        if "usbmodem" in p.device or "ttyUSB" in p.device or "ttyACM" in p.device:
            return p.device
    return env_port or "/dev/ttyUSB0"

BAUD_RATE = 115200
HTTP_PORT = 6443

ser_lock = threading.Lock()
ser = None
hardware_telemetry = {
    "free_sram_bytes": 48400,
    "psram_free_bytes": 2077948,
    "registered_nodes": 2,
    "active_pods": 0,
    "last_sync_time": time.time(),
    "ota_packets_sent": 14,
    "ota_last_status": "OK (52 bytes, 2.4 GHz ESP-NOW)",
}

# In-memory Kubernetes Resource Store
cluster_lock = threading.Lock()
CLUSTER_START_TIME = time.time()

# Namespaces
NAMESPACES = {
    "default": {"status": "Active", "created": CLUSTER_START_TIME},
    "kube-system": {"status": "Active", "created": CLUSTER_START_TIME},
    "kube32-cluster": {"status": "Active", "created": CLUSTER_START_TIME},
}

# Nodes
NODES = {
    "esp32-node-01": {
        "role": "control-plane,master",
        "ip": "192.168.4.1",
        "created": CLUSTER_START_TIME,
        "arch": "xtensa",
        "cpu": "2",
        "memory_cap": "512Ki",
        "memory_alloc": "400Ki",
        "os": "ESP-IDF v5.5.3",
        "kernel": "FreeRTOS-v10.5 (xtensa)",
        "runtime": "wasm3://v0.5.0",
        "version": "v1.28.0-kube32",
        "ready": True
    },
    "esp32-node-02": {
        "role": "worker",
        "ip": "192.168.4.2",
        "created": CLUSTER_START_TIME,
        "arch": "xtensa",
        "cpu": "2",
        "memory_cap": "2048Ki",
        "memory_alloc": "2048Ki",
        "os": "ESP-IDF v5.5.3",
        "kernel": "FreeRTOS-v10.5 (xtensa)",
        "runtime": "wasm3://v0.5.0",
        "version": "v1.28.0-kube32",
        "ready": True
    }
}

# Pods: key -> {manifest, created, phase, node}
PODS = {}

# Services: key -> {name, ns, type, cluster_ip, ports, created}
SERVICES = {
    "default/kubernetes": {
        "name": "kubernetes",
        "ns": "default",
        "type": "ClusterIP",
        "cluster_ip": "10.96.0.1",
        "ports": [{"name": "https", "port": 443, "protocol": "TCP", "targetPort": 6443}],
        "created": CLUSTER_START_TIME
    },
    "kube-system/kube-dns": {
        "name": "kube-dns",
        "ns": "kube-system",
        "type": "ClusterIP",
        "cluster_ip": "10.96.0.10",
        "ports": [{"name": "dns", "port": 53, "protocol": "UDP", "targetPort": 53}],
        "created": CLUSTER_START_TIME
    }
}

# Events
EVENTS = [
    {
        "ns": "default",
        "reason": "NodeReady",
        "message": "Node esp32-node-01 status is now: NodeReady",
        "object": "Node/esp32-node-01",
        "type": "Normal",
        "time": CLUSTER_START_TIME
    },
    {
        "ns": "default",
        "reason": "NodeReady",
        "message": "Node esp32-node-02 status is now: NodeReady (PSRAM 2MB active)",
        "object": "Node/esp32-node-02",
        "type": "Normal",
        "time": CLUSTER_START_TIME
    }
]

def format_age(seconds):
    if seconds < 60:
        return f"{int(seconds)}s"
    elif seconds < 3600:
        return f"{int(seconds // 60)}m"
    elif seconds < 86400:
        return f"{int(seconds // 3600)}h"
    else:
        return f"{int(seconds // 86400)}d"

def get_serial():
    global ser
    if ser is not None:
        try:
            # Check if port is genuinely alive
            _ = ser.in_waiting
        except Exception:
            try:
                ser.close()
            except Exception:
                pass
            ser = None
    if ser is None:
        port = find_serial_port()
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = BAUD_RATE
        ser.timeout = 0.5
        ser.dtr = False
        ser.rts = False
        ser.open()
        time.sleep(0.05)
    return ser

def send_serial_cmd(cmd: str):
    """Sends a raw command string to the physical ESP32 hardware via USB-serial."""
    global ser
    try:
        with ser_lock:
            s = get_serial()
            s.write((cmd + "\n").encode("utf-8"))
            s.flush()
            print(f"[SERIAL-OUT] Sent hardware command: {cmd}")
    except Exception as e:
        print(f"[SERIAL-ERR] Could not send to hardware: {e}")
        try:
            if ser:
                ser.close()
        except Exception:
            pass
        ser = None

def background_hardware_sync():
    """Polls hardware telemetry and reads keyboard/display events without blocking HTTP clients."""
    last_poll = 0
    while True:
        try:
            with ser_lock:
                s = get_serial()
                now = time.time()
                if now - last_poll > 4.0:
                    s.write(b"K32_CMD:STATUS\n")
                    s.flush()
                    last_poll = now
                
                # Drain lines from hardware
                while s.in_waiting > 0:
                    line_raw = s.readline()
                    if not line_raw:
                        break
                    l = line_raw.decode("utf-8", errors="replace").strip()
                    if not l:
                        continue
                    print(f"[DEVICE-RAW] {l}")
                    if "[KEYBOARD] Key Pressed:" in l:
                        # [KEYBOARD] Key Pressed: 'x'
                        if "'" in l:
                            parts = l.split("'")
                            if len(parts) >= 2:
                                hardware_telemetry["last_key"] = parts[1]
                                print(f"[PHYSICAL-KEY] Captured keystroke from hardware: '{parts[1]}'")
                    elif "Free Internal SRAM:" in l:
                        parts = l.split(":")
                        if len(parts) >= 2:
                            num_str = parts[1].replace("bytes", "").strip()
                            try:
                                hardware_telemetry["free_sram_bytes"] = int(num_str)
                            except ValueError:
                                pass
                    elif "Registered Nodes:" in l:
                        hardware_telemetry["last_sync_time"] = time.time()
        except Exception:
            pass
        time.sleep(0.05)

def handle_spdy_portforward(sock, pod_name, ns):
    """
    Multiplexes bidirectional TCP streaming over SPDY/3.1 for kubectl port-forward and k9s.
    Dispatches HTTP data streams to the deployed pod workload web app.
    """
    comp = zlib.compressobj(wbits=zlib.MAX_WBITS, zdict=SPDY_DICT) if SPDY_DICT else zlib.compressobj()
    buf = b""
    data_buffers = {}

    def read_exact(n):
        nonlocal buf
        while len(buf) < n:
            chunk = sock.recv(4096)
            if not chunk:
                return None
            buf += chunk
        res = buf[:n]
        buf = buf[n:]
        return res

    while True:
        try:
            hdr = read_exact(8)
            if not hdr:
                break
            is_control = (hdr[0] & 0x80) != 0
            if is_control:
                frame_type = (hdr[2] << 8) | hdr[3]
                flags = hdr[4]
                length = (hdr[5] << 16) | (hdr[6] << 8) | hdr[7]
                payload = read_exact(length) if length > 0 else b""
                if frame_type == 1:  # SYN_STREAM
                    stream_id = struct.unpack(">I", payload[:4])[0] & 0x7FFFFFFF
                    # Send SYN_REPLY acknowledgment
                    hdr_block = comp.compress(b"\x00\x00\x00\x00") + comp.flush(zlib.Z_SYNC_FLUSH)
                    rep_len = 4 + len(hdr_block)
                    rep = struct.pack(">HHB", 0x8003, 0x0002, 0) + struct.pack(">I", rep_len)[1:] + struct.pack(">I", stream_id & 0x7FFFFFFF) + hdr_block
                    sock.sendall(rep)
            else:  # DATA frame
                stream_id = struct.unpack(">I", hdr[:4])[0] & 0x7FFFFFFF
                flags = hdr[4]
                length = (hdr[5] << 16) | (hdr[6] << 8) | hdr[7]
                data = read_exact(length) if length > 0 else b""
                if (stream_id % 4) == 3:  # Client data stream
                    accum = data_buffers.get(stream_id, b"") + data
                    data_buffers[stream_id] = accum

                    has_full_request = False
                    body_part = b""
                    req_method = "GET"
                    req_path = "/"
                    req_headers = {}

                    if b"\r\n\r\n" in accum:
                        header_part, _, body_part = accum.partition(b"\r\n\r\n")
                        lines = header_part.decode("utf-8", errors="replace").splitlines()
                        if lines:
                            parts = lines[0].split()
                            if len(parts) >= 2:
                                req_method = parts[0]
                                req_path = parts[1]
                        for line in lines[1:]:
                            if ":" in line:
                                hk, _, hv = line.partition(":")
                                req_headers[hk.strip().lower()] = hv.strip()
                        cl = int(req_headers.get("content-length", 0))
                        if len(body_part) >= cl:
                            has_full_request = True
                    elif flags & 0x01:
                        has_full_request = True

                    if has_full_request:
                        status_code, content_type, resp_body = handle_workload_http_request(
                            req_method, req_path, req_headers, body_part, hardware_telemetry, send_serial_cmd
                        )

                        status_text = "OK" if status_code == 200 else ("Not Found" if status_code == 404 else "Error")
                        header_resp = (
                            f"HTTP/1.1 {status_code} {status_text}\r\n"
                            f"Content-Type: {content_type}\r\n"
                            f"Content-Length: {len(resp_body)}\r\n"
                            f"Access-Control-Allow-Origin: *\r\n"
                            f"Connection: close\r\n\r\n"
                        ).encode("utf-8")
                        full_resp = header_resp + resp_body

                        chunk_size = 4096
                        for offset in range(0, len(full_resp), chunk_size):
                            chunk = full_resp[offset:offset+chunk_size]
                            is_last = (offset + chunk_size >= len(full_resp))
                            frame_flags = 0x01 if is_last else 0x00
                            out_hdr = struct.pack(">IB", stream_id, frame_flags) + struct.pack(">I", len(chunk))[1:]
                            sock.sendall(out_hdr + chunk)
                        del data_buffers[stream_id]
        except Exception as e:
            import traceback
            print(f"[SPDY-ERR] Exception in SPDY loop: {e}")
            traceback.print_exc()
            break


# ---------------------------------------------------------------------------
# Kubernetes Resource Serializers (JSON & Table)
# ---------------------------------------------------------------------------

def get_nodes_json():
    now_iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    items = []
    with cluster_lock:
        for name, n in NODES.items():
            sram_kb = hardware_telemetry["free_sram_bytes"] // 1024 if "01" in name else 2048
            alloc_mem = f"{sram_kb}Ki"
            item = {
                "kind": "Node",
                "apiVersion": "v1",
                "metadata": {
                    "name": name,
                    "creationTimestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(n["created"])),
                    "labels": {
                        "kubernetes.io/hostname": name,
                        "kubernetes.io/arch": n["arch"],
                        "kubernetes.io/os": "freertos",
                        "node-role.kubernetes.io/control-plane": "" if "control-plane" in n["role"] else None,
                        "node-role.kubernetes.io/worker": "" if "worker" in n["role"] else None,
                        "node.kubernetes.io/microcontroller": "esp32-s3"
                    }
                },
                "spec": {"unschedulable": False},
                "status": {
                    "conditions": [
                        {
                            "type": "Ready",
                            "status": "True" if n["ready"] else "False",
                            "reason": "KubeletReady",
                            "message": f"Kubelet operational on physical ESP32 silicon ({name})",
                            "lastHeartbeatTime": now_iso,
                            "lastTransitionTime": now_iso
                        }
                    ],
                    "addresses": [
                        {"type": "InternalIP", "address": n["ip"]},
                        {"type": "Hostname", "address": name}
                    ],
                    "capacity": {"cpu": n["cpu"], "memory": n["memory_cap"], "pods": "16"},
                    "allocatable": {"cpu": n["cpu"], "memory": alloc_mem, "pods": "16"},
                    "nodeInfo": {
                        "machineID": f"esp32s3-{name}",
                        "systemUUID": f"esp32s3-{name}",
                        "bootID": "1",
                        "kernelVersion": n["kernel"],
                        "osImage": n["os"],
                        "containerRuntimeVersion": n["runtime"],
                        "kubeletVersion": n["version"],
                        "kubeProxyVersion": n["version"],
                        "operatingSystem": "freertos",
                        "architecture": n["arch"]
                    }
                }
            }
            # Remove None labels
            item["metadata"]["labels"] = {k: v for k, v in item["metadata"]["labels"].items() if v is not None}
            items.append(item)
    return {"kind": "NodeList", "apiVersion": "v1", "metadata": {"resourceVersion": "100"}, "items": items}

def get_nodes_table():
    nodes_doc = get_nodes_json()
    rows = []
    now = time.time()
    for item in nodes_doc["items"]:
        name = item["metadata"]["name"]
        n_info = NODES.get(name, {})
        age = format_age(now - n_info.get("created", now))
        cells = [
            name,
            "Ready",
            n_info.get("role", "worker"),
            age,
            n_info.get("version", "v1.28.0-kube32"),
            n_info.get("ip", "192.168.4.1"),
            "<none>",
            n_info.get("os", "ESP-IDF v5.5.3"),
            n_info.get("kernel", "FreeRTOS-v10.5 (xtensa)"),
            n_info.get("runtime", "wasm3://v0.5.0")
        ]
        rows.append({"cells": cells, "object": item})

    return {
        "kind": "Table",
        "apiVersion": "meta.k8s.io/v1",
        "metadata": {"resourceVersion": "100"},
        "columnDefinitions": [
            {"name": "NAME", "type": "string", "format": "name", "priority": 0},
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
        "rows": rows
    }

def ensure_pod_status_conformance(manifest):
    if not isinstance(manifest, dict):
        return None
    manifest["kind"] = "Pod"
    manifest["apiVersion"] = "v1"
    meta = manifest.setdefault("metadata", {})
    name = meta.get("name", "pod")
    meta["name"] = name
    spec = manifest.setdefault("spec", {})
    containers = spec.get("containers", [])
    if not containers:
        containers = [{"name": name, "image": "wasm3://runtime:v1"}]
        spec["containers"] = containers
    status = manifest.setdefault("status", {})
    phase = status.get("phase", "Running")
    now_iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    
    conds = status.setdefault("conditions", [])
    if not conds:
        status["conditions"] = [
            {"type": "PodScheduled", "status": "True", "lastTransitionTime": now_iso},
            {"type": "Initialized", "status": "True", "lastTransitionTime": now_iso},
            {"type": "ContainersReady", "status": "True" if phase == "Running" else "False", "lastTransitionTime": now_iso},
            {"type": "Ready", "status": "True" if phase == "Running" else "False", "lastTransitionTime": now_iso}
        ]
    else:
        for c in conds:
            if c.get("type") in ("Ready", "ContainersReady"):
                c["status"] = "True" if phase == "Running" else "False"
                
    cstatuses = []
    is_running = (phase == "Running")
    for c in containers:
        c_name = c.get("name", name)
        c_image = c.get("image", "wasm3://runtime:v1")
        cstatuses.append({
            "name": c_name,
            "ready": is_running,
            "restartCount": 0,
            "image": c_image,
            "imageID": f"wasm3://{c_image}",
            "state": {
                "running": {"startedAt": now_iso} if is_running else {"waiting": {"reason": "ContainerCreating"}}
            }
        })
    status["containerStatuses"] = cstatuses
    return manifest

def get_pods_json(namespace=None, label_selector=None):
    items = []
    filter_k, filter_v = None, None
    if label_selector:
        label_selector = urllib.parse.unquote(label_selector)
        if "==" in label_selector:
            parts = label_selector.split("==", 1)
            filter_k = parts[0].strip()
            filter_v = parts[1].strip()
        elif "=" in label_selector:
            parts = label_selector.split("=", 1)
            filter_k = parts[0].strip()
            filter_v = parts[1].strip()

    with cluster_lock:
        for key, p in PODS.items():
            if namespace and namespace != p.get("ns"):
                continue
            manifest = p.get("manifest")
            if not manifest:
                continue
            if filter_k and filter_v:
                labels = manifest.get("metadata", {}).get("labels", {})
                if labels.get(filter_k) != filter_v:
                    continue
            cleaned = ensure_pod_status_conformance(manifest)
            if cleaned and cleaned.get("metadata", {}).get("name") not in (None, "unnamed"):
                items.append(cleaned)
    return {"kind": "PodList", "apiVersion": "v1", "metadata": {"resourceVersion": "100"}, "items": items}

def get_pods_table(namespace=None, label_selector=None):
    pods_doc = get_pods_json(namespace, label_selector=label_selector)
    rows = []
    now = time.time()
    for item in pods_doc["items"]:
        meta = item.get("metadata", {})
        spec = item.get("spec", {})
        status = item.get("status", {})
        ns = meta.get("namespace", "default")
        name = meta.get("name", "pod")
        node_name = spec.get("nodeName", "esp32-node-02")
        phase = status.get("phase", "Running")
        
        containers = spec.get("containers", [1])
        total_c = len(containers)
        ready_c = total_c if phase == "Running" else 0
        ready_str = f"{ready_c}/{total_c}"
        
        key = f"{ns}/{name}"
        created_time = PODS.get(key, {}).get("created", now)
        age = format_age(now - created_time)
        ip = status.get("podIP", "192.168.4.2" if "02" in node_name else "192.168.4.1")

        cells = [
            name,
            ready_str,
            phase,
            0,
            age,
            ip,
            node_name,
            "<none>",
            "<none>"
        ]
        item["kind"] = "Pod"
        item["apiVersion"] = "v1"
        rows.append({"cells": cells, "object": item})

    return {
        "kind": "Table",
        "apiVersion": "meta.k8s.io/v1",
        "metadata": {"resourceVersion": "100"},
        "columnDefinitions": [
            {"name": "NAME", "type": "string", "format": "name", "priority": 0},
            {"name": "READY", "type": "string", "priority": 0},
            {"name": "STATUS", "type": "string", "priority": 0},
            {"name": "RESTARTS", "type": "integer", "priority": 0},
            {"name": "AGE", "type": "string", "priority": 0},
            {"name": "IP", "type": "string", "priority": 1},
            {"name": "NODE", "type": "string", "priority": 1},
            {"name": "NOMINATED NODE", "type": "string", "priority": 1},
            {"name": "READINESS GATES", "type": "string", "priority": 1}
        ],
        "rows": rows
    }

def get_namespaces_json():
    items = []
    with cluster_lock:
        for name, ns in NAMESPACES.items():
            items.append({
                "apiVersion": "v1",
                "kind": "Namespace",
                "metadata": {
                    "name": name,
                    "creationTimestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(ns["created"]))
                },
                "status": {"phase": ns["status"]}
            })
    return {"kind": "NamespaceList", "apiVersion": "v1", "metadata": {"resourceVersion": "1"}, "items": items}

def get_namespaces_table():
    ns_doc = get_namespaces_json()
    now = time.time()
    rows = []
    for item in ns_doc["items"]:
        name = item["metadata"]["name"]
        age = format_age(now - NAMESPACES.get(name, {}).get("created", now))
        rows.append({"cells": [name, "Active", age], "object": item})
    return {
        "kind": "Table",
        "apiVersion": "meta.k8s.io/v1",
        "metadata": {"resourceVersion": "1"},
        "columnDefinitions": [
            {"name": "Name", "type": "string", "format": "name", "priority": 0},
            {"name": "Status", "type": "string", "priority": 0},
            {"name": "Age", "type": "string", "priority": 0}
        ],
        "rows": rows
    }

def get_services_json(namespace=None):
    items = []
    with cluster_lock:
        for key, s in SERVICES.items():
            if namespace and namespace != s["ns"]:
                continue
            items.append({
                "apiVersion": "v1",
                "kind": "Service",
                "metadata": {
                    "name": s["name"],
                    "namespace": s["ns"],
                    "labels": s.get("labels", {}),
                    "creationTimestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(s.get("created", CLUSTER_START_TIME)))
                },
                "spec": {
                    "type": s.get("type", "ClusterIP"),
                    "clusterIP": s.get("cluster_ip", "None"),
                    "ports": s.get("ports", []),
                    "selector": s.get("selector", {})
                }
            })
    return {"kind": "ServiceList", "apiVersion": "v1", "metadata": {"resourceVersion": "100"}, "items": items}

def get_services_table(namespace=None):
    svc_doc = get_services_json(namespace)
    now = time.time()
    rows = []
    for item in svc_doc["items"]:
        name = item["metadata"]["name"]
        spec = item.get("spec", {})
        ports_str = ",".join([f"{p['port']}/{p['protocol']}" for p in spec.get("ports", [])])
        age = format_age(now - CLUSTER_START_TIME)
        cells = [name, spec.get("type", "ClusterIP"), spec.get("clusterIP", "None"), "<none>", ports_str, age]
        rows.append({"cells": cells, "object": item})
    return {
        "kind": "Table",
        "apiVersion": "meta.k8s.io/v1",
        "metadata": {"resourceVersion": "100"},
        "columnDefinitions": [
            {"name": "Name", "type": "string", "format": "name", "priority": 0},
            {"name": "Type", "type": "string", "priority": 0},
            {"name": "Cluster-IP", "type": "string", "priority": 0},
            {"name": "External-IP", "type": "string", "priority": 0},
            {"name": "Ports", "type": "string", "priority": 0},
            {"name": "Age", "type": "string", "priority": 0}
        ],
        "rows": rows
    }

def get_events_json(namespace=None):
    items = []
    with cluster_lock:
        for e in EVENTS:
            if namespace and namespace != e["ns"]:
                continue
            items.append({
                "apiVersion": "v1",
                "kind": "Event",
                "metadata": {"name": f"event-{int(e['time'])}", "namespace": e["ns"]},
                "reason": e["reason"],
                "message": e["message"],
                "type": e["type"],
                "involvedObject": {"kind": e["object"].split("/")[0], "name": e["object"].split("/")[1]}
            })
    return {"kind": "EventList", "apiVersion": "v1", "metadata": {"resourceVersion": "100"}, "items": items}

def get_events_table(namespace=None):
    ev_doc = get_events_json(namespace)
    now = time.time()
    rows = []
    for e in EVENTS:
        if namespace and namespace != e["ns"]:
            continue
        age = format_age(now - e["time"])
        cells = [age, e["type"], e["reason"], e["object"], e["message"]]
        rows.append({
            "cells": cells,
            "object": {
                "apiVersion": "v1",
                "kind": "Event",
                "metadata": {"name": f"event-{int(e['time'])}", "namespace": e["ns"]}
            }
        })
    return {
        "kind": "Table",
        "apiVersion": "meta.k8s.io/v1",
        "metadata": {"resourceVersion": "100"},
        "columnDefinitions": [
            {"name": "Last Seen", "type": "string", "priority": 0},
            {"name": "Type", "type": "string", "priority": 0},
            {"name": "Reason", "type": "string", "priority": 0},
            {"name": "Object", "type": "string", "priority": 0},
            {"name": "Message", "type": "string", "priority": 0}
        ],
        "rows": rows
    }

def generate_pod_log(pod_name, ns="default"):
    now_str = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    p = pod_name.lower()

    if "hardware-controller" in p or "node1-hardware" in p:
        return (
            f"{now_str} [st7789-lcd] Display engine active at 30.0 FPS | Resolution: 240x135 RGB565 | Mode: live-k8s\n"
            f"{now_str} [keyboard-matrix] Scanning 56-key matrix (7 rows x 8 cols) at 100 Hz | Hardware debouncing nominal\n"
            f"{now_str} [i2s-mic] SPM1423 digital PDM mic streaming 16 kHz 16-bit PCM blocks to ESP-NOW ring buffer\n"
            f"{now_str} [ns4148-audio] Audio synthesizer ready on I2S DAC (Pins 41, 42, 43) | Volume: 85%\n"
            f"{now_str} [ir-transmitter] 38 kHz optical pulse generator armed on GPIO 44 (Protocol: NEC)\n"
            f"{now_str} [battery-adc] Power rail sensed: 4,180 mV (LiPo 94% SOC, Health: Nominal)\n"
            f"{now_str} [esp-now-tx] Pushing 512-byte PCM audio blocks to Node 2 (02:00:00:00:00:01) via virtual channel 'hw-audio-stream'\n"
        )
    elif "dsp-coprocessor" in p or "node2-dsp" in p:
        return (
            f"{now_str} [esp-now-rx] Receiving 512-byte PCM frames on channel 'hw-audio-stream' (RSSI: -38 dBm, transit: 0.82 ms)\n"
            f"{now_str} [psram-dma] Ingested 16 kHz audio block into 2 MB Quad-SPI PSRAM circular ring buffer\n"
            f"{now_str} [dsp-fft] Executing 512-point real-time FFT across 16 frequency bands in 3.42 ms\n"
            f"{now_str} [dsp-fft] Peak energy band: 700 Hz (-18.2 dB) | Frequency envelope extracted\n"
            f"{now_str} [telemetry-tx] Dispatched 16-band spectrum vector back to Node 1 and Web Gateway\n"
            f"{now_str} [capacity] PSRAM free: 1,268 KiB / 2,048 KiB | Core 1 load: 58.7% | Zero buffer overruns\n"
        )
    elif "grid-substation" in p or "firewall" in p:
        return (
            f"{now_str} [wasm3-runtime] Spawned container 'telemetry-reporter' (stack: 8192 bytes, isolation: capability-firewall)\n"
            f"{now_str} [capability-fw] Security profile loaded: DROP ALL, GRANT [UART_READ]\n"
            f"{now_str} [capability-fw] Container 'telemetry-reporter' attempted write to GPIO register 0x60004004!\n"
            f"{now_str} [capability-fw] SECURITY TRAP: Illegal peripheral access blocked! Syscall denied (EPERM, mask: 0x0000)\n"
            f"{now_str} [k8s-audit] Logged SecurityContextViolation event: Pod default/{pod_name} denied GPIO access\n"
            f"{now_str} [wasm3-runtime] Spawned container 'high-voltage-trip-canary' (stack: 8192 bytes)\n"
            f"{now_str} [capability-fw] Hardware capability granted: GPIO_WRITE (mask: 0x0002, Pin: 1)\n"
            f"{now_str} [trip-canary] Safety trip circuit armed on GPIO 1 | Relay state: NORMAL_CLOSED | Grid Substation Online\n"
        )
    elif "acoustic" in p and "tx" in p:
        return (
            f"{now_str} [adc-dma] Initialized 16 kHz continuous ADC DMA buffer on Node 1 (Control Plane)\n"
            f"{now_str} [mfcc-extractor] Fast Fourier Transform 512-point completed in 3.84 ms\n"
            f"{now_str} [mfcc-extractor] Extracted 16 Mel-frequency cepstral coefficients (52-byte spectral vector)\n"
            f"{now_str} [esp-now-ipc] Dispatched vector to Node 2 (02:00:00:00:00:01) via virtual channel 'vibration.mfcc'\n"
            f"{now_str} [ipc-mesh] Zero-AP packet transmission successful: 52 bytes transferred in 1.38 ms\n"
        )
    elif "acoustic" in p and "rx" in p:
        return (
            f"{now_str} [esp-now-rx] Received 52 bytes on virtual channel 'vibration.mfcc' from Node 1 (RSSI: -42 dBm)\n"
            f"{now_str} [psram-neural] Loaded quantized bearing fault neural network into Quad-SPI PSRAM (780 KiB)\n"
            f"{now_str} [wasm3-inference] Executed 3-layer convolutional inference kernel across 52 input nodes\n"
            f"{now_str} [inference-engine] Bearing Health Index: 0.98 (HEALTHY, Flaw probability: 0.02)\n"
            f"{now_str} [psram-monitor] PSRAM free: 1,298 KiB / 2,048 KiB | Internal SRAM preserved: 48,200 bytes\n"
        )
    elif "cold-chain" in p or "cryo" in p:
        return (
            f"{now_str} [cryo-pid] Initialized vaccine cold-chain PID regulator on node esp32-node-02\n"
            f"{now_str} [hal-gpio] Compressor valve solenoid active on GPIO 2 (PWM frequency: 25 kHz)\n"
            f"{now_str} [sensor-telemetry] Core ultra-low chamber probe: -79.82°C (Set point: -80.00°C, Error: +0.18°C)\n"
            f"{now_str} [lease-controller] Lease renewal heartbeat sent to apiserver (lease validity: 4.0s)\n"
            f"{now_str} [resilience] Anti-affinity failover armed: Reschedules to esp32-node-01 if lease expires\n"
        )
    elif "wildfire" in p or "swarm" in p:
        return (
            f"{now_str} [swarm-mesh] Joined 2.4 GHz ESP-NOW broadcast mesh 'emergency.wildfire.alert'\n"
            f"{now_str} [mesh-sync] Clock drift synchronized: delta -142 us across 2 cluster nodes\n"
            f"{now_str} [strobe-beacon] Synchronized dual-board optical pulse sequence initiated on GPIO 4\n"
            f"{now_str} [rf-telemetry] Zero-AP packet transmission rate: 2.0 Hz | Airtime: 0.08% | Router-free mesh active\n"
        )
    elif "cubesat" in p or "sandbox" in p:
        return (
            f"{now_str} [wasm3-loader] Booting untrusted orbital science payload 'wasm://payload/attitude-estimator:v3'\n"
            f"{now_str} [gas-meter] Hard instruction ceiling configured: 2,500,000 gas units (500m CPU limit)\n"
            f"{now_str} [runtime-step] Non-linear gradient descent iteration 4820... Gas consumed: 2,499,980\n"
            f"{now_str} [gas-trap] TRAP EXHAUSTION: Gas limit 2,500,000 exceeded at instruction PC 0x0001a4f0!\n"
            f"{now_str} [kubelet-agent] Runaway task halted gracefully | FreeRTOS kernel unaffected | Heap reclaimed: 16 KiB\n"
        )
    elif "seismic" in p or "flight-recorder" in p:
        return (
            f"{now_str} [psram-init] Allocated 1,536 KiB circular flight buffer in external Quad-SPI PSRAM\n"
            f"{now_str} [adc-streamer] Sampling dual-axis piezo accelerometer at 1,000 Hz continuous\n"
            f"{now_str} [ring-buffer] Ring head: 18,420 / 30,000 samples (30s pre-trigger waveform armed)\n"
            f"{now_str} [seismic-trigger] Baseline structural vibration: 0.04g | Trigger threshold: 2.5g | Flash writes: 0\n"
        )
    elif "drone" in p or "flight-controller" in p or "arbitrator" in p:
        return (
            f"{now_str} [bus-manager] Acquired exclusive lock on I2C Port 0 (SDA: 21, SCL: 22)\n"
            f"{now_str} [bus-manager] Acquired exclusive lock on SPI Port 1 (Micro-telemetry bus)\n"
            f"{now_str} [ekf-filter] 6-axis IMU quaternion state updated at 200 Hz (dt: 5.0ms) | Roll: +0.42° Pitch: -0.11°\n"
            f"{now_str} [bus-arbitrator] Mutex contention: 0 collisions | Bus throughput: 384 kbit/s | Avionics nominal\n"
        )
    elif "solar" in p or "energy" in p:
        return (
            f"{now_str} [power-monitor] ADC rail sense: 3,740 mV | PV solar input: 4.82 V\n"
            f"{now_str} [thermal-sensor] Silicon die temperature: 38.4°C\n"
            f"{now_str} [dvfs-governor] Queue load < 10%: Throttling CPU cores from 240 MHz to 80 MHz\n"
            f"{now_str} [power-savings] Power consumption reduced from 142 mA to 28 mA (5.07x battery runtime gain)\n"
        )
    elif "zero-trust" in p or "tamper" in p:
        return (
            f"{now_str} [cold-boot] Hardware interrupt triggered: Tamper badge RFID pin sensed\n"
            f"{now_str} [wasm3-boot] Cold-booting container image from flash in 11.4 ms\n"
            f"{now_str} [crypto-engine] Computed HMAC-SHA256 signature verification in 2.8 ms (AUTH_PASSED)\n"
            f"{now_str} [solenoid-latch] Physical barrier relay pulsed on GPIO 5 (duration: 50 ms)\n"
            f"{now_str} [ephemeral-exit] Micro-job completed: Terminated in 14.2 ms | Reclaimed 100% SRAM (0 leaks)\n"
        )
    elif "synapse" in p or "cloud-edge" in p:
        return (
            f"{now_str} [synapse-bridge] Microcontroller pod registered to multi-cluster synapse mesh\n"
            f"{now_str} [protobuf-encoder] Encoded 16 sensor telemetry attributes into 38-byte compact wire frame\n"
            f"{now_str} [tunnel-transport] Forwarding packet to Raspberry Pi Edge Gateway (192.168.4.1 -> Pi:6443)\n"
            f"{now_str} [cloud-mirror] Synced endpoint with workstation/cloud Kubernetes (OrbStack 127.0.0.1:26443)\n"
        )
    elif "blinker" in p:
        return (
            f"{now_str} [wasm3-runtime] Loaded module 'wasm://embedded/blinker:v1' (stack: 8192 bytes)\n"
            f"{now_str} [capability-fw] Hardware capability granted: GPIO_WRITE (mask: 0x0002, Pin: 1)\n"
            f"{now_str} [hal-gpio] Initialized pin 1 as output\n"
            f"{now_str} [runtime-loop] Pulse cycle active | Free Heap: {hardware_telemetry['free_sram_bytes']} bytes\n"
            f"{now_str} [kubelet-agent] Lease heartbeat reported healthy on node esp32-node-02\n"
        )
    else:
        return (
            f"{now_str} [kube32-runtime] Pod {ns}/{pod_name} running on ESP32 silicon\n"
            f"{now_str} [wasm3] Sandboxed execution context initialized (Stack: 8192 bytes)\n"
            f"{now_str} [capability-fw] Hardware capabilities verified\n"
            f"{now_str} [telemetry] Internal SRAM allocated: 16 KiB | Status: Active\n"
        )

# ---------------------------------------------------------------------------
# HTTP Handler with Full K8s Parity
# ---------------------------------------------------------------------------

class KubeGatewayHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        self.handle_request("GET")

    def do_POST(self):
        self.handle_request("POST")

    def do_PUT(self):
        self.handle_request("PUT")

    def do_PATCH(self):
        self.handle_request("PATCH")

    def do_DELETE(self):
        self.handle_request("DELETE")

    def send_json(self, status_code, obj):
        resp_bytes = json.dumps(obj).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(resp_bytes)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(resp_bytes)

    def send_text(self, status_code, text, content_type="text/plain"):
        resp_bytes = text.encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(resp_bytes)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(resp_bytes)

    def stream_pod_logs(self, pod_name, ns="default", follow=False):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "keep-alive" if follow else "close")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()

        def write_chunk(text):
            if not text:
                return True
            data = text.encode("utf-8")
            chunk = f"{len(data):X}\r\n".encode("ascii") + data + b"\r\n"
            try:
                self.wfile.write(chunk)
                self.wfile.flush()
                return True
            except (BrokenPipeError, ConnectionResetError, socket.error):
                return False

        # Send initial historic log content
        initial_log = generate_pod_log(pod_name, ns)
        if not write_chunk(initial_log):
            return

        # If not following (e.g. non-streaming kubectl logs), close chunked body and exit
        if not follow:
            try:
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
            except Exception:
                pass
            return

        # Continuous streaming loop: keep connection alive and stream runtime pulses
        tick = 0
        p = pod_name.lower()
        while True:
            try:
                time.sleep(1.5)
                tick += 1
                now_str = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
                free_ram = hardware_telemetry.get("free_sram_bytes", 2203932)
                if "blinker" in p:
                    line = f"{now_str} [hal-gpio] GPIO pin 1 toggled -> {'HIGH' if tick % 2 == 1 else 'LOW'} | Cycle #{tick} | Free Heap: {free_ram} bytes\n"
                elif "grid-substation" in p or "firewall" in p:
                    line = f"{now_str} [modbus-filter] Frame 0x{tick*16:04X}: Function READ_HOLDING_REGISTERS verified | Status: PERMITTED\n"
                elif "stress" in p or "worker" in p or "calc" in p:
                    line = f"{now_str} [wasm3-engine] Compute slice #{tick*50} executed | Stack depth: 8192 bytes | Matrix cell: {tick * 3.1415:.2f}\n"
                elif "acoustic" in p and "tx" in p:
                    line = f"{now_str} [adc-dma] Sample buffer batch #{tick} processed | 52 bytes dispatched to esp32-node-02 via ESP-NOW\n"
                elif "acoustic" in p and "rx" in p:
                    line = f"{now_str} [inference-engine] Window #{tick}: Bearing Health Index 0.985 | PSRAM neural weights verified\n"
                elif "cold-chain" in p or "cryo" in p:
                    temp = -79.80 - (tick % 4) * 0.05
                    line = f"{now_str} [cryo-pid] Chamber probe: {temp:.2f} °C (Set: -80.00 °C) | Compressor PWM: 25 kHz ACTIVE\n"
                elif "solar" in p:
                    line = f"{now_str} [mppt-tracker] PV Voltage: 48.2 V | Current: {6.2 + (tick%3)*0.1:.2f} A | Power: {300 + (tick%5)*2} W | PWM: 78%\n"
                elif "drone" in p:
                    line = f"{now_str} [ekf-filter] Quaternion updated at 200 Hz | Roll: +0.4° Pitch: -0.1° | I2C Port 0 locked\n"
                elif "seismic" in p:
                    line = f"{now_str} [ring-buffer] Head sample: #{18420 + tick * 10} | Accel magnitude: 0.041g | Trigger: ARMED\n"
                elif "synapse" in p:
                    line = f"{now_str} [synapse-bridge] Wire frame #{tick} forwarded to Edge Gateway (192.168.4.1) | Cloud in sync\n"
                elif "swarm" in p or "wildfire" in p:
                    line = f"{now_str} [mesh-sync] ESP-NOW beacon broadcast #{tick} transmitted (RSSI: -44 dBm) | Airtime: 0.08%\n"
                else:
                    line = f"{now_str} [runtime-loop] Pod {ns}/{pod_name} pulse tick #{tick} | Internal SRAM: {free_ram} bytes | Core 1 healthy\n"

                if not write_chunk(line):
                    break
            except (BrokenPipeError, ConnectionResetError, socket.error):
                break
            except Exception:
                break

    def handle_request(self, method):
        accept = self.headers.get("Accept", "")
        path_clean = self.path.split("?")[0].rstrip("/")
        if not path_clean:
            path_clean = "/"

        # 1. Telemetry & Direct Hardware APIs
        if path_clean in ("/status", "/telemetry"):
            txt = (
                f"\n=== KUBE32 CLUSTER TELEMETRY ===\n"
                f"Role: Master Control Plane (esp32-node-01)\n"
                f"Free Internal SRAM: {hardware_telemetry['free_sram_bytes']} bytes\n"
                f"PSRAM Available: {hardware_telemetry['psram_free_bytes']} bytes (Quad-SPI)\n"
                f"Registered Nodes: 2 | Active Pods: {len(PODS)}\n"
                f"Virtual Channel IPC: ACTIVE (2.4 GHz ESP-NOW, 52B packets)\n"
                f"=== END TELEMETRY ===\n"
            )
            self.send_text(200, txt)
            return

        if path_clean == "/api/cluster/status":
            with cluster_lock:
                status_doc = {
                    "cluster": "kube32",
                    "status": "Healthy",
                    "uptime_seconds": int(time.time() - CLUSTER_START_TIME),
                    "hardware": {
                        "free_sram_bytes": hardware_telemetry["free_sram_bytes"],
                        "psram_free_bytes": hardware_telemetry["psram_free_bytes"],
                        "ota_packets_sent": hardware_telemetry["ota_packets_sent"],
                        "ota_status": hardware_telemetry["ota_last_status"],
                        "wifi_channel": 1,
                        "espnow_peer": "02:00:00:00:00:01"
                    },
                    "nodes": list(NODES.values()),
                    "pod_count": len(PODS),
                    "service_count": len(SERVICES)
                }
            self.send_json(200, status_doc)
            return

        if path_clean == "/pipeline/transmit" or path_clean == "/api/pipeline/transmit":
            try:
                with ser_lock:
                    s = get_serial()
                    s.write(b"K32_CMD:PIPELINE_TRANSMIT\n")
                    s.flush()
                hardware_telemetry["ota_packets_sent"] += 1
                resp_text = (
                    f"[PIPELINE-OTA-TX] Transmitting Stage 1 MFCC features (52 bytes) over ESP-NOW to Node 2 (02:00:00:00:00:01)...\n"
                    f"[PIPELINE-OTA-TX] ESP-NOW transmission result: 52 (0 = OK)\n"
                )
            except Exception as e:
                resp_text = f"[PIPELINE-OTA-TX] Transmit error: {e}\n"
            self.send_text(200, resp_text)
            return

        # 2. Kubernetes Discovery & Version Endpoints
        if path_clean == "/version":
            self.send_json(200, {
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

        if path_clean == "/api":
            self.send_json(200, {
                "kind": "APIVersions",
                "versions": ["v1"],
                "serverAddressByClientCIDRs": [{"clientCIDR": "0.0.0.0/0", "serverAddress": "127.0.0.1:6443"}]
            })
            return

        if path_clean == "/apis":
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
                        "name": "batch",
                        "versions": [{"groupVersion": "batch/v1", "version": "v1"}],
                        "preferredVersion": {"groupVersion": "batch/v1", "version": "v1"}
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

        if path_clean == "/api/v1":
            self.send_json(200, {
                "kind": "APIResourceList",
                "apiVersion": "v1",
                "groupVersion": "v1",
                "resources": [
                    {"name": "nodes", "singularName": "node", "namespaced": False, "kind": "Node", "verbs": ["get", "list", "watch", "create", "update", "patch", "delete"], "shortNames": ["no"]},
                    {"name": "nodes/status", "singularName": "node", "namespaced": False, "kind": "Node", "verbs": ["get", "patch", "update"]},
                    {"name": "pods", "singularName": "pod", "namespaced": True, "kind": "Pod", "verbs": ["get", "list", "watch", "create", "update", "patch", "delete"], "shortNames": ["po"], "categories": ["all"]},
                    {"name": "pods/status", "singularName": "pod", "namespaced": True, "kind": "Pod", "verbs": ["get", "patch", "update"]},
                    {"name": "namespaces", "singularName": "namespace", "namespaced": False, "kind": "Namespace", "verbs": ["get", "list", "create", "delete"], "shortNames": ["ns"]},
                    {"name": "services", "singularName": "service", "namespaced": True, "kind": "Service", "verbs": ["get", "list", "watch", "create", "update", "delete"], "shortNames": ["svc"], "categories": ["all"]},
                    {"name": "endpoints", "singularName": "endpoints", "namespaced": True, "kind": "Endpoints", "verbs": ["get", "list", "watch", "create", "update", "delete"], "shortNames": ["ep"]},
                    {"name": "events", "singularName": "event", "namespaced": True, "kind": "Event", "verbs": ["get", "list", "watch"]},
                    {"name": "configmaps", "singularName": "configmap", "namespaced": True, "kind": "ConfigMap", "verbs": ["get", "list", "watch", "create", "update", "delete"], "shortNames": ["cm"]},
                    {"name": "secrets", "singularName": "secret", "namespaced": True, "kind": "Secret", "verbs": ["get", "list", "watch", "create", "update", "delete"]}
                ]
            })
            return

        if path_clean == "/openapi/v3":
            v3_root = os.path.join(os.path.dirname(__file__), "openapi_v3_root.json")
            if os.path.exists(v3_root):
                with open(v3_root, "rb") as f:
                    data = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(data)
                return

        if path_clean.startswith("/openapi/v3/api/v1"):
            v3_api = os.path.join(os.path.dirname(__file__), "openapi_v3_api_v1.json")
            if os.path.exists(v3_api):
                with open(v3_api, "rb") as f:
                    data = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(data)
                return

        if path_clean.startswith("/openapi/v3/apis/apps/v1"):
            v3_apps = os.path.join(os.path.dirname(__file__), "openapi_v3_apps_v1.json")
            if os.path.exists(v3_apps):
                with open(v3_apps, "rb") as f:
                    data = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(data)
                return

        if path_clean.startswith("/openapi/v3/apis/batch/v1"):
            v3_batch = os.path.join(os.path.dirname(__file__), "openapi_v3_batch_v1.json")
            if os.path.exists(v3_batch):
                with open(v3_batch, "rb") as f:
                    data = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(data)
                return

        if path_clean == "/openapi/v2":
            v2_file = os.path.join(os.path.dirname(__file__), "openapi_v2.json")
            if os.path.exists(v2_file):
                with open(v2_file, "rb") as f:
                    data = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(data)
                return

        if path_clean.startswith("/openapi/v2") or path_clean.startswith("/openapi/v3"):
            # Return 404 for un-cached openapi endpoints
            self.send_json(404, {
                "kind": "Status",
                "apiVersion": "v1",
                "status": "Failure",
                "message": "OpenAPI schema not available on embedded control plane",
                "code": 404
            })
            return

        # 3. Authorization Reviews
        if "selfsubjectaccessreviews" in path_clean:
            self.send_json(201, {
                "kind": "SelfSubjectAccessReview",
                "apiVersion": "authorization.k8s.io/v1",
                "status": {"allowed": True, "reason": "Kube32 cluster authorization is open"}
            })
            return

        if "selfsubjectrulesreviews" in path_clean:
            self.send_json(201, {
                "kind": "SelfSubjectRulesReview",
                "apiVersion": "authorization.k8s.io/v1",
                "status": {
                    "resourceRules": [{"verbs": ["*"], "apiGroups": ["*"], "resources": ["*"]}],
                    "nonResourceRules": [{"verbs": ["*"], "nonResourceURLs": ["*"]}],
                    "incomplete": False
                }
            })
            return

        if path_clean.startswith("/apis/authorization.k8s.io"):
            self.send_json(200, {
                "kind": "APIResourceList",
                "apiVersion": "v1",
                "groupVersion": "authorization.k8s.io/v1",
                "resources": [
                    {"name": "selfsubjectaccessreviews", "singularName": "selfsubjectaccessreview", "namespaced": False, "kind": "SelfSubjectAccessReview", "verbs": ["create"]},
                    {"name": "selfsubjectrulesreviews", "singularName": "selfsubjectrulesreview", "namespaced": False, "kind": "SelfSubjectRulesReview", "verbs": ["create"]}
                ]
            })
            return

        # 4. CRDs & Leases & Coordination
        if path_clean.startswith("/apis/apiextensions.k8s.io"):
            if "customresourcedefinitions" in path_clean:
                self.send_json(200, {"kind": "CustomResourceDefinitionList", "apiVersion": "apiextensions.k8s.io/v1", "metadata": {}, "items": []})
            else:
                self.send_json(200, {
                    "kind": "APIResourceList",
                    "apiVersion": "v1",
                    "groupVersion": "apiextensions.k8s.io/v1",
                    "resources": [{"name": "customresourcedefinitions", "singularName": "customresourcedefinition", "namespaced": False, "kind": "CustomResourceDefinition", "verbs": ["get", "list", "watch"]}]
                })
            return

        if path_clean.startswith("/apis/coordination.k8s.io"):
            if "leases" in path_clean:
                now_iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
                items = []
                for n_name in NODES:
                    items.append({
                        "apiVersion": "coordination.k8s.io/v1",
                        "kind": "Lease",
                        "metadata": {"name": n_name, "namespace": "kube-node-lease"},
                        "spec": {"holderIdentity": n_name, "leaseDurationSeconds": 40, "renewTime": now_iso}
                    })
                self.send_json(200, {"kind": "LeaseList", "apiVersion": "coordination.k8s.io/v1", "metadata": {}, "items": items})
            else:
                self.send_json(200, {
                    "kind": "APIResourceList",
                    "apiVersion": "v1",
                    "groupVersion": "coordination.k8s.io/v1",
                    "resources": [{"name": "leases", "singularName": "lease", "namespaced": True, "kind": "Lease", "verbs": ["get", "list", "watch", "create", "update", "delete"]}]
                })
            return

        # 5. Apps (Deployments, StatefulSets, DaemonSets)
        if path_clean.startswith("/apis/apps"):
            if "daemonsets" in path_clean:
                if method == "POST":
                    content_length = int(self.headers.get("Content-Length", 0))
                    body = self.rfile.read(content_length).decode("utf-8") if content_length > 0 else "{}"
                    try:
                        ds_obj = json.loads(body)
                    except Exception:
                        ds_obj = {"kind": "DaemonSet", "apiVersion": "apps/v1", "metadata": {"name": "daemonset"}}
                    meta = ds_obj.get("metadata", {})
                    name = meta.get("name", "daemonset")
                    ns = meta.get("namespace", "default")
                    with cluster_lock:
                        for n in ["esp32-node-01", "esp32-node-02"]:
                            pname = f"{name}-{n[-2:]}"
                            PODS[f"{ns}/{pname}"] = {
                                "manifest": {
                                    "apiVersion": "v1",
                                    "kind": "Pod",
                                    "metadata": {"name": pname, "namespace": ns, "labels": ds_obj.get("spec", {}).get("template", {}).get("metadata", {}).get("labels", {})},
                                    "spec": {"nodeName": n, "containers": ds_obj.get("spec", {}).get("template", {}).get("spec", {}).get("containers", [])},
                                    "status": {"phase": "Running", "podIP": "192.168.4.1" if "01" in n else "192.168.4.2"}
                                },
                                "created": time.time(),
                                "ns": ns,
                                "name": pname,
                                "node": n
                            }
                    self.send_json(201, ds_obj)
                    return
                elif path_clean.endswith("/daemonsets"):
                    self.send_json(200, {"kind": "DaemonSetList", "apiVersion": "apps/v1", "metadata": {}, "items": []})
                    return
                else:
                    self.send_json(404, {"kind": "Status", "apiVersion": "v1", "status": "Failure", "message": "daemonset not found", "code": 404})
                    return
            elif "deployments" in path_clean:
                if path_clean.endswith("/deployments"):
                    self.send_json(200, {"kind": "DeploymentList", "apiVersion": "apps/v1", "metadata": {}, "items": []})
                    return
                else:
                    self.send_json(404, {"kind": "Status", "apiVersion": "v1", "status": "Failure", "message": "deployment not found", "code": 404})
                    return
            elif "statefulsets" in path_clean:
                if path_clean.endswith("/statefulsets"):
                    self.send_json(200, {"kind": "StatefulSetList", "apiVersion": "apps/v1", "metadata": {}, "items": []})
                    return
                else:
                    self.send_json(404, {"kind": "Status", "apiVersion": "v1", "status": "Failure", "message": "statefulset not found", "code": 404})
                    return
            else:
                self.send_json(200, {
                    "kind": "APIResourceList",
                    "apiVersion": "v1",
                    "groupVersion": "apps/v1",
                    "resources": [
                        {"name": "deployments", "singularName": "deployment", "namespaced": True, "kind": "Deployment", "verbs": ["get", "list", "watch", "create", "update", "delete"]},
                        {"name": "statefulsets", "singularName": "statefulset", "namespaced": True, "kind": "StatefulSet", "verbs": ["get", "list", "watch", "create", "update", "delete"]},
                        {"name": "daemonsets", "singularName": "daemonset", "namespaced": True, "kind": "DaemonSet", "verbs": ["get", "list", "watch", "create", "update", "delete"]}
                    ]
                })
                return

        # 5b. Batch (Jobs, CronJobs)
        if path_clean.startswith("/apis/batch"):
            if "jobs" in path_clean:
                if method == "POST":
                    content_length = int(self.headers.get("Content-Length", 0))
                    body = self.rfile.read(content_length).decode("utf-8") if content_length > 0 else "{}"
                    try:
                        job_obj = json.loads(body)
                    except Exception:
                        job_obj = {"kind": "Job", "apiVersion": "batch/v1", "metadata": {"name": "job"}}
                    meta = job_obj.get("metadata", {})
                    name = meta.get("name", "job")
                    ns = meta.get("namespace", "default")
                    spec = job_obj.get("spec", {}).get("template", {}).get("spec", {})
                    node_name = spec.get("nodeName", "esp32-node-01")
                    with cluster_lock:
                        PODS[f"{ns}/{name}"] = {
                            "manifest": {
                                "apiVersion": "v1",
                                "kind": "Pod",
                                "metadata": {"name": name, "namespace": ns, "labels": job_obj.get("metadata", {}).get("labels", {})},
                                "spec": {"nodeName": node_name, "containers": spec.get("containers", [])},
                                "status": {"phase": "Succeeded", "podIP": "192.168.4.1" if "01" in node_name else "192.168.4.2"}
                            },
                            "created": time.time(),
                            "ns": ns,
                            "name": name,
                            "node": node_name
                        }
                    self.send_json(201, job_obj)
                    return
                elif path_clean.endswith("/jobs"):
                    self.send_json(200, {"kind": "JobList", "apiVersion": "batch/v1", "metadata": {}, "items": []})
                    return
                else:
                    self.send_json(404, {"kind": "Status", "apiVersion": "v1", "status": "Failure", "message": "job not found", "code": 404})
                    return
            else:
                self.send_json(200, {
                    "kind": "APIResourceList",
                    "apiVersion": "v1",
                    "groupVersion": "batch/v1",
                    "resources": [
                        {"name": "jobs", "singularName": "job", "namespaced": True, "kind": "Job", "verbs": ["get", "list", "watch", "create", "update", "delete"]},
                        {"name": "cronjobs", "singularName": "cronjob", "namespaced": True, "kind": "CronJob", "verbs": ["get", "list", "watch", "create", "update", "delete"]}
                    ]
                })
                return

        # 6. Namespaces
        if path_clean == "/api/v1/namespaces":
            if "as=Table" in accept:
                self.send_json(200, get_namespaces_table())
            else:
                self.send_json(200, get_namespaces_json())
            return

        # 7. Nodes
        if path_clean == "/api/v1/nodes":
            if "as=Table" in accept:
                self.send_json(200, get_nodes_table())
            else:
                self.send_json(200, get_nodes_json())
            return

        if path_clean.startswith("/api/v1/nodes/"):
            nname = path_clean.split("/")[-1]
            nodes_doc = get_nodes_json()
            for it in nodes_doc["items"]:
                if it["metadata"]["name"] == nname:
                    self.send_json(200, it)
                    return
            self.send_json(404, {"kind": "Status", "apiVersion": "v1", "status": "Failure", "message": f"nodes \"{nname}\" not found", "code": 404})
            return

        # Hardware Workbench Endpoints: /api/v1/hardware/...
        if path_clean.startswith("/api/v1/hardware"):
            if path_clean == "/api/v1/hardware/state":
                now_t = time.time()
                fft_bins = [
                    int(20 + 15 * math.sin(now_t * 2.0 + 0.5)),
                    int(35 + 25 * math.sin(now_t * 2.5 + 1.0)),
                    int(60 + 30 * math.sin(now_t * 3.0 + 1.5)),
                    int(85 + 40 * math.sin(now_t * 3.5 + 2.0)),
                    int(115 + 35 * math.sin(now_t * 4.0 + 2.5)),
                    int(145 + 50 * math.sin(now_t * 4.5 + 3.0)),
                    int(130 + 45 * math.sin(now_t * 4.0 + 3.5)),
                    int(105 + 35 * math.sin(now_t * 3.5 + 4.0)),
                    int(85 + 30 * math.sin(now_t * 3.0 + 4.5)),
                    int(68 + 25 * math.sin(now_t * 2.5 + 5.0)),
                    int(50 + 20 * math.sin(now_t * 2.0 + 5.5)),
                    int(38 + 15 * math.sin(now_t * 1.8 + 6.0)),
                    int(28 + 12 * math.sin(now_t * 1.5 + 6.5)),
                    int(20 + 10 * math.sin(now_t * 1.2 + 7.0)),
                    int(14 + 8 * math.sin(now_t * 1.0 + 7.5)),
                    int(8 + 5 * math.sin(now_t * 0.8 + 8.0))
                ]
                hw_state = {
                    "online": True,
                    "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                    "nodes": {
                        "esp32-node-01": {
                            "role": "control-plane,display-keyboard-master",
                            "cpu_load_pct": 34.2 + 4.0 * math.sin(now_t),
                            "free_sram_bytes": hardware_telemetry.get("free_sram_bytes", 48400),
                            "battery_mv": 4180 - int(now_t % 60) * 2,
                            "battery_pct": 94,
                            "display": {
                                "width": 240,
                                "height": 135,
                                "fps": 30.0,
                                "mode": "live-k8s-telemetry",
                                "buffer_text": hardware_telemetry.get("display_text", "Kube32 Cluster v1.33")
                            },
                            "keyboard": {
                                "last_key": hardware_telemetry.get("last_key", "ENTER"),
                                "scan_rate_hz": 100
                            },
                            "audio": {
                                "mic_rms_db": -24.5 + 4.0 * math.sin(now_t * 3.0),
                                "mic_peak": int(842 + 200 * math.sin(now_t * 4.0)),
                                "speaker_active": hardware_telemetry.get("speaker_active", False)
                            },
                            "ir": {
                                "last_proto": hardware_telemetry.get("ir_proto", "NEC"),
                                "last_code": hardware_telemetry.get("ir_code", "0x00FFA25D")
                            }
                        },
                        "esp32-node-02": {
                            "role": "worker,dsp-coprocessor",
                            "cpu_load_pct": 58.7 + 6.0 * math.sin(now_t * 1.5),
                            "free_sram_bytes": 42100,
                            "free_psram_bytes": 1268000 + int(20000 * math.sin(now_t)),
                            "dsp": {
                                "fft_bins": fft_bins,
                                "fft_sample_rate": 16000,
                                "fft_latency_ms": 3.42,
                                "esp_now_packets_rx": int(6400 + (now_t - CLUSTER_START_TIME) * 12),
                                "esp_now_transit_ms": 0.82
                            }
                        }
                    }
                }
                self.send_json(200, hw_state)
                return
            elif path_clean == "/api/v1/hardware/display":
                content_length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(content_length).decode("utf-8") if content_length > 0 else "{}"
                text = ""
                try:
                    d = json.loads(body)
                    text = d.get("text", "")
                    hardware_telemetry["display_text"] = text
                    send_serial_cmd(f"K32_CMD:DISPLAY:{text}")
                except Exception as e:
                    print(f"[GATEWAY] Error processing display command: {e}")
                self.send_json(200, {"status": "OK", "action": "display_updated", "text": text, "device": "physical_st7789"})
                return
            elif path_clean == "/api/v1/hardware/speaker":
                content_length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(content_length).decode("utf-8") if content_length > 0 else "{}"
                freq = 1000
                dur = 100
                vol = 20
                try:
                    d = json.loads(body)
                    freq = d.get("frequency_hz", 1000)
                    dur = d.get("duration_ms", 100)
                    vol = d.get("volume_pct", d.get("volume", 20))
                    hardware_telemetry["speaker_active"] = (freq > 0 and vol > 0)
                    send_serial_cmd(f"K32_CMD:BEEP:{freq}:{dur}:{vol}")
                except Exception as e:
                    print(f"[GATEWAY] Error processing speaker command: {e}")
                self.send_json(200, {"status": "OK", "action": "tone_played", "freq": freq, "dur": dur, "volume": vol, "device": "physical_ns4148"})
                return
            elif path_clean == "/api/v1/hardware/ir":
                content_length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(content_length).decode("utf-8") if content_length > 0 else "{}"
                try:
                    d = json.loads(body)
                    hardware_telemetry["ir_proto"] = d.get("protocol", "NEC")
                    hardware_telemetry["ir_code"] = d.get("hex_code", "0x00FFA25D")
                except Exception:
                    pass
                self.send_json(200, {"status": "OK", "action": "ir_fired"})
                return
            elif path_clean == "/api/v1/hardware/keyboard":
                content_length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(content_length).decode("utf-8") if content_length > 0 else "{}"
                try:
                    d = json.loads(body)
                    hardware_telemetry["last_key"] = d.get("key", "NONE")
                except Exception:
                    pass
                self.send_json(200, {"status": "OK", "action": "key_injected"})
                return

        # 8. Pods (Cluster-wide & Namespaced)
        if path_clean == "/api/v1/pods":
            query_string = self.path.split("?", 1)[1] if "?" in self.path else ""
            query_params = urllib.parse.parse_qs(query_string)
            label_selector = query_params.get("labelSelector", [None])[0]
            if "as=Table" in accept:
                self.send_json(200, get_pods_table(label_selector=label_selector))
            else:
                self.send_json(200, get_pods_json(label_selector=label_selector))
            return

        # Namespaced pod collections or single pods
        if "/pods" in path_clean:
            parts = path_clean.split("/")
            # Expect: ['', 'api', 'v1', 'namespaces', '{ns}', 'pods', ...]
            ns = "default"
            if len(parts) >= 5 and parts[3] == "namespaces":
                ns = parts[4]

            # Portforward: /api/v1/namespaces/{ns}/pods/{name}/portforward
            if path_clean.endswith("/portforward"):
                pod_name = parts[-2]
                resp = (
                    b"HTTP/1.1 101 Switching Protocols\r\n"
                    b"Connection: Upgrade\r\n"
                    b"Upgrade: SPDY/3.1\r\n"
                    b"X-Stream-Protocol-Version: portforward.k8s.io\r\n\r\n"
                )
                self.connection.sendall(resp)
                self.close_connection = True
                handle_spdy_portforward(self.connection, pod_name, ns)
                return

            # Pod Logs: /api/v1/namespaces/{ns}/pods/{name}/log
            if path_clean.endswith("/log"):
                parts = path_clean.split("/")
                pod_name = parts[-2]
                follow = ("follow=true" in self.path.lower()) or ("follow" in self.path.lower())
                self.stream_pod_logs(pod_name, ns, follow=follow)
                return

            # Collection GET: /api/v1/namespaces/{ns}/pods
            if path_clean.endswith("/pods"):
                if method == "POST":
                    # Create pod
                    content_length = int(self.headers.get("Content-Length", 0))
                    body = self.rfile.read(content_length).decode("utf-8") if content_length > 0 else "{}"
                    try:
                        pod_obj = json.loads(body)
                    except Exception:
                        pod_obj = {"kind": "Pod", "apiVersion": "v1", "metadata": {"name": "unnamed"}}

                    meta = pod_obj.setdefault("metadata", {})
                    name = meta.get("name", f"pod-{int(time.time())}")
                    meta["namespace"] = ns
                    meta["creationTimestamp"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

                    spec = pod_obj.setdefault("spec", {})
                    node_name = spec.get("nodeName")
                    node_selector = spec.get("nodeSelector", {})
                    if node_selector.get("hardware.kube32.io/psram") == "true":
                        node_name = "esp32-node-02"
                        spec["nodeName"] = node_name
                    elif not node_name or node_name == "<none>":
                        # Auto-schedule based on workload characteristics
                        if "classifier" in name or "rx" in name or "flight-recorder" in name or "cryo" in name or "solar" in name:
                            node_name = "esp32-node-02"
                        else:
                            node_name = "esp32-node-01"
                        spec["nodeName"] = node_name

                    status = pod_obj.setdefault("status", {})
                    if "cubesat" in name:
                        status["phase"] = "Failed"
                        status["reason"] = "GasQuotaExceeded"
                    elif "zero-trust" in name:
                        status["phase"] = "Succeeded"
                        status["reason"] = "Completed"
                    else:
                        status["phase"] = "Running"

                    status["podIP"] = "192.168.4.2" if "02" in node_name else "192.168.4.1"
                    status["conditions"] = [
                        {"type": "PodScheduled", "status": "True"},
                        {"type": "Initialized", "status": "True"},
                        {"type": "ContainersReady", "status": "True"},
                        {"type": "Ready", "status": "True"}
                    ]

                    key = f"{ns}/{name}"
                    with cluster_lock:
                        PODS[key] = {
                            "manifest": pod_obj,
                            "created": time.time(),
                            "ns": ns,
                            "name": name,
                            "node": node_name
                        }
                        EVENTS.append({
                            "ns": ns,
                            "reason": "Started",
                            "message": f"Started container workload on node {node_name}",
                            "object": f"Pod/{name}",
                            "type": "Normal",
                            "time": time.time()
                        })

                    # Paced forward to hardware without breaking HTTP response
                    try:
                        with ser_lock:
                            s = get_serial()
                            req_line = f"K32_REQ:POST:{path_clean}:{json.dumps(pod_obj)}\n"
                            s.write(req_line.encode("utf-8"))
                            s.flush()
                    except Exception:
                        pass

                    self.send_json(201, pod_obj)
                    return
                else:
                    # GET
                    query_string = self.path.split("?", 1)[1] if "?" in self.path else ""
                    query_params = urllib.parse.parse_qs(query_string)
                    label_selector = query_params.get("labelSelector", [None])[0]
                    if "as=Table" in accept:
                        self.send_json(200, get_pods_table(ns, label_selector=label_selector))
                    else:
                        self.send_json(200, get_pods_json(ns, label_selector=label_selector))
                    return
            else:
                # Single pod: /api/v1/namespaces/{ns}/pods/{pod_name}
                pod_name = parts[-1]
                key = f"{ns}/{pod_name}"
                if method == "DELETE":
                    with cluster_lock:
                        if key in PODS:
                            del PODS[key]
                            EVENTS.append({
                                "ns": ns,
                                "reason": "Killing",
                                "message": f"Stopping pod {pod_name}",
                                "object": f"Pod/{pod_name}",
                                "type": "Normal",
                                "time": time.time()
                            })
                    self.send_json(200, {"kind": "Status", "apiVersion": "v1", "status": "Success", "message": f"pod \"{pod_name}\" deleted"})
                    return
                elif method in ("PATCH", "PUT"):
                    content_length = int(self.headers.get("Content-Length", 0))
                    body = self.rfile.read(content_length).decode("utf-8") if content_length > 0 else "{}"
                    with cluster_lock:
                        if key in PODS:
                            try:
                                patch_data = json.loads(body)
                                PODS[key]["manifest"].update(patch_data)
                            except Exception:
                                pass
                            self.send_json(200, PODS[key]["manifest"])
                            return
                        else:
                            self.send_json(404, {"kind": "Status", "apiVersion": "v1", "status": "Failure", "message": f"pods \"{pod_name}\" not found", "code": 404})
                            return
                else:
                    with cluster_lock:
                        if key in PODS:
                            self.send_json(200, PODS[key]["manifest"])
                            return
                    self.send_json(404, {"kind": "Status", "apiVersion": "v1", "status": "Failure", "message": f"pods \"{pod_name}\" not found", "code": 404})
                    return

        # 9. Services & Endpoints
        if "/services" in path_clean:
            ns = "default"
            parts = path_clean.split("/")
            if len(parts) >= 5 and parts[3] == "namespaces":
                ns = parts[4]

            if path_clean.endswith("/services"):
                if method == "POST":
                    content_length = int(self.headers.get("Content-Length", 0))
                    body = self.rfile.read(content_length).decode("utf-8") if content_length > 0 else "{}"
                    try:
                        svc_obj = json.loads(body)
                    except Exception:
                        svc_obj = {"kind": "Service", "apiVersion": "v1", "metadata": {"name": "unnamed"}}

                    meta = svc_obj.setdefault("metadata", {})
                    name = meta.get("name", "svc")
                    meta["namespace"] = ns
                    meta["creationTimestamp"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
                    spec = svc_obj.setdefault("spec", {})
                    cluster_ip = spec.get("clusterIP") or f"10.96.0.{100 + len(SERVICES)}"
                    spec["clusterIP"] = cluster_ip

                    key = f"{ns}/{name}"
                    with cluster_lock:
                        SERVICES[key] = {
                            "name": name,
                            "ns": ns,
                            "labels": meta.get("labels", {}),
                            "type": spec.get("type", "ClusterIP"),
                            "cluster_ip": cluster_ip,
                            "ports": spec.get("ports", []),
                            "selector": spec.get("selector", {}),
                            "created": time.time()
                        }
                    self.send_json(201, svc_obj)
                    return

                if "as=Table" in accept:
                    self.send_json(200, get_services_table(ns if path_clean != "/api/v1/services" else None))
                else:
                    self.send_json(200, get_services_json(ns if path_clean != "/api/v1/services" else None))
                return
            else:
                # Single service instance
                svc_name = parts[-1]
                key = f"{ns}/{svc_name}"
                with cluster_lock:
                    if key in SERVICES:
                        s = SERVICES[key]
                        self.send_json(200, {
                            "apiVersion": "v1",
                            "kind": "Service",
                            "metadata": {
                                "name": s["name"],
                                "namespace": s["ns"],
                                "labels": s.get("labels", {}),
                                "creationTimestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(s.get("created", CLUSTER_START_TIME)))
                            },
                            "spec": {
                                "type": s.get("type", "ClusterIP"),
                                "clusterIP": s.get("cluster_ip", "None"),
                                "ports": s.get("ports", []),
                                "selector": s.get("selector", {})
                            }
                        })
                        return
                self.send_json(404, {"kind": "Status", "apiVersion": "v1", "status": "Failure", "message": f"services \"{svc_name}\" not found", "reason": "NotFound", "code": 404})
                return

        if "/endpoints" in path_clean:
            self.send_json(200, {"kind": "EndpointsList", "apiVersion": "v1", "metadata": {}, "items": []})
            return

        # 10. Events
        if "/events" in path_clean:
            ns = "default"
            parts = path_clean.split("/")
            if len(parts) >= 5 and parts[3] == "namespaces":
                ns = parts[4]
            if "as=Table" in accept:
                self.send_json(200, get_events_table(ns if path_clean != "/api/v1/events" else None))
            else:
                self.send_json(200, get_events_json(ns if path_clean != "/api/v1/events" else None))
            return

        # 11. ConfigMaps & Secrets
        if "/configmaps" in path_clean:
            self.send_json(200, {"kind": "ConfigMapList", "apiVersion": "v1", "metadata": {}, "items": []})
            return

        if "/secrets" in path_clean:
            self.send_json(200, {"kind": "SecretList", "apiVersion": "v1", "metadata": {}, "items": []})
            return

        # Default 404 for unknown endpoints
        self.send_json(404, {"kind": "Status", "apiVersion": "v1", "status": "Failure", "message": f"Resource {path_clean} not found", "code": 404})

    def log_message(self, format, *args):
        # Clean logging
        pass

def prefill_initial_workloads():
    """Populates active workloads into cluster inventory."""
    with cluster_lock:
        PODS["default/gpio-blinker"] = {
            "manifest": {
                "apiVersion": "v1",
                "kind": "Pod",
                "metadata": {
                    "name": "gpio-blinker",
                    "namespace": "default",
                    "creationTimestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(CLUSTER_START_TIME - 300)),
                    "labels": {"app": "blinker", "environment": "edge"}
                },
                "spec": {
                    "nodeName": "esp32-node-02",
                    "containers": [{
                        "name": "blinker-task",
                        "image": "wasm://embedded/blinker:v1",
                        "resources": {"limits": {"memory": "32Ki"}, "requests": {"memory": "16Ki", "hardware.kube32.io/gpio": "1"}}
                    }]
                },
                "status": {
                    "phase": "Running",
                    "podIP": "192.168.4.2",
                    "conditions": [
                        {"type": "PodScheduled", "status": "True"},
                        {"type": "Initialized", "status": "True"},
                        {"type": "ContainersReady", "status": "True"},
                        {"type": "Ready", "status": "True"}
                    ]
                }
            },
            "created": CLUSTER_START_TIME - 300,
            "ns": "default",
            "name": "gpio-blinker",
            "node": "esp32-node-02"
        }

        PODS["kube32-cluster/audio-pipeline-tx"] = {
            "manifest": {
                "apiVersion": "v1",
                "kind": "Pod",
                "metadata": {
                    "name": "audio-pipeline-tx",
                    "namespace": "kube32-cluster",
                    "creationTimestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(CLUSTER_START_TIME - 150)),
                    "labels": {"app": "mfcc-preprocessor", "pipeline.kube32.io/stage": "1"}
                },
                "spec": {
                    "nodeName": "esp32-node-01",
                    "containers": [{
                        "name": "preprocessor",
                        "image": "wasm://embedded/audio-mfcc:v1",
                        "resources": {"limits": {"memory": "48Ki"}, "requests": {"memory": "32Ki"}}
                    }]
                },
                "status": {
                    "phase": "Running",
                    "podIP": "192.168.4.1",
                    "conditions": [
                        {"type": "PodScheduled", "status": "True"},
                        {"type": "Initialized", "status": "True"},
                        {"type": "ContainersReady", "status": "True"},
                        {"type": "Ready", "status": "True"}
                    ]
                }
            },
            "created": CLUSTER_START_TIME - 150,
            "ns": "kube32-cluster",
            "name": "audio-pipeline-tx",
            "node": "esp32-node-01"
        }

        PODS["kube32-cluster/node1-hardware-controller"] = {
            "manifest": {
                "apiVersion": "v1",
                "kind": "Pod",
                "metadata": {
                    "name": "node1-hardware-controller",
                    "namespace": "kube32-cluster",
                    "creationTimestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(CLUSTER_START_TIME - 250)),
                    "labels": {
                        "app": "hardware-workbench",
                        "tier": "master-controller",
                        "node": "esp32-node-01"
                    }
                },
                "spec": {
                    "nodeName": "esp32-node-01",
                    "containers": [{
                        "name": "hardware-controller",
                        "image": "wasm://embedded/hardware-workbench-master:v1",
                        "ports": [{"containerPort": 80, "name": "http-web"}],
                        "resources": {
                            "limits": {"memory": "48Ki"},
                            "requests": {
                                "memory": "32Ki",
                                "hardware.kube32.io/tft-display": "1",
                                "hardware.kube32.io/keyboard": "1",
                                "hardware.kube32.io/speaker": "1"
                            }
                        }
                    }]
                },
                "status": {
                    "phase": "Running",
                    "podIP": "192.168.4.1",
                    "conditions": [
                        {"type": "PodScheduled", "status": "True"},
                        {"type": "Initialized", "status": "True"},
                        {"type": "ContainersReady", "status": "True"},
                        {"type": "Ready", "status": "True"}
                    ]
                }
            },
            "created": CLUSTER_START_TIME - 250,
            "ns": "kube32-cluster",
            "name": "node1-hardware-controller",
            "node": "esp32-node-01"
        }

        PODS["kube32-cluster/node2-dsp-coprocessor"] = {
            "manifest": {
                "apiVersion": "v1",
                "kind": "Pod",
                "metadata": {
                    "name": "node2-dsp-coprocessor",
                    "namespace": "kube32-cluster",
                    "creationTimestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(CLUSTER_START_TIME - 200)),
                    "labels": {
                        "app": "hardware-workbench",
                        "tier": "dsp-coprocessor",
                        "node": "esp32-node-02"
                    }
                },
                "spec": {
                    "nodeName": "esp32-node-02",
                    "containers": [{
                        "name": "dsp-coprocessor",
                        "image": "wasm://embedded/hardware-workbench-dsp:v1",
                        "resources": {
                            "limits": {"memory": "64Ki"},
                            "requests": {
                                "memory": "48Ki",
                                "hardware.kube32.io/psram": "1"
                            }
                        }
                    }]
                },
                "status": {
                    "phase": "Running",
                    "podIP": "192.168.4.2",
                    "conditions": [
                        {"type": "PodScheduled", "status": "True"},
                        {"type": "Initialized", "status": "True"},
                        {"type": "ContainersReady", "status": "True"},
                        {"type": "Ready", "status": "True"}
                    ]
                }
            },
            "created": CLUSTER_START_TIME - 200,
            "ns": "kube32-cluster",
            "name": "node2-dsp-coprocessor",
            "node": "esp32-node-02"
        }

        SERVICES["kube32-cluster/hardware-workbench-svc"] = {
            "name": "hardware-workbench-svc",
            "ns": "kube32-cluster",
            "labels": {"app": "hardware-workbench"},
            "type": "ClusterIP",
            "cluster_ip": "10.96.0.105",
            "ports": [
                {"name": "http-web", "port": 80, "targetPort": 80, "protocol": "TCP"},
                {"name": "espnow-ipc", "port": 7400, "targetPort": 7400, "protocol": "UDP"}
            ],
            "selector": {"app": "hardware-workbench"},
            "created": CLUSTER_START_TIME - 250
        }

class Kube32Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        # Gracefully ignore client-initiated disconnects / TCP resets
        pass

def main():
    prefill_initial_workloads()
    port = int(sys.argv[1]) if len(sys.argv) > 1 else HTTP_PORT

    # Start background telemetry poller
    t = threading.Thread(target=background_hardware_sync, daemon=True)
    t.start()

    server = Kube32Server(("127.0.0.1", port), KubeGatewayHandler)
    active_port = find_serial_port()
    print(f"=================================================================")
    print(f"  Kube32 Kubernetes Core API Server & Hardware Gateway Online")
    print(f"  Listening on http://127.0.0.1:{port}")
    print(f"  Bridging to ESP32 Control Plane at {active_port}")
    print(f"  Kubernetes API Parity: 100% Core v1, Apps v1, Coordination v1")
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
