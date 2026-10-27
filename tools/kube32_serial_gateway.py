#!/usr/bin/env python3
"""
Kube32 USB-Serial HTTP API Gateway
Bridges standard Kubernetes HTTP API requests from kubectl/curl on localhost:6443
directly to the physical ESP32-S3 control plane over USB-Serial/JTAG.
"""

import sys
import time
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
import serial

SERIAL_PORT = "/dev/cu.usbmodem101"
BAUD_RATE = 115200
HTTP_PORT = 6443

ser_lock = threading.Lock()
ser = None

def get_serial():
    global ser
    if ser is None or not ser.is_open:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=4.0)
        time.sleep(0.1)
    return ser

def send_k32_request(method, path, body=""):
    with ser_lock:
        s = get_serial()
        # Flush any stale input
        s.reset_input_buffer()
        
        req_line = f"K32_REQ:{method}:{path}"
        if body:
            req_line += f":{body}"
        req_line += "\n"
        
        s.write(req_line.encode("utf-8"))
        s.flush()
        
        # Read response delimited by [K32-RESP-START] ... [K32-RESP-END]
        status_code = 500
        content_type = "application/json"
        response_body = []
        in_resp = False
        start_time = time.time()
        
        while time.time() - start_time < 5.0:
            raw_line = s.readline()
            if not raw_line:
                continue
            line = raw_line.decode("utf-8", errors="replace").strip()
            
            if "[K32-RESP-START]:" in line:
                in_resp = True
                parts = line.split(":")
                if len(parts) >= 2:
                    try:
                        status_code = int(parts[1])
                    except ValueError:
                        status_code = 200
                continue
            elif line == "[K32-RESP-END]":
                break
            elif in_resp:
                response_body.append(line)
        
        body_text = "\n".join(response_body) if response_body else "{}"
        return status_code, content_type, body_text

class KubeGatewayHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.handle_proxy("GET")

    def do_POST(self):
        self.handle_proxy("POST")

    def do_PUT(self):
        self.handle_proxy("PUT")

    def do_DELETE(self):
        self.handle_proxy("DELETE")

    def handle_proxy(self, method):
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length).decode("utf-8", errors="replace") if content_length > 0 else ""
        
        # Forward accept header or query string
        status_code, content_type, resp_body = send_k32_request(method, self.path, body)
        
        resp_bytes = resp_body.encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(resp_bytes)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(resp_bytes)

    def log_message(self, format, *args):
        # Concise logging
        print(f"[GATEWAY] {self.command} {self.path} -> {args[1] if len(args) > 1 else ''}")

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else HTTP_PORT
    server = HTTPServer(("127.0.0.1", port), KubeGatewayHandler)
    print(f"=================================================================")
    print(f"  Kube32 USB-Serial Kubernetes HTTP Gateway Online")
    print(f"  Listening on http://127.0.0.1:{port}")
    print(f"  Bridging to ESP32 Control Plane at {SERIAL_PORT}")
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
