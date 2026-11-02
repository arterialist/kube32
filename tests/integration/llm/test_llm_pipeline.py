"""
Kube32 LLM Tests: one language model split into pipeline stages across nodes
Three kube32-sim processes: "devkit" runs the control plane and its kubelet
with a board's PSRAM pool, "cardputer" is a worker without PSRAM (the 80 KiB
pod pool) and "laptop" is a worker that serves the model's files from its
storage, as the model host. examples/llm-pipeline.yaml, rendered with the
modules of apps/llm-pipeline, puts the embedding and layers 0-2 on devkit,
layer 3 (int4) on cardputer and layer 4 with the classifier on laptop.

A greedy generation through the pipeline yields the tokens that the
reference in apps/llm-pipeline/tools/llm.py computes from the same slices,
the stage on cardputer draws the text on the screen, and the stage on laptop
moves to devkit with its KV cache when laptop leaves in the middle of a
generation, which then ends with the same tokens.

Needs clang and a wasm linker (the modules), uv or numpy (the model tool),
the model files (downloaded once into apps/llm-pipeline/build/model) and
kubectl; without them the tests are skipped.
"""

import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

from tests.integration.framework import BaseTestCase, HTTPClient
from tests.integration.cluster_harness import BIN_PATH, PROJECT_ROOT, SimulatedNode, node_ready_condition

APP = os.path.join(PROJECT_ROOT, "apps", "llm-pipeline")
BUNDLE = os.path.join(APP, "build", "card", "kube32", "images", "llm-store")
CARDPUTER_POOL_KIB = 80


def _tool_command():
    """How to run tools/llm.py: uv with its inline dependencies, or this Python with numpy"""
    if shutil.which("uv"):
        return ["uv", "run", "-q", "--script", os.path.join(APP, "tools", "llm.py")]
    try:
        import numpy  # noqa: F401
    except ImportError:
        return None
    return [sys.executable, os.path.join(APP, "tools", "llm.py")]


def _kib(quantity: str) -> float:
    for suffix, scale in (("Ki", 1), ("Mi", 1024)):
        if quantity.endswith(suffix):
            return float(quantity[:-len(suffix)]) * scale
    return float(quantity) / 1024


class LlmClusterCase(BaseTestCase):
    API_PORT = 17043
    POD_PORTS = {"devkit": 17050, "cardputer": 17051, "laptop": 17052}
    INGRESS_PORT = 17053
    IPC_PORTS = {"devkit": 17060, "cardputer": 17061, "laptop": 17062}

    def setUp(self):
        super().setUp()
        self.nodes = {}
        self.dirs = []
        self.logs = {}
        self.api = HTTPClient(f"http://127.0.0.1:{self.API_PORT}", default_timeout=5.0)
        self.skip_reason = self._prepare()

    def tearDown(self):
        for n in self.nodes.values():
            n.terminate()
        for d in self.dirs:
            shutil.rmtree(d, ignore_errors=True)
        super().tearDown()

    # ---- the app: modules, model files, manifest ---------------------------
    def _prepare(self):
        if shutil.which("kubectl") is None:
            return "kubectl not found"
        if shutil.which("clang") is None:
            return "clang not found"
        self.tool = _tool_command()
        if self.tool is None:
            return "neither uv nor numpy found"
        make = subprocess.run(["make", "-C", APP, "modules", f"PYTHON={sys.executable}"], capture_output=True, text=True)
        if make.returncode != 0:
            return "the modules do not build here: " + (make.stderr or make.stdout).strip().splitlines()[-1]
        build = subprocess.run(self.tool + ["build", "--store-module", os.path.join(APP, "build", "llm-store.wasm")],
                               capture_output=True, text=True, timeout=600)
        if build.returncode != 0:
            return "no model files (download failed?): " + (build.stderr or build.stdout).strip()[-200:]
        self.manifest = os.path.join(self._dir("manifest"), "llm-pipeline.yaml")
        r = subprocess.run(self.tool + ["manifest", "--host-node", "laptop", "--prompt", "", "--out", self.manifest],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return None

    def _skip(self) -> bool:
        if self.skip_reason:
            print(f"[SKIP] {self.skip_reason}")
            return True
        return False

    def _reference(self, prompt: str, steps: int, context: int):
        r = subprocess.run(self.tool + ["generate", "--prompt", prompt, "--steps", str(steps), "--context", str(context), "--json"],
                           capture_output=True, text=True, timeout=600)
        self.assertEqual(r.returncode, 0, r.stderr)
        return json.loads(r.stdout)

    # ---- the cluster ---------------------------------------------------------
    def _dir(self, name: str) -> str:
        d = f"/tmp/k32_llm_{name}_{os.getpid()}_{int(time.time() * 1000) % 100000}"
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(os.path.join(d, "card"))
        self.dirs.append(d)
        return d

    def _start(self, name: str, args, sd_root=None):
        d = self._dir(name)
        env = dict(os.environ, KUBE32_SD_ROOT=sd_root or os.path.join(d, "card"))
        peers = []
        for i, (other, port) in enumerate(self.IPC_PORTS.items()):
            if other != name:
                peers += ["--peer", f"{other}:{i}:{port}"]
        self.logs[name] = os.path.join(d, "sim.log")
        log = open(self.logs[name], "w")
        node = SimulatedNode("standalone", name, 0, self.API_PORT, 0, d)
        node.process = subprocess.Popen([BIN_PATH] + args + peers + ["--data-dir", d, "--lease-interval", "1000",
                                                                     "--ipc-port", str(self.IPC_PORTS[name]),
                                                                     "--pod-port", str(self.POD_PORTS[name])],
                                        stdout=log, stderr=subprocess.STDOUT, env=env)
        self.nodes[name] = node

    def _cluster(self):
        join = f"http://127.0.0.1:{self.API_PORT}"
        self._start("devkit", ["--role", "standalone", "--workers", "", "--node-name", "cp-0", "--kubelet-name", "devkit",
                               "--port", str(self.API_PORT), "--ingress-port", str(self.INGRESS_PORT),
                               "--psram", "1284Ki", "--heap-budget", "448Ki", "--reconcile-interval", "500"])
        self.wait_for_condition(lambda: self._get("/version") is not None, timeout=10.0, description="API server")
        self._start("cardputer", ["--role", "worker", "--node-name", "cardputer", "--node-id", "1", "--join", join,
                                  "--board", "cardputer", "--psram", "0"])
        self._start("laptop", ["--role", "worker", "--node-name", "laptop", "--node-id", "2", "--join", join],
                    sd_root=os.path.join(APP, "build", "card"))
        for name in self.nodes:
            self.wait_for_condition(lambda name=name: (node_ready_condition(self.api.base_url, name) or {}).get("status") == "True",
                                    timeout=15.0, description=f"{name} Ready")
        config = os.path.join(self.dirs[0], "kubeconfig")
        with open(config, "w") as f:
            f.write("apiVersion: v1\nkind: Config\nclusters: []\ncontexts: []\nusers: []\n")
        r = subprocess.run(["kubectl", f"--server={join}", "apply", "-f", self.manifest], capture_output=True, text=True,
                           timeout=60, env=dict(os.environ, KUBECONFIG=config))
        self.assertEqual(r.returncode, 0, f"kubectl apply: {r.stdout}{r.stderr}")
        want = {"llm-store": "laptop", "llm-shard-0": "devkit", "llm-shard-1": "cardputer", "llm-shard-2": "laptop",
                "llm-coordinator": "devkit"}
        self.wait_for_condition(lambda: self._placement() == want, timeout=30.0, description="the pods Running where they belong")
        # Every stage has its slice; the coordinator its tokenizer
        for stage, node in (("0", "devkit"), ("1", "cardputer"), ("2", "laptop")):
            self.wait_for_condition(lambda stage=stage, node=node: (self._shard(stage, node) or {}).get("context", 0) > 0,
                                    timeout=30.0, description=f"stage {stage} loaded")

    def _get(self, path: str):
        r = self.api.get(path)
        return r.json() if r.status_code == 200 else None

    def _pods(self):
        return [p for p in (self._get("/api/v1/namespaces/default/pods") or {}).get("items", [])
                if p["metadata"].get("labels", {}).get("app") == "llm"]

    def _placement(self):
        out = {}
        for p in self._pods():
            if p["status"].get("phase") == "Running":
                out[p["metadata"]["name"].rsplit("-", 2)[0]] = p["spec"].get("nodeName")
        return out

    def _shard(self, stage: str, node: str):
        pods = [p for p in self._pods() if p["metadata"]["labels"].get("stage") == stage]
        if not pods:
            return None
        req = urllib.request.Request(f"http://127.0.0.1:{self.POD_PORTS[node]}/",
                                     headers={"X-Kube32-Pod": "default/" + pods[0]["metadata"]["name"]})
        try:
            with urllib.request.urlopen(req, timeout=5.0) as resp:
                return json.loads(resp.read().decode())
        except (urllib.error.URLError, OSError, ValueError):
            return None

    def _coordinator(self, method="GET", path="", body=None):
        req = urllib.request.Request(f"http://127.0.0.1:{self.INGRESS_PORT}/llm{path}", method=method,
                                     data=json.dumps(body).encode() if body is not None else None)
        with urllib.request.urlopen(req, timeout=10.0) as resp:
            return json.loads(resp.read().decode())

    def _generate(self, prompt: str, steps: int, during=None):
        """Starts a generation, calls during(status) while it runs, returns the final status"""
        start = self.wait_for_condition(lambda: self._coordinator("POST", "/generate", {"prompt": prompt, "steps": steps}),
                                        timeout=20.0, description="the coordinator takes a prompt")
        deadline = time.time() + 300
        while time.time() < deadline:
            st = self._coordinator()
            if st["state"] != "running":
                self.assertEqual(st["state"], "done", f"generation: {st['state']}")
                return st
            if during:
                during(st)
            time.sleep(0.1)
        raise TimeoutError(f"no end of generation {start['session']}")


class TestLlmPipeline(LlmClusterCase):
    def test_pipeline_matches_reference(self):
        """Tokens through three nodes equal the reference's; the Cardputer stage fits its pool and draws the text"""
        if self._skip():
            return
        self._cluster()

        # What the Cardputer's pods take as the board admits them: requests
        # plus Pod Overhead within the 80 KiB pool
        taken = 0.0
        for p in self._pods():
            if p["spec"].get("nodeName") == "cardputer":
                c = p["spec"]["containers"][0]
                taken += _kib(c["resources"]["requests"]["memory"]) + _kib(p["spec"].get("overhead", {}).get("memory", "0"))
        self.assertTrue(0 < taken <= CARDPUTER_POOL_KIB, f"the Cardputer's pods take {taken} KiB of its {CARDPUTER_POOL_KIB} KiB pool")

        with open(self.logs["cardputer"]) as f:
            screen_from = len(f.read())
        st = self._generate("Lily went to the park", 80)
        ref = self._reference("Lily went to the park", 80, st["context"])
        self.assertEqual(st["tokens"], ref["tokens"], "the pipeline's tokens differ from the reference's")
        self.assertEqual(st["text"], ref["text"])
        self.assertEqual(st["generated"], 80)
        self.assertEqual(st["replays"], 0)
        self.assertEqual([(s["stage"], s["layers"], s["node"]) for s in st["stages"]],
                         [(0, [0, 3], "devkit"), (1, [3, 4], "cardputer"), (2, [4, 5], "laptop")])
        self.assertGreater(st["tokens_per_s"], 0)
        print(f"         {st['tokens_per_s']} tokens/s, token round trip {st['token_ms']['mean']} ms, "
              + ", ".join(f"stage {s['stage']} {s['compute_ms']} ms" for s in st["stages"]))

        # The Cardputer stage drew every token after BOS: the prompt and the
        # generated text, without the spaces it drops where it wraps
        with open(self.logs["cardputer"]) as f:
            lines = f.read()[screen_from:].splitlines()
        drawn = [m.groups() for m in (re.match(r"^\[DISPLAY (\d+),(\d+)\] (.*)$", l) for l in lines) if m]
        text = "".join(t for x, y, t in drawn if y != "0")
        self.assertEqual(re.sub(r"\s", "", text), re.sub(r"\s", "", st["text"]), "the screen's text")
        self.assertIn("80 tokens", [t for x, y, t in drawn if y == "0"][-1])

    def test_stage_moves_when_its_node_leaves(self):
        """laptop leaves mid-generation: its stage moves to devkit with its cache and the tokens stay the reference's"""
        if self._skip():
            return
        self._cluster()
        left = []

        def leave(st):
            if not left and st["generated"] >= 20:
                left.append(st["generated"])
                self.nodes["laptop"].process.send_signal(signal.SIGTERM)

        st = self._generate("Once upon a time", 110, during=leave)
        self.assertTrue(left, "laptop left during the generation")
        ref = self._reference("Once upon a time", 110, st["context"])
        self.assertEqual(st["tokens"], ref["tokens"], "the tokens after the move differ from the reference's")
        self.assertEqual(st["replays"], 0, "the stage kept its cache")
        self.assertEqual(st["stages"][2]["node"], "devkit")
        moved = [e for e in (self._get("/api/v1/namespaces/default/events") or {}).get("items", []) if e.get("reason") == "Moved"]
        self.assertTrue(any("llm-shard-2" in e["involvedObject"]["name"] for e in moved), f"Moved events: {moved}")
