"""kubectl: the commands that talk to a cluster run it, so kubeconfig contexts and credentials apply."""
import json
import os
import shutil
import subprocess
import tempfile

KUBECTL_INSTALL = "https://kubernetes.io/docs/tasks/tools/"


class ApiError(Exception):
    pass


def kubectl_flags(sp):
    sp.add_argument("--context", help="kubectl context (default: the current context)")
    sp.add_argument("--kubeconfig", help="kubectl kubeconfig path")
    sp.add_argument("--server", help="kubectl API server URL")
    sp.add_argument("--kubectl", help="kubectl binary (default: kubectl from PATH)")


def kubectl_base(args):
    cmd = [args.kubectl or "kubectl"]
    if args.kubeconfig:
        cmd += ["--kubeconfig", args.kubeconfig]
    if args.context:
        cmd += ["--context", args.context]
    if args.server:
        cmd += ["--server", args.server]
    return cmd


def kubectl_missing(base):
    """Why kubectl cannot run (it is not on PATH), or None when it can."""
    if shutil.which(base[0]):
        return None
    if base[0] == "kubectl":
        return "kubectl not found in PATH: this command runs kubectl; install it (%s) or pass --kubectl <path>" % KUBECTL_INSTALL
    return "%s not found in PATH" % base[0]


def kubectl(cmd, stdin=None):
    proc = subprocess.run(cmd, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return proc.returncode, proc.stdout, proc.stderr


def api_call(args, verb, path, body=None):
    """kubectl <verb> --raw <path> (kubeconfig credentials apply). Returns the
    parsed JSON answer, None for an empty one; raises ApiError."""
    base = kubectl_base(args)
    missing = kubectl_missing(base)
    if missing:
        raise ApiError(missing)
    cmd = base + [verb, "--raw", path]
    tmp = None
    if body is not None:
        with tempfile.NamedTemporaryFile(prefix="kube32-fw-", delete=False) as f:
            f.write(body)
            tmp = f.name
        cmd += ["-f", tmp]
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    finally:
        if tmp:
            os.unlink(tmp)
    if proc.returncode != 0:
        msg = (proc.stderr or proc.stdout).decode("utf-8", "replace").strip()
        raise ApiError(msg or "kubectl exit %d" % proc.returncode)
    out = proc.stdout.decode("utf-8", "replace").strip()
    if not out:
        return None
    try:
        return json.loads(out)
    except ValueError:
        raise ApiError("unexpected answer: %s" % out[:200])
