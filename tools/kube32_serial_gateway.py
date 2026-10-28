#!/usr/bin/env python3
"""
Kube32 serial gateway: a transparent HTTP-over-USB bridge to a board.

It holds no cluster state. Every HTTP request received on a local port is
framed and written to the board's console, where the firmware replays it over
loopback against the real server on that port and returns the raw response.

    local :6443  ->  board :6443   (Kubernetes API)
    local :8080  ->  board :80     (application ingress)

Usage:
    tools/kube32_serial_gateway.py [--port /dev/cu.usbmodemXXXX] [--map 6443:6443] [--map 8080:80]

Device log lines that are not protocol frames are echoed to stdout.
"""
import argparse
import os
import socket
import socketserver
import sys
import threading
import time

import serial
import serial.tools.list_ports

import zlib

BAUD = 115200          # native USB-Serial/JTAG ignores the baud rate
BAUD_CH340 = 115200    # the CH340 bridge corrupts received bytes above this


def find_serial_port(prefer=None):
    if prefer and os.path.exists(prefer):
        return prefer
    env_port = os.environ.get("KUBE32_SERIAL_PORT")
    if env_port and os.path.exists(env_port):
        return env_port
    # Any node works: workers relay console HTTP to the control plane over
    # Wi-Fi. Prefer native USB-Serial/JTAG (fast, no bridge), then CH340.
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x303A:
            return p.device
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x1A86:
            return p.device
    for p in serial.tools.list_ports.comports():
        if "usbmodem" in p.device or "ttyACM" in p.device or "ttyUSB" in p.device:
            return p.device
    return None


class SerialLink:
    """Single serial connection shared by every HTTP handler, one request in flight."""

    def __init__(self, port, baud):
        self.port = port
        self.lock = threading.Lock()
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.2
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.buf = b""
        self.stats = {"requests": 0, "bytes_up": 0, "bytes_down": 0, "errors": 0}

    def _read_line(self, deadline):
        while time.time() < deadline:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line = self.buf[:nl]
                self.buf = self.buf[nl + 1:]
                return line
            chunk = self.ser.read(4096)
            if chunk:
                self.buf += chunk
        return None

    def _read_exact(self, n, deadline):
        while len(self.buf) < n and time.time() < deadline:
            chunk = self.ser.read(min(4096, n - len(self.buf)))
            if chunk:
                self.buf += chunk
        data = self.buf[:n]
        self.buf = self.buf[n:]
        return data

    def pump_logs(self):
        """Drains unsolicited device output (boot logs, pod logs) to stdout."""
        with self.lock:
            chunk = self.ser.read(4096)
            if chunk:
                self.buf += chunk
            while True:
                nl = self.buf.find(b"\n")
                if nl < 0:
                    break
                line = self.buf[:nl]
                self.buf = self.buf[nl + 1:]
                text = line.decode("utf-8", "replace").rstrip()
                if text:
                    print(f"[device] {text}", flush=True)

    def http(self, port, raw_request, timeout=15.0):
        with self.lock:
            for attempt in range(3):
                data = self._exchange(port, raw_request, timeout)
                if data is not None:
                    return data
            self.stats["errors"] += 1
            return None

    def _exchange(self, port, raw_request, timeout):
        self.stats["requests"] += 1
        self.stats["bytes_up"] += len(raw_request)
        crc = zlib.crc32(raw_request) & 0xFFFFFFFF
        header = f"K32_HTTP {port} {len(raw_request)} {crc:08x}\n".encode()
        self.ser.write(header + raw_request)
        self.ser.flush()
        deadline = time.time() + timeout
        if True:
            while True:
                line = self._read_line(deadline)
                if line is None:
                    return None
                if line.startswith(b"K32_ERR"):
                    print(f"[gateway] device reported {line.decode('ascii','replace')}, resending", flush=True)
                    return None
                if line.startswith(b"K32_RESP "):
                    try:
                        n = int(line.split()[1])
                    except (IndexError, ValueError):
                        continue
                    data = self._read_exact(n, deadline)
                    self.stats["bytes_down"] += len(data)
                    return data
                text = line.decode("utf-8", "replace").rstrip()
                if text:
                    print(f"[device] {text}", flush=True)


def make_handler(link, device_port):
    class Proxy(socketserver.BaseRequestHandler):
        def handle(self):
            sock = self.request
            sock.settimeout(10)
            data = b""
            try:
                while b"\r\n\r\n" not in data:
                    chunk = sock.recv(8192)
                    if not chunk:
                        return
                    data += chunk
                head, _, body = data.partition(b"\r\n\r\n")
                lines = head.split(b"\r\n")
                request_line = lines[0]
                headers = []
                content_length = 0
                for h in lines[1:]:
                    k, _, v = h.partition(b":")
                    kl = k.strip().lower()
                    if kl == b"content-length":
                        content_length = int(v.strip() or 0)
                    if kl in (b"connection", b"keep-alive", b"accept-encoding"):
                        continue
                    headers.append(h)
                while len(body) < content_length:
                    chunk = sock.recv(min(65536, content_length - len(body)))
                    if not chunk:
                        break
                    body += chunk
                headers.append(b"Connection: close")
                headers.append(b"Accept-Encoding: gzip")
                raw = request_line + b"\r\n" + b"\r\n".join(headers) + b"\r\n\r\n" + body
                resp = link.http(device_port, raw)
                if resp is None:
                    resp = b"HTTP/1.1 504 Gateway Timeout\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                sock.sendall(resp)
            except (socket.timeout, ConnectionError, OSError):
                pass
            finally:
                try:
                    sock.close()
                except OSError:
                    pass

    return Proxy


class ThreadedServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial device (auto-detected when omitted)")
    ap.add_argument("--map", action="append", default=None, help="local:device port mapping, repeatable (default 6443:6443 and 8080:80)")
    ap.add_argument("legacy_port", nargs="?", help=argparse.SUPPRESS)
    args = ap.parse_args()

    port = find_serial_port(args.port)
    if not port:
        print("no serial device found; pass --port", file=sys.stderr)
        return 1
    maps = args.map or ["6443:6443", "8080:80"]
    baud = BAUD
    for p in serial.tools.list_ports.comports():
        if p.device == port and p.vid == 0x1A86:
            baud = BAUD_CH340
    link = SerialLink(port, baud)
    print(f"[gateway] serial {port} @ {baud}")

    servers = []
    for m in maps:
        local, _, device = m.partition(":")
        srv = ThreadedServer(("127.0.0.1", int(local)), make_handler(link, int(device)))
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        servers.append(srv)
        print(f"[gateway] http://127.0.0.1:{local} -> board :{device}")

    try:
        while True:
            link.pump_logs()
            time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    for s in servers:
        s.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
