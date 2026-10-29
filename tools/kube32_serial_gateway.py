#!/usr/bin/env python3
"""
Kube32 serial gateway: HTTP to a board over its USB console.

Every connection to a local port becomes a stream on the board's console
relay (include/app/console_relay.hpp). The board sends the request to the
mapped port on itself, or on the control plane when it is a worker, and
streams the response back as it arrives. Streams run side by side, so
Server-Sent Events, watches, `kubectl logs -f` and page loads all work.

    local :6443  ->  board :6443   (Kubernetes API)
    local :8080  ->  board :80     (ingress)

Usage:
    tools/kube32_serial_gateway.py [--port /dev/cu.usbmodemXXXX] [--map 8080:80]...

Without --port the gateway uses KUBE32_SERIAL_PORT, or asks every USB serial
device that looks like a board for its role and takes the control plane.
Device output goes to stdout; lines read from stdin go to the console
(K32_CMD:STATUS, for example).
"""
import argparse
import os
import queue
import select
import socket
import socketserver
import sys
import threading
import time
import zlib

import serial
import serial.tools.list_ports

BAUD = 115200            # native USB-Serial/JTAG ignores the baud rate; CH340 corrupts bytes above it
MARK = b"\x1e"           # board frames start with this byte
MAX_REQUEST = 60 * 1024  # the board takes requests below 64 KiB
FIRST_BYTE_TIMEOUT = 30.0
HELLO_TIMEOUT = 3.0

END_OK, END_CUT = object(), object()


class Stream:
    def __init__(self, sid, port, request):
        self.id = sid
        self.port = port
        self.request = request
        self.chunks = queue.Queue()   # bytes, then END_OK or END_CUT


class Relay:
    """One board console: writes host frames, parses board frames, logs the rest."""

    def __init__(self, ser, log=None):
        self.ser = ser
        self.log = log or (lambda line: print(f"[device] {line}", flush=True))
        self.write_lock = threading.Lock()
        self.lock = threading.Lock()
        self.streams = {}
        self.next_id = 1
        self.buf = b""
        self.text = b""
        self.ready = threading.Event()   # the board answered K32_RESET since it last booted
        self.slots = 0
        self.role = ""
        self.free = None
        self.alive = True
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    # -- host side ---------------------------------------------------------

    def write(self, data):
        with self.write_lock:
            self.ser.write(data)
            self.ser.flush()

    def hello(self, timeout=HELLO_TIMEOUT):
        """Ends every stream on the board and learns its slot count and role."""
        self.ready.clear()
        self.write(b"\nK32_RESET\n")
        if not self.ready.wait(timeout):
            return False
        if self.free is None:
            self.free = threading.BoundedSemaphore(self.slots)
        return True

    def open(self, port, request):
        with self.lock:
            sid = self.next_id
            self.next_id = self.next_id % 0x7FFFFFFF + 1
            stream = Stream(sid, port, request)
            self.streams[sid] = stream
        self._send_open(stream)
        return stream

    def close(self, stream):
        """The client is done with the stream; tells the board unless it ended it."""
        with self.lock:
            live = self.streams.pop(stream.id, None) is not None
        if live and self.alive:
            try:
                self.write(f"K32_CLOSE {stream.id}\n".encode())
            except (serial.SerialException, OSError):
                pass

    def _send_open(self, stream):
        crc = zlib.crc32(stream.request) & 0xFFFFFFFF
        self.write(f"K32_OPEN {stream.id} {stream.port} {len(stream.request)} {crc:08x}\n".encode() + stream.request)

    # -- board side --------------------------------------------------------

    def _read_loop(self):
        try:
            while self.alive:
                data = self.ser.read(4096)
                if data:
                    self.feed(data)
        except (serial.SerialException, OSError, TypeError):
            pass
        self.alive = False
        with self.lock:
            streams, self.streams = list(self.streams.values()), {}
        for s in streams:
            s.chunks.put(END_CUT)

    def feed(self, data):
        self.buf += data
        while self.buf:
            mark = self.buf.find(MARK)
            if mark < 0:
                self._text(self.buf)
                self.buf = b""
                return
            if mark:
                self._text(self.buf[:mark])
                self.buf = self.buf[mark:]
            nl = self.buf.find(b"\n")
            if nl < 0:
                if len(self.buf) > 128:   # no frame header after all
                    self._text(self.buf[:1])
                    self.buf = self.buf[1:]
                    continue
                return
            head = self.buf[1:nl].decode("ascii", "replace").split()
            if len(head) == 4 and head[0] == "K32_DATA":
                try:
                    sid, n, crc = int(head[1]), int(head[2]), int(head[3], 16)
                except ValueError:
                    sid = None
                if sid is not None:
                    end = nl + 1 + n
                    if len(self.buf) < end:
                        return
                    payload = self.buf[nl + 1:end]
                    self.buf = self.buf[end:]
                    self._data(sid, payload, crc)
                    continue
            self.buf = self.buf[nl + 1:]
            self._control(head)

    def _data(self, sid, payload, crc):
        with self.lock:
            stream = self.streams.get(sid)
        if stream is None:
            return
        if zlib.crc32(payload) & 0xFFFFFFFF != crc:
            print(f"[gateway] stream {sid}: corrupt frame, closing it", flush=True)
            self.close(stream)
            stream.chunks.put(END_CUT)
            return
        stream.chunks.put(payload)

    def _control(self, head):
        if len(head) >= 3 and head[0] == "K32_END":
            with self.lock:
                stream = self.streams.pop(int(head[1]), None)
            if stream:
                stream.chunks.put(END_OK if head[2] == "0" else END_CUT)
        elif len(head) >= 3 and head[0] == "K32_READY":
            self.slots, self.role = int(head[1]), head[2]
            self.ready.set()
        elif len(head) >= 3 and head[:2] == ["K32_ERR", "crc"]:
            with self.lock:
                stream = self.streams.get(int(head[2]))
            if stream:
                print(f"[gateway] stream {stream.id}: board reported a CRC mismatch, resending", flush=True)
                self._send_open(stream)
        else:
            self._text(MARK + " ".join(head).encode() + b"\n")

    def _text(self, data):
        self.text += data
        while b"\n" in self.text:
            line, _, self.text = self.text.partition(b"\n")
            line = line.replace(MARK, b"").decode("utf-8", "replace").rstrip("\r")
            if line:
                self.log(line)
            if line.startswith("ESP-ROM:"):
                self._board_reset()

    def _board_reset(self):
        """The board booted (its USB port stays open across a reset): its streams are gone."""
        self.ready.clear()
        with self.lock:
            streams, self.streams = list(self.streams.values()), {}
        for s in streams:
            s.chunks.put(END_CUT)


def read_request(sock):
    """Reads one HTTP request; returns it with `Connection: close`, or an error status."""
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(8192)
        if not chunk:
            return None, None
        data += chunk
        if len(data) > MAX_REQUEST:
            return None, "431 Request Header Fields Too Large"
    head, _, body = data.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    headers = []
    length = 0
    for h in lines[1:]:
        name = h.partition(b":")[0].strip().lower()
        if name == b"content-length":
            length = int(h.partition(b":")[2].strip() or 0)
        elif name == b"transfer-encoding":
            return None, "411 Length Required"
        if name in (b"connection", b"keep-alive", b"proxy-connection"):
            continue
        headers.append(h)
    if len(head) + length > MAX_REQUEST:
        return None, "413 Content Too Large"
    while len(body) < length:
        chunk = sock.recv(min(65536, length - len(body)))
        if not chunk:
            return None, None
        body += chunk
    headers.append(b"Connection: close")
    return lines[0] + b"\r\n" + b"\r\n".join(headers) + b"\r\n\r\n" + body[:length], None


def client_gone(sock):
    readable, _, _ = select.select([sock], [], [], 0)
    if not readable:
        return False
    try:
        return sock.recv(1, socket.MSG_PEEK) == b""
    except OSError:
        return True


def reply(sock, status):
    sock.sendall(f"HTTP/1.1 {status}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".encode())


def make_handler(state, device_port):
    """`state["relay"]` is the board in use; a new one replaces it after a reconnect."""
    class Proxy(socketserver.BaseRequestHandler):
        def handle(self):
            sock = self.request
            sock.settimeout(10)
            relay = state["relay"]
            try:
                request, error = read_request(sock)
                if error:
                    reply(sock, error)
                if not request:
                    return
                if not relay.alive:
                    reply(sock, "502 Bad Gateway")
                    return
                free = relay.free
                if not relay.ready.wait(FIRST_BYTE_TIMEOUT) or not free.acquire(timeout=FIRST_BYTE_TIMEOUT):
                    reply(sock, "503 Service Unavailable")
                    return
                try:
                    self.forward(sock, relay, relay.open(device_port, request))
                finally:
                    free.release()
            except (socket.timeout, ConnectionError, OSError):
                pass
            finally:
                try:
                    sock.close()
                except OSError:
                    pass

        def forward(self, sock, relay, stream):
            waited, started = 0.0, False
            try:
                while True:
                    try:
                        chunk = stream.chunks.get(timeout=1.0)
                    except queue.Empty:
                        waited += 1.0
                        if client_gone(sock):
                            return
                        if not started and waited >= FIRST_BYTE_TIMEOUT:
                            reply(sock, "504 Gateway Timeout")
                            return
                        continue
                    if chunk is END_OK or chunk is END_CUT:
                        return   # END_CUT: closing the client marks the response incomplete
                    started = True
                    sock.sendall(chunk)
            finally:
                relay.close(stream)

    return Proxy


class ThreadedServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def open_serial(port):
    """Opens a board's console without resetting it.

    Opening the port raises DTR and RTS. RTS high with DTR low is the reset
    signal of the ESP auto-reset circuit (the USB-Serial/JTAG controller
    emulates it), so RTS goes low before DTR: dsrdtr keeps open() from
    clearing DTR first.
    """
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 0.2
    ser.dsrdtr = True
    ser.rts = False
    ser.open()
    try:
        ser.dtr = False
    except OSError:
        pass   # no modem lines (a pseudo-terminal)
    return ser


def candidate_ports():
    found = serial.tools.list_ports.comports()
    native = [p.device for p in found if p.vid == 0x303A]
    bridge = [p.device for p in found if p.vid == 0x1A86]
    other = [p.device for p in found if p.device not in native + bridge
             and any(k in p.device for k in ("usbmodem", "ttyACM", "ttyUSB"))]
    return native + bridge + other


def connect(prefer):
    """Opens the board to relay through: `prefer`, else the control plane among the boards found."""
    ports = [prefer] if prefer else ([os.environ["KUBE32_SERIAL_PORT"]] if os.environ.get("KUBE32_SERIAL_PORT") else candidate_ports())
    fallback = None
    for port in ports:
        try:
            relay = Relay(open_serial(port))
        except (serial.SerialException, OSError) as e:
            print(f"[gateway] {port}: {e}", flush=True)
            continue
        if not relay.hello():
            print(f"[gateway] {port}: no answer to K32_RESET (firmware without the console relay?)", flush=True)
            relay.alive = False
            relay.ser.close()
            continue
        print(f"[gateway] {port}: {relay.role}, {relay.slots} streams", flush=True)
        if relay.role == "control-plane" or prefer:
            if fallback:
                fallback[1].alive = False
                fallback[1].ser.close()
            return port, relay
        if fallback:
            relay.alive = False
            relay.ser.close()
        else:
            fallback = (port, relay)
    return fallback or (None, None)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial device (found automatically when omitted)")
    ap.add_argument("--map", action="append", default=None,
                    help="local:device port mapping, repeatable (default 6443:6443 and 8080:80)")
    args = ap.parse_args()

    port, relay = connect(args.port)
    if not relay:
        print("no board answered; pass --port", file=sys.stderr)
        return 1

    state = {"relay": relay}
    servers = []
    for m in args.map or ["6443:6443", "8080:80"]:
        local, _, device = m.partition(":")
        handler = make_handler(state, int(device))
        srv = ThreadedServer(("127.0.0.1", int(local)), handler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        servers.append(srv)
        print(f"[gateway] http://127.0.0.1:{local} -> board :{device}", flush=True)

    def console_input():
        for line in sys.stdin:
            try:
                state["relay"].write(line.rstrip("\r\n").encode() + b"\n")
            except (serial.SerialException, OSError):
                pass
    threading.Thread(target=console_input, daemon=True).start()

    try:
        while True:
            time.sleep(1)
            relay = state["relay"]
            if relay.alive and not relay.ready.is_set():
                # The board reset: wait for its console, then start over
                if relay.hello():
                    print(f"[gateway] {port}: board restarted, {relay.slots} streams", flush=True)
                continue
            if relay.alive:
                continue
            # The board went away (reset, unplugged): reopen it and start over
            print(f"[gateway] {port}: lost, reconnecting", flush=True)
            try:
                state["relay"].ser.close()
            except (serial.SerialException, OSError):
                pass
            while True:
                time.sleep(1)
                try:
                    candidate = Relay(open_serial(port))
                except (serial.SerialException, OSError):
                    continue
                if candidate.hello():
                    state["relay"] = candidate
                    print(f"[gateway] {port}: {candidate.role}, {candidate.slots} streams", flush=True)
                    break
                candidate.alive = False
                candidate.ser.close()
    except KeyboardInterrupt:
        pass
    for s in servers:
        s.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
