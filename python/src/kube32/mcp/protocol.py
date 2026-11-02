"""The Model Context Protocol server side: JSON-RPC 2.0 over stdio and over streamable HTTP.

The server offers tools, resources and prompts. Over stdio each message is one
line of JSON on stdin and stdout; log text goes to stderr. Over HTTP one
endpoint (/mcp) takes JSON-RPC messages in POST requests and answers with
application/json; it opens no event streams (GET answers 405).
"""
import http.server
import json
import socket
import sys
import threading
import traceback
from urllib.parse import urlsplit

# Newest first; a client that asks for another version gets the newest
PROTOCOL_VERSIONS = ("2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05")

PARSE_ERROR, INVALID_REQUEST, METHOD_NOT_FOUND, INVALID_PARAMS, INTERNAL_ERROR = -32700, -32600, -32601, -32602, -32603
RESOURCE_NOT_FOUND = -32002


class ToolError(Exception):
    """A tool that could not do its work: the agent receives the message as a tool result with isError."""


class RpcError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


class Tool:
    def __init__(self, name, title, description, properties, required, fn, read_only=True, destructive=False,
                 idempotent=False):
        self.name = name
        self.fn = fn
        self.properties = properties
        self.required = list(required)
        self.definition = {
            "name": name,
            "title": title,
            "description": description,
            "inputSchema": {"type": "object", "properties": properties, "required": self.required, "additionalProperties": False},
            "annotations": {"title": title, "readOnlyHint": read_only, "destructiveHint": destructive,
                            "idempotentHint": idempotent, "openWorldHint": True},
        }


_JSON_TYPES = {"string": (str,), "integer": (int,), "number": (int, float), "boolean": (bool,), "object": (dict,), "array": (list,)}


def check_arguments(tool, args):
    """Why the arguments do not fit the tool's input schema, or None."""
    if not isinstance(args, dict):
        return "arguments must be an object"
    for key in tool.required:
        if key not in args:
            return "missing argument '%s'" % key
    for key, value in args.items():
        schema = tool.properties.get(key)
        if schema is None:
            return "unknown argument '%s' (the tool takes %s)" % (key, ", ".join(sorted(tool.properties)) or "none")
        types = schema.get("type")
        types = types if isinstance(types, list) else [types]
        ok = any(isinstance(value, _JSON_TYPES[t]) and not (t in ("integer", "number") and isinstance(value, bool))
                 for t in types if t in _JSON_TYPES)
        if not ok:
            return "argument '%s' must be %s" % (key, " or ".join(types))
        if "enum" in schema and value not in schema["enum"]:
            return "argument '%s' must be one of %s" % (key, ", ".join(json.dumps(v) for v in schema["enum"]))
    return None


class Context:
    """What a tool may do besides returning: report progress when the client asked for it."""

    def __init__(self, notify=None, token=None):
        self._notify = notify
        self._token = token
        self._count = 0

    def progress(self, message, done=None, total=None):
        if self._notify is None or self._token is None:
            return
        self._count += 1
        params = {"progressToken": self._token, "progress": done if done is not None else self._count, "message": message}
        if total is not None:
            params["total"] = total
        self._notify("notifications/progress", params)


class Server:
    """Dispatches JSON-RPC messages to tools, resources and prompts.

    resources() lists {uri, name, ...}; read_resource(uri) returns (mime type, text) or raises KeyError;
    prompts maps a name to (definition, fn(arguments) -> {description, messages})."""

    def __init__(self, name, version, instructions, tools, resources=None, resource_templates=(), read_resource=None,
                 prompts=None, log=None):
        self.name = name
        self.version = version
        self.instructions = instructions
        self.tools = {t.name: t for t in tools}
        self.resources = resources or (lambda: [])
        self.resource_templates = list(resource_templates)
        self.read_resource = read_resource
        self.prompts = prompts or {}
        self.log = log or (lambda text: print(text, file=sys.stderr, flush=True))
        self.client = None

    # -- messages --------------------------------------------------------------
    def handle_message(self, msg, notify=None):
        """The answer to one parsed message, or a list for a batch; None when nothing answers."""
        if isinstance(msg, list):
            if not msg:
                return error_response(None, INVALID_REQUEST, "empty batch")
            answers = [a for a in (self.handle_message(m, notify) for m in msg) if a is not None]
            return answers or None
        if not isinstance(msg, dict) or msg.get("jsonrpc") != "2.0" or not isinstance(msg.get("method", ""), str):
            return error_response(msg.get("id") if isinstance(msg, dict) else None, INVALID_REQUEST, "not a JSON-RPC 2.0 message")
        if "method" not in msg:
            return None                    # a response to a request of ours: the server sends none
        method, rid, params = msg["method"], msg.get("id"), msg.get("params") or {}
        if "id" not in msg:
            return None                    # a notification: initialized, cancelled, roots changed
        try:
            if not isinstance(params, dict):
                raise RpcError(INVALID_PARAMS, "params must be an object")
            return {"jsonrpc": "2.0", "id": rid, "result": self.dispatch(method, params, notify)}
        except RpcError as e:
            return error_response(rid, e.code, str(e))
        except Exception as e:  # noqa: BLE001 - a bug must not take the server down
            self.log("kube32 mcp: %s failed:\n%s" % (method, traceback.format_exc()))
            return error_response(rid, INTERNAL_ERROR, "%s: %s" % (type(e).__name__, e))

    def dispatch(self, method, params, notify):
        if method == "initialize":
            asked = params.get("protocolVersion")
            self.client = params.get("clientInfo") or {}
            return {
                "protocolVersion": asked if asked in PROTOCOL_VERSIONS else PROTOCOL_VERSIONS[0],
                "capabilities": {"tools": {"listChanged": False}, "resources": {"subscribe": False, "listChanged": False},
                                 "prompts": {"listChanged": False}},
                "serverInfo": {"name": self.name, "title": self.name, "version": self.version},
                "instructions": self.instructions,
            }
        if method == "ping":
            return {}
        if method == "tools/list":
            return {"tools": [t.definition for t in self.tools.values()]}
        if method == "tools/call":
            return self.call_tool(params, notify)
        if method == "resources/list":
            return {"resources": self.resources()}
        if method == "resources/templates/list":
            return {"resourceTemplates": self.resource_templates}
        if method == "resources/read":
            uri = params.get("uri")
            if not isinstance(uri, str) or self.read_resource is None:
                raise RpcError(INVALID_PARAMS, "resources/read takes a uri")
            try:
                mime, text = self.read_resource(uri)
            except KeyError:
                raise RpcError(RESOURCE_NOT_FOUND, "no resource %s" % uri)
            except ToolError as e:
                raise RpcError(INTERNAL_ERROR, str(e))
            return {"contents": [{"uri": uri, "mimeType": mime, "text": text}]}
        if method == "prompts/list":
            return {"prompts": [definition for definition, _ in self.prompts.values()]}
        if method == "prompts/get":
            entry = self.prompts.get(params.get("name"))
            if entry is None:
                raise RpcError(INVALID_PARAMS, "no prompt %s" % params.get("name"))
            definition, fn = entry
            args = params.get("arguments") or {}
            for a in definition.get("arguments", []):
                if a.get("required") and not args.get(a["name"]):
                    raise RpcError(INVALID_PARAMS, "prompt %s needs the argument %s" % (definition["name"], a["name"]))
            return fn(args)
        if method == "logging/setLevel":
            return {}
        raise RpcError(METHOD_NOT_FOUND, "no method %s" % method)

    def call_tool(self, params, notify):
        tool = self.tools.get(params.get("name"))
        if tool is None:
            raise RpcError(INVALID_PARAMS, "no tool %s (tools: %s)" % (params.get("name"), ", ".join(self.tools)))
        args = params.get("arguments") or {}
        why = check_arguments(tool, args)
        if why:
            return tool_result("%s: %s" % (tool.name, why), error=True)
        token = (params.get("_meta") or {}).get("progressToken")
        try:
            out = tool.fn(args, Context(notify, token))
        except ToolError as e:
            return tool_result(str(e), error=True)
        return tool_result(out)


def tool_result(out, error=False):
    text = out if isinstance(out, str) else json.dumps(out, ensure_ascii=False, indent=1)
    return {"content": [{"type": "text", "text": text}], "isError": error}


def error_response(rid, code, message):
    return {"jsonrpc": "2.0", "id": rid, "error": {"code": code, "message": message}}


# ---------------------------------------------------------------------------
# stdio
# ---------------------------------------------------------------------------
def serve_stdio(server, stdin=None, stdout=None):
    """Answers the messages on stdin, one per line, until stdin closes. What else the process prints
    goes to stderr, so stdout carries the protocol only."""
    stdin = stdin or sys.stdin.buffer
    out = stdout or sys.stdout.buffer
    if stdout is None:
        sys.stdout = sys.stderr

    def send(message):
        out.write(json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n")
        out.flush()

    def notify(method, params):
        send({"jsonrpc": "2.0", "method": method, "params": params})

    for raw in stdin:
        line = raw.decode("utf-8", "replace").strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError as e:
            send(error_response(None, PARSE_ERROR, "not JSON: %s" % e))
            continue
        answer = server.handle_message(msg, notify)
        if answer is not None:
            send(answer)
    return 0


# ---------------------------------------------------------------------------
# streamable HTTP
# ---------------------------------------------------------------------------
LOCAL_HOSTS = ("localhost", "127.0.0.1", "::1")


def origin_allowed(origin, host):
    """Browsers send Origin: a page on another site must not reach a server on this machine
    (DNS rebinding). Requests without it come from programs, not pages."""
    if not origin:
        return True
    name = urlsplit(origin).hostname or ""
    return name in LOCAL_HOSTS or name == host


def make_http_server(server, host, port, path="/mcp", token=None):
    """A ThreadingHTTPServer that serves `server` at `path`. One message runs at a time."""
    lock = threading.Lock()

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            server.log("kube32 mcp: %s %s" % (self.address_string(), fmt % args))

        def reply(self, code, body=None, headers=()):
            data = b"" if body is None else json.dumps(body, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            if body is not None:
                self.send_header("Content-Type", "application/json")
            for k, v in headers:
                self.send_header(k, v)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def refused(self):
            if urlsplit(self.path).path != path:
                self.reply(404, {"error": "the MCP endpoint is %s" % path})
                return True
            if not origin_allowed(self.headers.get("Origin"), host):
                self.reply(403, {"error": "origin %s is not allowed" % self.headers.get("Origin")})
                return True
            if token and self.headers.get("Authorization", "") != "Bearer " + token:
                self.reply(401, {"error": "this server needs Authorization: Bearer <token>"}, [("WWW-Authenticate", "Bearer")])
                return True
            return False

        def do_POST(self):
            if self.refused():
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                msg = json.loads(self.rfile.read(length).decode("utf-8"))
            except ValueError as e:
                self.reply(400, error_response(None, PARSE_ERROR, "not JSON: %s" % e))
                return
            with lock:
                answer = server.handle_message(msg)
            if answer is None:
                self.reply(202)
            else:
                self.reply(200, answer)

        def do_GET(self):
            if not self.refused():
                self.reply(405, {"error": "this server sends no event stream: POST JSON-RPC messages"}, [("Allow", "POST")])

        def do_DELETE(self):
            if not self.refused():
                self.reply(405, {"error": "this server keeps no sessions"}, [("Allow", "POST")])

    class HTTPServer(http.server.ThreadingHTTPServer):
        address_family = socket.AF_INET6 if ":" in host else socket.AF_INET
        daemon_threads = True

    return HTTPServer((host, port), Handler)
