"""USB serial ports of ESP32-S3 boards."""
import os

NATIVE_USB_VID = 0x303A   # the ESP32-S3's own USB-Serial/JTAG controller
CH340_VID = 0x1A86        # CH340 USB-UART bridges


class PortError(Exception):
    pass


def comports():
    import serial.tools.list_ports
    return serial.tools.list_ports.comports()


def board_ports(ports=None):
    """[(device, kind)] of the ports with the native USB or the CH340 vendor ID, native USB first."""
    ports = comports() if ports is None else ports
    native = sorted(p.device for p in ports if p.vid == NATIVE_USB_VID)
    bridge = sorted(p.device for p in ports if p.vid == CH340_VID)
    return [(d, "native USB") for d in native] + [(d, "CH340") for d in bridge]


def is_bridge(device, ports=None):
    """Whether `device` is a CH340 bridge (False when the ports cannot be listed)."""
    try:
        ports = comports() if ports is None else ports
    except Exception:
        return False
    return any(p.device == device and p.vid == CH340_VID for p in ports)


def choose_port(explicit=None, ports=None):
    """The board's port: `explicit`, else KUBE32_SERIAL_PORT, else the one board connected. Raises PortError."""
    if explicit:
        return explicit
    if os.environ.get("KUBE32_SERIAL_PORT"):
        return os.environ["KUBE32_SERIAL_PORT"]
    found = board_ports(ports)
    if not found:
        raise PortError("no ESP32-S3 board found (USB vendor ID 303a for native USB, 1a86 for a CH340 bridge): "
                        "connect one or pass --port")
    if len(found) > 1:
        raise PortError("%d boards are connected (%s): pass --port"
                        % (len(found), ", ".join("%s %s" % (d, kind) for d, kind in found)))
    return found[0][0]
