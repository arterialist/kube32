#!/usr/bin/env python3
"""
Kube32 Milestone 2 Adversarial Security & Empirical Stress Testing Suite
Author: m2_challenger_1 (Empirical Challenger)

Attack Vectors:
1. Malformed JSON & Inbound Validation (Recursion depth limit, unquoted keys, trailing commas, null bytes)
2. Query Parameter & Header Fuzzing (Extreme lengths, conflicting Accept headers, malformed HTTP syntax)
3. Socket Streaming & Early Disconnects (EPIPE, ECONNRESET, SIGPIPE handling, TCP RST mid-transmission)
4. Denial of Service & Resource Exhaustion (Slowloris unhandled timeout, negative Content-Length wrap)
"""

import sys
import os
import socket
import time
import json
import struct
import signal
import subprocess

SERVER_HOST = "127.0.0.1"
SERVER_PORT = 6443
BASE_URL = f"http://{SERVER_HOST}:{SERVER_PORT}"
BINARY_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "kube32-sim")

sim_proc = None

def spawn_server():
    global sim_proc
    if is_server_alive():
        return
    sim_proc = subprocess.Popen(
        [BINARY_PATH],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        preexec_fn=os.setsid
    )
    for _ in range(40):
        time.sleep(0.05)
        if is_server_alive():
            return
        if sim_proc.poll() is not None:
            stdout, stderr = sim_proc.communicate()
            print(f"[FATAL] Server exited immediately with code {sim_proc.returncode}!")
            print(f"Stdout:\n{stdout.decode('utf-8', 'replace')}")
            print(f"Stderr:\n{stderr.decode('utf-8', 'replace')}")
            sys.exit(1)
    print("[FATAL] Server did not become ready.")
    sys.exit(1)

def kill_server():
    global sim_proc
    if sim_proc and sim_proc.poll() is None:
        try:
            os.killpg(os.getpgid(sim_proc.pid), signal.SIGTERM)
            sim_proc.wait(timeout=1.0)
        except Exception:
            try:
                os.killpg(os.getpgid(sim_proc.pid), signal.SIGKILL)
            except Exception:
                pass
    sim_proc = None

def is_server_alive():
    if sim_proc and sim_proc.poll() is not None:
        return False
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(0.5)
        s.connect((SERVER_HOST, SERVER_PORT))
        s.sendall(b"GET /version HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nConnection: close\r\n\r\n")
        resp = s.recv(1024)
        s.close()
        return b"200 OK" in resp
    except Exception:
        return False

def raw_http_request(request_bytes, read_response=True, timeout=2.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect((SERVER_HOST, SERVER_PORT))
        s.sendall(request_bytes)
        if read_response:
            data = b""
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
            return data
        return b""
    finally:
        s.close()

results = []

def record(category, test_name, status, detail=""):
    results.append({
        "category": category,
        "name": test_name,
        "status": status,
        "detail": detail
    })
    prefix = "[PASS]" if status == "PASS" else "[FAIL]"
    print(f"  {prefix} {test_name}" + (f": {detail}" if detail and status == "FAIL" else ""))

def main():
    print("==================================================================")
    print("   Kube32 Milestone 2 Adversarial Stress & Vulnerability Suite    ")
    print("==================================================================")

    # --------------------------------------------------------------------------
    # VECTOR 1: Malformed JSON & Inbound Validation
    # --------------------------------------------------------------------------
    print("\n--- [Vector 1: Malformed JSON & Inbound Validation] ---")
    spawn_server()

    # 1.1 JSON Nesting Depth Limits
    for depth in [10, 50, 75, 100, 300]:
        payload = '{"a":' * depth + '1' + '}' * depth
        req = f"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nContent-Type: application/json\r\nContent-Length: {len(payload)}\r\nConnection: close\r\n\r\n{payload}".encode("utf-8")
        try:
            resp = raw_http_request(req, timeout=1.0)
        except Exception:
            pass
        time.sleep(0.1)
        if not is_server_alive():
            poll = sim_proc.poll() if sim_proc else "died"
            sig_name = f"Signal {-poll}" if isinstance(poll, int) and poll < 0 else str(poll)
            record("Vector 1", f"JSON Nesting Depth {depth}", "FAIL", f"Server CRASHED with {sig_name} (Stack Overflow / Guard Page Violation)")
            kill_server()
            spawn_server()
        else:
            record("Vector 1", f"JSON Nesting Depth {depth}", "PASS", "Rejected or handled cleanly without crash")

    # 1.2 Unquoted keys
    unquoted = "{metadata: {name: test}, spec: {}}"
    req = f"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nContent-Type: application/json\r\nContent-Length: {len(unquoted)}\r\nConnection: close\r\n\r\n{unquoted}".encode("utf-8")
    resp = raw_http_request(req)
    record("Vector 1", "Unquoted keys in POST body", "PASS" if b"400 Bad Request" in resp else "FAIL", f"Got: {resp[:30]}")

    # 1.3 Missing closing bracket
    unclosed = '{"kind": "Pod", "metadata": {"name": "test"'
    req = f"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nContent-Type: application/json\r\nContent-Length: {len(unclosed)}\r\nConnection: close\r\n\r\n{unclosed}".encode("utf-8")
    resp = raw_http_request(req)
    record("Vector 1", "Unclosed JSON brace", "PASS" if b"400 Bad Request" in resp else "FAIL", f"Got: {resp[:30]}")

    # 1.4 Trailing commas
    trailing = '{"kind": "Pod", "metadata": {"name": "test",},}'
    req = f"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nContent-Type: application/json\r\nContent-Length: {len(trailing)}\r\nConnection: close\r\n\r\n{trailing}".encode("utf-8")
    resp = raw_http_request(req)
    record("Vector 1", "Trailing commas in object", "PASS" if b"400 Bad Request" in resp else "FAIL", f"Got: {resp[:30]}")

    # 1.5 Binary garbage payload
    garbage = b"\x00\xFF\xFE\xFD\xAA\xBB\xCC\xDD\xEE\x12\x34\x56\x78\x9A\xBC\xDE"
    req = f"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nContent-Type: application/json\r\nContent-Length: {len(garbage)}\r\nConnection: close\r\n\r\n".encode("utf-8") + garbage
    resp = raw_http_request(req)
    record("Vector 1", "Binary garbage payload", "PASS" if b"400 Bad Request" in resp else "FAIL", f"Got: {resp[:30]}")

    # 1.6 Empty payload
    req = b"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nContent-Type: application/json\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
    resp = raw_http_request(req)
    record("Vector 1", "Empty body with Content-Length: 0", "PASS" if b"400 Bad Request" in resp else "FAIL", f"Got: {resp[:30]}")

    # 1.7 Null byte injection
    null_byte = '{"kind": "Pod", "metadata": {"name": "null\\u0000byte"}}'
    req = f"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nContent-Type: application/json\r\nContent-Length: {len(null_byte)}\r\nConnection: close\r\n\r\n{null_byte}".encode("utf-8")
    resp = raw_http_request(req)
    record("Vector 1", "Escaped null byte in JSON string", "PASS" if is_server_alive() else "FAIL", "Server crashed on null byte")

    # --------------------------------------------------------------------------
    # VECTOR 2: Query Parameter & Header Fuzzing
    # --------------------------------------------------------------------------
    print("\n--- [Vector 2: Query Parameter & Header Fuzzing] ---")
    spawn_server()

    for qlen in [1000, 8000, 30000]:
        qstr = "timeout=999999999s&" + ("x=" + "a" * 100 + "&") * (qlen // 100)
        req = f"GET /api/v1/nodes?{qstr} HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nAccept: application/json\r\nConnection: close\r\n\r\n".encode("utf-8")
        try:
            resp = raw_http_request(req, timeout=1.0)
        except Exception:
            pass
        alive = is_server_alive()
        record("Vector 2", f"Query parameter length ~{qlen} bytes", "PASS" if alive else "FAIL", "Server died on long query string")
        if not alive:
            kill_server()
            spawn_server()

    # Conflicting Accept headers
    for acc in ["application/json;as=Table;v=v1, application/json", "as=Table", "invalid/mime;;"]:
        req = f"GET /api/v1/nodes HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nAccept: {acc}\r\nConnection: close\r\n\r\n".encode("utf-8")
        resp = raw_http_request(req)
        record("Vector 2", f"Accept header '{acc}'", "PASS" if (b"200 OK" in resp and is_server_alive()) else "FAIL")

    # --------------------------------------------------------------------------
    # VECTOR 3: Socket Streaming & Early Disconnects (EPIPE/ECONNRESET/SIGPIPE)
    # --------------------------------------------------------------------------
    print("\n--- [Vector 3: Socket Streaming & Early Disconnects] ---")
    spawn_server()

    # 3.1 Immediate TCP RST after sending request
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
    s.connect((SERVER_HOST, SERVER_PORT))
    s.sendall(b"GET /version HTTP/1.1\r\nHost: 127.0.0.1:6443\r\n\r\n")
    s.close()
    time.sleep(0.2)
    alive = is_server_alive()
    if not alive:
        poll = sim_proc.poll() if sim_proc else "died"
        record("Vector 3", "Immediate TCP RST after request", "FAIL", f"Server CRASHED with Exit Code {poll} (Unhandled SIGPIPE / Broken Pipe)")
        kill_server()
        spawn_server()
    else:
        record("Vector 3", "Immediate TCP RST after request", "PASS", "Server survived TCP RST")

    # 3.2 Mid-stream RST while streaming table data
    # First seed 50 pods
    for i in range(50):
        body = json.dumps({"apiVersion": "v1", "kind": "Pod", "metadata": {"name": f"pod-{i:03d}", "namespace": "default"}})
        req = f"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n{body}".encode("utf-8")
        raw_http_request(req)

    # Now request table and RST mid-chunk
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
    s.connect((SERVER_HOST, SERVER_PORT))
    s.sendall(b"GET /api/v1/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nAccept: application/json;as=Table\r\nConnection: close\r\n\r\n")
    chunk = s.recv(32)
    s.close()
    time.sleep(0.2)
    alive = is_server_alive()
    if not alive:
        poll = sim_proc.poll() if sim_proc else "died"
        record("Vector 3", "TCP RST mid-read during Table streaming", "FAIL", f"Server CRASHED with Exit Code {poll} (Unhandled SIGPIPE)")
        kill_server()
        spawn_server()
    else:
        record("Vector 3", "TCP RST mid-read during Table streaming", "PASS", "Server survived mid-stream RST")

    # --------------------------------------------------------------------------
    # VECTOR 4: Denial of Service & Resource Robustness
    # --------------------------------------------------------------------------
    print("\n--- [Vector 4: Denial of Service & Resource Robustness] ---")
    spawn_server()

    # 4.1 Slowloris / Missing read timeout on client socket
    # Client 1 connects and claims Content-Length 1000 but doesn't send it
    s1 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s1.connect((SERVER_HOST, SERVER_PORT))
    s1.sendall(b"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nContent-Length: 1000\r\n\r\n")
    time.sleep(0.2)

    # Client 2 attempts to connect
    s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s2.settimeout(1.5)
    c2_success = False
    try:
        s2.connect((SERVER_HOST, SERVER_PORT))
        s2.sendall(b"GET /version HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nConnection: close\r\n\r\n")
        resp = s2.recv(1024)
        c2_success = (b"200 OK" in resp)
    except socket.timeout:
        c2_success = False
    finally:
        s2.close()
        s1.close()

    record("Vector 4", "Single-threaded Blocking (Slowloris / Missing Recv Timeout)", "PASS" if c2_success else "FAIL",
           "Server blocked all concurrent traffic while one client held partial body without timeout")

    # 4.2 Negative Content-Length (-1 integer underflow / wrap)
    spawn_server()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(1.5)
    s.connect((SERVER_HOST, SERVER_PORT))
    s.sendall(b"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nContent-Length: -1\r\n\r\n{}")
    c_wrap_hung = False
    try:
        resp = s.recv(1024)
    except socket.timeout:
        c_wrap_hung = True
    finally:
        s.close()

    record("Vector 4", "Negative Content-Length (-1 underflow to UINT64_MAX)", "PASS" if not c_wrap_hung else "FAIL",
           "Server hung indefinitely attempting to read 18446744073709551615 bytes")

    kill_server()

    # --------------------------------------------------------------------------
    # SUMMARY
    # --------------------------------------------------------------------------
    total = len(results)
    passed = sum(1 for r in results if r["status"] == "PASS")
    failed = sum(1 for r in results if r["status"] == "FAIL")

    print("\n==================================================================")
    print(f"   ADVERSARIAL STRESS TEST SUMMARY: {passed}/{total} Passed, {failed} Failed")
    print("==================================================================")
    for r in results:
        status_symbol = "✓" if r["status"] == "PASS" else "✗"
        print(f" [{status_symbol}] {r['category']:<10} | {r['name']:<50} | {r['status']}")
        if r['detail'] and r['status'] == "FAIL":
            print(f"     -> {r['detail']}")

    return 0 if failed == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
