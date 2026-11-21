"""
Kube32 Cluster Tests: triple modular redundancy across nodes
Three lanes of the lockstep example (apps/lockstep) integrate one double
pendulum on three workers and report the bits of their state every 100
steps to the voter (apps/voter) on a fourth worker, which has a stand-in
screen and keyboard. The lanes agree bit for bit at every checkpoint. A bit
flipped in lane b, from the voter and from the injector pod (its LED
stand-in on worker b), is outvoted at the next checkpoint, with lanes a and
c as the majority. A repair copies a healthy lane into lane b through a
Copy checkpoint, and lane b agrees again. With worker c killed the vote
goes on two of three; restarted, lane c starts over and a repair brings it
back.
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

LANE_WASM = os.path.join(PROJECT_ROOT, "images", "sd", "lockstep.wasm")
VOTER_WASM = os.path.join(PROJECT_ROOT, "images", "sd", "voter.wasm")
INJECTOR_WASM = os.path.join(PROJECT_ROOT, "images", "sd", "lockstep-injector.wasm")
INGRESS_PORT = 18480
EVERY = 100


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


def module(path):
    with open(path, "rb") as f:
        return base64.b64encode(f.read()).decode()


class Output:
    """A node's output, read as it comes so that its pipe never fills"""

    def __init__(self, node):
        self.lines = []
        self.lock = threading.Lock()
        for stream in (node.process.stdout, node.process.stderr):
            threading.Thread(target=self._read, args=(stream,), daemon=True).start()

    def _read(self, stream):
        for raw in iter(stream.readline, b""):
            with self.lock:
                self.lines.append(raw.decode("utf-8", errors="replace").rstrip())

    def grep(self, text):
        with self.lock:
            return [line for line in self.lines if text in line]


def pod(name, node, image, wasm, env, memory, caps, requests=None, labels=None, tick="100ms"):
    return {
        "apiVersion": "v1", "kind": "Pod",
        "metadata": {"name": name, "namespace": "default", "labels": labels or {"app": name},
                     "annotations": {"kube32.io/wasm-base64": wasm, "kube32.io/tick-interval": tick,
                                     "kube32.io/wasm-stack": "8Ki" if name == "voter" else "1Ki"}},
        "spec": {"nodeName": node, "restartPolicy": "Always",
                 "containers": [{"name": image, "image": "wasm://inline/" + image,
                                 "ports": [{"containerPort": 7000, "name": "http"}],
                                 "env": [{"name": k, "value": v} if isinstance(v, str) else {"name": k, "valueFrom": v}
                                         for k, v in env.items()],
                                 "resources": {"requests": dict(memory=memory, **(requests or {})), "limits": {"memory": memory}},
                                 "securityContext": {"capabilities": {"add": caps}}}]},
    }


def service(name, selector):
    return {"apiVersion": "v1", "kind": "Service", "metadata": {"name": name, "namespace": "default"},
            "spec": {"selector": selector, "ports": [{"name": "http", "port": 80, "targetPort": 7000}]}}


class TestLockstepVote(BaseTestCase):
    def setUp(self):
        super().setUp()
        self.harness = ClusterHarness(base_api_port=17443, base_ipc_port=18400)
        self.out = {}

    def tearDown(self):
        self.harness.teardown()
        super().tearDown()

    def _dump(self):
        """The last lines of each node, after a failure"""
        for name, out in self.out.items():
            with out.lock:
                tail = out.lines[-30:]
            print(f"--- {name} ---\n" + "\n".join(tail))

    def _worker(self, name, node_id, cp, extra=None):
        w = self.harness.start_worker(name, node_id, cp, lease_interval_ms=500, pod_port=18430 + node_id, extra_args=extra)
        self.out[name] = Output(w)
        return w

    def _state(self):
        try:
            status, body = http("GET", f"http://127.0.0.1:{INGRESS_PORT}/voter/state", timeout=3.0)
        except OSError:
            return None
        return json.loads(body) if status == 200 else None

    def _lane(self, s, name):
        return next(l for l in s["lanes"] if l["name"] == name)

    def _until(self, pred, timeout, description):
        def check():
            s = self._state()
            return s if s and pred(s) else None
        try:
            return self.wait_for_condition(check, timeout=timeout, interval=0.2, description=description)
        except Exception:
            print(json.dumps(self._state(), indent=1)[:4000])
            raise

    def _statuses(self, s):
        return {l["name"]: l["status"] for l in s["lanes"]}

    def _checkpoint_done(self, cp, name):
        status, body = http("GET", f"{cp.api_url}/apis/kube32.io/v1/namespaces/default/checkpoints/{name}")
        c = json.loads(body) if status == 200 else {}
        return c if c.get("status", {}).get("phase") in ("Completed", "Failed") else None

    def _repair(self, cp, lane):
        status, body = http("GET", f"http://127.0.0.1:{INGRESS_PORT}/voter/repair?lane={lane}")
        self.assertEqual(status, 201, body[:300])
        r = json.loads(body)
        ckpt = self.wait_for_condition(lambda: self._checkpoint_done(cp, r["checkpoint"]), timeout=30.0,
                                       description=f"the copy into lane {lane} finished")
        self.assertEqual(ckpt["status"]["phase"], "Completed", json.dumps(ckpt["status"]))
        self.assertEqual(ckpt["spec"]["action"], "Copy")
        self.assertEqual(ckpt["spec"]["target"]["podName"], f"lockstep-{lane}")
        return r

    def test_vote_detects_repairs_and_survives_a_lost_lane(self):
        """Lanes agree bit for bit; a flipped bit is outvoted at the next checkpoint; a copy repairs it; two of three vote on"""
        try:
            self._vote()
        except Exception:
            self._dump()
            raise

    def _vote(self):
        for path in (LANE_WASM, VOTER_WASM, INJECTOR_WASM):
            self.assertTrue(os.path.exists(path), f"{path} missing: make -C apps/lockstep && make -C apps/voter")
        cp = self.harness.start_control_plane(name="cp-tmr", lease_interval_ms=500, reconcile_interval_ms=200,
                                              ingress_port=INGRESS_PORT)
        self.out["cp-tmr"] = Output(cp)
        a = self._worker("tmr-a", 1, cp)
        b = self._worker("tmr-b", 2, cp, ["--sim-devices", "led"])
        c = self._worker("tmr-c", 3, cp)
        v = self._worker("tmr-voter", 4, cp, ["--sim-devices", "tft-display,keyboard"])
        api = cp.api_url

        lane_wasm, voter_wasm, injector_wasm = module(LANE_WASM), module(VOTER_WASM), module(INJECTOR_WASM)
        field = lambda p: {"fieldRef": {"fieldPath": p}}
        objects = [("/api/v1/namespaces/default/services", service("voter", {"app": "voter"}))]
        for lane, node in (("a", a), ("b", b), ("c", c)):
            objects.append(("/api/v1/namespaces/default/services",
                            service(f"lockstep-{lane}", {"app": "lockstep", "lockstep.kube32.io/lane": lane})))
            objects.append(("/api/v1/namespaces/default/pods", pod(
                f"lockstep-{lane}", node.name, "lockstep", lane_wasm,
                {"LANE": lane, "POD_NAME": field("metadata.name"), "NODE_NAME": field("spec.nodeName"),
                 "VOTER_URL": "http://voter/report", "STEPS_PER_TICK": "100", "CHECK_EVERY": str(EVERY),
                 "REPORT_MS": "150", "FAULT_BIT": "40"},
                "16Ki", ["SERVICE_CALL"], labels={"app": "lockstep", "lockstep.kube32.io/lane": lane})))
        objects.append(("/api/v1/namespaces/default/pods", pod(
            "voter", v.name, "voter", voter_wasm,
            {"API_URL": "http://$(KUBERNETES_SERVICE_HOST):$(KUBERNETES_SERVICE_PORT)", "POD_NAMESPACE": field("metadata.namespace"),
             "LANES": "a,b,c", "CHECK_EVERY": str(EVERY), "RATE": "1000", "LEAD": "400", "LOST_MS": "3000",
             "AUTO_REPAIR_S": "0", "FAULT_BIT": "40"},
            "192Ki", ["DISPLAY", "KEYBOARD", "SERVICE_CALL"],
            {"hardware.kube32.io/tft-display": "1", "hardware.kube32.io/keyboard": "1"})))
        objects.append(("/api/v1/namespaces/default/pods", pod(
            "lockstep-injector", b.name, "lockstep-injector", injector_wasm,
            {"LANE": "b", "FAULT_URL": "http://lockstep-b/fault", "STATUS_URL": "http://voter/lane?name=b",
             "BUTTON_PIN": "none", "LED": "on", "POLL_MS": "200"},
            "8Ki", ["SERVICE_CALL"], {"hardware.kube32.io/led": "1"}, tick="50ms")))
        objects.append(("/apis/networking.k8s.io/v1/namespaces/default/ingresses", {
            "apiVersion": "networking.k8s.io/v1", "kind": "Ingress", "metadata": {"name": "voter", "namespace": "default"},
            "spec": {"rules": [{"http": {"paths": [{"path": "/voter", "pathType": "Prefix", "backend": {
                "service": {"name": "voter", "port": {"number": 80}}}}]}}]}}))
        for path, obj in objects:
            status, body = http("POST", api + path, obj)
            self.assertEqual(status, 201, body[:300])

        # The three lanes agree bit for bit at every checkpoint
        s = self._until(lambda s: s["voted"] and s["step"] >= 3000 and s["majority"] == 3
                        and set(self._statuses(s).values()) == {"agree"}, 40.0, "three lanes agree past step 3000")
        self.assertEqual(s["votes"], s["unanimous"], "every vote so far was unanimous: " + json.dumps(s)[:3000])
        self.assertEqual({self._lane(s, n)["hash"] for n in "abc"}, {s["hash"]})
        for lane, node in (("a", a), ("b", b), ("c", c)):
            l = self._lane(s, lane)
            self.assertEqual((l["pod"], l["node"]), (f"lockstep-{lane}", node.name))
            _, body = http("GET", f"{api}/api/v1/nodes/{node.name}")
            info = json.loads(body)["status"]["nodeInfo"]
            self.assertEqual((l["arch"], l["os"]), (info["architecture"], info["operatingSystem"]))
            self.assertGreater(l["us"], 0)
        # The lanes run in lockstep: none is more than LEAD steps past the vote
        self.assertTrue(all(s["step"] <= self._lane(s, n)["step"] <= s["target"] for n in "abc"), json.dumps(s["lanes"])[:500])

        # The page and the recorded native runs, through the Ingress
        status, page = http("GET", f"http://127.0.0.1:{INGRESS_PORT}/voter")
        self.assertEqual(status, 200)
        self.assertIn(b'<base href="/voter/">', page)
        self.assertIn(b"Recorded native runs", page)
        status, native = http("GET", f"http://127.0.0.1:{INGRESS_PORT}/voter/native.js")
        self.assertEqual(status, 200)
        self.assertTrue(native.startswith(b"NATIVE={every:40,runs:["), native[:60])

        # The injector's LED is green while lane b agrees
        self.wait_for_condition(lambda: self.out["tmr-b"].grep("[LED] #00300a"), timeout=10.0, description="the LED turns green")
        status, body = http("GET", f"http://127.0.0.1:{INGRESS_PORT}/voter/lane?name=b")
        self.assertEqual((status, body), (200, b"agree\n"))

        # Fault injection through the voter: lane b flips bit 40 of theta1 at its next report
        status, body = http("GET", f"http://127.0.0.1:{INGRESS_PORT}/voter/fault?lane=b&bit=40")
        self.assertEqual(status, 202, body)
        s = self._until(lambda s: self._lane(s, "b")["status"] == "outvoted", 15.0, "lane b outvoted")
        det = self._lane(s, "b")["det"]
        self.assertTrue(det["injected"])
        self.assertEqual(det["bit"], 40)
        # Detected at the first checkpoint after the flip
        self.assertEqual(det["step"], (det["fault_step"] // EVERY + 1) * EVERY, json.dumps(det))
        self.assertGreaterEqual(EVERY, det["steps"])
        self.assertEqual(self._statuses(s), {"a": "agree", "b": "outvoted", "c": "agree"})
        self.assertGreaterEqual(s["majority"], 2)
        print(f"lane b: bit 40 flipped at step {det['fault_step']}, outvoted at step {det['step']}: "
              f"{det['steps']} steps, {det['ms']} ms")
        # The vote goes on with a and c, and the LED turns red
        s2 = self._until(lambda x: x["step"] >= s["step"] + 1000, 15.0, "the vote goes on two of three")
        self.assertEqual(s2["majority"], 2)
        self.assertEqual(self._lane(s2, "a")["hash"], s2["hash"])
        self.assertTrue(self._lane(s2, "b")["hash"] != s2["hash"], "lane b keeps its own state")
        self.wait_for_condition(lambda: self.out["tmr-b"].grep("[LED] #400000"), timeout=10.0, description="the LED turns red")

        # Repair: a copy of a lane of the majority restores into lane b on its node
        started = time.time()
        r = self._repair(cp, "b")
        self.assertIn(r["from"], ("a", "c"))
        s = self._until(lambda s: self._lane(s, "b")["status"] == "agree" and self._lane(s, "b")["repairs"] == 1
                        and s["majority"] == 3, 30.0, "lane b agrees again")
        b_lane = self._lane(s, "b")
        self.assertEqual(b_lane["node"], b.name, "lane b restores on its own node")
        self.assertGreater(b_lane["from"], 0, "it goes on from the copied step")
        self.assertEqual(s["majority"], 3)
        print(f"repair: lane b agrees again {b_lane['repair_ms']} ms after the repair began "
              f"({time.time() - started:.1f} s from the request to the test seeing it)")
        self.wait_for_condition(lambda: len(self.out["tmr-b"].grep("[LED] #00300a")) >= 2, timeout=10.0,
                                description="the LED turns green again")

        # Fault injection through the injector pod: GET /press calls lane b's /fault
        status, body = http("GET", f"http://127.0.0.1:{b.pod_port}/press", headers={"X-Kube32-Pod": "default/lockstep-injector"})
        self.assertEqual(status, 202, body)
        s = self._until(lambda s: self._lane(s, "b")["status"] == "outvoted", 15.0, "lane b outvoted after the press")
        self.assertEqual(self._lane(s, "b")["det"]["bit"], 40)
        self._repair(cp, "b")
        self._until(lambda s: set(self._statuses(s).values()) == {"agree"} and self._lane(s, "b")["repairs"] == 2,
                    30.0, "lane b agrees after the second repair")

        # Losing worker c: lane c leaves the vote and a and b vote on, two of three
        self.harness.kill_node(c.name)
        s = self._until(lambda s: self._lane(s, "c")["status"] == "lost", 15.0, "lane c lost")
        s2 = self._until(lambda x: x["step"] >= s["step"] + 2000, 15.0, "the vote goes on without lane c")
        self.assertEqual((s2["majority"], self._statuses(s2)["a"], self._statuses(s2)["b"]), (2, "agree", "agree"))
        self.assertEqual(self._lane(s2, "a")["hash"], self._lane(s2, "b")["hash"])

        # Worker c comes back: lane c starts over from step 0, and a repair brings it to the vote
        self.harness.restart_node(c.name, cp, lease_interval_ms=500)
        self.out["tmr-c"] = Output(self.harness.nodes[c.name])
        self._until(lambda s: self._lane(s, "c")["status"] not in ("lost",) and self._lane(s, "c")["age_ms"] < 2000,
                    40.0, "lane c reports again")
        self._repair(cp, "c")
        s = self._until(lambda s: set(self._statuses(s).values()) == {"agree"} and s["majority"] == 3, 30.0,
                        "three lanes agree again")
        self.assertEqual({self._lane(s, n)["hash"] for n in "abc"}, {s["hash"]})
