"""`kube32 devices`, `checkpoint`, `move` and `checkpoints`."""
import json
import os
import time
import urllib.error
import urllib.request

from . import common
from .common import EXIT_KUBECTL, EXIT_MOVE, EXIT_OK, EXIT_USAGE, err_exit, fmt_bytes, print_table
from .kube import ApiError, api_call
from .pods import _NAME_RE


def device_reading(pod, timeout):
    """The latest reading of a driver pod, from its node's pod server
    (GET / with X-Kube32-Pod): 'key=value ...', or why there is none."""
    meta, status = pod.get("metadata", {}), pod.get("status", {}) or {}
    ip = status.get("podIP") or ""
    port = 0
    for c in pod.get("spec", {}).get("containers", []):
        for p in c.get("ports", []) or []:
            if p.get("name") == "http":
                port = p.get("containerPort", 0)
    if not ip or not port:
        return "-"
    req = urllib.request.Request("http://%s:%d/" % (ip, port),
                                 headers={"X-Kube32-Pod": "%s/%s" % (meta.get("namespace", "default"), meta.get("name", ""))})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = json.loads(resp.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as e:
        try:
            body = json.loads(e.read().decode("utf-8", "replace"))
        except ValueError:
            return "http %d" % e.code
    except (OSError, ValueError) as e:
        return "unreachable (%s)" % (getattr(e, "reason", None) or e)
    if not body.get("ok", False):
        return "error: %s" % body.get("error", "?")
    readings = body.get("readings", {}) or {}
    return " ".join("%s=%s" % (k, v) for k, v in readings.items()) or "-"


def device_key(d):
    return (d["metadata"].get("namespace", "default"), d["metadata"]["name"])


def activate_devices(args, path, devices):
    """A reading is a use: POST devices/<name>/activate for each OnDemand
    Device, then wait (--activation-timeout) for the Idle ones to run.
    Returns the Device list as it is afterwards."""
    waiting = set()
    for d in devices:
        spec, st = d.get("spec", {}), d.get("status", {}) or {}
        if spec.get("activation", "OnDemand") == "Always" or st.get("phase") in ("Absent", "Failed"):
            continue
        ns, name = device_key(d)
        api_call(args, "create", "/apis/kube32.io/v1/namespaces/%s/devices/%s/activate" % (ns, name), b"{}")
        if st.get("phase") != "Running":
            waiting.add((ns, name))
    deadline = time.time() + args.activation_timeout
    while waiting and time.time() < deadline:
        time.sleep(0.5)
        devices = [d for d in (api_call(args, "get", path) or {}).get("items", []) if not args.names or d["metadata"]["name"] in args.names]
        waiting = {device_key(d) for d in devices if device_key(d) in waiting and (d.get("status", {}) or {}).get("phase") != "Running"}
    return devices


def cmd_devices(args):
    """Every Device, with the latest reading of its driver"""
    path = "/apis/kube32.io/v1/devices" if args.all_namespaces else "/apis/kube32.io/v1/namespaces/%s/devices" % args.namespace
    try:
        devices = [d for d in (api_call(args, "get", path) or {}).get("items", []) if not args.names or d["metadata"]["name"] in args.names]
        if not args.no_activate:
            devices = activate_devices(args, path, devices)
        rows = []
        for d in sorted(devices, key=lambda d: (d["metadata"].get("namespace", ""), d["metadata"]["name"])):
            meta, spec, st = d["metadata"], d.get("spec", {}), d.get("status", {}) or {}
            reading = "-"
            if st.get("phase") == "Running" and st.get("podName"):
                pod = api_call(args, "get", "/api/v1/namespaces/%s/pods/%s" % (meta.get("namespace", "default"), st["podName"])) or {}
                reading = device_reading(pod, args.timeout)
            elif st.get("message"):
                reading = st["message"]
            rows.append({"ns": meta.get("namespace", ""), "name": meta["name"], "driver": spec.get("driver", ""),
                         "node": st.get("nodeName") or spec.get("nodeName") or "-", "phase": st.get("phase", "Pending"), "reading": reading})
    except ApiError as e:
        return err_exit(str(e), EXIT_KUBECTL)
    cols = [("NAME", "name"), ("DRIVER", "driver"), ("NODE", "node"), ("PHASE", "phase"), ("READING", "reading")]
    print_table(rows, ([("NAMESPACE", "ns")] if args.all_namespaces else []) + cols)
    return EXIT_OK


CHECKPOINTS_PATH = "/apis/kube32.io/v1/namespaces/%s/checkpoints"


def cmd_checkpoint(args):
    """checkpoint and move: a Checkpoint with action Save or Move, then its result"""
    if not _NAME_RE.match(args.pod):
        return err_exit("invalid pod name '%s'" % args.pod, EXIT_USAGE)
    name = "%s-%s" % (args.pod[:56], os.urandom(3).hex()[:5])
    spec = {"podName": args.pod, "action": args.action}
    if args.container:
        spec["containerName"] = args.container
    if getattr(args, "to", None):
        spec["target"] = {"nodeName": args.to}
    manifest = {"apiVersion": "kube32.io/v1", "kind": "Checkpoint", "metadata": {"name": name, "namespace": args.namespace}, "spec": spec}
    path = CHECKPOINTS_PATH % args.namespace
    try:
        api_call(args, "create", path, json.dumps(manifest).encode())
    except ApiError as e:
        return err_exit(str(e), EXIT_KUBECTL)
    print("checkpoint %s: %s of pod %s" % (name, "move" if args.action == "Move" else "save", args.pod))
    if args.no_wait:
        print("follow it with: %s checkpoints, kubectl get checkpoint %s, kubectl get events" % (common.PROG, name))
        return EXIT_OK
    done = ("Completed", "Failed") if args.action == "Move" else ("Saved", "Failed")
    last = None
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        time.sleep(0.5)
        try:
            st = (api_call(args, "get", "%s/%s" % (path, name)) or {}).get("status", {}) or {}
        except ApiError as e:
            return err_exit(str(e), EXIT_KUBECTL)
        phase = st.get("phase", "Pending")
        if phase != last and phase not in done:
            print("  %s" % phase, flush=True)
            last = phase
        if phase == "Failed":
            blockers = st.get("blockers") or []
            return err_exit("%s%s" % (st.get("message", "failed"), "".join("\n  " + b for b in blockers)), EXIT_MOVE)
        if phase == "Saved":
            print("saved %s bytes from node %s" % (st.get("sizeBytes", "?"), st.get("nodeName", "?")))
            return EXIT_OK
        if phase == "Completed":
            print(st.get("message") or "moved to node %s" % st.get("targetNode", "?"))
            return EXIT_OK
    return err_exit("checkpoint %s did not finish within %d s (%s checkpoints)" % (name, args.timeout, common.PROG), EXIT_MOVE)


def cmd_checkpoints(args):
    """Every Checkpoint, oldest first"""
    path = "/apis/kube32.io/v1/checkpoints" if args.all_namespaces else CHECKPOINTS_PATH % args.namespace
    try:
        items = (api_call(args, "get", path) or {}).get("items", [])
    except ApiError as e:
        return err_exit(str(e), EXIT_KUBECTL)
    rows = []
    for c in sorted(items, key=lambda c: int(c["metadata"].get("resourceVersion", "0") or 0)):
        meta, spec, st = c["metadata"], c.get("spec", {}), c.get("status", {}) or {}
        size = st.get("sizeBytes")
        rows.append({"ns": meta.get("namespace", ""), "name": meta["name"], "pod": spec.get("podName", ""), "action": spec.get("action", "Save"),
                     "phase": st.get("phase", "Pending"), "node": st.get("nodeName", "-"), "target": st.get("targetNode", "-"),
                     "size": fmt_bytes(size) if size is not None else "-",
                     "trigger": (meta.get("labels") or {}).get("kube32.io/trigger", "-"),
                     "message": st.get("message", "") + "".join("; " + b for b in st.get("blockers") or [])})
    cols = [("NAME", "name"), ("POD", "pod"), ("ACTION", "action"), ("PHASE", "phase"), ("NODE", "node"), ("TARGET", "target"),
            ("SIZE", "size"), ("TRIGGER", "trigger"), ("MESSAGE", "message")]
    print_table(rows, ([("NAMESPACE", "ns")] if args.all_namespaces else []) + cols)
    return EXIT_OK
