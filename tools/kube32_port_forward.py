#!/usr/bin/env python3
"""
Kube32 Kubernetes Port Forwarder CLI Utility
Directly bridges local workstation TCP port (e.g. 8080) to deployed Kube32
cluster workloads (Pods and Services) without requiring Wi-Fi connection.
"""

import sys
import os
import time
import socket
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler

# Import gateway telemetry and workload application
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import tools.kube32_serial_gateway as gateway
from tools.kube32_workbench_app import handle_workload_http_request

class ForwarderHTTPHandler(BaseHTTPRequestHandler):
    def handle_request(self, method):
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length) if content_length > 0 else b""
        
        status_code, content_type, resp_body = handle_workload_http_request(
            method, self.path, dict(self.headers), body, gateway.hardware_telemetry, gateway.send_serial_cmd
        )

        self.send_response(status_code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(resp_body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(resp_body)

    def do_GET(self):
        self.handle_request("GET")

    def do_POST(self):
        self.handle_request("POST")

    def log_message(self, format, *args):
        # Clean logging
        pass

def main():
    target = "svc/hardware-workbench-svc"
    port_mapping = "8080:80"

    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if len(args) == 1:
        if ":" in args[0]:
            port_mapping = args[0]
        else:
            target = args[0]
    elif len(args) >= 2:
        target = args[0]
        port_mapping = args[1]

    local_port, _, target_port = port_mapping.partition(":")
    local_port = int(local_port) if local_port else 8080

    print("=================================================================")
    print("  Kube32 Workload Port Forwarder Online")
    print(f"  Forwarding from 127.0.0.1:{local_port} -> {target}:{target_port}")
    print(f"  Workload Target: {target} (Namespace: kube32-cluster)")
    print(f"  Open in browser: http://localhost:{local_port}")
    print("=================================================================")

    server = HTTPServer(("127.0.0.1", local_port), ForwarderHTTPHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nPort forwarding stopped.")
    finally:
        server.server_close()

if __name__ == "__main__":
    main()
