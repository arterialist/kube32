"""`kube32 firmware`: stage an image on the control plane and roll it out to every node."""
import hashlib
import json
import sys
import time

from . import common
from .common import (EXIT_EXISTS, EXIT_INPUT, EXIT_KUBECTL, EXIT_OK, EXIT_ROLLOUT, EXIT_USAGE, err_exit, fmt_bytes,
                     parse_quantity, print_table)
from .kube import ApiError, api_call


# ---------------------------------------------------------------------------
# firmware: stage an image on the control plane and roll it out
# ---------------------------------------------------------------------------
FIRMWARE_PATH = "/apis/kube32.io/v1/firmware"
ROLLOUTS_PATH = "/apis/kube32.io/v1/firmwarerollouts"
ROLLOUT_NAME = "firmware"
ESP_IMAGE_MAGIC = 0xE9
VERSION_MARKER = b"kube32-version:"    # src/api/version_info.cpp


def firmware_image_info(data):
    """(version, sha256 hex) of a firmware file, or raise ValueError: an ESP32
    image whose last 32 bytes are the SHA-256 of the rest, as esptool writes it."""
    if len(data) < 64 or data[0] != ESP_IMAGE_MAGIC:
        raise ValueError("not an ESP32 firmware image (first byte 0x%02x, expected 0xe9)" % (data[0] if data else 0))
    if hashlib.sha256(data[:-32]).digest() != data[-32:]:
        raise ValueError("the image does not end with its own SHA-256 (a truncated or modified file?)")
    version = ""
    at = data.find(VERSION_MARKER)
    if at >= 0:
        end = data.find(b"\0", at, at + len(VERSION_MARKER) + 33)
        if end > 0:
            version = data[at + len(VERSION_MARKER):end].decode("ascii", "replace")
    return version, hashlib.sha256(data).hexdigest()


def staged_status(args):
    return (api_call(args, "get", FIRMWARE_PATH) or {}).get("status", {})


def short(sha):
    return (sha or "")[:12] or "-"


def cmd_firmware_push(args):
    try:
        with open(args.file, "rb") as f:
            data = f.read()
    except OSError as e:
        return err_exit("cannot read %s: %s" % (args.file, e.strerror or e), EXIT_INPUT)
    try:
        version, sha = firmware_image_info(data)
    except ValueError as e:
        return err_exit("%s: %s" % (args.file, e), EXIT_INPUT)
    try:
        chunk = parse_quantity(args.chunk)
    except ValueError as e:
        return err_exit(str(e), EXIT_USAGE)
    if chunk < 1024:
        return err_exit("--chunk must be at least 1Ki", EXIT_USAGE)
    print("image %s: %s bytes, version %s, sha256 %s" % (args.file, fmt_bytes(len(data)), version or "unknown", sha))
    try:
        st = staged_status(args)
    except ApiError as e:
        return err_exit("cannot read the staged image: %s" % e, EXIT_KUBECTL)
    slot = int(st.get("slotSize", 0))
    if slot and len(data) > slot:
        return err_exit("the image is %s bytes, the control plane's update slot %s" % (fmt_bytes(len(data)), fmt_bytes(slot)), EXIT_INPUT)
    if st.get("phase") == "Staged" and st.get("sha256") == sha:
        print("already staged in %s" % st.get("slot", "?"))
        return EXIT_OK
    offset = 0
    if st.get("phase") == "Receiving" and st.get("sha256") == sha and int(st.get("size", 0)) == len(data):
        offset = int(st.get("received", 0))
        print("resuming at byte %s" % fmt_bytes(offset))
    t0 = time.time()
    sent = 0
    retries = 0
    shown = -1
    while True:
        n = min(chunk, len(data) - offset)
        path = "%s/image?offset=%d&size=%d&sha256=%s" % (FIRMWARE_PATH, offset, len(data), sha)
        try:
            st = (api_call(args, "replace", path, data[offset:offset + n]) or {}).get("status", {})
            retries = 0
        except ApiError as e:
            # The connection broke or the range was refused: continue where
            # the control plane says the upload stands
            retries += 1
            if retries > args.retries:
                return err_exit("upload failed at byte %s: %s" % (fmt_bytes(offset), e), EXIT_KUBECTL)
            print("range at byte %s failed (%s), retrying" % (fmt_bytes(offset), str(e).splitlines()[-1][:120]), file=sys.stderr)
            time.sleep(min(2 * retries, 10))
            try:
                st = staged_status(args)
            except ApiError:
                continue
            if st.get("phase") == "Failed":
                return err_exit("the control plane refused the image: %s" % st.get("message", ""), EXIT_KUBECTL)
            if st.get("phase") == "Receiving" and st.get("sha256") == sha:
                offset = int(st.get("received", 0))
            elif st.get("phase") == "Staged" and st.get("sha256") == sha:
                break
            else:
                offset = 0
            continue
        sent += n
        offset = int(st.get("received", offset + n))
        if st.get("phase") == "Staged":
            break
        if st.get("phase") != "Receiving":
            return err_exit("the control plane answered phase %s: %s" % (st.get("phase"), st.get("message", "")), EXIT_KUBECTL)
        if offset * 10 // len(data) != shown:
            shown = offset * 10 // len(data)
            took = max(time.time() - t0, 0.001)
            print("  %s / %s bytes  %.1f KiB/s" % (fmt_bytes(offset), fmt_bytes(len(data)), sent / 1024.0 / took), flush=True)
    took = max(time.time() - t0, 0.001)
    print("staged %s (%s) in %s on the control plane: %s bytes in %.0f s" %
          (st.get("version") or version or "image", short(sha), st.get("slot", "?"), fmt_bytes(len(data)), took))
    print("next: %s firmware rollout" % common.PROG)
    return EXIT_OK


def get_rollout(args):
    try:
        return api_call(args, "get", "%s/%s" % (ROLLOUTS_PATH, ROLLOUT_NAME))
    except ApiError as e:
        if "NotFound" in str(e) or "not found" in str(e):
            return None
        raise


def node_rows(args):
    nodes = (api_call(args, "get", "/api/v1/nodes") or {}).get("items", [])
    rows = []
    for n in nodes:
        meta = n.get("metadata", {})
        ann = meta.get("annotations", {}) or {}
        labels = meta.get("labels", {}) or {}
        cond = {}
        for c in (n.get("status", {}) or {}).get("conditions", []) or []:
            if c.get("type") == "FirmwareUpdate":
                cond = c
        rows.append({
            "name": meta.get("name", ""),
            "role": "control-plane" if "node-role.kubernetes.io/control-plane" in labels else "worker",
            "version": ann.get("kube32.io/firmware-version", "-"),
            "sha256": ann.get("kube32.io/firmware-sha256", ""),
            "slot": ann.get("kube32.io/firmware-slot", "-"),
            "state": ann.get("kube32.io/firmware-state", "-"),
            "target": ann.get("kube32.io/firmware", ""),
            "failed": ann.get("kube32.io/firmware-failed", ""),
            "update": ("%s: %s" % (cond.get("reason", ""), cond.get("message", ""))) if cond else "-",
        })
    rows.sort(key=lambda r: (r["role"] == "control-plane", r["name"]))
    return rows


def print_firmware_status(args, staged=True):
    if staged:
        st = staged_status(args)
        line = "staged:  %s" % st.get("phase", "?")
        if st.get("sha256"):
            line += "  %s (%s)  %s/%s bytes  slot %s" % (st.get("version") or "-", short(st.get("sha256")), fmt_bytes(int(st.get("received", 0))),
                                                         fmt_bytes(int(st.get("size", 0))), st.get("slot", "?"))
        if st.get("message"):
            line += "  %s" % st["message"]
        print(line)
    ro = get_rollout(args)
    if ro:
        rs = ro.get("status", {}) or {}
        print("rollout: %s  %s (%s)  %s/%s nodes%s  %s" % (rs.get("phase", "Pending"), rs.get("version") or "-", short(ro.get("spec", {}).get("sha256")),
                                                         rs.get("updatedNodes", 0), rs.get("totalNodes", 0),
                                                         ("  node " + rs["node"]) if rs.get("node") else "", rs.get("message", "")))
    else:
        print("rollout: none")
    rows = node_rows(args)
    for r in rows:
        r["sha256"] = short(r["sha256"])
        r["target"] = short(r["target"])
    print_table(rows, [("NODE", "name"), ("ROLE", "role"), ("VERSION", "version"), ("IMAGE", "sha256"), ("SLOT", "slot"),
                       ("STATE", "state"), ("TARGET", "target"), ("UPDATE", "update")])
    return ro


def cmd_firmware_status(args):
    try:
        print_firmware_status(args)
    except ApiError as e:
        return err_exit(str(e), EXIT_KUBECTL)
    return EXIT_OK


def cmd_firmware_rollout(args):
    try:
        st = staged_status(args)
        if st.get("phase") != "Staged":
            return err_exit("no image is staged (phase %s): %s firmware push <firmware.bin> first" % (st.get("phase", "?"), common.PROG),
                            EXIT_INPUT)
        sha = st["sha256"]
        if args.sha256 and args.sha256.lower() != sha:
            return err_exit("the staged image is %s, not %s" % (sha, args.sha256), EXIT_INPUT)
        ro = get_rollout(args)
        if ro:
            phase = (ro.get("status") or {}).get("phase", "")
            if phase not in ("Complete", "Failed"):
                return err_exit("rollout %s is in progress (%s firmware status; %s firmware abort stops it)" % (ROLLOUT_NAME, common.PROG, common.PROG),
                                EXIT_EXISTS)
            api_call(args, "delete", "%s/%s" % (ROLLOUTS_PATH, ROLLOUT_NAME))
        spec = {"sha256": sha}
        if args.node_timeout:
            spec["nodeTimeoutSeconds"] = args.node_timeout
        manifest = {"apiVersion": "kube32.io/v1", "kind": "FirmwareRollout", "metadata": {"name": ROLLOUT_NAME}, "spec": spec}
        api_call(args, "create", ROLLOUTS_PATH, json.dumps(manifest).encode())
    except ApiError as e:
        return err_exit(str(e), EXIT_KUBECTL)
    print("rolling out %s (%s): workers one at a time, then the control plane" % (st.get("version") or "image", short(sha)))
    if args.no_wait:
        print("follow it with: %s firmware status, kubectl get firmwarerollouts, kubectl get events" % common.PROG)
        return EXIT_OK
    # The control plane restarts last: the API is away for a while then
    last = None
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        time.sleep(args.interval)
        try:
            ro = get_rollout(args)
        except ApiError:
            if last != "unreachable":
                print("  the API server does not answer (the control plane restarts into the image?), waiting")
                last = "unreachable"
            continue
        rs = (ro or {}).get("status", {}) or {}
        phase = rs.get("phase", "Pending")
        line = "  %s %s/%s nodes  %s" % (phase, rs.get("updatedNodes", 0), rs.get("totalNodes", 0), rs.get("message", ""))
        if line != last:
            print(line, flush=True)
            last = line
        if not ro:
            return err_exit("the rollout was deleted", EXIT_KUBECTL)
        if phase == "Complete":
            try:
                print_firmware_status(args, staged=False)
            except ApiError:
                pass
            return EXIT_OK
        if phase == "Failed":
            return err_exit("rollout failed: %s" % rs.get("message", ""), EXIT_ROLLOUT)
    return err_exit("the rollout did not finish within %d s (%s firmware status)" % (args.timeout, common.PROG), EXIT_ROLLOUT)


def cmd_firmware_abort(args):
    try:
        if not get_rollout(args):
            print("no rollout")
            return EXIT_OK
        api_call(args, "delete", "%s/%s" % (ROLLOUTS_PATH, ROLLOUT_NAME))
    except ApiError as e:
        return err_exit(str(e), EXIT_KUBECTL)
    print("rollout deleted: nodes that have not switched yet keep their firmware")
    return EXIT_OK


def cmd_firmware_discard(args):
    try:
        st = staged_status(args)
        if st.get("phase", "Empty") == "Empty":
            print("no image is staged")
            return EXIT_OK
        api_call(args, "delete", FIRMWARE_PATH)
    except ApiError as e:
        return err_exit(str(e), EXIT_KUBECTL)
    print("staged image %s discarded: %s is free for blobs until the next push" % (short(st.get("sha256")), st.get("slot") or "the update slot"))
    return EXIT_OK


def cmd_firmware_info(args):
    try:
        with open(args.file, "rb") as f:
            data = f.read()
        version, sha = firmware_image_info(data)
    except OSError as e:
        return err_exit("cannot read %s: %s" % (args.file, e.strerror or e), EXIT_INPUT)
    except ValueError as e:
        return err_exit("%s: %s" % (args.file, e), EXIT_INPUT)
    print("version: %s\nsha256:  %s\nsize:    %s bytes" % (version or "unknown", sha, fmt_bytes(len(data))))
    return EXIT_OK
