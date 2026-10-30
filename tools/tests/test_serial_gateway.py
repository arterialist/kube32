"""Tests for tools/kube32_serial_gateway.py against a fake board on a pty.

Run from the repository root:

    python3 -m unittest discover -s tools/tests -v

The fake board speaks the console relay protocol (include/app/console_relay.hpp)
on the master side of a pseudo-terminal; the gateway opens the slave side as
its serial port and serves HTTP on a local port.
"""
import importlib.util
import os
import socket
import threading
import time
import unittest
import zlib

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
GATEWAY = os.path.join(os.path.dirname(TESTS_DIR), "kube32_serial_gateway.py")

try:
    import serial  # noqa: F401
    HAVE_SERIAL = hasattr(os, "openpty")
except ImportError:
    HAVE_SERIAL = False


def load_gateway():
    spec = importlib.util.spec_from_file_location("kube32_serial_gateway", GATEWAY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


HELLO_BODY = b"hello from the board\n" * 200   # spans several frames


class FakeBoard:
    """The board side: answers K32_RESET and K32_OPEN by path, records K32_CLOSE."""

    def __init__(self, fd):
        self.fd = fd
        self.lock = threading.Lock()
        self.buf = b""
        self.requests = []
        self.closed = set()
        self.crc_errors_left = {"/resend": 1}
        self.stop = False
        threading.Thread(target=self.loop, daemon=True).start()

    def send(self, data):
        with self.lock:
            view = memoryview(data)
            while view:
                n = os.write(self.fd, view)
                view = view[n:]

    def data(self, sid, payload, crc=None):
        crc = zlib.crc32(payload) & 0xFFFFFFFF if crc is None else crc
        return b"\x1eK32_DATA %d %d %08x\n" % (sid, len(payload), crc) + payload

    def end(self, sid, status=0):
        return b"\x1eK32_END %d %d\n" % (sid, status)

    def loop(self):
        while not self.stop:
            try:
                chunk = os.read(self.fd, 4096)
            except OSError:
                return
            self.buf += chunk
            while b"\n" in self.buf:
                line, _, rest = self.buf.partition(b"\n")
                words = line.decode().split()
                if words[:1] == ["K32_OPEN"]:
                    sid, port, n, crc = int(words[1]), int(words[2]), int(words[3]), int(words[4], 16)
                    if len(rest) < n:
                        break   # the payload is still arriving
                    request, self.buf = rest[:n], rest[n:]
                    assert zlib.crc32(request) & 0xFFFFFFFF == crc
                    self.requests.append(request)
                    self.serve(sid, request)
                    continue
                self.buf = rest
                if words == ["K32_RESET"]:
                    self.send(b"\x1eK32_READY 4 control-plane\n")
                elif words[:1] == ["K32_CLOSE"]:
                    self.closed.add(int(words[1]))

    def serve(self, sid, request):
        path = request.split(b" ")[1].decode()
        if self.crc_errors_left.get(path):
            self.crc_errors_left[path] -= 1
            self.send(b"\x1eK32_ERR crc %d\n" % sid)
            return
        if path == "/hello":
            response = b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(HELLO_BODY) + HELLO_BODY
            # A log line written around a frame, as a board's log output can be
            self.send(b"[NET] boot log li" + self.data(sid, response[:1500]) + b"ne\n")
            self.send(self.data(sid, response[1500:]) + self.end(sid))
        elif path in ("/resend", "/echo"):
            response = b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
            self.send(self.data(sid, response) + self.end(sid))
        elif path == "/cut":
            self.send(self.data(sid, b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly part") + self.end(sid, 1))
        elif path == "/corrupt":
            self.send(self.data(sid, b"HTTP/1.1 200 OK\r\n\r\nbad", crc=1))
        elif path == "/events":
            def events():
                self.send(self.data(sid, b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n"))
                for i in range(500):
                    if sid in self.closed or self.stop:
                        return
                    self.send(self.data(sid, b"data: %d\n\n" % i))
                    time.sleep(0.02)
            threading.Thread(target=events, daemon=True).start()


@unittest.skipUnless(HAVE_SERIAL, "needs pyserial and a pty")
class SerialGatewayTest(unittest.TestCase):
    def setUp(self):
        self.gw = load_gateway()
        master, slave = os.openpty()
        self.board = FakeBoard(master)
        self.slave = slave
        self.logs = []
        self.relay = self.gw.Relay(self.gw.open_serial(os.ttyname(slave)), log=self.logs.append)
        self.assertTrue(self.relay.hello())
        self.server = self.gw.ThreadedServer(("127.0.0.1", 0), self.gw.make_handler({"relay": self.relay}, 80))
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.port = self.server.server_address[1]

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.board.stop = True
        self.relay.alive = False
        self.relay.ser.close()
        os.close(self.slave)

    def connect(self):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        return s

    def get(self, path, extra=b""):
        s = self.connect()
        s.sendall(b"GET " + path.encode() + b" HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n" + extra + b"\r\n")
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
        s.close()
        return data

    def wait_for(self, cond, timeout=3.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if cond():
                return True
            time.sleep(0.02)
        return cond()

    def test_hello_reports_slots_and_role(self):
        self.assertEqual((self.relay.slots, self.relay.role), (4, "control-plane"))

    def test_response_across_frames_and_log_text(self):
        data = self.get("/hello")
        self.assertTrue(data.endswith(HELLO_BODY))
        self.assertIn(b"Content-Length: %d" % len(HELLO_BODY), data)
        self.assertTrue(self.wait_for(lambda: "[NET] boot log line" in self.logs), self.logs)

    def test_request_is_sent_with_connection_close(self):
        self.get("/echo", extra=b"Keep-Alive: timeout=5\r\nX-Test: 1\r\n")
        request = self.board.requests[-1]
        self.assertIn(b"\r\nX-Test: 1\r\n", request)
        self.assertEqual(request.count(b"Connection:"), 1)
        self.assertIn(b"\r\nConnection: close\r\n", request)
        self.assertNotIn(b"Keep-Alive", request)

    def test_chunked_body_is_sent_with_a_length(self):
        s = self.connect()
        s.sendall(b"PUT /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                  b"5\r\nhello\r\n7;ext=1\r\n, world\r\n0\r\n\r\n")
        while s.recv(65536):
            pass
        s.close()
        request = self.board.requests[-1]
        self.assertNotIn(b"Transfer-Encoding", request)
        self.assertIn(b"\r\nContent-Length: 12\r\n", request)
        self.assertTrue(request.endswith(b"\r\n\r\nhello, world"), request)

    def test_event_stream_beside_requests_and_close(self):
        events = self.connect()
        events.sendall(b"GET /events HTTP/1.1\r\nHost: x\r\n\r\n")
        got = b""
        while b"data: 3\n\n" not in got:
            got += events.recv(4096)
        self.assertTrue(self.get("/echo").endswith(b"ok"))
        self.assertEqual(self.board.closed, set())   # the board ended /echo itself
        events.close()
        # The gateway notices the client is gone and tells the board
        self.assertTrue(self.wait_for(lambda: len(self.board.closed) == 1), self.board.closed)

    def test_cut_response_closes_the_client_early(self):
        data = self.get("/cut")
        self.assertTrue(data.endswith(b"only part"))

    def test_corrupt_frame_closes_the_stream(self):
        data = self.get("/corrupt")
        self.assertEqual(data, b"")
        self.assertTrue(self.wait_for(lambda: len(self.board.closed) == 1))

    def test_board_reset_ends_streams_until_hello(self):
        events = self.connect()
        events.sendall(b"GET /events HTTP/1.1\r\nHost: x\r\n\r\n")
        got = b""
        while b"data: 1\n\n" not in got:
            got += events.recv(4096)
        self.board.closed.add(1)   # the board's streams end with its reset
        self.board.send(b"ESP-ROM:esp32s3-20210327\r\nrst:0xc (RTC_SW_CPU_RST)\r\n")
        events.settimeout(5)
        while events.recv(4096):
            pass                    # the gateway closes the client: the stream is gone
        events.close()
        self.assertTrue(self.wait_for(lambda: not self.relay.ready.is_set()))
        self.assertTrue(self.relay.hello())
        self.assertTrue(self.get("/echo").endswith(b"ok"))

    def test_crc_error_resends_the_request(self):
        self.assertTrue(self.get("/resend").endswith(b"ok"))
        self.assertEqual(len([r for r in self.board.requests if b"/resend" in r]), 2)


if __name__ == "__main__":
    unittest.main()
