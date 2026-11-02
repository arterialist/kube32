"""Tests for `kube32 mcp`: the MCP protocol over stdio and HTTP, the limits grammar, the driver catalog,
and every tool against a fake cluster.

Run from the repository root:

    python3 -m unittest discover -s tools/tests -v

The fake cluster is a kubectl that keeps API objects in a JSON file and a
local HTTP server that answers as driver pods do. The server under test runs
as `kube32 mcp` and talks JSON-RPC on its stdin and stdout. The test that
builds C from source skips itself without clang and a wasm linker.
"""
import base64
import glob
import http.server
import io
import json
import os
import re
import shutil
import stat
import subprocess
import tempfile
import textwrap
import threading
import time
import unittest
import urllib.error
import urllib.request

from kube32_under_test import INSTALLED, REPO, cli_command

from kube32.build import find_rust_lld
from kube32.mcp import catalog, limits, protocol, reference

ECHO = os.path.join(REPO, "images", "echo.wasm")
CHIP_TEMP = os.path.join(REPO, "images", "driver-chip-temp.wasm")


def have_c_toolchain():
    return bool(shutil.which(os.environ.get("CLANG", "clang")) and find_rust_lld())


# ---------------------------------------------------------------------------
# protocol
# ---------------------------------------------------------------------------
def echo_server():
    calls = []

    def echo(args, ctx):
        calls.append(args)
        ctx.progress("working")
        if args.get("fail"):
            raise protocol.ToolError("it failed")
        return {"said": args["text"]}

    tool = protocol.Tool("echo", "Echo", "Says it back", {"text": {"type": "string"}, "fail": {"type": "boolean"},
                                                          "n": {"type": "integer"}}, ["text"], echo)
    resources = (lambda: [{"uri": "x://a", "name": "a"}], [{"uriTemplate": "x://{n}", "name": "t"}],
                 lambda uri: ("text/plain", "A") if uri == "x://a" else (_ for _ in ()).throw(KeyError(uri)))
    prompt = ({"name": "p", "arguments": [{"name": "who", "required": True}]},
              lambda a: {"messages": [{"role": "user", "content": {"type": "text", "text": "hi " + a["who"]}}]})
    server = protocol.Server("t", "1", "be nice", [tool], resources[0], resources[1], resources[2], {"p": prompt}, log=lambda t: None)
    return server, calls


def rpc(server, method, params=None, rid=1):
    return server.handle_message({"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}})


class TestProtocol(unittest.TestCase):
    def test_initialize_negotiates_the_version(self):
        server, _ = echo_server()
        r = rpc(server, "initialize", {"protocolVersion": "2025-06-18", "clientInfo": {"name": "c"}})["result"]
        self.assertEqual(r["protocolVersion"], "2025-06-18")
        self.assertEqual(r["serverInfo"]["name"], "t")
        self.assertEqual(r["instructions"], "be nice")
        self.assertEqual(set(r["capabilities"]), {"tools", "resources", "prompts"})
        r = rpc(server, "initialize", {"protocolVersion": "1999-01-01"})["result"]
        self.assertEqual(r["protocolVersion"], protocol.PROTOCOL_VERSIONS[0])

    def test_tools(self):
        server, calls = echo_server()
        tools = rpc(server, "tools/list")["result"]["tools"]
        self.assertEqual(tools[0]["inputSchema"]["required"], ["text"])
        self.assertTrue(tools[0]["annotations"]["readOnlyHint"])
        r = rpc(server, "tools/call", {"name": "echo", "arguments": {"text": "hi"}})["result"]
        self.assertEqual((r["isError"], json.loads(r["content"][0]["text"])), (False, {"said": "hi"}))
        r = rpc(server, "tools/call", {"name": "echo", "arguments": {"text": "hi", "fail": True}})["result"]
        self.assertEqual((r["isError"], r["content"][0]["text"]), (True, "it failed"))
        # Arguments that do not fit the schema come back as a tool error the agent can correct
        for args, why in [({}, "missing argument 'text'"), ({"text": 1}, "argument 'text' must be string"),
                          ({"text": "a", "n": True}, "argument 'n' must be integer"),
                          ({"text": "a", "bogus": 1}, "unknown argument 'bogus'")]:
            r = rpc(server, "tools/call", {"name": "echo", "arguments": args})["result"]
            self.assertTrue(r["isError"])
            self.assertIn(why, r["content"][0]["text"])
        self.assertEqual(len(calls), 2)
        self.assertEqual(rpc(server, "tools/call", {"name": "nope"})["error"]["code"], protocol.INVALID_PARAMS)

    def test_progress_goes_to_the_client_that_asked(self):
        server, _ = echo_server()
        sent = []
        server.handle_message({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                               "params": {"name": "echo", "arguments": {"text": "x"}, "_meta": {"progressToken": "t1"}}},
                              lambda m, p: sent.append((m, p)))
        self.assertEqual(sent, [("notifications/progress", {"progressToken": "t1", "progress": 1, "message": "working"})])
        sent.clear()
        rpc(server, "tools/call", {"name": "echo", "arguments": {"text": "x"}})
        self.assertEqual(sent, [])

    def test_resources_prompts_and_errors(self):
        server, _ = echo_server()
        self.assertEqual(rpc(server, "resources/list")["result"]["resources"][0]["uri"], "x://a")
        self.assertEqual(rpc(server, "resources/templates/list")["result"]["resourceTemplates"][0]["name"], "t")
        self.assertEqual(rpc(server, "resources/read", {"uri": "x://a"})["result"]["contents"][0]["text"], "A")
        self.assertEqual(rpc(server, "resources/read", {"uri": "x://b"})["error"]["code"], protocol.RESOURCE_NOT_FOUND)
        self.assertEqual(rpc(server, "prompts/list")["result"]["prompts"][0]["name"], "p")
        msg = rpc(server, "prompts/get", {"name": "p", "arguments": {"who": "you"}})["result"]["messages"][0]
        self.assertEqual(msg["content"]["text"], "hi you")
        self.assertEqual(rpc(server, "prompts/get", {"name": "p"})["error"]["code"], protocol.INVALID_PARAMS)
        self.assertEqual(rpc(server, "ping")["result"], {})
        self.assertEqual(rpc(server, "no/such")["error"]["code"], protocol.METHOD_NOT_FOUND)
        self.assertIsNone(server.handle_message({"jsonrpc": "2.0", "method": "notifications/initialized"}))
        self.assertEqual(server.handle_message({"id": 3})["error"]["code"], protocol.INVALID_REQUEST)
        batch = server.handle_message([{"jsonrpc": "2.0", "id": 1, "method": "ping"}, {"jsonrpc": "2.0", "method": "notifications/x"}])
        self.assertEqual(batch, [{"jsonrpc": "2.0", "id": 1, "result": {}}])

    def test_stdio_one_message_per_line(self):
        server, _ = echo_server()
        lines = [{"jsonrpc": "2.0", "id": 1, "method": "ping"}, "not json",
                 {"jsonrpc": "2.0", "method": "notifications/initialized"},
                 {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "echo", "arguments": {"text": "a\nb"}}}]
        stdin = io.BytesIO(b"".join((x if isinstance(x, str) else json.dumps(x)).encode() + b"\n\n" for x in lines))
        stdout = io.BytesIO()
        protocol.serve_stdio(server, stdin, stdout)
        out = [json.loads(line) for line in stdout.getvalue().decode().splitlines()]
        self.assertEqual([o.get("id") for o in out], [1, None, 2])
        self.assertEqual(out[1]["error"]["code"], protocol.PARSE_ERROR)
        self.assertIn("a\\nb", out[2]["result"]["content"][0]["text"])

    def test_http(self):
        server, _ = echo_server()
        httpd = protocol.make_http_server(server, "127.0.0.1", 0, token="s3cret")
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        url = "http://127.0.0.1:%d/mcp" % httpd.server_address[1]

        def post(body, headers=None, path=url, method="POST"):
            h = {"Content-Type": "application/json", "Authorization": "Bearer s3cret"}
            h.update(headers or {})
            req = urllib.request.Request(path, data=json.dumps(body).encode() if body is not None else None, headers=h, method=method)
            try:
                with urllib.request.urlopen(req, timeout=10) as resp:
                    return resp.status, resp.read()
            except urllib.error.HTTPError as e:
                with e:
                    return e.code, e.read()

        try:
            code, body = post({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-03-26"}})
            self.assertEqual((code, json.loads(body)["result"]["protocolVersion"]), (200, "2025-03-26"))
            self.assertEqual(post({"jsonrpc": "2.0", "method": "notifications/initialized"}), (202, b""))
            self.assertEqual(post(None, method="GET")[0], 405)
            self.assertEqual(post({"jsonrpc": "2.0", "id": 1, "method": "ping"}, {"Authorization": "Bearer wrong"})[0], 401)
            self.assertEqual(post({"jsonrpc": "2.0", "id": 1, "method": "ping"}, {"Origin": "http://evil.example"})[0], 403)
            self.assertEqual(post({"jsonrpc": "2.0", "id": 1, "method": "ping"}, {"Origin": "http://localhost:3000"})[0], 200)
            self.assertEqual(post({"jsonrpc": "2.0", "id": 1, "method": "ping"}, path=url[:-4] + "/other")[0], 404)
        finally:
            httpd.shutdown()
            httpd.server_close()


# ---------------------------------------------------------------------------
# limits and the catalog
# ---------------------------------------------------------------------------
LIMITS = """\
# the kitchen scale
deny tare: the calibration weight stays the zero point
led.r <= 64: the LED faces a bedroom
led every 5s
weight_g < 4500
"""
SCALE_ACTIONS = {"tare": {}, "led": {"r": "0-255", "g": "0-255", "b": "0-255"}}
SCALE_READINGS = {"weight_g": "g", "raw": "adc"}


class TestLimits(unittest.TestCase):
    def parsed(self, text=LIMITS):
        return limits.check_names(limits.parse(text), SCALE_READINGS, SCALE_ACTIONS)

    def test_parse(self):
        lim = self.parsed()
        self.assertEqual(lim.errors, [])
        self.assertEqual([(r.kind, r.line) for r in lim.rules], [("deny", 2), ("argument", 3), ("rate", 4), ("reading", 5)])
        self.assertEqual(str(lim.rules[1]), "led.r <= 64: the LED faces a bedroom")
        self.assertEqual(lim.rules[2].seconds, 5)
        self.assertEqual(limits.parse("x every 250ms").rules[0].seconds, 0.25)

    def test_actions(self):
        lim = self.parsed()
        self.assertIn("refused by the limit 'deny tare' (kube32.io/limits, line 2): the action tare is not allowed; "
                      "the limit says: the calibration weight stays the zero point", limits.check_action(lim, "tare", {}, {}, 0))
        self.assertIn("r is 200", limits.check_action(lim, "led", {"r": 200}, {}, 0))
        self.assertIn("the action must give r", limits.check_action(lim, "led", {"g": 1}, {}, 0))
        self.assertIn("r is true", limits.check_action(lim, "led", {"r": True}, {}, 0))     # a bool is no number
        self.assertIsNone(limits.check_action(lim, "led", {"r": 64}, {}, 0))
        self.assertIn("the last one was 2.0 s ago; wait 3.0 s", limits.check_action(lim, "led", {"r": 1}, {"led": 98.0}, 100.0))
        self.assertIsNone(limits.check_action(lim, "led", {"r": 1}, {"led": 90.0}, 100.0))
        self.assertIn("is not allowed", limits.check_action(limits.parse("deny *"), "anything", {}, {}, 0))
        rate_all = limits.parse("* every 1m")
        self.assertIn("wait", limits.check_action(rate_all, "set", {}, {"toggle": 50.0}, 60.0))
        self.assertIsNone(limits.check_action(limits.parse("set.on == false"), "set", {"on": False}, {}, 0))
        self.assertIn("on is true", limits.check_action(limits.parse("set.on == false"), "set", {"on": True}, {}, 0))

    def test_fail_closed(self):
        for text, error in [("led.r <== 64", "line 1: 'led.r <== 64' is not a rule"),
                            ("deny tar", "the driver has no action tar"),
                            ("led.red <= 64", "action led has no argument red"),
                            ("weight_kg < 5", "the driver has no reading weight_kg"),
                            ("led.r < bright", "< compares numbers only")]:
            lim = self.parsed(text)
            self.assertEqual(len(lim.errors), 1, text)
            self.assertIn(error, lim.errors[0])
            refusal = limits.check_action(lim, "led", {"r": 1}, {}, 0)
            self.assertIn("do not parse, so every action is refused", refusal)

    def test_readings(self):
        lim = self.parsed()
        self.assertEqual(limits.check_readings(lim, {"weight_g": 100.0}), [])
        self.assertEqual(limits.check_readings(lim, {"weight_g": 4800.5}), ["weight_g is 4800.5, outside the limit 'weight_g < 4500'"])
        self.assertEqual(limits.check_readings(lim, {"weight_g": None}), [])


@unittest.skipIf(INSTALLED and not os.path.isdir(os.path.join(REPO, "apps", "drivers")), "needs the repository's apps/drivers")
class TestCatalog(unittest.TestCase):
    def test_matches_the_drivers(self):
        """Each catalog entry is its driver's GET /describe (DESCRIBE) and default period (INTERVAL_MS)."""
        sources = sorted(glob.glob(os.path.join(REPO, "apps", "drivers", "*", "*.c")))
        seen = set()
        for src in sources:
            with open(src) as f:
                text = f.read()
            driver = re.search(r'#define DRIVER "([^"]+)"', text).group(1)
            describe = json.loads(re.search(r'#define DESCRIBE "(.*)"\n', text).group(1).replace('\\"', '"'))
            period = int(re.search(r"#define INTERVAL_MS (\d+)", text).group(1))
            entry = catalog.DRIVERS[driver]
            self.assertEqual((entry["readings"], entry["actions"], entry["period_ms"]),
                             (describe["readings"], describe["actions"], period), driver)
            seen.add(driver)
        self.assertEqual(seen, set(catalog.DRIVERS))

    def test_arguments(self):
        self.assertIsNone(catalog.check_argument("0-255", 7))
        self.assertEqual(catalog.check_argument("0-255", 300), "takes 0 to 255, not 300")
        self.assertEqual(catalog.check_argument("0-255", 1.5), "takes a whole number from 0 to 255")
        self.assertEqual(catalog.check_argument("bool", 1), "takes true or false")
        self.assertIsNone(catalog.check_argument("bool", False))


# ---------------------------------------------------------------------------
# The tools against a fake cluster
# ---------------------------------------------------------------------------
# kubectl over a JSON file {"objects": {path: object}, "logs": {pod: text}}.
# get --raw lists a collection from the objects under it (labelSelector and
# fieldSelector a=b filters); create --raw stores the body (an activate starts
# the Device's driver; a Pod runs at once; a Deployment gets one pod); delete
# --raw removes the object (with a Deployment's pods). Each call is logged.
FAKE_KUBECTL = textwrap.dedent('''\
    #!/usr/bin/env python3
    import json, os, sys, urllib.parse
    state_path, log_path = os.environ["FAKE_STATE"], os.environ["FAKE_LOG"]
    state = json.load(open(state_path))
    objs = state["objects"]
    args = sys.argv[1:]
    with open(log_path, "a") as f:
        f.write(json.dumps(args) + "\\n")
    def save():
        json.dump(state, open(state_path, "w"))
    def fail(msg):
        sys.stderr.write(msg + "\\n")
        sys.exit(1)
    if args[0] == "logs":
        pod = args[1]
        if pod not in state.get("logs", {}):
            fail('Error from server (BadRequest): container "%s" is waiting to start: ContainerCreating' % pod)
        print(state["logs"][pod])
        sys.exit(0)
    verb, raw = args[0], args[args.index("--raw") + 1]
    path, _, query = raw.partition("?")
    q = dict(urllib.parse.parse_qsl(query))
    def under(prefix):
        out = []
        for k, v in objs.items():
            rest = k[len(prefix):]
            if k.startswith(prefix + "/") and rest.count("/") == 1:
                out.append(v)
            elif prefix == "/apis/kube32.io/v1/devices" and "/devices/" in k and k.split("/devices/")[1].count("/") == 0:
                out.append(v)
        return out
    def match(o, sel, field=False):
        for part in filter(None, sel.split(",")):
            key, _, want = part.partition("=")
            have = o
            if field:
                for p in key.split("."):
                    have = (have or {}).get(p)
            else:
                have = (o["metadata"].get("labels") or {}).get(key)
            if have != want:
                return False
        return True
    if verb == "get":
        if path in objs:
            print(json.dumps(objs[path])); sys.exit(0)
        items = under(path)
        if items or path.endswith(("/pods", "/events", "/devices", "/nodes")):
            items = [o for o in items if match(o, q.get("labelSelector", "")) and match(o, q.get("fieldSelector", ""), True)]
            print(json.dumps({"items": items})); sys.exit(0)
        fail('Error from server (NotFound): the server could not find the requested resource')
    if verb == "create":
        body = json.load(open(args[args.index("-f") + 1]))
        if path.endswith("/activate"):
            dev = objs[path[:-len("/activate")]]
            state.setdefault("activated", []).append(path)
            if dev["status"].get("phase") == "Idle":
                dev["status"].update({"phase": "Running", "podName": dev["metadata"]["name"] + "-driver"})
            save(); print('{"kind":"Status","status":"Success"}'); sys.exit(0)
        name = body["metadata"]["name"]
        if path + "/" + name in objs:
            fail('Error from server (AlreadyExists): %s "%s" already exists' % (body["kind"], name))
        objs[path + "/" + name] = body
        if body["kind"] == "Pod":
            body["spec"]["nodeName"] = body["spec"].get("nodeSelector", {}).get("kubernetes.io/hostname", "board-2")
            body["status"] = {"phase": "Running", "podIP": "127.0.0.1",
                              "containerStatuses": [{"restartCount": 0, "state": {"running": {}}}]}
            state.setdefault("logs", {})[name] = "app: started"
        if body["kind"] == "Deployment":
            t = body["spec"]["template"]
            pod = {"apiVersion": "v1", "kind": "Pod", "metadata": {"name": name + "-5d8f9", "namespace": body["metadata"]["namespace"],
                   "labels": t["metadata"]["labels"]}, "spec": dict(t["spec"], nodeName="board-2"),
                   "status": {"phase": "Running", "containerStatuses": [{"restartCount": 0, "state": {"running": {}}}]}}
            objs[path.replace("/apis/apps/v1", "/api/v1").replace("/deployments", "/pods") + "/" + name + "-5d8f9"] = pod
            state.setdefault("logs", {})[name + "-5d8f9"] = "app: started"
        save(); print(json.dumps(body)); sys.exit(0)
    if verb == "delete":
        if path not in objs:
            fail('Error from server (NotFound): not found')
        gone = objs.pop(path)
        if gone["kind"] == "Deployment":
            for k in [k for k, v in objs.items() if v.get("kind") == "Pod" and (v["metadata"].get("labels") or {}).get("app") == gone["metadata"]["name"]]:
                objs.pop(k)
        save(); print(json.dumps(gone)); sys.exit(0)
    sys.exit(2)
''')

DEV = "/apis/kube32.io/v1/namespaces/default/devices/"
POD = "/api/v1/namespaces/default/pods/"


class DriverServer:
    """Answers as driver pods on a node's pod server: by X-Kube32-Pod, GET /, GET /describe and POST /."""

    def __init__(self):
        self.posts = []
        self.readings = {"default/kitchen-env-driver": {"temperature_c": 21.4, "humidity_pct": 41.0, "pressure_hpa": 1007.2, "gas_ohms": None},
                         "default/scale-driver": {"weight_g": 5012.5, "raw": 8388}}
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def answer(self, code, body):
                data = json.dumps(body).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                pod = self.headers.get("X-Kube32-Pod", "")
                driver = "bme688" if "kitchen" in pod else "miniscale"
                if self.path == "/describe":
                    d = catalog.DRIVERS[driver]
                    return self.answer(200, {"driver": driver, "readings": d["readings"], "actions": d["actions"]})
                if pod not in outer.readings:
                    return self.answer(503, {"ok": False, "error": "no answer at 0x26 on bus 0 (error -19)"})
                self.answer(200, {"ok": True, "uptime_ms": int(time.time() * 1000) % 100000, "readings": outer.readings[pod]})

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                outer.posts.append((self.headers.get("X-Kube32-Pod"), body))
                self.answer(200, {"ok": True, "readings": outer.readings.get(self.headers.get("X-Kube32-Pod"), {})})

            def log_message(self, *args):
                pass

        self.httpd = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class McpProcess:
    """`kube32 mcp` with JSON-RPC on its stdin and stdout."""

    def __init__(self, args, env):
        self.proc = subprocess.Popen(cli_command() + ["mcp"] + args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, env=env, cwd=REPO)
        self.watchdog = threading.Timer(120, self.proc.kill)
        self.watchdog.start()
        self.next_id = 0
        r = self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test"}})
        assert r["result"]["serverInfo"]["name"] == "kube32", r
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def send(self, msg):
        self.proc.stdin.write((json.dumps(msg) + "\n").encode())
        self.proc.stdin.flush()

    def request(self, method, params):
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params})
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise AssertionError("the server closed stdout: %s" % self.proc.stderr.read().decode())
            msg = json.loads(line)
            if msg.get("id") == self.next_id:
                return msg

    def call(self, tool, **arguments):
        """(isError, the text, or the JSON of the text)"""
        r = self.request("tools/call", {"name": tool, "arguments": arguments})["result"]
        text = r["content"][0]["text"]
        try:
            return r["isError"], json.loads(text)
        except ValueError:
            return r["isError"], text

    def close(self):
        self.proc.stdin.close()
        rc = self.proc.wait(timeout=30)
        self.watchdog.cancel()
        err = self.proc.stderr.read().decode()
        self.proc.stdout.close()
        self.proc.stderr.close()
        return rc, err


class TestTools(unittest.TestCase):
    def setUp(self):
        self.tmp = os.path.realpath(tempfile.mkdtemp(prefix="kube32-mcp-test-"))
        self.driver = DriverServer()
        self.kubectl = os.path.join(self.tmp, "kubectl")
        with open(self.kubectl, "w") as f:
            f.write(FAKE_KUBECTL)
        os.chmod(self.kubectl, os.stat(self.kubectl).st_mode | stat.S_IXUSR)
        self.state_path = os.path.join(self.tmp, "state.json")
        self.log_path = os.path.join(self.tmp, "kubectl.log")
        port = self.driver.port

        def device(name, driver, phase, notes=None, limits_text=None, ns="default"):
            ann = {}
            if notes:
                ann["kube32.io/notes"] = notes
            if limits_text:
                ann["kube32.io/limits"] = limits_text
            st = {"phase": phase, "nodeName": "board-2", "chip": driver.upper()}
            if phase == "Running":
                st["podName"] = name + "-driver"
            else:
                st["message"] = "idle: the driver starts on the next use"
            return {"apiVersion": "kube32.io/v1", "kind": "Device",
                    "metadata": {"name": name, "namespace": ns, "uid": "u-" + name, "annotations": ann, "labels": {"location": "kitchen"}},
                    "spec": {"driver": driver, "nodeName": "board-2", "activation": "OnDemand", "idleSeconds": 300,
                             "i2c": {"bus": 0, "address": "0x77", "mux": {"address": "0x70", "channel": 2}}},
                    "status": st}

        def driver_pod(name):
            return {"apiVersion": "v1", "kind": "Pod", "metadata": {"name": name, "namespace": "default", "labels": {}},
                    "spec": {"nodeName": "board-2", "containers": [{"name": "driver", "ports": [{"name": "http", "containerPort": port}]}]},
                    "status": {"phase": "Running", "podIP": "127.0.0.1", "containerStatuses": [{"restartCount": 0, "state": {"running": {}}}]}}

        node = {"metadata": {"name": "board-2", "labels": {"kube32.io/board": "cardputer"},
                             "annotations": {"kube32.io/memory-free": "40Ki",
                                             "kube32.io/i2c-devices": json.dumps([{"bus": 0, "address": "0x70", "chip": "PCA9548A"},
                                                                                   {"bus": 0, "mux": "0x70", "channel": 2, "address": "0x77", "chip": "BME688"}])}},
                "status": {"conditions": [{"type": "Ready", "status": "True"}], "allocatable": {"memory": "80Ki"}}}
        objects = {
            "/api/v1/nodes/board-2": node,
            DEV + "kitchen-env": device("kitchen-env", "bme688", "Idle", notes="On the shelf above the kettle.",
                                        limits_text="humidity_pct <= 80: the shelf is rated to 80 %RH"),
            DEV + "scale": device("scale", "miniscale", "Running", limits_text=LIMITS),
            DEV + "broken": device("broken", "miniscale", "Running", limits_text="led.red <= 1"),
            POD + "kitchen-env-driver": driver_pod("kitchen-env-driver"),
            POD + "scale-driver": driver_pod("scale-driver"),
            POD + "broken-driver": driver_pod("broken-driver"),
            POD + "other": {"apiVersion": "v1", "kind": "Pod", "metadata": {"name": "other", "namespace": "default", "labels": {"app": "other"}},
                            "spec": {"nodeName": "board-1"}, "status": {"phase": "Running"}},
        }
        with open(self.state_path, "w") as f:
            json.dump({"objects": objects, "logs": {"other": "other: hello\nother: still here"}}, f)
        self.env = dict(os.environ, FAKE_STATE=self.state_path, FAKE_LOG=self.log_path, KUBE32_CACHE_DIR=os.path.join(self.tmp, "cache"))
        self.servers = []

    def tearDown(self):
        for s in self.servers:
            if s.proc.poll() is None:
                s.proc.kill()
            s.watchdog.cancel()
            for stream in (s.proc.stdin, s.proc.stdout, s.proc.stderr):
                try:
                    stream.close()
                except (OSError, ValueError):
                    pass
            s.proc.wait()
        self.driver.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def server(self, *flags):
        s = McpProcess(list(flags) + ["--kubectl", self.kubectl], self.env)
        self.servers.append(s)
        return s

    def state(self):
        with open(self.state_path) as f:
            return json.load(f)

    def test_list_devices(self):
        s = self.server()
        err, out = s.call("list_devices")
        self.assertFalse(err, out)
        self.assertEqual(out["nodes"][0]["name"], "board-2")
        self.assertEqual(out["nodes"][0]["i2c_parts"], ["0x70 PCA9548A", "mux 0x70 channel 2, 0x77 BME688"])
        devices = {d["device"]: d for d in out["devices"]}
        env = devices["default/kitchen-env"]
        self.assertEqual((env["driver"], env["phase"], env["address"]), ("bme688", "Idle", "i2c0/0x70:2/0x77"))
        self.assertEqual(env["reads"]["humidity_pct"], "%RH")
        self.assertEqual(env["notes"], "On the shelf above the kettle.")
        self.assertEqual(env["limits"], ["humidity_pct <= 80: the shelf is rated to 80 %RH"])
        self.assertEqual(devices["default/scale"]["actions"], ["tare", "led"])
        self.assertIn("action led has no argument red", devices["default/broken"]["limit_errors"][0])
        self.assertTrue(out["writes"].startswith("off"))
        self.assertNotIn("activated", self.state())        # listing starts no driver
        rc, err_text = s.close()
        self.assertEqual(rc, 0)
        self.assertIn("kube32 mcp: serving on stdio (writes off, deploys off", err_text)

    def test_read_starts_an_idle_device_and_samples(self):
        s = self.server()
        err, out = s.call("read_device", device="kitchen-env")
        self.assertFalse(err, out)
        self.assertEqual(out["readings"]["humidity_pct"], 41.0)
        self.assertEqual(out["units"]["humidity_pct"], "%RH")
        self.assertEqual(self.state()["activated"], [DEV + "kitchen-env/activate"])
        self.driver.readings["default/kitchen-env-driver"] = dict(self.driver.readings["default/kitchen-env-driver"], humidity_pct=85.5)
        err, out = s.call("read_device", device="default/kitchen-env", samples=3, interval_seconds=0.2)
        self.assertFalse(err, out)
        self.assertEqual(out["samples"]["columns"][:3], ["t_s", "uptime_ms", "temperature_c"])
        self.assertEqual(len(out["samples"]["rows"]), 3)
        self.assertEqual(out["summary"]["humidity_pct"]["max"], 85.5)
        self.assertNotIn("gas_ohms", out["summary"])                       # null is no number
        self.assertEqual(out["warnings"], ["humidity_pct is 85.5, outside the limit 'humidity_pct <= 80' (the shelf is rated to 80 %RH)"])
        for args, why in [({"device": "nope"}, "no Device nope"), ({"device": "scale", "samples": 0}, "samples is 1 to 120"),
                          ({"device": "scale", "samples": 100, "interval_seconds": 10}, "at most 300 s")]:
            err, out = s.call("read_device", **args)
            self.assertTrue(err)
            self.assertIn(why, out)

    def test_writes_are_off_by_default(self):
        s = self.server()
        err, out = s.call("write_device", device="scale", action="led", arguments={"r": 1})
        self.assertTrue(err)
        self.assertIn("writes are off", out)
        self.assertEqual(self.driver.posts, [])

    def test_write_within_limits(self):
        s = self.server("--allow-writes")
        err, out = s.call("write_device", device="scale", action="tare")
        self.assertTrue(err)
        self.assertIn("refused by the limit 'deny tare'", out)
        err, out = s.call("write_device", device="scale", action="led", arguments={"r": 200})
        self.assertIn("r is 200; the limit says: the LED faces a bedroom", out)
        err, out = s.call("write_device", device="scale", action="led", arguments={"r": 300})
        self.assertIn("led.r takes 0 to 255, not 300", out)                 # the driver's own range first
        err, out = s.call("write_device", device="scale", action="spin")
        self.assertIn("has no action spin; it has: led, tare", out)
        err, out = s.call("write_device", device="scale", action="led", arguments={"r": 10, "g": 20})
        self.assertFalse(err, out)
        self.assertEqual(out["answer"]["ok"], True)
        self.assertEqual(self.driver.posts, [("default/scale-driver", {"r": 10, "g": 20, "action": "led"})])
        err, out = s.call("write_device", device="scale", action="led", arguments={"r": 10})
        self.assertIn("refused by the limit 'led every 5s'", out)
        err, out = s.call("write_device", device="broken", action="led", arguments={"r": 1})
        self.assertIn("do not parse, so every action is refused", out)
        err, out = s.call("write_device", device="kitchen-env", action="led")
        self.assertIn("has no action led; it has: none, it only reads", out)
        self.assertEqual(len(self.driver.posts), 1)
        events = [v for k, v in self.state()["objects"].items() if k.startswith("/api/v1/namespaces/default/events/")]
        self.assertEqual(len(events), 1)
        self.assertEqual((events[0]["involvedObject"]["kind"], events[0]["involvedObject"]["name"], events[0]["reason"]),
                         ("Device", "scale", "Action"))
        self.assertIn('sent led {"r": 10, "g": 20}', events[0]["message"])
        _, err_text = s.close()
        self.assertIn("kube32 mcp: refused default/scale tare {}", err_text)

    def test_describe_and_resources(self):
        s = self.server()
        err, doc = s.call("describe_device", device="kitchen-env")
        self.assertFalse(err, doc)
        for text in ["# Device kitchen-env", "Bosch BME688", "i2c0/0x70:2/0x77", "| `humidity_pct` | %RH | 41.0 |",
                     "- humidity_pct <= 80: the shelf is rated to 80 %RH", "On the shelf above the kettle.",
                     "None: the driver only reads.", "channel topic `device.kitchen-env`", "Labels: location=kitchen"]:
            self.assertIn(text, doc)
        err, doc = s.call("describe_device", device="scale", live=False)
        self.assertIn("- `led` `r`: 0-255", doc)
        self.assertNotIn("Latest", doc)
        uris = [r["uri"] for r in s.request("resources/list", {})["result"]["resources"]]
        self.assertIn("kube32://devices/default/scale", uris)
        self.assertIn(reference.GUIDE_URI, uris)
        text = s.request("resources/read", {"uri": "kube32://devices/default/scale"})["result"]["contents"][0]["text"]
        self.assertIn("- deny tare: the calibration weight stays the zero point", text)
        text = s.request("resources/read", {"uri": reference.ABI_URI})["result"]["contents"][0]["text"]
        self.assertIn('K32_IMPORT("k32_event_emit")', text)
        text = s.request("resources/read", {"uri": reference.REFERENCE_URI})["result"]["contents"][0]["text"]
        self.assertEqual(text.count("# Device "), 3)
        self.assertEqual(s.request("resources/read", {"uri": "kube32://devices/default/nope"})["error"]["code"], protocol.RESOURCE_NOT_FOUND)
        prompt = s.request("prompts/get", {"name": "explore-then-codify", "arguments": {"device": "kitchen-env", "goal": "warn on damp air"}})
        self.assertIn("Goal: warn on damp air", prompt["result"]["messages"][0]["content"]["text"])

    def test_deploy_needs_opt_in(self):
        s = self.server("--allow-writes")
        for tool, args in [("deploy_code", {"name": "x", "path": ECHO}), ("delete_workload", {"name": "other"})]:
            err, out = s.call(tool, **args)
            self.assertTrue(err)
            self.assertIn("deploys are off", out)

    def test_deploy_a_module_bound_to_a_device(self):
        s = self.server("--allow-deploy")
        err, out = s.call("deploy_code", name="echo-env", path=ECHO, device="kitchen-env", near_device=True, env={"MODE": "fast", "N": 3})
        self.assertFalse(err, out)
        self.assertEqual((out["kind"], out["hardware"], out["node"], out["memory"]), ("request", ["ipc"], "board-2", "64Ki"))
        self.assertEqual(out["status"]["phase"], "Running")
        self.assertEqual(out["log"], "app: started")
        pod = self.state()["objects"][POD + "echo-env"]
        meta, spec = pod["metadata"], pod["spec"]
        with open(ECHO, "rb") as f:
            self.assertEqual(base64.b64decode(meta["annotations"]["kube32.io/wasm-base64"]), f.read())
        self.assertEqual(meta["labels"], {"app": "echo-env", "app.kubernetes.io/managed-by": "kube32-mcp"})
        self.assertEqual(meta["annotations"]["kube32.io/event-topics"], "device.kitchen-env")
        self.assertEqual(spec["nodeSelector"], {"kubernetes.io/hostname": "board-2"})
        c = spec["containers"][0]
        self.assertEqual({e["name"]: e["value"] for e in c["env"]},
                         {"DEVICE_NAME": "kitchen-env", "DEVICE_NAMESPACE": "default", "DEVICE_TOPIC": "device.kitchen-env",
                          "DEVICE_URL": "http://kitchen-env/", "MODE": "fast", "N": "3"})
        self.assertEqual(c["resources"]["limits"], {"memory": "64Ki", "hardware.kube32.io/ipc": "1"})
        self.assertEqual(c["securityContext"]["capabilities"]["add"], ["CHANNEL_IPC"])
        self.assertEqual(c["ports"], [{"containerPort": 7000, "name": "http"}])
        # Deploying again replaces the pod deploy_code made
        err, out = s.call("deploy_code", name="echo-env", path=ECHO, wait_seconds=0)
        self.assertFalse(err, out)
        self.assertNotIn("nodeSelector", self.state()["objects"][POD + "echo-env"]["spec"])
        with open(self.log_path) as f:
            calls = [json.loads(line) for line in f]
        self.assertIn(["delete", "--raw", POD + "echo-env"], calls)
        # ... and refuses to replace anything else
        err, out = s.call("deploy_code", name="other", path=ECHO)
        self.assertIn("Pod other exists and was not deployed by kube32 mcp", out)

    def test_deploy_refusals(self):
        s = self.server("--allow-deploy")
        err, out = s.call("deploy_code", name="temp", path=CHIP_TEMP)
        self.assertTrue(err)
        self.assertIn("deploy_code grants events (ipc) and Service calls only", out)
        for args, why in [({"name": "Bad_Name", "path": ECHO}, "name: lowercase letters"),
                          ({"name": "x"}, "give either source"),
                          ({"name": "x", "source": "int x;", "path": ECHO}, "give either source"),
                          ({"name": "x", "path": os.path.join(self.tmp, "none.wasm")}, "no such file"),
                          ({"name": "x", "path": ECHO, "env": {"1X": "v"}}, "env: names are letters"),
                          ({"name": "x", "path": ECHO, "kind": "Job"}, "argument 'kind' must be one of"),
                          ({"name": "x", "path": ECHO, "memory": "lots"}, "memory: invalid quantity 'lots'"),
                          ({"name": "x", "path": ECHO, "wait_seconds": 500}, "wait_seconds is 0 to 120")]:
            err, out = s.call("deploy_code", **args)
            self.assertTrue(err, args)
            self.assertIn(why, out, args)

    def test_deployment_status_and_delete(self):
        s = self.server("--allow-deploy")
        err, out = s.call("deploy_code", name="echo-d", path=ECHO, kind="Deployment", wait_seconds=5)
        self.assertFalse(err, out)
        self.assertEqual(out["status"]["pod"], "echo-d-5d8f9")
        d = self.state()["objects"]["/apis/apps/v1/namespaces/default/deployments/echo-d"]
        self.assertIn("kube32.io/wasm-base64", d["spec"]["template"]["metadata"]["annotations"])
        err, out = s.call("workload_status", name="echo-d")
        self.assertFalse(err, out)
        self.assertEqual((out["kind"], out["managed_by_kube32_mcp"], out["pods"][0]["log"]), ("Deployment", True, "app: started"))
        err, out = s.call("workload_status", name="other", tail_lines=5)
        self.assertEqual((out["kind"], out["managed_by_kube32_mcp"], out["pods"][0]["log"]), ("Pod", False, "other: hello\nother: still here"))
        err, out = s.call("delete_workload", name="other")
        self.assertIn("was not deployed by kube32 mcp", out)
        err, out = s.call("delete_workload", name="echo-d")
        self.assertEqual(out, {"deleted": "Deployment default/echo-d"})
        self.assertFalse([k for k in self.state()["objects"] if "echo-d" in k])
        err, out = s.call("workload_status", name="echo-d")
        self.assertIn("no pod or Deployment echo-d", out)

    @unittest.skipUnless(have_c_toolchain(), "clang with the wasm32 target and a wasm linker are needed")
    def test_deploy_c_source(self):
        s = self.server("--allow-deploy")
        err, out = s.call("deploy_code", name="humidity-watch", source=reference.EXAMPLE_POD, device="kitchen-env", wait_seconds=0)
        self.assertFalse(err, out)
        self.assertEqual((out["kind"], out["hardware"]), ("event", ["ipc"]))
        self.assertLessEqual(int(out["memory"].rstrip("Ki")), 8)            # sized from __heap_base, not a whole page
        self.assertEqual(out["build"][-1], "ok")
        self.assertTrue(os.path.isfile(os.path.join(self.tmp, "cache", "mcp", "humidity-watch", "src", "humidity-watch.c")))
        err, out = s.call("deploy_code", name="humidity-watch", source=reference.EXAMPLE_POD, memory="2Ki")
        self.assertTrue(err)
        self.assertIn("memory 2Ki is less than the", out)
        err, out = s.call("deploy_code", name="bad", source='#include "runtime/k32_app_abi.h"\nint f(void) { return nope; }\n')
        self.assertTrue(err)
        self.assertIn("the build failed", out)
        self.assertIn("use of undeclared identifier 'nope'", out)
        err, out = s.call("deploy_code", name="caller", source='#include "runtime/k32_app_abi.h"\nstatic char b[8];\n'
                          'K32_EXPORT("k32_init") int32_t k32_init(void) { uint32_t n; return k32_http_call("GET", 3, "http://a/", 9, 0, 0, 0, 0, b, 8, &n, 0); }\n')
        self.assertTrue(err)
        self.assertIn("deploys it only with --allow-writes", out)


class TestCommand(unittest.TestCase):
    def test_http_flags(self):
        from kube32.mcp import parse_listen
        self.assertEqual(parse_listen("8765"), ("127.0.0.1", 8765))
        self.assertEqual(parse_listen("0.0.0.0:80"), ("0.0.0.0", 80))
        self.assertEqual(parse_listen("[::1]:8765"), ("::1", 8765))
        with self.assertRaises(ValueError):
            parse_listen("host:port")
        proc = subprocess.run(cli_command() + ["mcp", "--http", "0.0.0.0:8765"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, env=dict(os.environ, KUBE32_MCP_TOKEN=""), timeout=60)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("give it --token", proc.stderr)


if __name__ == "__main__":
    unittest.main()
