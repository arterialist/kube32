#!/usr/bin/env python3
"""
Kube32 Web Application Server
Serves the Material 3 Cluster Console and proxies Kubernetes API requests
to the local Kube32 cluster on http://127.0.0.1:6443.
"""

import sys
import os
import urllib.request
import urllib.error
from http.server import HTTPServer, SimpleHTTPRequestHandler
import json

WEB_DIR = os.path.dirname(os.path.abspath(__file__))
KUBE32_API = "http://127.0.0.1:6443"
PORT = 8080

class Kube32WebHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB_DIR, **kwargs)

    def do_GET(self):
        if self.path.startswith("/api/"):
            self.proxy_to_kube32("GET")
        else:
            super().do_GET()

    def do_POST(self):
        if self.path.startswith("/api/"):
            self.proxy_to_kube32("POST")
        else:
            self.send_error(405, "Method Not Allowed")

    def do_DELETE(self):
        if self.path.startswith("/api/"):
            self.proxy_to_kube32("DELETE")
        else:
            self.send_error(405, "Method Not Allowed")

    def proxy_to_kube32(self, method):
        target_url = f"{KUBE32_API}{self.path}"
        content_length = int(self.headers.get("Content-Length", 0))
        body_bytes = self.rfile.read(content_length) if content_length > 0 else None

        # Custom handler for GPIO pin toggle simulation/capability check
        if self.path == "/api/gpio/write":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b'{"status": "OK", "capability": "GPIO_WRITE", "authorized": true}')
            return

        headers = {}
        for k, v in self.headers.items():
            if k.lower() in ("accept", "content-type"):
                headers[k] = v

        try:
            req = urllib.request.Request(target_url, data=body_bytes, headers=headers, method=method)
            with urllib.request.urlopen(req, timeout=5.0) as resp:
                resp_data = resp.read()
                self.send_response(resp.status)
                self.send_header("Content-Type", resp.headers.get("Content-Type", "application/json"))
                self.send_header("Content-Length", str(len(resp_data)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(resp_data)
        except urllib.error.HTTPError as e:
            err_data = e.read()
            self.send_response(e.code)
            self.send_header("Content-Type", e.headers.get("Content-Type", "application/json"))
            self.send_header("Content-Length", str(len(err_data)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(err_data)
        except Exception as e:
            err_obj = json.dumps({"error": str(e)}).encode("utf-8")
            self.send_response(502)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(err_obj)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(err_obj)

    def log_message(self, format, *args):
        # Concise logging
        pass

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else PORT
    server = HTTPServer(("0.0.0.0", port), Kube32WebHandler)
    print("=================================================================")
    print(f"  Kube32 Fullstack Web App Server Online")
    print(f"  Access in browser: http://localhost:{port}")
    print(f"  Bridged to Kube32 Cluster at {KUBE32_API}")
    print("=================================================================")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

if __name__ == "__main__":
    main()
