"""
Kube32 Cluster Tests: Hardware-Following Pods
The checkin job (apps/checkin, examples/checkin.yaml) steps through its
needs on worker processes that offer different devices (--sim-devices): it
starts where kube32.io/prefer-nodes puts it and reads the scale's Device
Service there, moves to the node with the display and keyboard for its
label, gets MoveFailed while no node has both the speaker and the LED and
moves once one joins. The weight and the label travel with it; on each node
the firewall grants the listed devices that node has and refuses the rest.
"""

import base64
import json
import os
import threading
import time
import urllib.error
import urllib.request

from tests.integration.framework import BaseTestCase
from tests.integration.cluster_harness import ClusterHarness, PROJECT_ROOT

MODULE = os.path.join(PROJECT_ROOT, "images", "sd", "checkin.wasm")


def http(method, url, body=None, headers=None, timeout=10.0):
    """(status, body bytes) of one request; HTTP errors are answers too"""
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


class Output:
    """A worker's output, read as it comes so that its pipe never fills"""

    def __init__(self, node):
        self.lines = []
        self.lock = threading.Lock()
        for stream in (node.process.stdout, node.process.stderr):
            threading.Thread(target=self._read, args=(stream,), daemon=True).start()

    def _read(self, stream):
        for raw in iter(stream.readline, b""):
            with self.lock:
                self.lines.append(raw.decode("utf-8", errors="replace").rstrip())

    def has(self, text):
        with self.lock:
            return any(text in line for line in self.lines)


class TestHardwareFollowing(BaseTestCase):
    def setUp(self):
        super().setUp()
        self.harness = ClusterHarness(base_api_port=17143, base_ipc_port=18000)
        self.out = {}

    def tearDown(self):
        self.harness.teardown()
        super().tearDown()

    def _worker(self, name, node_id, cp, devices, extra=None):
        w = self.harness.start_worker(name, node_id, cp, lease_interval_ms=500, pod_port=18010 + node_id,
                                      extra_args=["--sim-devices", devices] + (extra or []))
        self.out[name] = Output(w)
        return w

    def _get(self, cp, path):
        status, body = http("GET", f"{cp.api_url}{path}")
        return json.loads(body) if status == 200 else None

    def _job(self, cp):
        return self._get(cp, "/api/v1/namespaces/default/pods/checkin")

    def _node_of(self, cp):
        pod = self._job(cp) or {}
        return pod.get("spec", {}).get("nodeName") if pod.get("status", {}).get("phase") == "Running" else None

    def _state(self, worker):
        """GET / of the job through its node's pod server: the record and where its steps ran"""
        status, body = http("GET", f"http://127.0.0.1:{worker.pod_port}/", headers={"X-Kube32-Pod": "default/checkin"})
        return json.loads(body) if status == 200 else None

    def _events(self, cp, reason):
        _, body = http("GET", f"{cp.api_url}/api/v1/namespaces/default/events")
        return [e.get("message", "") for e in json.loads(body).get("items", [])
                if e.get("reason") == reason and e.get("involvedObject", {}).get("name") == "checkin"]

    def test_job_follows_its_hardware(self):
        """Each step runs on a node with its devices; state carries, grants follow, MoveFailed until a node fits"""
        self.assertTrue(os.path.exists(MODULE), "images/sd/checkin.wasm (make -C apps/checkin)")
        cp = self.harness.start_control_plane(name="cp-hw", lease_interval_ms=500, reconcile_interval_ms=200)
        display = self._worker("hw-display", 1, cp, "tft-display,keyboard")
        speaker = self._worker("hw-speaker", 2, cp, "speaker")
        self._worker("hw-scale", 3, cp, "led", ["--sim-i2c", "miniscale"])

        # Each node advertises its devices, and only those
        def devices(name):
            node = self._get(cp, f"/api/v1/nodes/{name}") or {}
            alloc = node.get("status", {}).get("allocatable", {})
            return sorted(k.split("/")[1] for k in alloc if k.startswith("hardware.kube32.io/") and k not in
                          ("hardware.kube32.io/ipc", "hardware.kube32.io/chip-temp"))
        self.wait_for_condition(lambda: devices("hw-display") == ["keyboard", "tft-display"], timeout=10.0, description="hw-display devices")
        self.assertEqual(devices("hw-speaker"), ["speaker"])
        self.assertEqual(devices("hw-scale"), ["led"])

        # The scale on hw-scale: discovery makes it a Device, served by its Service
        def scale():
            items = (self._get(cp, "/apis/kube32.io/v1/namespaces/default/devices") or {}).get("items", [])
            return next((d["metadata"]["name"] for d in items if d.get("spec", {}).get("driver") == "miniscale"), None)
        device = self.wait_for_condition(scale, timeout=20.0, description="the scale's Device")

        with open(MODULE, "rb") as f:
            module = base64.b64encode(f.read()).decode()
        pod = {
            "apiVersion": "v1", "kind": "Pod",
            "metadata": {"name": "checkin", "namespace": "default", "labels": {"app": "checkin"}, "annotations": {
                "kube32.io/wasm-base64": module,
                "kube32.io/migration": "Auto",
                "kube32.io/placement": "hardware",
                "kube32.io/prefer-nodes": "hw-speaker",
                "kube32.io/tick-interval": "100ms",
            }},
            "spec": {"enableServiceLinks": False, "containers": [{
                "name": "checkin", "image": "wasm://inline/checkin",
                "env": [{"name": "NODE_NAME", "valueFrom": {"fieldRef": {"fieldPath": "spec.nodeName"}}},
                        {"name": "SCALE_URL", "value": f"http://{device}/"},
                        {"name": "LABEL", "value": "parcel-7"},
                        {"name": "VOLUME", "value": "5"}],
                "resources": {"requests": {"memory": "8Ki",
                                           "hardware.kube32.io/tft-display": "1", "hardware.kube32.io/keyboard": "1",
                                           "hardware.kube32.io/speaker": "1", "hardware.kube32.io/led": "1"},
                              "limits": {"memory": "8Ki"}},
                "securityContext": {"capabilities": {"add": ["SERVICE_CALL"]}}}]},
        }
        status, body = http("POST", f"{cp.api_url}/api/v1/namespaces/default/pods", pod)
        self.assertEqual(status, 201, body[:300])

        # Step 1 where the preference puts it, step 2 on the one node with
        # the display and the keyboard: the moves carry the reading
        self.wait_for_condition(lambda: self._events(cp, "Moved"), timeout=60.0, description="the job's first move")
        moving = self._events(cp, "Moving")
        self.assertTrue(any("from node hw-speaker to node hw-display for its need "
                            "hardware.kube32.io/tft-display,hardware.kube32.io/keyboard" in m for m in moving), moving)

        # Step 3: no node has the speaker and the LED. The job stays on
        # hw-display, its need recorded, and says why
        failed = self.wait_for_condition(lambda: self._events(cp, "MoveFailed"), timeout=30.0, description="MoveFailed")
        self.assertIn("No node has hardware.kube32.io/speaker,hardware.kube32.io/led for the pod's need", failed[0])
        self.assertIn("it runs on node hw-display and moves once a node has it", failed[0])
        job = self._job(cp)
        self.assertEqual(job["spec"]["nodeName"], "hw-display")
        self.assertEqual(job["metadata"]["annotations"].get("kube32.io/needs"), "hardware.kube32.io/speaker,hardware.kube32.io/led")
        state = self.wait_for_condition(lambda: (lambda s: s if s and s["step"] == "confirm" else None)(self._state(display)),
                                        timeout=10.0, description="the job waiting at step confirm")
        self.assertEqual(state["weight_g"], 0, state)          # the scale's reading, through its Service
        self.assertEqual(state["label"], "parcel-7")
        self.assertEqual(state["trail"][:2], ["hw-speaker", "hw-display"])
        self.assertEqual(state["need"], 1)                     # K32_NEED_MOVING
        self.assertEqual(state["display"], 0)                  # granted here
        self.assertTrue(self.out["hw-display"].has("[DISPLAY 8,52] label   parcel-7"))
        time.sleep(2.0)
        self.assertEqual(len(self._events(cp, "MoveFailed")), 1)

        # A node with both joins: the job moves there, plays and lights up
        self._worker("hw-signal", 4, cp, "speaker,led")
        signal = self.harness.nodes["hw-signal"]
        done = self.wait_for_condition(lambda: (lambda s: s if s and s["step"] == "done" else None)(self._state(signal)),
                                       timeout=60.0, description="the job done on hw-signal")
        self.assertEqual(done["trail"], ["hw-speaker", "hw-display", "hw-signal", "hw-signal"])
        self.assertEqual((done["weight_g"], done["label"], done["moves"]), (0, "parcel-7", 2))
        self.assertEqual(done["led"], 0)
        self.assertEqual(done["tone"], 0)
        self.assertEqual(done["display"], -1)                  # listed in the spec, not on this node: K32_ERR_PERM
        self.wait_for_condition(lambda: self._job(cp)["metadata"]["annotations"].get("kube32.io/needs") == "",
                                timeout=5.0, description="no need recorded after the last step")
        self.assertTrue(any("to node hw-signal for its need hardware.kube32.io/speaker,hardware.kube32.io/led" in m
                            for m in self._events(cp, "Moved")))
        self.assertTrue(self.out["hw-signal"].has("[TONE] 523 Hz for 120 ms"))
        self.assertTrue(self.out["hw-signal"].has("[LED] #004000"))
        self.assertFalse(self.out["hw-display"].has("[TONE]"))
        self.assertFalse(self.out["hw-speaker"].has("[DISPLAY"))
        self.assertEqual(len(self._events(cp, "MoveFailed")), 1)
