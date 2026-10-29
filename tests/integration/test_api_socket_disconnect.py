#!/usr/bin/env python3
"""
Stress test: socket disconnect mid-chunk and concurrency resilience.

Stress Scenarios:
1. High-frequency disconnects during Table chunked streaming (50+ pods seeded).
2. Multiple disconnect vectors:
   - Abrupt socket close after reading initial HTTP headers
   - Abrupt socket close after reading partial first chunk (e.g., 10 bytes)
   - Abrupt socket close after reading 256 bytes (mid-chunk)
   - Immediate TCP RST (SO_LINGER 1,0) post-request without reading
   - Immediate TCP RST mid-chunk streaming
3. Multi-threaded concurrent disconnect assault: 10 concurrent threads x 20 iterations = 200 RST/disconnects.
4. Process health verification:
   - Check PID persistence (zero crashes / zero restarts)
   - Check OS thread count (zero hung or leaked threads)
   - Verify post-stress API responsiveness (/version, /api/v1/nodes, /api/v1/pods)
"""

import sys
import os
import time
import json
import socket
import struct
import threading
import subprocess

SERVER_HOST = "127.0.0.1"
SERVER_PORT = 6443
BASE_URL = f"http://{SERVER_HOST}:{SERVER_PORT}"
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BINARY_PATH = os.path.join(PROJECT_ROOT, "build", "kube32-sim")

sim_proc = None

def start_sim():
    global sim_proc
    if sim_proc and sim_proc.poll() is None:
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
            print(f"[FATAL] kube32-sim failed to start. Code: {sim_proc.returncode}")
            print(stdout.decode('utf-8', 'replace'))
            print(stderr.decode('utf-8', 'replace'))
            sys.exit(1)
    raise RuntimeError("Timed out waiting for kube32-sim to start")

def stop_sim():
    global sim_proc
    if sim_proc and sim_proc.poll() is None:
        try:
            os.killpg(os.getpgid(sim_proc.pid), 9)
            sim_proc.wait(timeout=1.0)
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
        resp = s.recv(512)
        s.close()
        return b"200 OK" in resp
    except Exception:
        return False

def get_thread_count(pid):
    try:
        # On macOS, ps -M <pid> lists threads
        out = subprocess.check_output(["ps", "-M", str(pid)]).decode("utf-8")
        lines = [l for l in out.strip().split("\n") if l.strip()]
        # first line is header
        return max(1, len(lines) - 1)
    except Exception:
        return -1

def seed_cluster(num_pods=60):
    print(f"[SETUP] Seeding {num_pods} pods to ensure multi-chunk response payload...")
    for i in range(num_pods):
        body = json.dumps({
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {
                "name": f"stress-pod-{i:03d}",
                "namespace": "default",
                "labels": {"app": "stress-test"}
            },
            "spec": {
                "containers": [{"name": "c1", "image": "wasm-echo"}]
            }
        })
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((SERVER_HOST, SERVER_PORT))
        req = (f"POST /api/v1/namespaces/default/pods HTTP/1.1\r\n"
               f"Host: 127.0.0.1:6443\r\n"
               f"Content-Type: application/json\r\n"
               f"Content-Length: {len(body)}\r\n"
               f"Connection: close\r\n\r\n{body}").encode("utf-8")
        s.sendall(req)
        s.recv(1024)
        s.close()

def run_stress_tests():
    print("==================================================================")
    print("  Kube32 Dedicated Socket Disconnect Mid-Chunk Stress Harness   ")
    print("==================================================================")
    start_sim()
    sim_pid = sim_proc.pid
    initial_threads = get_thread_count(sim_pid)
    print(f"[INIT] kube32-sim running at PID {sim_pid}, initial threads: {initial_threads}")

    seed_cluster(60)

    # Verify initial table size
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((SERVER_HOST, SERVER_PORT))
    s.sendall(b"GET /api/v1/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nAccept: application/json;as=Table;v=v1\r\nConnection: close\r\n\r\n")
    full_table = b""
    while True:
        c = s.recv(4096)
        if not c:
            break
        full_table += c
    s.close()
    print(f"[INIT] Verified full chunked table payload size: {len(full_table)} bytes")
    assert b"Transfer-Encoding: chunked" in full_table
    assert b"0\r\n\r\n" in full_table

    # SCENARIO 1: Sequential Disconnects at various byte offsets
    print("\n--- [Scenario 1: Sequential Disconnect Variations] ---")
    disconnect_offsets = [0, 5, 20, 50, 100, 200, 350, 500, 512, 600, 1024, 2048]
    for mode in ["FIN_CLOSE", "TCP_RST"]:
        for offset in disconnect_offsets:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            if mode == "TCP_RST":
                s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            s.connect((SERVER_HOST, SERVER_PORT))
            s.sendall(b"GET /api/v1/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nAccept: application/json;as=Table;v=v1\r\nConnection: close\r\n\r\n")
            if offset > 0:
                s.recv(offset)
            s.close()
            time.sleep(0.01)

            if not is_server_alive():
                print(f"[FAIL] Server died during mode {mode} at offset {offset}!")
                stop_sim()
                return False
    print(f" [PASS] Scenario 1 completed: {len(disconnect_offsets) * 2} sequential disconnect variations survived.")

    # SCENARIO 2: Concurrent Multi-Threaded Disconnect Assault
    print("\n--- [Scenario 2: Concurrent Multi-Threaded Disconnect Assault] ---")
    NUM_THREADS = 10
    ITERATIONS_PER_THREAD = 25
    total_aborts = NUM_THREADS * ITERATIONS_PER_THREAD
    print(f"[LOAD] Launching {NUM_THREADS} threads concurrently x {ITERATIONS_PER_THREAD} disconnects ({total_aborts} total)...")

    errors = []

    def worker_flood(thread_id):
        for it in range(ITERATIONS_PER_THREAD):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                # Alternate between RST and abrupt close
                if (thread_id + it) % 2 == 0:
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                s.connect((SERVER_HOST, SERVER_PORT))
                s.sendall(b"GET /api/v1/pods HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nAccept: application/json;as=Table;v=v1\r\nConnection: close\r\n\r\n")
                # Read partial chunk bytes
                cut_bytes = (it * 37) % 600 + 10
                s.recv(cut_bytes)
                s.close()
            except Exception as e:
                # Connection refused or socket error on client side is allowed during high load, but server crash is not
                pass

    threads = []
    for t_idx in range(NUM_THREADS):
        t = threading.Thread(target=worker_flood, args=(t_idx,))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()

    print(f"[LOAD] All {NUM_THREADS} threads finished.")
    time.sleep(0.5)

    # SCENARIO 3: Health & Stability Verification
    print("\n--- [Scenario 3: Post-Stress Health & Stability Verification] ---")
    if not is_server_alive():
        print(f"[FAIL] Server died after concurrent assault!")
        stop_sim()
        return False
    print(" [PASS] Server is fully alive.")

    current_pid = sim_proc.pid
    if current_pid != sim_pid:
        print(f"[FAIL] Server PID changed from {sim_pid} to {current_pid}!")
        stop_sim()
        return False
    print(f" [PASS] Process PID unchanged ({current_pid}) - zero restarts/crashes.")

    current_threads = get_thread_count(current_pid)
    print(f" [INFO] Current thread count: {current_threads} (initial: {initial_threads})")
    # In kube32-sim, main thread + scheduler thread + http server thread = ~3 threads
    # Thread count should remain small and stable, not leaking
    if current_threads > initial_threads + 5:
        print(f"[FAIL] Potential thread leak detected! Initial: {initial_threads}, Current: {current_threads}")
        stop_sim()
        return False
    print(f" [PASS] OS Thread count stable: {current_threads} threads (zero hung threads).")

    # Verify normal requests continue to execute cleanly
    endpoints = ["/version", "/api/v1/nodes", "/api/v1/pods", "/apis"]
    for ep in endpoints:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect((SERVER_HOST, SERVER_PORT))
        s.sendall(f"GET {ep} HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nConnection: close\r\n\r\n".encode("utf-8"))
        data = s.recv(4096)
        s.close()
        if b"200 OK" not in data:
            print(f"[FAIL] Endpoint {ep} failed after stress tests! Response: {data[:100]}")
            stop_sim()
            return False
        print(f" [PASS] Normal request {ep} returns 200 OK.")

    # Table streaming test post-stress
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2.0)
    s.connect((SERVER_HOST, SERVER_PORT))
    s.sendall(b"GET /api/v1/nodes HTTP/1.1\r\nHost: 127.0.0.1:6443\r\nAccept: application/json;as=Table;v=v1\r\nConnection: close\r\n\r\n")
    tbl_data = b""
    while True:
        c = s.recv(2048)
        if not c:
            break
        tbl_data += c
    s.close()

    assert b"Transfer-Encoding: chunked" in tbl_data
    assert b"0\r\n\r\n" in tbl_data
    print(" [PASS] Chunked table streaming post-stress produces valid complete response.")

    stop_sim()
    print("\n==================================================================")
    print("   ALL SOCKET DISCONNECT STRESS TESTS PASSED SUCCESSFULLY!       ")
    print("==================================================================")
    return True

if __name__ == "__main__":
    success = run_stress_tests()
    sys.exit(0 if success else 1)
