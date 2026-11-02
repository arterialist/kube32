"""`kube32 mcp`: a Model Context Protocol server for AI agents such as Claude Code.

It lists the cluster's nodes and Devices, reads Devices, sends them actions
within the limits their owners declare, describes each Device in a reference
document, and builds and deploys pods that the agent writes. See docs/mcp.md.

The protocol is JSON-RPC 2.0 over stdio, or over streamable HTTP with --http
(protocol.py). It needs nothing beyond the Python standard library.
"""
import os
import sys

from ..common import EXIT_INPUT, EXIT_OK, EXIT_USAGE, err_exit

LOOPBACK = ("127.0.0.1", "localhost", "::1")


def parse_listen(text):
    """[HOST:]PORT -> (host, port); the host defaults to 127.0.0.1."""
    host, sep, port = text.rpartition(":")
    if not sep:
        host = "127.0.0.1"
    host = host.strip("[]") or "127.0.0.1"
    if not port.isdigit() or not 0 < int(port) < 65536:
        raise ValueError("--http takes [HOST:]PORT, e.g. 8765 or 127.0.0.1:8765, not '%s'" % text)
    return host, int(port)


def log(text):
    print(text, file=sys.stderr, flush=True)


def cmd_mcp(args):
    from .. import version
    from ..kube import kubectl_base, kubectl_missing
    from .protocol import Server, make_http_server, serve_stdio
    from .reference import INSTRUCTIONS
    from .tools import Kube32, prompts, resources, tools

    token = args.token or os.environ.get("KUBE32_MCP_TOKEN") or None
    listen = None
    if args.http:
        try:
            listen = parse_listen(args.http)
        except ValueError as e:
            return err_exit(str(e), EXIT_USAGE)
        if listen[0] not in LOOPBACK and not token:
            return err_exit("--http on %s reaches this server from the network: give it --token (or KUBE32_MCP_TOKEN)"
                            % listen[0], EXIT_USAGE)
    missing = kubectl_missing(kubectl_base(args))
    if missing:
        log("kube32 mcp: warning: %s" % missing)
    k = Kube32(args, namespace=args.namespace, allow_writes=args.allow_writes, allow_deploy=args.allow_deploy, log=log)
    listing, templates, read = resources(k)
    server = Server("kube32", version(), INSTRUCTIONS, tools(k), listing, templates, read, prompts(), log)
    mode = "writes %s, deploys %s, namespace %s%s" % ("on" if args.allow_writes else "off", "on" if args.allow_deploy else "off",
                                                     args.namespace, ", context " + args.context if args.context else "")
    if listen is None:
        log("kube32 mcp: serving on stdio (%s)" % mode)
        return serve_stdio(server)
    try:
        httpd = make_http_server(server, listen[0], listen[1], token=token)
    except OSError as e:
        return err_exit("cannot listen on %s:%d: %s" % (listen[0], listen[1], e.strerror or e), EXIT_INPUT)
    host = "[%s]" % listen[0] if ":" in listen[0] else listen[0]
    log("kube32 mcp: serving on http://%s:%d/mcp (%s)" % (host, httpd.server_address[1], mode))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
    return EXIT_OK
