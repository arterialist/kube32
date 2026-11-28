"""The MCP server's tools, resources and prompt: Devices and pods of a kube32 cluster.

The server reaches the API server with kubectl, so kubeconfig contexts and credentials apply,
and reads drivers on their nodes' pod servers as `kube32 devices` does.
"""
import argparse
import base64
import contextlib
import hashlib
import http.client
import io
import json
import os
import re
import shutil
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from urllib.parse import quote, urlencode

from .. import releases
from ..build import cmd_build
from ..cluster import driver_address, driver_request
from ..common import format_quantity, parse_quantity, shared_file
from ..kube import ApiError, api_call, api_call_raw, kubectl, kubectl_base, kubectl_missing
from ..pods import _ENV_RE, _NAME_RE
from ..wasm import HARDWARE, PAGE, check_module, format_report, read_wasm, static_memory_bytes
from . import catalog, limits as lim, reference
from .protocol import Tool, ToolError

DEVICES = "/apis/kube32.io/v1/devices"
MANAGED_BY = "app.kubernetes.io/managed-by"
MANAGER = "kube32-mcp"
MCP_TOOLS_ANNOTATION = "kube32.io/mcp-tools"
_TOOL_NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_-]{0,63}$")
# What deploy_code grants a pod: events and Service calls. Hardware stays with the drivers.
DEPLOY_HARDWARE = ("ipc", "service-call")
NO_READING_YET = "no reading yet"
MAX_SAMPLE_SECONDS = 300
ACTIVATION_TIMEOUT = 20.0
DRIVER_TIMEOUT = 3.0
SERVICE_TIMEOUT = 8.0
DEFAULT_CACHE_TTL = 300.0
DEFAULT_WORKLOADS_TTL = 60.0
ERROR_BACKOFF_S = 15.0


def _env_float(name, default):
    raw = (os.environ.get(name) or "").strip()
    if not raw:
        return default
    try:
        val = float(raw)
        return val if val >= 0.0 else default
    except ValueError:
        return default


def _now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _first_line(text):
    lines = [x for x in (text or "").strip().splitlines() if x.strip()]
    return lines[-1] if lines else ""


class Kube32:
    """One cluster, reached through kubectl with `kube` (argparse namespace of kubectl, kubeconfig,
    context and server). Writes and deploys need the server's opt-in."""

    def __init__(self, kube, namespace="default", allow_writes=False, allow_deploy=False, log=None, ingress_url=None):
        self.kube = kube
        self.namespace = namespace
        self.allow_writes = allow_writes
        self.allow_deploy = allow_deploy
        self.log = log or (lambda text: None)
        self.ingress_url = (ingress_url or "").strip().rstrip("/") or None
        self.cache_ttl = _env_float("KUBE32_MCP_CACHE_TTL", DEFAULT_CACHE_TTL)
        self.workloads_ttl = min(_env_float("KUBE32_MCP_WORKLOADS_TTL", DEFAULT_WORKLOADS_TTL),
                                 self.cache_ttl if self.cache_ttl > 0 else 0.0)
        self.last_action_at = {}      # (namespace, device) -> {action: time.time() of the last one sent}
        self._svc_tools_cache = (0.0, [])
        self._svc_tools_last_attempt = 0.0
        self._endpoint_cache = {}     # (ns, ref) -> (ts, ns, pod_name, ip, port, node_name, svc_name, is_local)
        self._workloads_cache = {}    # ns -> (ts, result_dict)

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
            hw_caps = sorted(k[len("hardware.kube32.io/"):] for k, v in (st.get("allocatable") or {}).items()
                             if k.startswith("hardware.kube32.io/") and str(v) not in ("0", ""))
            if hw_caps:
                entry["hardware"] = hw_caps
            slots = {k[len("mosaico.kube32.io/"):]: v for k, v in labels.items() if k.startswith("mosaico.kube32.io/")}
            if slots:
                entry["mosaico_slots"] = slots
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

    def read_pod_logs(self, args, ctx):
        name = args["name"]
        tail = args.get("tail_lines", 50)
        if not 1 <= tail <= 500:
            raise ToolError("tail_lines is 1 to 500")
        previous = bool(args.get("previous", False))
        # Fast path: single `kubectl logs` call for a Pod (avoids extra API roundtrips)
        base = kubectl_base(self.kube)
        missing = kubectl_missing(base)
        if missing:
            raise ToolError(missing)
        cmd = base + ["logs", name, "-n", self.namespace, "--tail=%d" % tail] + (["--previous"] if previous else [])
        rc, out, _ = kubectl(cmd)
        if rc == 0:
            return {
                "pod": name,
                "namespace": self.namespace,
                "tail_lines": tail,
                "previous": previous,
                "log": out.rstrip("\n"),
            }
        # Fallback: Deployment name or pod error lookup
        kind, obj = self.workload(name)
        if kind is None:
            raise ToolError("no pod or Deployment %s in namespace %s" % (name, self.namespace))
        pods = self.pods_of(kind, obj)
        if not pods:
            raise ToolError("%s %s has no pods in namespace %s" % (kind, name, self.namespace))
        pod = pods[0]
        pod_name = pod["metadata"]["name"]
        return {
            "pod": pod_name,
            "namespace": self.namespace,
            "node": (pod.get("spec") or {}).get("nodeName") or "not scheduled",
            "phase": (pod.get("status") or {}).get("phase", "Pending"),
            "tail_lines": tail,
            "previous": previous,
            "log": self.pod_logs(pod_name, tail, previous),
        }

    # -- Services & Workloads --------------------------------------------------
    def _node_map(self):
        nodes = (self.get("/api/v1/nodes") or {}).get("items", [])
        return {n["metadata"]["name"]: n for n in nodes if (n.get("metadata") or {}).get("name")}

    def _node_pod_port(self, node_obj):
        if not node_obj:
            return 0
        st = node_obj.get("status") or {}
        port = ((st.get("daemonEndpoints") or {}).get("kubeletEndpoint") or {}).get("Port", 0)
        return int(port) if isinstance(port, int) and 0 < port <= 65535 else 0

    def _pod_endpoint(self, pod, nodes=None):
        """(ip, port, node_obj) for a Pod, preferring Node kubeletEndpoint.Port over containerPort."""
        st = pod.get("status") or {}
        spec = pod.get("spec") or {}
        node_name = spec.get("nodeName") or ""
        node_obj = (nodes or {}).get(node_name)
        ip = st.get("podIP") or ""
        if not ip and node_obj:
            for addr in (node_obj.get("status") or {}).get("addresses") or []:
                if addr.get("type") == "InternalIP" and addr.get("address"):
                    ip = addr["address"]
                    break
        port = self._node_pod_port(node_obj)
        if not port:
            for c in spec.get("containers") or []:
                for p in c.get("ports") or []:
                    if p.get("name") == "http" and p.get("containerPort"):
                        port = int(p["containerPort"])
                        break
                if port:
                    break
        if not port:
            for c in spec.get("containers") or []:
                for p in c.get("ports") or []:
                    if p.get("containerPort"):
                        port = int(p["containerPort"])
                        break
                if port:
                    break
        return ip, (port or 7000), node_obj

    def _parse_ref(self, ref, default_ns=None):
        ns, sep, name = str(ref or "").rpartition("/")
        return (ns if sep else (default_ns or self.namespace)), name

    def _resolve_service_pod(self, ref, default_ns=None):
        """Resolve a Service or Pod reference to (namespace, pod_obj, service_name_or_None)."""
        ns, name = self._parse_ref(ref, default_ns)
        if not name:
            raise ToolError("service name must not be empty")
        svc = self.get("/api/v1/namespaces/%s/services/%s" % (ns, name))
        if svc is not None:
            selector = (svc.get("spec") or {}).get("selector") or {}
            if selector:
                sel = ",".join("%s=%s" % kv for kv in sorted(selector.items()))
                pods = (self.get("/api/v1/namespaces/%s/pods?labelSelector=%s" % (ns, quote(sel, safe=""))) or {}).get("items", [])
                running = [p for p in pods if (p.get("status") or {}).get("phase") == "Running"]
                chosen = next((p for p in running if (p.get("status") or {}).get("podIP")), None) or (running[0] if running else (pods[0] if pods else None))
                if chosen is not None:
                    return ns, chosen, name
            # Service exists but selector matched nothing (or selector empty); try same-named pod
            pod = self.get("/api/v1/namespaces/%s/pods/%s" % (ns, name))
            if pod is not None:
                return ns, pod, name
            raise ToolError("Service %s/%s has no running backend pods" % (ns, name))
        pod = self.get("/api/v1/namespaces/%s/pods/%s" % (ns, name))
        if pod is not None:
            return ns, pod, None
        raise ToolError("no Service or Pod %s in namespace %s (list_workloads lists them)" % (name, ns))

    def _ingress_prefix(self, ns, service_name):
        if not self.ingress_url or not service_name:
            return None
        ings = (self.get("/apis/networking.k8s.io/v1/namespaces/%s/ingresses" % ns) or {}).get("items", [])
        matches = []
        for ing in ings:
            for rule in (ing.get("spec") or {}).get("rules") or []:
                for p in ((rule.get("http") or {}).get("paths") or []):
                    backend = ((p.get("backend") or {}).get("service") or {}).get("name")
                    if backend == service_name and p.get("path"):
                        matches.append(p["path"].rstrip("/"))
        if not matches:
            return None
        matches.sort(key=len, reverse=True)
        return matches[0]

    def _raw_http_json(self, url, method, headers, body_bytes, timeout):
        req = urllib.request.Request(url, data=body_bytes, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                status = resp.status
                raw = resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            status = e.code
            raw = e.read().decode("utf-8", "replace")
        except http.client.HTTPException as e:
            raise OSError("%s: %s" % (type(e).__name__, e))
        try:
            parsed = json.loads(raw) if raw.strip() else {}
        except ValueError:
            parsed = {"text": raw}
        return status, parsed

    def _resolve_endpoint_cached(self, ref, default_ns=None, force=False):
        cache_key = (default_ns or self.namespace, str(ref or ""))
        now = time.time()
        entry = self._endpoint_cache.get(cache_key)
        if not force and entry and self.cache_ttl > 0 and (now - entry[0]) < self.cache_ttl:
            return entry[1:]
        try:
            ns, pod, svc_name = self._resolve_service_pod(ref, default_ns=default_ns)
            nodes = self._node_map()
        except ToolError as e:
            if not force and entry:
                self.log("kube32 mcp: keeping cached endpoint for %s after API error: %s" % (ref, e))
                # Back off API retries without dropping a working local endpoint
                self._endpoint_cache[cache_key] = (now - self.cache_ttl + ERROR_BACKOFF_S,) + entry[1:]
                return entry[1:]
            raise
        ip, port, node_obj = self._pod_endpoint(pod, nodes)
        pod_name = pod["metadata"]["name"]
        node_name = (pod.get("spec") or {}).get("nodeName") or "?"
        labels = ((node_obj or {}).get("metadata") or {}).get("labels") or {}
        is_local = labels.get("kube32.io/board") == "macos" or labels.get("kube32.io/simulator") == "true"
        resolved = (ns, pod_name, ip, port, node_name, svc_name, is_local)
        if ip:
            self._endpoint_cache[cache_key] = (now,) + resolved
        return resolved

    def _http_service_call(self, ref, method="GET", path="/", query=None, body=None, timeout=SERVICE_TIMEOUT, default_ns=None):
        method = str(method or "GET").upper()
        if method not in ("GET", "POST", "PUT", "DELETE"):
            raise ToolError("method must be GET, POST, PUT or DELETE")
        if method != "GET":
            self.require("writes")
        req_path = "/" + str(path or "/").lstrip("/")
        if query:
            pairs = []
            for k, v in query.items():
                if v is None:
                    continue
                if isinstance(v, bool):
                    pairs.append((k, "true" if v else "false"))
                elif isinstance(v, list):
                    pairs.append((k, ",".join(str(x) for x in v)))
                else:
                    pairs.append((k, str(v)))
            if pairs:
                sep = "&" if "?" in req_path else "?"
                req_path = req_path + sep + urlencode(pairs)
        body_bytes = None
        if body is not None:
            body_bytes = json.dumps(body, ensure_ascii=False).encode("utf-8")

        last_err = None
        status, parsed, used_url = 0, None, None
        ns = pod_name = node_name = svc_name = ""
        for attempt_idx in range(2):
            ns, pod_name, ip, port, node_name, svc_name, is_local = self._resolve_endpoint_cached(
                ref, default_ns=default_ns, force=(attempt_idx > 0))
            headers = {"X-Kube32-Pod": "%s/%s" % (ns, pod_name), "Accept": "application/json", "Connection": "close"}
            if body_bytes is not None:
                headers["Content-Type"] = "application/json"

            candidates = []
            if ip:
                candidates.append(("http://%s:%d%s" % (ip, port, req_path), headers))
            if ip != "127.0.0.1" and is_local:
                candidates.append(("http://127.0.0.1:%d%s" % (port, req_path), headers))

            for url, hdrs in candidates:
                try:
                    status, parsed = self._raw_http_json(url, method, hdrs, body_bytes, timeout)
                    used_url = url
                    break
                except OSError as e:
                    last_err = "%s (%s)" % (url, getattr(e, "reason", None) or e)
            if used_url is not None:
                break
            # Invalidate stale cache on failure before retrying once
            cache_key = (default_ns or self.namespace, str(ref or ""))
            if attempt_idx == 0 and cache_key in self._endpoint_cache:
                self._endpoint_cache.pop(cache_key, None)
                continue
            # Lazy Ingress fallback ONLY when direct podIP:port is unreachable
            prefix = self._ingress_prefix(ns, svc_name or pod_name)
            if prefix is not None:
                ing_headers = {"Accept": "application/json", "Connection": "close"}
                if body_bytes is not None:
                    ing_headers["Content-Type"] = "application/json"
                ing_url = "%s%s%s" % (self.ingress_url, prefix, req_path)
                try:
                    status, parsed = self._raw_http_json(ing_url, method, ing_headers, body_bytes, timeout)
                    used_url = ing_url
                except OSError as e:
                    last_err = "%s (%s)" % (ing_url, getattr(e, "reason", None) or e)
            break

        if used_url is None:
            if not last_err:
                raise ToolError("Pod %s/%s has no podIP or reachable endpoint yet" % (ns, pod_name))
            raise ToolError("cannot reach %s/%s (pod %s on %s): %s" % (ns, svc_name or pod_name, pod_name, node_name, last_err))
        if method != "GET":
            self.log("kube32 mcp: %s %s/%s%s -> http %d" % (method, ns, svc_name or pod_name, req_path, status))
        if status >= 400:
            detail = parsed.get("error", parsed) if isinstance(parsed, dict) else parsed
            raise ToolError("%s %s%s answered http %d: %s" % (method, svc_name or pod_name, req_path, status, detail))
        return {
            "service": "%s/%s" % (ns, svc_name or pod_name),
            "pod": pod_name,
            "node": node_name,
            "method": method,
            "path": req_path,
            "status": status,
            "ok": True,
            "response": parsed,
        }

    def _warm_endpoint_cache(self, ns, nodes, pods_raw, svcs_raw, now):
        """Populate endpoint cache from an already-fetched nodes/pods/services snapshot."""
        by_pod = {}
        for p in pods_raw:
            ip, port, node_obj = self._pod_endpoint(p, nodes)
            if not ip:
                continue
            pname = p["metadata"]["name"]
            node_name = (p.get("spec") or {}).get("nodeName") or "?"
            labels = ((node_obj or {}).get("metadata") or {}).get("labels") or {}
            is_local = labels.get("kube32.io/board") == "macos" or labels.get("kube32.io/simulator") == "true"
            entry = (now, ns, pname, ip, port, node_name, None, is_local)
            by_pod[pname] = entry
            self._endpoint_cache[(ns, pname)] = entry
            self._endpoint_cache[(ns, "%s/%s" % (ns, pname))] = entry
        for s in svcs_raw:
            sname = s["metadata"]["name"]
            sel = (s.get("spec") or {}).get("selector") or {}
            matched = [p for p in pods_raw
                       if sel and all((p["metadata"].get("labels") or {}).get(k) == v for k, v in sel.items())]
            running = [p for p in matched if (p.get("status") or {}).get("phase") == "Running"]
            chosen = next((p for p in running if (p.get("status") or {}).get("podIP")), None) or (
                running[0] if running else (matched[0] if matched else None))
            if chosen is not None and chosen["metadata"]["name"] in by_pod:
                base = by_pod[chosen["metadata"]["name"]]
                svc_entry = (now, base[1], base[2], base[3], base[4], base[5], sname, base[7])
                self._endpoint_cache[(ns, sname)] = svc_entry
                self._endpoint_cache[(ns, "%s/%s" % (ns, sname))] = svc_entry

    def list_workloads(self, args, ctx):
        ns = args.get("namespace") or self.namespace
        now = time.time()
        cached = self._workloads_cache.get(ns)
        if cached and self.workloads_ttl > 0 and (now - cached[0]) < self.workloads_ttl:
            return cached[1]
        try:
            nodes = self._node_map()
            pods_raw = (self.get("/api/v1/namespaces/%s/pods" % ns) or {}).get("items", [])
            svcs_raw = (self.get("/api/v1/namespaces/%s/services" % ns) or {}).get("items", [])
            ings_raw = (self.get("/apis/networking.k8s.io/v1/namespaces/%s/ingresses" % ns) or {}).get("items", [])
        except ToolError as e:
            if cached:
                self.log("kube32 mcp: keeping cached list_workloads(%s) after API error: %s" % (ns, e))
                self._workloads_cache[ns] = (now - self.workloads_ttl + ERROR_BACKOFF_S, cached[1])
                return cached[1]
            raise

        self._warm_endpoint_cache(ns, nodes, pods_raw, svcs_raw, now)
        if ns == self.namespace:
            tools_built = self._build_service_mcp_tools(svcs_raw)
            self._svc_tools_cache = (now, tools_built)
            self._svc_tools_last_attempt = now

        pods_out = []
        for p in sorted(pods_raw, key=lambda x: x["metadata"]["name"]):
            meta = p["metadata"]
            ann = meta.get("annotations") or {}
            entry = pod_summary(p)
            ip, port, _ = self._pod_endpoint(p, nodes)
            if ip:
                entry["pod_ip"] = ip
                entry["pod_port"] = port
            if meta.get("labels"):
                entry["labels"] = meta["labels"]
            if ann.get("kube32.io/event-topics"):
                entry["event_topics"] = [t.strip() for t in ann["kube32.io/event-topics"].split(",") if t.strip()]
            if ann.get(lim.NOTES_ANNOTATION):
                entry["notes"] = ann[lim.NOTES_ANNOTATION].strip()
            pods_out.append(entry)

        svcs_out = []
        for s in sorted(svcs_raw, key=lambda x: x["metadata"]["name"]):
            meta, spec = s["metadata"], s.get("spec") or {}
            ann = meta.get("annotations") or {}
            sel = spec.get("selector") or {}
            matched = [p["metadata"]["name"] for p in pods_raw
                       if sel and all((p["metadata"].get("labels") or {}).get(k) == v for k, v in sel.items())]
            entry = {"name": meta["name"], "selector": sel,
                     "ports": [p.get("port") for p in spec.get("ports") or []],
                     "pods": sorted(matched)}
            if ann.get(lim.NOTES_ANNOTATION):
                entry["notes"] = ann[lim.NOTES_ANNOTATION].strip()
            declared = self._parse_service_mcp_tools(s)
            if declared:
                entry["mcp_tools"] = [t["name"] for t in declared]
            svcs_out.append(entry)

        ings_out = []
        for ing in sorted(ings_raw, key=lambda x: x["metadata"]["name"]):
            meta = ing["metadata"]
            ann = meta.get("annotations") or {}
            routes = []
            for rule in (ing.get("spec") or {}).get("rules") or []:
                for p in ((rule.get("http") or {}).get("paths") or []):
                    svc = (p.get("backend") or {}).get("service") or {}
                    routes.append({"path": p.get("path", "/"), "service": svc.get("name", ""),
                                   "port": (svc.get("port") or {}).get("number")})
            entry = {"name": meta["name"], "routes": routes}
            if ann.get("kube32.io/event-stream"):
                entry["event_stream"] = ann["kube32.io/event-stream"]
            ings_out.append(entry)

        out = {"namespace": ns, "pods": pods_out, "services": svcs_out,
               "writes": "on" if self.allow_writes else "off (kube32 mcp --allow-writes)",
               "deploy": "on" if self.allow_deploy else "off (kube32 mcp --allow-deploy)"}
        if ings_out:
            out["ingresses"] = ings_out
        self._workloads_cache[ns] = (now, out)
        return out

    def call_service(self, args, ctx):
        return self._http_service_call(
            args["service"],
            method=args.get("method", "GET"),
            path=args.get("path", "/"),
            query=args.get("query"),
            body=args.get("body"),
        )

    def _parse_service_mcp_tools(self, svc):
        """Parse valid tool declarations from a Service's kube32.io/mcp-tools JSON annotation."""
        ann = (svc.get("metadata") or {}).get("annotations") or {}
        raw = ann.get(MCP_TOOLS_ANNOTATION)
        if not raw:
            return []
        try:
            items = json.loads(raw)
        except ValueError as e:
            self.log("kube32 mcp: ignoring invalid %s on Service %s: %s" % (
                MCP_TOOLS_ANNOTATION, (svc.get("metadata") or {}).get("name", "?"), e))
            return []
        if not isinstance(items, list):
            return []
        out = []
        for item in items:
            if not isinstance(item, dict):
                continue
            name = item.get("name")
            if not isinstance(name, str) or not _TOOL_NAME_RE.match(name):
                continue
            method = str(item.get("method", "GET")).upper()
            if method not in ("GET", "POST", "PUT", "DELETE"):
                continue
            props = item.get("properties")
            if props is None:
                props = {}
            if not isinstance(props, dict):
                continue
            req = item.get("required") or []
            if not isinstance(req, list) or not all(isinstance(r, str) for r in req):
                continue
            out.append({
                "name": name,
                "title": str(item.get("title") or name),
                "description": str(item.get("description") or ("%s %s on Service %s" % (
                    method, item.get("path", "/"), (svc.get("metadata") or {}).get("name", "")))),
                "method": method,
                "path": str(item.get("path") or "/"),
                "properties": props,
                "required": req,
                "query_keys": set(item.get("query_keys") or ()),
            })
        return out

    def _build_service_mcp_tools(self, svcs):
        result = []
        seen = set()
        for svc in sorted(svcs, key=lambda x: x["metadata"]["name"]):
            meta = svc.get("metadata") or {}
            ns = meta.get("namespace", self.namespace)
            svc_name = meta.get("name", "")
            for spec in self._parse_service_mcp_tools(svc):
                if spec["name"] in seen:
                    continue
                seen.add(spec["name"])

                def make_fn(ref, method, path, query_keys, target_ns):
                    def fn(args, ctx):
                        if method == "GET":
                            res = self._http_service_call(ref, method="GET", path=path, query=args or None, default_ns=target_ns)
                        else:
                            q = {k: v for k, v in (args or {}).items() if k in query_keys} or None
                            b = {k: v for k, v in (args or {}).items() if k not in query_keys}
                            res = self._http_service_call(ref, method=method, path=path, query=q, body=b, default_ns=target_ns)
                        return res["response"]
                    return fn

                is_read = spec["method"] == "GET"
                result.append(Tool(
                    spec["name"],
                    spec["title"],
                    spec["description"],
                    spec["properties"],
                    spec["required"],
                    make_fn("%s/%s" % (ns, svc_name), spec["method"], spec["path"], spec["query_keys"], ns),
                    read_only=is_read,
                    destructive=not is_read,
                    idempotent=is_read,
                ))
        return result

    def service_mcp_tools(self, refresh=False, force=False):
        """Dynamic MCP Tools declared by Services in self.namespace via kube32.io/mcp-tools.

        Caches discovered tools for self.cache_ttl seconds (default 300s) and falls back to the
        last known good cache on transient API errors so high-frequency polling never hammers
        a constrained control-plane node.
        """
        now = time.time()
        ts, cached = self._svc_tools_cache
        if not force and self.cache_ttl > 0 and ts > 0 and (now - ts) < self.cache_ttl:
            if cached or not refresh:
                return cached
        if not cached and self._svc_tools_last_attempt > 0 and (now - self._svc_tools_last_attempt) < ERROR_BACKOFF_S and not force:
            return cached
        if force and self._svc_tools_last_attempt > 0 and (now - self._svc_tools_last_attempt) < ERROR_BACKOFF_S:
            return cached
        self._svc_tools_last_attempt = now
        try:
            svcs = (self.get("/api/v1/namespaces/%s/services" % self.namespace) or {}).get("items", [])
        except ToolError as e:
            self.log("kube32 mcp: keeping %d cached service tools after API error: %s" % (len(cached), e))
            return cached
        result = self._build_service_mcp_tools(svcs)
        self._svc_tools_cache = (now, result)
        return result

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

    def capture_camera(self, args, ctx):
        """Captures a live JPEG snapshot (or triggers an AMOLED preview) from a node with hardware.kube32.io/camera."""
        node = args.get("node")
        if not node:
            nodes = (self.api("get", "/api/v1/nodes") or {}).get("items", [])
            for n in sorted(nodes, key=lambda x: x["metadata"]["name"]):
                alloc = (n.get("status") or {}).get("allocatable") or {}
                labels = (n.get("metadata") or {}).get("labels") or {}
                if alloc.get("hardware.kube32.io/camera") or labels.get("hardware.kube32.io/camera") == "true":
                    node = n["metadata"]["name"]
                    break
            if not node:
                raise ToolError("no node advertises hardware.kube32.io/camera (list_devices shows node hardware)")
        if args.get("preview"):
            res = self.api("create", "/api/v1/nodes/%s/proxy/camera/preview" % quote(node, safe=""), {})
            return {"node": node, "preview": True, "result": res}
        quality = args.get("quality", 75)
        if not 10 <= quality <= 95:
            raise ToolError("quality is 10 to 95")
        path = "/api/v1/nodes/%s/proxy/camera/snapshot?quality=%d" % (quote(node, safe=""), quality)
        try:
            data = api_call_raw(self.kube, "get", path)
        except ApiError as e:
            raise ToolError("camera snapshot on %s failed: %s" % (node, e))
        if len(data) < 4 or data[:2] != b"\xff\xd8":
            raise ToolError("camera snapshot on %s did not return JPEG (%s)" % (node, data[:160].decode("utf-8", "replace")))
        out = {"node": node, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "mime": "image/jpeg"}
        save_path = args.get("save_path")
        if save_path:
            dest = os.path.abspath(save_path)
            with open(dest, "wb") as f:
                f.write(data)
            out["saved_to"] = dest
        if args.get("include_base64", False):
            out["jpeg_base64"] = base64.b64encode(data).decode("ascii")
        return out

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
        Tool("pod_logs", "Read recent logs of a pod",
             "Returns the last log lines of a Pod (or the first pod of a Deployment) in the server's namespace.",
             {"name": {"type": "string", "description": "Pod or Deployment name"},
              "tail_lines": {"type": "integer", "description": "Log lines to return, 1 to 500 (default 50)"},
              "previous": {"type": "boolean", "description": "Logs of the previous container instance (default false)"}},
             ["name"], k.read_pod_logs, idempotent=True),
        Tool("list_workloads", "List pods, services and ingresses",
             "Lists Pods (phase, node, IP, resolved pod server port, labels, event topics), Services (selectors, "
             "matching pods, notes, and any kube32.io/mcp-tools declared on them), and Ingresses in the namespace.",
             {"namespace": {"type": "string", "description": "Namespace to inspect (default: server's namespace)"}},
             [], k.list_workloads, idempotent=True),
        Tool("call_service", "Call an HTTP endpoint on a Service or Pod",
             "Sends an HTTP request (GET, POST, PUT or DELETE) to a kube32 Service or Pod, automatically resolving "
             "the target node's pod HTTP port (status.daemonEndpoints.kubeletEndpoint.Port). GET is always allowed; "
             "POST, PUT and DELETE require a server started with --allow-writes.",
             {"service": {"type": "string", "description": "Service or Pod name, or namespace/name (list_workloads lists them)"},
              "path": {"type": "string", "description": "HTTP path starting with '/' (default '/')"},
              "method": {"type": "string", "enum": ["GET", "POST", "PUT", "DELETE"], "description": "HTTP method (default GET)"},
              "query": {"type": "object", "description": "Optional URL query parameters"},
              "body": {"type": "object", "description": "Optional JSON request body for POST or PUT"}},
             ["service"], k.call_service, read_only=False),
        Tool("delete_workload", "Delete a deployed pod",
             "Deletes a pod or Deployment that deploy_code created. Refuses anything else. Needs --allow-deploy.",
             {"name": {"type": "string", "description": "Pod or Deployment name"}},
             ["name"], k.delete_workload, read_only=False, destructive=True, idempotent=True),
        Tool("capture_camera", "Capture a JPEG snapshot or AMOLED preview from a Camera Module",
             "Captures a live JPEG frame from a node advertising hardware.kube32.io/camera (ESP-Mosaico left-slot Camera Module), "
             "returning its byte count and sha256 (and optionally saving it to save_path or returning base64), or blits a live "
             "RGB565 preview to the node's AMOLED display when preview=true.",
             {"node": {"type": "string", "description": "Node name (default: first node advertising hardware.kube32.io/camera)"},
              "quality": {"type": "integer", "description": "JPEG quality 10 to 95 (default 75)"},
              "save_path": {"type": "string", "description": "Optional local path to write the JPEG file"},
              "include_base64": {"type": "boolean", "description": "Include base64-encoded JPEG in response (default false)"},
              "preview": {"type": "boolean", "description": "Blit live preview to the node's AMOLED instead of returning JPEG"}},
             [], k.capture_camera),
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
