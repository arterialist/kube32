"""`kube32 console`: a board's serial console. Prints what the board writes and sends the lines typed."""
import sys
import threading
import time

OPEN_TIMEOUT = 10.0   # a board that just restarted is back on USB within a few seconds


def run(port, stdin=None, stdout=None):
    import serial

    from .gateway import open_serial   # opens the port without resetting the board

    stdin = stdin or sys.stdin
    stdout = stdout or sys.stdout
    stop = threading.Event()

    def open_port(deadline):
        while True:
            try:
                return open_serial(port)
            except (serial.SerialException, OSError):
                if stop.is_set() or time.time() > deadline:
                    raise
                time.sleep(0.5)

    state = {"ser": open_port(time.time() + OPEN_TIMEOUT)}
    stdout.write("[console] %s at 115200 baud: lines typed go to the board (K32_CMD:STATUS), Ctrl-C quits\n" % port)
    stdout.flush()

    def reader():
        while not stop.is_set():
            ser = state["ser"]
            try:
                data = ser.read(4096)
            except (serial.SerialException, OSError, TypeError):
                if stop.is_set():
                    return
                stdout.write("\n[console] %s: lost, reopening\n" % port)
                stdout.flush()
                try:
                    ser.close()
                except (serial.SerialException, OSError):
                    pass
                try:
                    state["ser"] = open_port(float("inf"))
                except (serial.SerialException, OSError):
                    return
                continue
            if data:
                stdout.write(data.replace(b"\x1e", b"").decode("utf-8", "replace"))
                stdout.flush()

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    try:
        for line in stdin:
            try:
                state["ser"].write(line.rstrip("\r\n").encode() + b"\n")
                state["ser"].flush()
            except (serial.SerialException, OSError):
                stdout.write("[console] %s: not connected, line dropped\n" % port)
                stdout.flush()
    except KeyboardInterrupt:
        pass
    stop.set()
    try:
        state["ser"].close()
    except (serial.SerialException, OSError):
        pass
    thread.join(1.0)
    return 0
