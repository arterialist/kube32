"""`kube32 flash`: write the release firmware, or a local image, to an ESP32-S3 board with esptool."""
import os
import sys
import time

from . import common, releases
from .common import EXIT_INPUT, EXIT_OK, err_exit
from .ports import PortError, choose_port, is_bridge

FIRMWARE_ASSET = "kube32-firmware-esp32s3-%s.bin"
FLASH_PARTS_ASSET = "kube32-firmware-esp32s3-%s-flash.json"
BAUDS = (921600, 460800)
ESP_IMAGE_MAGIC = 0xE9
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_MAGIC = b"\xaa\x50"
QUICKSTART = "https://arterialist.github.io/kube32/quickstart/"

SETUP_HELP = """\
Set the board up on its serial console ({prog} console --port {port}). Each command
saves the setting and restarts the board:

  K32_CMD:WIFI:<ssid>:<password>     join a 2.4 GHz Wi-Fi network
  K32_CMD:AP:off                     control plane: no fallback access point
  K32_CMD:BOARD:cardputer            an M5Stack Cardputer worker
  K32_CMD:API:<control-plane-ip>     a worker: the control plane's address
  K32_CMD:STATUS                     role, network and API server address

Without Wi-Fi settings the control plane opens the access point Kube32-Cluster,
and K32_CMD:APPASS prints its password. Guide: {guide}
"""


def check_image(path):
    """Raises ValueError unless `path` is a merged image: bootloader at 0x0, partition table at 0x8000."""
    with open(path, "rb") as f:
        head = f.read(PARTITION_TABLE_OFFSET + 2)
    if not head or head[0] != ESP_IMAGE_MAGIC:
        raise ValueError("not an ESP32 image (first byte 0x%02x, expected 0xe9)" % (head[0] if head else 0))
    if head[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + 2] != PARTITION_MAGIC:
        raise ValueError("no partition table at 0x8000: flash writes a merged image (bootloader, partition table "
                         "and firmware) at offset 0x0; a PlatformIO build flashes with scripts/flash.sh")


def esptool_argv(esptool_version, port, baud, image, bridge):
    """esptool arguments that write `image` (a path, written at 0x0, or a list of (offset, path))
    esptool 5 names commands and reset modes with hyphens, esptool 4 with underscores."""
    kebab = int(str(esptool_version).split(".")[0]) >= 5

    def name(s):
        return s.replace("_", "-") if kebab else s.replace("-", "_")
    before = os.environ.get("KUBE32_FLASH_BEFORE", "default-reset")
    # A reset over native USB can leave the chip in download mode; the watchdog reset does not
    after = os.environ.get("KUBE32_FLASH_AFTER", "hard-reset" if bridge else "watchdog-reset")
    argv = ["--chip", "esp32s3", "--port", port, "--baud", str(baud), "--before", name(before), "--after", name(after),
            name("write-flash")]
    if bridge:
        argv.append("-u")   # uncompressed: the paced blocks below
    parts = [(0, image)] if isinstance(image, str) else image
    for offset, path in parts:
        argv += [hex(offset), path]
    return argv


def pace_bridge_writes(esptool):
    """CH340 bridges drop bytes with esptool's default block size: write 256-byte blocks, paced."""
    loader = getattr(esptool, "loader", None)
    try:
        loader.ESPLoader.ESP_RAM_BLOCK = 256
        loader.ESPLoader.FLASH_WRITE_SIZE = 256
        if hasattr(loader, "StubMixin"):
            loader.StubMixin.FLASH_WRITE_SIZE = 256
        flash_block = loader.ESPLoader.flash_block
    except AttributeError:
        return False

    def paced_flash_block(self, data, seq, timeout=20.0, encrypted=False):
        result = flash_block(self, data, seq, timeout=20.0, encrypted=encrypted)
        time.sleep(0.0015)
        return result
    loader.ESPLoader.flash_block = paced_flash_block
    return True


def release_parts(release, out=None):
    """[(offset, path)] of a release's bootloader, partition table, otadata and app, each checked
    against its SHA-256: everything but NVS, so the board keeps its settings"""
    import json
    with open(releases.fetch(release, FLASH_PARTS_ASSET % release, out)) as f:
        layout = json.load(f)
    parts = []
    for p in layout.get("parts", []):
        path = releases.fetch(release, p["file"], out)
        if releases.sha256_file(path) != p["sha256"]:
            raise releases.ReleaseError("%s does not match the SHA-256 in %s" % (p["file"], FLASH_PARTS_ASSET % release))
        parts.append((int(p["offset"]), path))
    if not parts:
        raise releases.ReleaseError("%s lists no parts" % (FLASH_PARTS_ASSET % release))
    return parts


def write_image(port, image, bauds, out=None):
    """Flashes `image` (a merged image path, or [(offset, path)]) at the first baud rate that works.
    Returns None, or why every attempt failed."""
    out = out or sys.stdout
    try:
        import esptool
    except ImportError:
        return "esptool is not installed in this Python environment (pip install esptool)"
    bridge = is_bridge(port)
    loader = getattr(esptool, "loader", None)
    if loader is not None:
        for attr, value in (("DEFAULT_TIMEOUT", 30.0), ("WRITE_BLOCK_ATTEMPTS", 5)):
            if hasattr(loader, attr):
                setattr(loader, attr, value)
    if bridge and pace_bridge_writes(esptool):
        out.write("CH340 bridge: writing 256-byte paced blocks\n")
    error = None
    for i, baud in enumerate(bauds):
        argv = esptool_argv(getattr(esptool, "__version__", "4"), port, baud, image, bridge)
        out.write("esptool %s\n" % " ".join(argv))
        out.flush()
        try:
            esptool.main(argv)
            return None
        except KeyboardInterrupt:
            raise
        except (Exception, SystemExit) as e:
            error = "esptool failed at %d baud: %s" % (baud, e or type(e).__name__)
            if i + 1 < len(bauds):
                out.write("%s; retrying at %d baud\n" % (error, bauds[i + 1]))
    return error


def cmd_flash(args):
    if args.file:
        image, what = args.file, args.file
        if not os.path.isfile(image):
            return err_exit("no such file: %s" % image, EXIT_INPUT)
    else:
        try:
            release = releases.release_for(args.version, "firmware",
                                           "pass --version X.Y.Z to flash a release, or --file <image> to flash an image file")
            if args.erase_settings:
                image = releases.fetch(release, FIRMWARE_ASSET % release)
            else:
                image = release_parts(release)
        except releases.ReleaseError as e:
            return err_exit(str(e), EXIT_INPUT)
        what = "kube32 %s" % release
    if isinstance(image, str):
        try:
            check_image(image)
        except (OSError, ValueError) as e:
            return err_exit("%s: %s" % (image, e), EXIT_INPUT)
        print("writing the whole image from 0x0: the board's stored settings (Wi-Fi, node name, cluster) are erased")
    else:
        print("writing the bootloader, partition table and firmware: the board keeps its stored settings")
    try:
        port = choose_port(args.port)
    except PortError as e:
        return err_exit(str(e), EXIT_INPUT)
    except ImportError:
        return err_exit("pyserial is not installed in this Python environment: pass --port", EXIT_INPUT)
    print("flashing %s to %s" % (what, port))
    error = write_image(port, image, (args.baud,) if args.baud else BAUDS)
    if error:
        return err_exit("%s (see https://arterialist.github.io/kube32/flashing-troubleshooting/)" % error, EXIT_INPUT)
    print("\nflashed %s with %s\n" % (port, what))
    print(SETUP_HELP.format(prog=common.PROG, port=port, guide=QUICKSTART))
    if args.console is False:
        return EXIT_OK
    if args.console is None:
        if not (sys.stdin.isatty() and sys.stdout.isatty()):
            return EXIT_OK
        try:
            answer = input("Open the serial console now? [Y/n] ").strip().lower()
        except EOFError:
            return EXIT_OK
        if answer not in ("", "y", "yes"):
            return EXIT_OK
    from . import console
    return console.run(port)
