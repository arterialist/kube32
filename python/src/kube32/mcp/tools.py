"""The MCP server's tools, resources and prompt: Devices and pods of a kube32 cluster.

The server reaches the API server with kubectl, so kubeconfig contexts and credentials apply,
and reads drivers on their nodes' pod servers as `kube32 devices` does.
"""
import argparse
import base64
import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import time
from datetime import datetime, timezone
from urllib.parse import quote

from .. import releases
from ..build import cmd_build
from ..cluster import driver_address, driver_request
from ..common import format_quantity, parse_quantity, shared_file
from ..kube import ApiError, api_call, kubectl, kubectl_base, kubectl_missing
from ..pods import _ENV_RE, _NAME_RE
from ..wasm import HARDWARE, PAGE, check_module, format_report, read_wasm, static_memory_bytes
from . import catalog, limits as lim, reference
from .protocol import Tool, ToolError

DEVICES = "/apis/kube32.io/v1/devices"
MANAGED_BY = "app.kubernetes.io/managed-by"
MANAGER = "kube32-mcp"
# What deploy_code grants a pod: events and Service calls. Hardware stays with the drivers.
DEPLOY_HARDWARE = ("ipc", "service-call")
NO_READING_YET = "no reading yet"
MAX_SAMPLE_SECONDS = 300
ACTIVATION_TIMEOUT = 20.0
DRIVER_TIMEOUT = 3.0


def _now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _first_line(text):
    lines = [x for x in (text or "").strip().splitlines() if x.strip()]
    return lines[-1] if lines else ""


class Kube32:
    """One cluster, reached through kubectl with `kube` (argparse namespace of kubectl, kubeconfig,
    context and server). Writes and deploys need the server's opt-in."""

    def __init__(self, kube, namespace="default", allow_writes=False, allow_deploy=False, log=None):
        self.kube = kube
        self.namespace = namespace
        self.allow_writes = allow_writes
        self.allow_deploy = allow_deploy
        self.log = log or (lambda text: None)
        self.last_action_at = {}      # (namespace, device) -> {action: time.time() of the last one sent}

    # -- API ------------------------------------------------------------------
    def api(self, verb, path, body=None):
        try:
            return api_call(self.kube, verb, path, json.dumps(body).encode() if body is not None else None)
        except ApiError as e:
            raise ToolError("kubectl %s %s: %s" % (verb, path, e))

    def get(self, path):
        """The object at `path`, or None when the API server does not have it."""
        try:
            return api_call(self.kube, "get", path)
        except ApiError as e:
            if "NotFound" in str(e) or "not found" in str(e):
                return None
            raise ToolError("kubectl get %s: %s" % (path, e))

    def require(self, what):
        if what == "writes" and not self.allow_writes:
            raise ToolError("writes are off: this server sends no actions to Devices. The user starts it with "
                            "`kube32 mcp --allow-writes` to allow them, within each Device's kube32.io/limits.")
        if what == "deploy" and not self.allow_deploy:
            raise ToolError("deploys are off: this server creates and deletes no pods. The user starts it with "
                            "`kube32 mcp --allow-deploy` to allow them.")

    # -- Devices --------------------------------------------------------------
    def devices(self):
        return sorted((self.api("get", DEVICES) or {}).get("items", []), key=reference.device_ref)

    def find_device(self, ref):
        ns, _, name = ref.rpartition("/")
        matches = [d for d in self.devices() if d["metadata"]["name"] == name
                   and (not ns or d["metadata"].get("namespace", "default") == ns)]
        if not matches:
            raise ToolError("no Device %s (list_devices lists them)" % ref)
        if len(matches) > 1:
            if not ns:
                in_default = [d for d in matches if d["metadata"].get("namespace", "default") == self.namespace]
                if len(in_default) == 1:
                    return in_default[0]
            raise ToolError("Device %s is in several namespaces: name it namespace/name (%s)"
                            % (ref, ", ".join(reference.device_ref(d) for d in matches)))
        return matches[0]

    def device_path(self, device):
        return "/apis/kube32.io/v1/namespaces/%s/devices/%s" % (device["metadata"].get("namespace", "default"), device["metadata"]["name"])

    def driver_info(self, device, describe=None):
        """The catalog entry of the Device's driver, with readings and actions from its own /describe."""
        info = dict(catalog.DRIVERS.get(device.get("spec", {}).get("driver"), {}))
        if describe:
            info["readings"] = describe.get("readings") or {}
            info["actions"] = describe.get("actions") or []
        info.setdefault("readings", {})
        info.setdefault("actions", [])
        return info

    def device_limits(self, device, info):
        text = (device["metadata"].get("annotations") or {}).get(lim.ANNOTATION, "")
        actions = {a["action"]: catalog.action_arguments(a) for a in info["actions"]}
        return lim.check_names(lim.parse(text), info["readings"], actions)

    def driver_pod(self, device):
        st = device.get("status", {}) or {}
        if st.get("phase") != "Running" or not st.get("podName"):
            return None
        return self.get("/api/v1/namespaces/%s/pods/%s" % (device["metadata"].get("namespace", "default"), st["podName"]))

    def start_driver(self, device, ctx):
        """Reports a use of the Device (activate) and waits for its driver's first answer.
        Returns (device, driver pod, GET / answer)."""
        ref = reference.device_ref(device)
        spec, st = device.get("spec", {}), device.get("status", {}) or {}
        if st.get("phase") in ("Absent", "Failed"):
            raise ToolError("Device %s is %s: %s" % (ref, st["phase"], st.get("message", "")))
        if spec.get("activation", "OnDemand") != "Always":
            self.api("create", self.device_path(device) + "/activate", {})
            if st.get("phase") != "Running":
                ctx.progress("starting the driver of %s" % ref)
        deadline = time.time() + ACTIVATION_TIMEOUT
        unreachable = 0
        why = "the driver pod is not running"
        while True:
            pod = self.driver_pod(device)
            if pod is not None and driver_address(pod):
                try:
                    status, answer = driver_request(pod, DRIVER_TIMEOUT)
                    unreachable = 0
                    if status != 503 or NO_READING_YET not in str(answer.get("error", "")):
                        return device, pod, answer
                    why = "the driver has no reading yet"
                except (OSError, ValueError) as e:
                    unreachable += 1
                    ip, port = driver_address(pod)
                    why = "node %s's pod server at %s:%d does not answer (%s)" % (
                        pod.get("spec", {}).get("nodeName", "?"), ip, port, getattr(e, "reason", None) or e)
                    if unreachable >= 2:
                        raise ToolError("cannot read Device %s: %s. The server reads drivers on their nodes' pod servers, "
                                        "as `kube32 devices` does: this machine must reach the nodes' addresses." % (ref, why))
            elif (device.get("status") or {}).get("message"):
                why = device["status"]["message"]
            if time.time() >= deadline:
                raise ToolError("the driver of Device %s did not answer within %d s: %s" % (ref, ACTIVATION_TIMEOUT, why))
            time.sleep(0.5)
            device = self.get(self.device_path(device)) or device

    def list_devices(self, args, ctx):
        nodes = (self.api("get", "/api/v1/nodes") or {}).get("items", [])
        out_nodes = []
        for n in sorted(nodes, key=lambda n: n["metadata"]["name"]):
            meta, st = n["metadata"], n.get("status", {}) or {}
            labels, ann = meta.get("labels") or {}, meta.get("annotations") or {}
            ready = next((c.get("status") for c in st.get("conditions", []) if c.get("type") == "Ready"), "Unknown")
            entry = {"name": meta["name"], "ready": ready == "True",
                     "role": "control-plane" if "node-role.kubernetes.io/control-plane" in labels else "worker",
                     "board": labels.get("kube32.io/board", "?"),
                     "memory_allocatable": (st.get("allocatable") or {}).get("memory"),
                     "memory_free": ann.get("kube32.io/memory-free"),
                     "firmware": ann.get("kube32.io/firmware-version")}
            if labels.get("kube32.io/simulator") == "true":
                entry["simulator"] = True
            try:
                parts = json.loads(ann.get("kube32.io/i2c-devices", "[]"))
            except ValueError:
                parts = []
            if parts:
                entry["i2c_parts"] = ["%s%s %s" % ("mux %s channel %s, " % (p["mux"], p.get("channel")) if p.get("mux") else "",
                                                   p.get("address"), p.get("chip", "unidentified")) for p in parts]
            out_nodes.append(entry)
        out_devices = []
        for d in self.devices():
            meta, spec, st = d["metadata"], d.get("spec", {}), d.get("status", {}) or {}
            info = self.driver_info(d)
            rules = self.device_limits(d, info)
            entry = {"device": reference.device_ref(d), "driver": spec.get("driver"), "part": info.get("part", "unknown"),
                     "node": st.get("nodeName") or spec.get("nodeName"), "address": reference.address(spec),
                     "phase": st.get("phase", "Pending"), "activation": spec.get("activation", "OnDemand"),
                     "reads": info["readings"], "actions": [a["action"] for a in info["actions"]]}
            if st.get("chip"):
                entry["chip"] = st["chip"]
            if st.get("message") and st.get("phase") != "Running":
                entry["message"] = st["message"]
            notes = (meta.get("annotations") or {}).get(lim.NOTES_ANNOTATION, "").strip()
            if notes:
                entry["notes"] = notes
            if rules.rules:
                entry["limits"] = [str(r) for r in rules.rules]
            if rules.errors:
                entry["limit_errors"] = rules.errors
            out_devices.append(entry)
        return {"nodes": out_nodes, "devices": out_devices,
                "writes": "on, within each Device's limits" if self.allow_writes else "off (kube32 mcp --allow-writes)",
                "deploy": "on" if self.allow_deploy else "off (kube32 mcp --allow-deploy)"}

    def read_device(self, args, ctx):
        device = self.find_device(args["device"])
        samples = args.get("samples", 1)
        if not 1 <= samples <= 120:
            raise ToolError("samples is 1 to 120")
        info = self.driver_info(device)
        period = (device.get("spec", {}).get("intervalMs") or info.get("period_ms") or 1000) / 1000.0
        interval = args.get("interval_seconds", period)
        if interval < 0.1 or interval * (samples - 1) > MAX_SAMPLE_SECONDS:
            raise ToolError("interval_seconds is at least 0.1, and samples x interval_seconds at most %d s" % MAX_SAMPLE_SECONDS)
        device, pod, answer = self.start_driver(device, ctx)
        rules = self.device_limits(device, info)
        out = {"device": reference.device_ref(device), "driver": device["spec"].get("driver"),
               "node": (device.get("status") or {}).get("nodeName"), "units": info["readings"],
               "reading_period_s": period}
        rows, t0 = [], time.time()
        for i in range(samples):
            if i:
                time.sleep(max(0.0, t0 + i * interval - time.time()))
                try:
                    _, answer = driver_request(pod, DRIVER_TIMEOUT)
                except (OSError, ValueError) as e:
                    answer = {"ok": False, "error": "no answer (%s)" % (getattr(e, "reason", None) or e)}
                ctx.progress("sample %d of %d" % (i + 1, samples), i + 1, samples)
            row = {"t_s": round(time.time() - t0, 2)}
            if answer.get("ok"):
                row["readings"] = answer.get("readings") or {}
                if "uptime_ms" in answer:
                    row["uptime_ms"] = answer["uptime_ms"]
            else:
                row["error"] = answer.get("error", "no reading")
            rows.append(row)
        last = next((r for r in reversed(rows) if "readings" in r), None)
        if last:
            out["readings"] = last["readings"]
            warnings = lim.check_readings(rules, last["readings"])
            if warnings:
                out["warnings"] = warnings
        else:
            out["error"] = rows[-1]["error"]
        if samples > 1:
            out["summary"] = summarize(rows)
            out["samples"] = sample_table(rows)
        return out

    def write_device(self, args, ctx):
        self.require("writes")
        device = self.find_device(args["device"])
        action, arguments = args["action"], args.get("arguments") or {}
        if "action" in arguments:
            raise ToolError("arguments must not contain 'action'; name the action in 'action'")
        device, pod, _ = self.start_driver(device, ctx)
        try:
            status, describe = driver_request(pod, DRIVER_TIMEOUT, path="/describe")
        except (OSError, ValueError) as e:
            raise ToolError("the driver of %s does not answer /describe: %s" % (reference.device_ref(device), e))
        info = self.driver_info(device, describe if status == 200 else None)
        actions = {a["action"]: catalog.action_arguments(a) for a in info["actions"]}
        ref = reference.device_ref(device)
        if action not in actions:
            raise ToolError("Device %s (driver %s) has no action %s; it has: %s" % (ref, device["spec"].get("driver"), action,
                                                                                   ", ".join(sorted(actions)) or "none, it only reads"))
        for key, value in arguments.items():
            if key not in actions[action]:
                raise ToolError("action %s takes %s, not %s" % (action, ", ".join(sorted(actions[action])) or "no arguments", key))
            why = catalog.check_argument(actions[action][key], value)
            if why:
                raise ToolError("%s.%s %s" % (action, key, why))
        rules = self.device_limits(device, info)
        key = (device["metadata"].get("namespace", "default"), device["metadata"]["name"])
        refusal = lim.check_action(rules, action, arguments, self.last_action_at.get(key, {}), time.time())
        if refusal:
            self.log("kube32 mcp: refused %s %s %s: %s" % (ref, action, json.dumps(arguments), refusal.splitlines()[0]))
            raise ToolError("Device %s: %s" % (ref, refusal))
        body = dict(arguments, action=action)
        try:
            status, answer = driver_request(pod, DRIVER_TIMEOUT, method="POST", body=body)
        except (OSError, ValueError) as e:
            raise ToolError("the driver of %s did not answer the action: %s" % (ref, e))
        self.last_action_at.setdefault(key, {})[action] = time.time()
        self.log("kube32 mcp: sent %s %s to %s: http %d" % (action, json.dumps(arguments), ref, status))
        self.record_event(device, action, arguments, status)
        if status != 200:
            raise ToolError("the driver of %s answered %d: %s" % (ref, status, answer.get("error", answer)))
        return {"device": ref, "action": action, "arguments": arguments, "answer": answer}

    def record_event(self, device, action, arguments, status):
        """An Event on the Device for each action sent, for `kubectl describe device`."""
        meta = device["metadata"]
        ns = meta.get("namespace", "default")
        event = {"apiVersion": "v1", "kind": "Event",
                 "metadata": {"name": "%s.%s" % (meta["name"][:200], os.urandom(4).hex()), "namespace": ns},
                 "involvedObject": {"apiVersion": "kube32.io/v1", "kind": "Device", "name": meta["name"], "namespace": ns,
                                    "uid": meta.get("uid", "")},
                 "reason": "Action", "type": "Normal" if status == 200 else "Warning",
                 "message": "kube32 mcp sent %s %s: http %d" % (action, json.dumps(arguments), status),
                 "source": {"component": MANAGER}, "firstTimestamp": _now_iso(), "lastTimestamp": _now_iso(), "count": 1}
        try:
            api_call(self.kube, "create", "/api/v1/namespaces/%s/events" % ns, json.dumps(event).encode())
        except ApiError as e:
            self.log("kube32 mcp: no Event for the action: %s" % e)

    def describe_device(self, args, ctx):
        device = self.find_device(args["device"])
        answer, describe = None, None
        if args.get("live", True):
            device, pod, answer = self.start_driver(device, ctx)
            try:
                status, body = driver_request(pod, DRIVER_TIMEOUT, path="/describe")
                describe = body if status == 200 else None
            except (OSError, ValueError):
                describe = None
        info = self.driver_info(device, describe)
        rules = self.device_limits(device, info)
        warnings = lim.check_readings(rules, (answer or {}).get("readings") or {})
        return reference.render_device(device, info, rules, answer, warnings)

    # -- pods ------------------------------------------------------------------
    def workload(self, name):
        """("Pod" or "Deployment", the object), or (None, None)."""
        ns = self.namespace
        pod = self.get("/api/v1/namespaces/%s/pods/%s" % (ns, name))
        if pod is not None:
            return "Pod", pod
        deploy = self.get("/apis/apps/v1/namespaces/%s/deployments/%s" % (ns, name))
        if deploy is not None:
            return "Deployment", deploy
        return None, None

    def pods_of(self, kind, obj):
        if kind == "Pod":
            return [obj]
        selector = ",".join("%s=%s" % kv for kv in sorted(((obj.get("spec") or {}).get("selector") or {}).get("matchLabels", {}).items()))
        items = (self.api("get", "/api/v1/namespaces/%s/pods?labelSelector=%s" % (self.namespace, quote(selector, safe=""))) or {}).get("items", [])
        return sorted(items, key=lambda p: p["metadata"]["name"])

    def pod_logs(self, name, tail, previous=False):
        base = kubectl_base(self.kube)
        missing = kubectl_missing(base)
        if missing:
            raise ToolError(missing)
        cmd = base + ["logs", name, "-n", self.namespace, "--tail=%d" % tail] + (["--previous"] if previous else [])
        rc, out, err = kubectl(cmd)
        return out.rstrip("\n") if rc == 0 else "(no log: %s)" % _first_line(err)

    def pod_events(self, name):
        selector = quote("involvedObject.name=" + name, safe="")
        items = (self.api("get", "/api/v1/namespaces/%s/events?fieldSelector=%s" % (self.namespace, selector)) or {}).get("items", [])
        return ["%s %s: %s" % (e.get("type", ""), e.get("reason", ""), e.get("message", "")) for e in items][-10:]

    def workload_status(self, args, ctx):
        name = args["name"]
        tail = args.get("tail_lines", 40)
        if not 1 <= tail <= 500:
            raise ToolError("tail_lines is 1 to 500")
        kind, obj = self.workload(name)
        if kind is None:
            raise ToolError("no pod or Deployment %s in namespace %s" % (name, self.namespace))
        out = {"kind": kind, "name": name, "namespace": self.namespace,
               "managed_by_kube32_mcp": (obj["metadata"].get("labels") or {}).get(MANAGED_BY) == MANAGER}
        if kind == "Deployment":
            st = obj.get("status") or {}
            out["replicas"] = {"desired": (obj.get("spec") or {}).get("replicas", 1), "ready": st.get("readyReplicas", 0)}
        pods = []
        for p in self.pods_of(kind, obj):
            entry = pod_summary(p)
            entry["events"] = self.pod_events(p["metadata"]["name"])
            entry["log"] = self.pod_logs(p["metadata"]["name"], tail, args.get("previous", False))
            pods.append(entry)
        out["pods"] = pods
        return out

    def delete_workload(self, args, ctx):
        self.require("deploy")
        name = args["name"]
        kind, obj = self.workload(name)
        if kind is None:
            raise ToolError("no pod or Deployment %s in namespace %s" % (name, self.namespace))
        if (obj["metadata"].get("labels") or {}).get(MANAGED_BY) != MANAGER:
            raise ToolError("%s %s was not deployed by kube32 mcp (no label %s=%s): delete it with kubectl"
                            % (kind, name, MANAGED_BY, MANAGER))
        self.delete(kind, name)
        return {"deleted": "%s %s/%s" % (kind, self.namespace, name)}

    def delete(self, kind, name):
        path = ("/api/v1/namespaces/%s/pods/%s" if kind == "Pod" else "/apis/apps/v1/namespaces/%s/deployments/%s") % (self.namespace, name)
        self.api("delete", path)
        return path

    def deploy_code(self, args, ctx):
        self.require("deploy")
        name = args["name"]
        kind = args.get("kind", "Pod")
        longest = 63 if kind == "Pod" else 40     # a Deployment's pods add a hash and a suffix to its name
        if not _NAME_RE.match(name) or len(name) > longest:
            raise ToolError("name: lowercase letters, digits and '-', at most %d characters for a %s" % (longest, kind))
        if ("source" in args) == ("path" in args):
            raise ToolError("give either source (C code) or path (a .c or .wasm file, or a C or Rust project directory)")
        env = args.get("env") or {}
        for k, v in env.items():
            if not _ENV_RE.match(k) or not isinstance(v, (str, int, float, bool)):
                raise ToolError("env: names are letters, digits and '_', values strings or numbers (%s)" % k)
        device = self.find_device(args["device"]) if args.get("device") else None
        node = args.get("node")
        if node is None and device is not None and args.get("near_device", False):
            node = (device.get("status") or {}).get("nodeName") or device["spec"].get("nodeName")
        wait = args.get("wait_seconds", 20)
        if not 0 <= wait <= 120:
            raise ToolError("wait_seconds is 0 to 120")
        memory = None
        if args.get("memory"):
            try:
                memory = parse_quantity(args["memory"])
            except ValueError as e:
                raise ToolError("memory: %s" % e)

        existing_kind, existing = self.workload(name)
        if existing is not None and (existing["metadata"].get("labels") or {}).get(MANAGED_BY) != MANAGER:
            raise ToolError("%s %s exists and was not deployed by kube32 mcp: pick another name" % (existing_kind, name))

        ctx.progress("building %s" % name)
        data, wasm_path, build_log = self.build(name, args, memory)
        res = check_module(wasm_path, data)
        refusal = deploy_refusal(res, self.allow_writes)
        if refusal:
            raise ToolError("%s\n\n%s" % (refusal, format_report(res)))
        used = static_memory_bytes(res.module)
        if memory and used and memory < used and res.memory_bytes <= PAGE:
            raise ToolError("memory %s is less than the %s bytes that the module's static data and stack use"
                            % (format_quantity(memory), used))
        limit = memory or pod_memory(res)
        res = check_module(wasm_path, data, memory_limit=limit)
        if not res.ok:
            raise ToolError("the module does not fit memory %s:\n%s" % (format_quantity(limit), format_report(res)))

        manifest = build_manifest(name, self.namespace, kind, data, res, limit, node, device, env)
        if existing is not None:      # deploy_code's own, replaced once the new module builds and checks
            ctx.progress("replacing %s %s" % (existing_kind, name))
            self.delete(existing_kind, name)
            self.wait_gone(existing_kind, name)
        path = ("/api/v1/namespaces/%s/pods" if kind == "Pod" else "/apis/apps/v1/namespaces/%s/deployments") % self.namespace
        self.api("create", path, manifest)
        self.log("kube32 mcp: deployed %s %s/%s (%d bytes, sha256 %s)" % (kind, self.namespace, name, len(data),
                                                                           hashlib.sha256(data).hexdigest()[:12]))
        ctx.progress("waiting for %s to start" % name)
        out = {"deployed": "%s %s/%s" % (kind, self.namespace, name), "module": "%s, %d bytes" % (wasm_path, len(data)),
               "kind": res.kind, "memory": format_quantity(limit), "hardware": res.hardware,
               "node": node or "chosen by the scheduler"}
        if device is not None:
            out["device"] = {"device": reference.device_ref(device), "topic": "device." + device["metadata"]["name"],
                             "url": device_url(device)}
        if build_log.strip():
            out["build"] = build_log.strip().splitlines()[-12:]
        # With a Device, the pod's first reading arrives within one reading period of its start
        settle = 0.0
        if device is not None:
            settle = (device["spec"].get("intervalMs") or self.driver_info(device).get("period_ms") or 1000) / 1000.0 + 1.0
        out.update(self.wait_started(kind, name, wait, settle))
        return out

    def build(self, name, args, memory):
        """Builds the source with `kube32 build` (a .wasm file is taken as it is): (module bytes, its path,
        the build's output)."""
        work = os.path.join(releases.cache_dir(), "mcp", name)
        stack = args.get("stack", "4Ki")
        if "path" in args and args["path"].endswith(".wasm"):
            try:
                return read_wasm(args["path"]), os.path.abspath(args["path"]), ""
            except FileNotFoundError as e:
                raise ToolError(str(e))
        if "source" in args or args["path"].endswith(".c"):
            src_dir = os.path.join(work, "src")
            shutil.rmtree(src_dir, ignore_errors=True)
            os.makedirs(src_dir)
            dest = os.path.join(src_dir, name + ".c")
            if "source" in args:
                with open(dest, "w") as f:
                    f.write(args["source"])
            else:
                try:
                    shutil.copyfile(args["path"], dest)
                except OSError as e:
                    raise ToolError("cannot read %s: %s" % (args["path"], e.strerror or e))
            project = src_dir
        else:
            project = args["path"]
            if not os.path.isdir(project):
                raise ToolError("no such file or directory: %s (path takes a .c or .wasm file, or a project directory)" % project)
        pages = max(1, -(-(memory or PAGE) // PAGE))
        out_path = os.path.join(work, name + ".wasm")
        build_args = argparse.Namespace(dir=project, lang=None, name=name, out=out_path, artifact=None,
                                        memory=format_quantity(pages * PAGE), stack=stack,
                                        target="wasm32-unknown-unknown", verbose=False)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            rc = cmd_build(build_args)
        text = buf.getvalue()
        if rc != 0 or not os.path.isfile(out_path):
            raise ToolError("the build failed (kube32 build exit %d):\n%s" % (rc, text.strip()[-6000:]))
        return read_wasm(out_path), out_path, text

    def wait_gone(self, kind, name, timeout=60):
        deadline = time.time() + timeout
        while self.workload(name)[1] is not None:
            if time.time() >= deadline:
                raise ToolError("%s %s did not go within %d s" % (kind, name, timeout))
            time.sleep(0.5)

    def wait_started(self, kind, name, wait, settle=0.0):
        """Polls until the pod has run for `settle` seconds and has logged, stops, or `wait` seconds pass;
        its status and log."""
        deadline = time.time() + wait
        summary, log, running_since = None, "", None
        while True:
            _, obj = self.workload(name)
            pods = self.pods_of(kind, obj) if obj is not None else []
            if pods:
                pod = pods[0]
                summary = pod_summary(pod)
                state = summary.get("state", "")
                if summary["phase"] in ("Running", "Succeeded", "Failed") or state.startswith("waiting"):
                    log = self.pod_logs(pod["metadata"]["name"], 40)
                stopped = summary["phase"] in ("Succeeded", "Failed") or "CrashLoopBackOff" in state or "Error" in state
                if summary["phase"] == "Running" and running_since is None:
                    running_since = time.time()
                logged = bool(log) and not log.startswith("(no log")
                if stopped or (logged and running_since is not None and time.time() - running_since >= settle):
                    break
            if time.time() >= deadline:
                break
            time.sleep(1.0)
        if summary is None:
            return {"status": "no pod yet after %d s; workload_status %s follows it" % (wait, name)}
        events = self.pod_events(summary["pod"])
        return {"status": summary, "log": log or "(nothing logged yet)", "events": events}


def summarize(rows):
    """min, max, first, last and the change per minute of each numeric reading over the samples."""
    series = {}
    for r in rows:
        for k, v in (r.get("readings") or {}).items():
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                series.setdefault(k, []).append((r["t_s"], v))
    out = {}
    for k, pts in series.items():
        vals = [v for _, v in pts]
        entry = {"min": min(vals), "max": max(vals), "first": vals[0], "last": vals[-1]}
        span = pts[-1][0] - pts[0][0]
        if span > 0:
            entry["change_per_min"] = round((vals[-1] - vals[0]) * 60.0 / span, 3)
            steps = [abs(b[1] - a[1]) * 60.0 / (b[0] - a[0]) for a, b in zip(pts, pts[1:]) if b[0] > a[0]]
            if steps:
                entry["fastest_change_per_min"] = round(max(steps), 3)
        out[k] = entry
    return out


def sample_table(rows):
    """The samples as columns and rows. The same uptime_ms twice is one measurement read twice (samples
    closer than the reading period)."""
    keys = []
    for r in rows:
        keys += [k for k in r.get("readings") or {} if k not in keys]
    columns = ["t_s", "uptime_ms"] + keys
    table = {"columns": columns, "rows": [[r["t_s"], r.get("uptime_ms")] + [(r.get("readings") or {}).get(k) for k in keys]
                                          for r in rows if "readings" in r]}
    errors = ["t_s %s: %s" % (r["t_s"], r["error"]) for r in rows if "error" in r]
    if errors:
        table["errors"] = errors
    return table


def pod_summary(pod):
    meta, spec, st = pod["metadata"], pod.get("spec", {}), pod.get("status", {}) or {}
    out = {"pod": meta["name"], "phase": st.get("phase", "Pending"), "node": spec.get("nodeName") or "not scheduled"}
    if st.get("reason") or st.get("message"):
        out["message"] = ("%s: %s" % (st.get("reason", ""), st.get("message", ""))).strip(": ")
    cs = (st.get("containerStatuses") or [{}])[0]
    if cs:
        out["restarts"] = cs.get("restartCount", 0)
        state = cs.get("state") or {}
        for key, val in state.items():
            detail = val.get("reason") or ""
            if key == "terminated":
                detail = "%s exit %s %s" % (detail, val.get("exitCode"), val.get("message", ""))
            elif val.get("message"):
                detail = "%s: %s" % (detail, val["message"])
            out["state"] = ("%s %s" % (key, detail)).strip()
        last = (cs.get("lastState") or {}).get("terminated")
        if last:
            out["last_termination"] = "%s exit %s %s" % (last.get("reason", ""), last.get("exitCode"), last.get("message", ""))
    for c in st.get("conditions") or []:
        if c.get("type") == "PodScheduled" and c.get("status") != "True":
            out["scheduling"] = "%s: %s" % (c.get("reason", ""), c.get("message", ""))
    return out


def deploy_refusal(res, allow_writes):
    """Why deploy_code does not deploy a checked module, or None."""
    if not res.ok:
        return "the module fails the ABI check"
    if res.peripheral_imports:
        return ("the module imports %s: a pod from deploy_code gets no bus or pin. It reaches parts through their "
                "Device drivers (events, or k32_http_call to the Device's Service)" % ", ".join(res.peripheral_imports))
    other = [h for h in res.hardware if h not in DEPLOY_HARDWARE]
    if other:
        return ("the module uses %s: deploy_code grants events (ipc) and Service calls only. Deploy modules that use "
                "the display, keyboard, speaker, SD card or chip temperature with `kube32 deploy`" % ", ".join(other))
    if "service-call" in res.hardware and not allow_writes:
        return ("the module imports k32_http_call, which can send actions to Devices past their limits: the server "
                "deploys it only with --allow-writes. Read Devices through events (the device argument) instead")
    return None


def pod_memory(res):
    """The pod's memory limit: what the module's static layout uses, in KiB, when it tells (C and Rust
    from kube32 build); else its declared linear memory."""
    used = static_memory_bytes(res.module)
    if used is None or res.memory_bytes > PAGE:
        return res.memory_bytes or PAGE
    return min(res.memory_bytes or PAGE, max(1024, -(-used // 1024) * 1024))


def device_url(device):
    ns = device["metadata"].get("namespace", "default")
    return "http://%s%s/" % (device["metadata"]["name"], "" if ns == "default" else "." + ns)


def build_manifest(name, namespace, kind, data, res, memory, node, device, env):
    """A Pod or Deployment that carries the module inline (kube32.io/wasm-base64), labeled as deploy_code's."""
    digest = hashlib.sha256(data).hexdigest()
    labels = {"app": name, MANAGED_BY: MANAGER}
    annotations = {"kube32.io/wasm-base64": base64.b64encode(data).decode("ascii"), "kube32.io/wasm-sha256": digest,
                   "kube32.io/wasm-size": str(len(data))}
    env_list = []
    if device is not None:
        dname = device["metadata"]["name"]
        annotations["kube32.io/event-topics"] = "device." + dname
        env_list += [{"name": "DEVICE_NAME", "value": dname},
                     {"name": "DEVICE_NAMESPACE", "value": device["metadata"].get("namespace", "default")},
                     {"name": "DEVICE_TOPIC", "value": "device." + dname},
                     {"name": "DEVICE_URL", "value": device_url(device)}]
    env_list += [{"name": k, "value": json.dumps(v) if isinstance(v, bool) else str(v)} for k, v in sorted(env.items())]
    quantity = format_quantity(memory)
    resources = {"memory": quantity}
    resources.update({HARDWARE[h][0]: "1" for h in res.hardware if HARDWARE[h][0]})
    container = {"name": name, "image": "wasm://inline/%s-%s" % (name, digest[:12]), "env": env_list,
                 "resources": {"requests": dict(resources), "limits": dict(resources)}}
    if res.hardware:
        container["securityContext"] = {"capabilities": {"add": [HARDWARE[h][1] for h in res.hardware]}}
    if res.kind == "request":
        container["ports"] = [{"containerPort": 7000, "name": "http"}]
    spec = {"restartPolicy": "Always", "enableServiceLinks": False, "containers": [container]}
    if node:
        spec["nodeSelector"] = {"kubernetes.io/hostname": node}
    if kind == "Pod":
        return {"apiVersion": "v1", "kind": "Pod",
                "metadata": {"name": name, "namespace": namespace, "labels": labels, "annotations": annotations}, "spec": spec}
    return {"apiVersion": "apps/v1", "kind": "Deployment", "metadata": {"name": name, "namespace": namespace, "labels": labels},
            "spec": {"replicas": 1, "selector": {"matchLabels": {"app": name}},
                     "template": {"metadata": {"labels": labels, "annotations": annotations}, "spec": spec}}}


# ---------------------------------------------------------------------------
# Definitions
# ---------------------------------------------------------------------------
DEVICE_ARG = {"type": "string", "description": "The Device: its name, or namespace/name (list_devices lists them)"}


def tools(k):
    return [
        Tool("list_devices", "List nodes and Devices",
             "The cluster's nodes (role, board, memory, the I2C parts each one found) and its Devices: driver, part, "
             "node, address, phase, what each reads (with units), its actions, and its owner's notes and limits. "
             "Starts no driver. Also tells whether this server allows writes and deploys.",
             {}, [], k.list_devices, idempotent=True),
        Tool("read_device", "Read a Device",
             "Reads a Device's driver: the latest readings with units. An Idle OnDemand Device's driver starts first "
             "(about 2 s on a board). With samples > 1 it reads every interval_seconds (default: the reading period) and "
             "adds min, max and the change per minute of each reading: use it to learn how the part behaves over time.",
             {"device": DEVICE_ARG,
              "samples": {"type": "integer", "description": "Readings to take, 1 to 120 (default 1)"},
              "interval_seconds": {"type": "number", "description": "Seconds between samples (default: the Device's reading period); "
                                                                     "samples x interval at most 300"}},
             ["device"], k.read_device),
        Tool("write_device", "Send an action to a Device",
             "Sends an action to a Device's driver, such as tare or led for a scale, or set for a switch. The server "
             "checks the action against the driver's actions and the Device's kube32.io/limits, and refuses it with the "
             "reason when it falls outside them. Needs a server started with --allow-writes. Moves real hardware: send "
             "only what the user asked for.",
             {"device": DEVICE_ARG,
              "action": {"type": "string", "description": "The action, as describe_device lists it"},
              "arguments": {"type": "object", "description": "The action's arguments, e.g. {\"r\": 0, \"g\": 32, \"b\": 0}"}},
             ["device", "action"], k.write_device, read_only=False, destructive=True),
        Tool("describe_device", "Describe a Device",
             "The Device's reference document (Markdown): the part, node and address, its readings with units and "
             "latest values, its actions, the limits and notes its owner wrote (annotations kube32.io/limits and "
             "kube32.io/notes), and how its driver behaves. Starts the driver of an Idle Device unless live is false.",
             {"device": DEVICE_ARG,
              "live": {"type": "boolean", "description": "Read the driver for the latest values and its own action list (default true)"}},
             ["device"], k.describe_device),
        Tool("deploy_code", "Build and deploy a pod",
             "Builds C source (or a .c file, a C or Rust project directory, or a .wasm file) to WebAssembly with "
             "`kube32 build`, checks it against the pod ABI, and deploys it as a Pod or Deployment that carries the "
             "module. With device, the pod receives each reading of the Device in k32_event (kube32.io/event-topics "
             "device.<name>) and gets DEVICE_NAME, DEVICE_TOPIC and DEVICE_URL. Waits for the pod to start and returns "
             "its status and first log lines. Replaces a pod or Deployment of the same name that deploy_code created. "
             "Needs --allow-deploy. Read kube32://guides/device-pod first.",
             {"name": {"type": "string", "description": "Pod or Deployment name (lowercase, digits, '-')"},
              "source": {"type": "string", "description": "C source of one file; include \"runtime/k32_app_abi.h\""},
              "path": {"type": "string", "description": "Instead of source: a .c file, a .wasm file, or a C or Rust project directory"},
              "device": DEVICE_ARG,
              "near_device": {"type": "boolean", "description": "Run on the Device's node (default false: the scheduler picks)"},
              "node": {"type": "string", "description": "Run on this node"},
              "kind": {"type": "string", "enum": ["Pod", "Deployment"], "description": "Default Pod"},
              "env": {"type": "object", "description": "Environment variables, e.g. {\"THRESHOLD\": \"10\"}"},
              "memory": {"type": "string", "description": "Linear memory limit, e.g. 8Ki (default: what the module's static data and stack use)"},
              "stack": {"type": "string", "description": "Shadow stack for C and Rust (default 4Ki)"},
              "wait_seconds": {"type": "integer", "description": "How long to wait for the first log line, 0 to 120 (default 20)"}},
             ["name"], k.deploy_code, read_only=False),
        Tool("workload_status", "Status and log of a pod",
             "The status of a pod or Deployment in the server's namespace: phase, node, restarts, the container's state "
             "and exit reason, recent Events, and the last log lines of each pod.",
             {"name": {"type": "string", "description": "Pod or Deployment name"},
              "tail_lines": {"type": "integer", "description": "Log lines per pod, 1 to 500 (default 40)"},
              "previous": {"type": "boolean", "description": "The log of the previous container, after a restart"}},
             ["name"], k.workload_status, idempotent=True),
        Tool("delete_workload", "Delete a deployed pod",
             "Deletes a pod or Deployment that deploy_code created. Refuses anything else. Needs --allow-deploy.",
             {"name": {"type": "string", "description": "Pod or Deployment name"}},
             ["name"], k.delete_workload, read_only=False, destructive=True, idempotent=True),
    ]


# ---------------------------------------------------------------------------
# Resources and the prompt
# ---------------------------------------------------------------------------
_DEVICE_URI_RE = re.compile(r"^kube32://devices/([^/]+)/([^/]+)$")


def resources(k):
    def listing():
        out = [{"uri": reference.GUIDE_URI, "name": "device-pod-guide", "title": "Writing a pod that uses a Device",
                "mimeType": "text/markdown"},
               {"uri": reference.ABI_URI, "name": "k32_app_abi.h", "title": "The pod ABI header for C", "mimeType": "text/x-c"},
               {"uri": reference.REFERENCE_URI, "name": "reference", "title": "Every Device's reference document",
                "mimeType": "text/markdown"}]
        try:
            devices = k.devices()
        except ToolError:
            devices = []
        for d in devices:
            ns, name = d["metadata"].get("namespace", "default"), d["metadata"]["name"]
            out.append({"uri": reference.DEVICE_URI % (ns, name), "name": "%s/%s" % (ns, name),
                        "title": "Device %s (%s)" % (name, d.get("spec", {}).get("driver", "?")), "mimeType": "text/markdown"})
        return out

    def read(uri):
        if uri == reference.GUIDE_URI:
            return "text/markdown", reference.GUIDE
        if uri == reference.ABI_URI:
            with open(shared_file("include/runtime/k32_app_abi.h")) as f:
                return "text/x-c", f.read()
        if uri == reference.REFERENCE_URI:
            docs = []
            for d in k.devices():
                info = k.driver_info(d)
                docs.append(reference.render_device(d, info, k.device_limits(d, info)))
            return "text/markdown", "\n".join(docs) or "No Devices.\n"
        m = _DEVICE_URI_RE.match(uri)
        if m:
            try:
                d = k.find_device("%s/%s" % m.groups())
            except ToolError:
                raise KeyError(uri)
            info = k.driver_info(d)
            return "text/markdown", reference.render_device(d, info, k.device_limits(d, info))
        raise KeyError(uri)

    templates = [{"uriTemplate": "kube32://devices/{namespace}/{name}", "name": "device",
                  "title": "A Device's reference document", "mimeType": "text/markdown"}]
    return listing, templates, read


def prompts():
    def get(args):
        text = reference.PROMPT_TEXT.format(device=args["device"], goal=args["goal"], guide=reference.GUIDE_URI)
        return {"description": reference.PROMPT["description"],
                "messages": [{"role": "user", "content": {"type": "text", "text": text}}]}
    return {reference.PROMPT_NAME: (reference.PROMPT, get)}
