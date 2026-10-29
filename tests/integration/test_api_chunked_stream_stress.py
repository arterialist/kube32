#!/usr/bin/env python3
"""
Live stress test: HTTP chunked stream malformations and socket resilience.
Target: ./build/kube32-sim
"""

import socket
import struct
import subprocess
import time
import sys
import os
import signal
import threading
import urllib.request
import json

HOST = "127.0.0.1"
PORT = 6443
BASE_URL = f"http://{HOST}:{PORT}"

def is_server_ready():
    try:
        req = urllib.request.Request(f"{BASE_URL}/version")
        with urllib.request.urlopen(req, timeout=1) as resp:
            return resp.getcode() == 200
    except Exception:
        return False

def open_socket():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2.0)
    s.connect((HOST, PORT))
    return s

def test_truncated_chunk_requests():
    print("[TEST] Sending truncated chunked HTTP requests...")
    vectors = [
        b"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nTransfer-Encoding: chunked\r\n\r\n10\r\npartial",
        b"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nTransfer-Encoding: chunked\r\n\r\nDEADBEEF\r\n",
        b"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nTransfer-Encoding: chunked\r\n\r\n",
        b"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nTransfer-Encoding: chunked\r\n\r\n-1\r\n",
        b"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nTransfer-Encoding: chunked\r\n\r\n0x20\r\n",
        b"POST /api/v1/namespaces/default/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nTransfer-Encoding: chunked\r\n\r\n;;;;;\r\n",
    ]

    for i, vec in enumerate(vectors):
        s = open_socket()
        try:
            s.sendall(vec)
            # Give server a moment to respond or close
            time.sleep(0.05)
            s.close()
        except Exception:
            pass

    print("  [PASS] Server survived all truncated chunk request vectors.")

def test_chunked_table_stream_abrupt_disconnects():
    print("[TEST] Abrupt disconnects during Table chunked response streaming...")
    # Table streaming triggers when Accept: application/json;as=Table
    req_headers = (
        b"GET /api/v1/pods HTTP/1.1\r\n"
        b"Host: 127.0.0.1:6443\r\n"
        b"Accept: application/json;as=Table\r\n"
        b"Connection: close\r\n\r\n"
    )

    # Vector 1: Close immediately after sending request
    for _ in range(10):
        s = open_socket()
        s.sendall(req_headers)
        s.close()

    # Vector 2: Read only 10 bytes then close
    for _ in range(10):
        s = open_socket()
        s.sendall(req_headers)
        try:
            _ = s.recv(10)
        except Exception:
            pass
        s.close()

    # Vector 3: Immediate TCP RST via SO_LINGER(1, 0)
    for _ in range(10):
        s = open_socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        s.sendall(req_headers)
        s.close()

    print("  [PASS] Server survived 30 abrupt disconnects during chunked streaming.")

def test_concurrent_malformed_chunk_flood():
    print("[TEST] Concurrent multi-threaded malformed chunk assault...")
    errors = []

    def worker(worker_id):
        for _ in range(10):
            try:
                s = open_socket()
                s.sendall(
                    f"POST /api/v1/namespaces/default/pods HTTP/1.1\r\n"
                    f"Host: 127.0.0.1:6443\r\n"
                    f"Transfer-Encoding: chunked\r\n\r\n"
                    f"{worker_id:x}\r\n"
                    f"junk".encode("utf-8")
                )
                time.sleep(0.01)
                s.close()
            except Exception as e:
                # Socket errors on client side are expected if server closes
                pass

    threads = []
    for wid in range(8):
        t = threading.Thread(target=worker, args=(wid,))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    print("  [PASS] 8 threads x 10 malformed connections completed.")

def main():
    print("==========================================================")
    print(" Live HTTP Chunked Stream & Malformation Stress Test      ")
    print("==========================================================")

    sim_binary = "./build/kube32-sim"
    if not os.path.exists(sim_binary):
        print(f"[ERROR] Simulation binary {sim_binary} not found!")
        sys.exit(1)

    print(f"[INIT] Spawning {sim_binary}...")
    proc = subprocess.Popen([sim_binary], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    sim_pid = proc.pid

    try:
        # Wait for ready
        ready = False
        for _ in range(30):
            if is_server_ready():
                ready = True
                break
            time.sleep(0.1)

        assert ready, "Server failed to start"
        print(f"[READY] kube32-sim running on PID {sim_pid}")

        test_truncated_chunk_requests()
        test_chunked_table_stream_abrupt_disconnects()
        test_concurrent_malformed_chunk_flood()

        # Check process liveness
        poll_res = proc.poll()
        assert poll_res is None, f"kube32-sim died during stress test! Exit code: {poll_res}"

        # Check API health post-stress
        time.sleep(0.2)
        assert is_server_ready(), "Server not responsive post-stress!"

        # Query /api/v1/nodes and /api/v1/pods
        req_nodes = urllib.request.Request(f"{BASE_URL}/api/v1/nodes")
        with urllib.request.urlopen(req_nodes, timeout=2) as resp:
            assert resp.getcode() == 200
            data = json.loads(resp.read().decode("utf-8"))
            assert data.get("kind") == "NodeList"

        req_pods = urllib.request.Request(f"{BASE_URL}/api/v1/pods")
        with urllib.request.urlopen(req_pods, timeout=2) as resp:
            assert resp.getcode() == 200
            data = json.loads(resp.read().decode("utf-8"))
            assert data.get("kind") == "PodList"

        print("[HEALTH] Post-stress API sanity check passed cleanly.")

    finally:
        print("[TEARDOWN] Terminating kube32-sim...")
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        print("[TEARDOWN] Stopped.")

    print("==========================================================")
    print(" ALL LIVE CHUNKED STRESS SCENARIOS: 100% PASSED!         ")
    print("==========================================================")

if __name__ == "__main__":
    main()
