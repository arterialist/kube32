"""`kube32 sim`: runs kube32-sim from PATH, from its release tarball, or from its container image."""
import os
import platform
import shutil
import signal
import subprocess
import sys
import tarfile

from . import releases
from .common import EXIT_INPUT, err_exit

SIM_ASSET = "kube32-sim-%s-%s-%s.tar.gz"   # version, os, arch
IMAGE = "ghcr.io/arterialist/kube32-sim"
DEV_ALTERNATIVES = ("pass --version X.Y.Z to run a release, or build kube32-sim from source "
                    "(cmake -S . -B build && cmake --build build) and put it on PATH")


def platform_tag(system=None, machine=None):
    """(os, arch) of the release tarball for this machine; raises ReleaseError where there is none."""
    system = (system or platform.system()).lower()
    machine = (machine or platform.machine()).lower()
    os_name = {"darwin": "macos", "linux": "linux"}.get(system)
    arch = {"arm64": "arm64", "aarch64": "arm64", "x86_64": "x86_64", "amd64": "x86_64"}.get(machine)
    if not os_name or not arch:
        raise releases.ReleaseError("kube32-sim releases are built for macOS and Linux on arm64 and x86_64, not %s %s: "
                                    "pass --docker to run the container image" % (system, machine))
    return os_name, arch


def extract_sim(tarball, dest):
    """Writes the tarball's top-level kube32-sim to `dest` (executable); raises ReleaseError."""
    try:
        with tarfile.open(tarball, "r:gz") as tf:
            member = next((m for m in tf.getmembers() if m.name in ("kube32-sim", "./kube32-sim")), None)
            if member is None or not member.isfile():
                raise releases.ReleaseError("%s has no top-level kube32-sim file" % os.path.basename(tarball))
            src = tf.extractfile(member)
            part = dest + ".part"
            with open(part, "wb") as f:
                shutil.copyfileobj(src, f)
    except (OSError, tarfile.TarError) as e:
        raise releases.ReleaseError("cannot unpack %s: %s" % (tarball, e))
    os.chmod(part, 0o755)
    os.replace(part, dest)


def release_sim(release):
    """The path of the release's kube32-sim for this machine, downloaded and unpacked into the cache."""
    os_name, arch = platform_tag()
    tarball = releases.fetch(release, SIM_ASSET % (release, os_name, arch))
    exe = os.path.join(os.path.dirname(tarball), "kube32-sim-%s-%s" % (os_name, arch), "kube32-sim")
    if not os.path.isfile(exe) or os.path.getmtime(exe) < os.path.getmtime(tarball):
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        extract_sim(tarball, exe)
    return exe


def option_value(args, name, default):
    """The value of `--name v` or `--name=v` in the simulator's arguments (the last one wins)."""
    value = default
    for i, a in enumerate(args):
        if a == name and i + 1 < len(args):
            value = args[i + 1]
        elif a.startswith(name + "="):
            value = a.split("=", 1)[1]
    return value


def docker_command(release, sim_args, interactive):
    """docker run for the release's image, publishing the API server, ingress and pod ports on 127.0.0.1."""
    cmd = ["docker", "run", "--rm", "--init"]
    if interactive:
        cmd.append("-it")
    ports = [option_value(sim_args, "--port", "6443"), option_value(sim_args, "--ingress-port", "0"),
             option_value(sim_args, "--pod-port", "7000")]
    for p in ports:
        if p.isdigit() and int(p) > 0:
            cmd += ["-p", "127.0.0.1:%s:%s" % (p, p)]
    return cmd + ["%s:%s" % (IMAGE, release)] + list(sim_args)


def run_child(cmd):
    """Runs `cmd` to its end. Ctrl-C reaches it too (one process group), and SIGTERM is passed
    on, so the simulator leaves the cluster in its own time."""
    try:
        proc = subprocess.Popen(cmd)
    except OSError as e:
        return err_exit("cannot run %s: %s" % (cmd[0], e.strerror or e), EXIT_INPUT)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, lambda signum, _frame: proc.send_signal(signum))
    while True:
        try:
            rc = proc.wait()
            return 128 - rc if rc < 0 else rc
        except KeyboardInterrupt:
            continue


def cmd_sim(args, sim_args):
    if sim_args[:1] == ["--"]:
        sim_args = sim_args[1:]
    try:
        if args.docker:
            if not shutil.which("docker"):
                return err_exit("docker not found in PATH: --docker runs the container image with Docker", EXIT_INPUT)
            release = releases.release_for(args.version, "kube32-sim", "pass --version X.Y.Z to run a release's image")
            cmd = docker_command(release, sim_args, sys.stdin.isatty())
        else:
            exe = None if args.version else shutil.which("kube32-sim")
            if exe is None:
                exe = release_sim(releases.release_for(args.version, "kube32-sim", DEV_ALTERNATIVES))
            cmd = [exe] + list(sim_args)
    except releases.ReleaseError as e:
        return err_exit(str(e), EXIT_INPUT)
    return run_child(cmd)
