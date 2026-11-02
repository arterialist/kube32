"""The kube32 command line (also installed as kube32-cli).

    kube32 new     <dir> --lang c|rust      a new app project
    kube32 build   [<dir>]                  compile a C, Rust or AssemblyScript app to .wasm and check it
    kube32 check   <file.wasm>              validate a module against the app ABI
    kube32 deploy  <file.wasm>              create a Pod that carries the module inline (no reflash)
    kube32 embed   <file.wasm>              copy the module into images/ for the next firmware build
    kube32 sd-push <file.wasm>              copy the module onto a microSD card as kube32/images/<name>.wasm
                                            (a mounted card with --to, or a node's card over the network with --node)
    kube32 push    <file|dir>               upload a file or directory tree to a node's storage (kube32/<path>) through the API server
    kube32 firmware push <firmware.bin>     stage a firmware image on the control plane
    kube32 firmware rollout                 update every node to the staged image, one at a time
    kube32 firmware status|abort|info       the rollout and each node's firmware; stop a rollout; read an image file
    kube32 devices [<name>...]              every Device with the latest reading of its driver (starts Idle ones)
    kube32 checkpoint <pod>                 save a running pod's state in a Checkpoint
    kube32 move <pod> [--to <node>]         move a running pod to another node with its state
    kube32 checkpoints                      list Checkpoints
    kube32 flash                            write the release firmware to a board over USB
    kube32 console                          a board's serial console
    kube32 gateway                          HTTP to a board over its USB console
    kube32 sim [<args>...]                  run the kube32-sim simulator
    kube32 version                          this tool's version and the cluster's

The commands that talk to a cluster run kubectl, so kubeconfig contexts and
credentials apply. Exit codes:
    0  success
    1  input or toolchain problem (missing file or directory, compiler not found, build failed)
    2  usage error (bad arguments)
    3  the module does not satisfy the ABI (check failed)
    4  kubectl failed
    5  deploy: the pod already exists and --replace was not given; firmware rollout: one is in progress
    6  firmware rollout: the rollout failed or did not finish in time
    7  checkpoint, move: it failed or did not finish in time
"""
import argparse
import json
import subprocess
import sys

from . import common, version
from .build import cmd_build, cmd_check
from .cluster import cmd_checkpoint, cmd_checkpoints, cmd_devices
from .common import EXIT_CODES_HELP, EXIT_INPUT, EXIT_OK, EXIT_USAGE, err_exit
from .firmware import cmd_firmware_abort, cmd_firmware_info, cmd_firmware_push, cmd_firmware_rollout, cmd_firmware_status
from .kube import kubectl_base, kubectl_flags, kubectl_missing
from .pods import cmd_deploy, cmd_embed, cmd_push, cmd_sd_push


def cmd_new(args):
    from .scaffold import cmd_new as run
    return run(args)


def cmd_flash(args):
    from .flash import cmd_flash as run
    return run(args)


def missing_pyserial():
    try:
        import serial  # noqa: F401
    except ImportError:
        return err_exit("this command needs pyserial, which the kube32 package installs (pip install pyserial)", EXIT_INPUT)
    return None


def cmd_console(args):
    if missing_pyserial():
        return EXIT_INPUT
    from .ports import PortError, choose_port
    try:
        port = choose_port(args.port)
    except PortError as e:
        return err_exit(str(e), EXIT_INPUT)
    from . import console
    try:
        return console.run(port)
    except OSError as e:
        return err_exit("%s: %s" % (port, e), EXIT_INPUT)


def cmd_gateway(args):
    if missing_pyserial():
        return EXIT_INPUT
    from . import gateway
    return gateway.run(args)


def cmd_version(args):
    print("kube32 %s" % version())
    if args.client:
        return EXIT_OK
    base = kubectl_base(args)
    missing = kubectl_missing(base)
    if missing:
        print("server: unknown (%s)" % missing)
        return EXIT_OK
    try:
        proc = subprocess.run(base + ["get", "--raw", "/version", "--request-timeout=5s"], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, timeout=30)
        info = json.loads(proc.stdout) if proc.returncode == 0 else None
    except (OSError, subprocess.SubprocessError, ValueError):
        info = None
        proc = None
    if not isinstance(info, dict):
        why = (proc.stderr.strip().splitlines() or ["no answer"])[-1] if proc is not None else "no answer"
        print("server: not reachable (%s)" % why)
        return EXIT_OK
    extra = [x for x in (info.get("platform"), info.get("gitCommit") and "commit " + info["gitCommit"][:12]) if x]
    print("server: %s%s" % (info.get("gitVersion", "?"), " (%s)" % ", ".join(extra) if extra else ""))
    return EXIT_OK


class VersionAction(argparse.Action):
    """--version, which reads the version only when given."""
    def __init__(self, option_strings, dest=argparse.SUPPRESS, default=argparse.SUPPRESS, help=None):
        super().__init__(option_strings, dest=dest, default=default, nargs=0, help=help)

    def __call__(self, parser, namespace, values, option_string=None):
        print("%s %s" % (parser.prog, version()))
        parser.exit()


def build_parser(prog="kube32"):
    p = argparse.ArgumentParser(prog=prog, description="Build, check and deploy kube32 WebAssembly pods, flash ESP32-S3 "
                                                       "boards, and run the kube32-sim simulator.",
                                epilog=EXIT_CODES_HELP)
    p.add_argument("--version", action=VersionAction, help="show the version and exit")
    sub = p.add_subparsers(dest="command", metavar="<command>")

    n = sub.add_parser("new", help="create an app project in a new directory: a C Makefile project or a Rust crate")
    n.add_argument("path", help="the directory to create; its name names the app, the module and the crate")
    n.add_argument("--lang", choices=["c", "rust"], default="c", help="language (default c)")
    n.add_argument("--sdk-path", help="Rust: depend on the kube32-sdk crate in this directory (sdk/rust/kube32-sdk of a checkout)")
    n.set_defaults(func=cmd_new)

    b = sub.add_parser("build", help="compile a C, Rust or AssemblyScript app directory to .wasm and check it")
    b.add_argument("dir", nargs="?", default=".", help="the app directory (default: the current directory)")
    b.add_argument("--lang", choices=["c", "c-make", "rust", "assemblyscript"], help="override language detection")
    b.add_argument("--name", help="module name (default: crate name or directory name)")
    b.add_argument("-o", "--out", help="output path (default: <dir>/build/<name>.wasm; Makefile projects: where the Makefile writes it)")
    b.add_argument("--artifact", help="Makefile projects: the .wasm the Makefile produces, when the dry run cannot tell")
    b.add_argument("--memory", default="64Ki", help="linear memory, initial = maximum, multiple of 64Ki (default 64Ki); not used for Makefile projects")
    b.add_argument("--stack", default="16Ki", help="shadow stack for C and Rust (default 16Ki)")
    b.add_argument("--target", default="wasm32-unknown-unknown", help="Rust target (default wasm32-unknown-unknown)")
    b.add_argument("-v", "--verbose", action="store_true", help="print the compiler commands and their output")
    b.set_defaults(func=cmd_build)

    c = sub.add_parser("check", help="validate a .wasm against the app ABI and this runtime's opcode set")
    c.add_argument("file")
    c.add_argument("--memory", help="fail when the module declares more linear memory than this (e.g. 64Ki, 1280Ki)")
    c.set_defaults(func=cmd_check)

    d = sub.add_parser("deploy", help="create a Pod that carries the module in the kube32.io/wasm-base64 annotation (no reflash)")
    d.add_argument("file")
    d.add_argument("--name", help="pod name (default: file stem)")
    d.add_argument("--node", help="pin to a node (spec.nodeSelector kubernetes.io/hostname)")
    d.add_argument("--namespace", default="default")
    d.add_argument("--memory", default="64Ki", help="resources memory request and limit (default 64Ki)")
    d.add_argument("--psram", help="esp32.io/psram request and limit; the Wasm sandbox is max(memory, psram)")
    d.add_argument("--hardware", help="comma list of ipc, display, keyboard, speaker, sd, service-call, chip-temp, or 'none' (default: inferred from the module's imports)")
    d.add_argument("--sd-claim", help="volume claim for the sd hardware (default: the pod name)")
    d.add_argument("--env", action="append", metavar="K=V", help="container environment variable (repeatable)")
    d.add_argument("--image", help="use this wasm:// reference (wasm://sd/<name>, wasm://embedded/<name>:v1) instead of carrying the module inline")
    d.add_argument("--port", type=int, default=7000, help="containerPort for request pods (default 7000)")
    d.add_argument("--replace", action="store_true", help="delete an existing pod of that name first")
    d.add_argument("--timeout", type=int, default=60, help="seconds to wait for the old pod to go away with --replace (default 60)")
    d.add_argument("--dry-run", action="store_true", help="print the manifest instead of creating the pod")
    kubectl_flags(d)
    d.set_defaults(func=cmd_deploy)

    e = sub.add_parser("embed", help="copy a checked module into images/ so the next firmware build embeds it")
    e.add_argument("file")
    e.add_argument("--name", help="image name (default: file stem); pods reference wasm://embedded/<name>:v1")
    e.add_argument("--force", action="store_true", help="replace an existing image")
    e.add_argument("--images-dir", help="images directory (default: images/ of the kube32 checkout)")
    e.set_defaults(func=cmd_embed)

    s = sub.add_parser("sd-push", help="copy a checked module onto a microSD card as kube32/images/<name>.wasm")
    s.add_argument("file")
    s.add_argument("--to", help="the card's mount point when it is in this machine, e.g. /Volumes/CARD")
    s.add_argument("--node", help="the node whose card gets it, over the network (when --to is not given)")
    s.add_argument("--name", help="image name (default: file stem); pods reference wasm://sd/<name>")
    s.add_argument("--tag", help="image tag: written as <name>-<tag>.wasm, referenced as wasm://sd/<name>:<tag>")
    kubectl_flags(s)
    s.set_defaults(func=cmd_sd_push)

    u = sub.add_parser("push", help="upload a file, or every file below a directory, to a node's storage (<storage root>/kube32/<path>) through the API server")
    u.add_argument("file", help="file or directory")
    u.add_argument("--node", required=True, help="node name (kubectl get nodes)")
    u.add_argument("--path", help="destination below kube32/ (default: images/<file name>; for a directory, images/ is the parent of its contents)")
    kubectl_flags(u)
    u.set_defaults(func=cmd_push)

    fw = sub.add_parser("firmware", help="stage a firmware image on the control plane and roll it out to every node")
    fws = fw.add_subparsers(dest="firmware_command")
    fp = fws.add_parser("push", help="upload firmware.bin into the control plane's update slot, in ranges (resumes an interrupted upload)")
    fp.add_argument("file", help="the image, e.g. .pio/build/esp32s3/firmware.bin")
    fp.add_argument("--chunk", default="16Ki", help="bytes per request (default 16Ki: the control plane holds a request in memory while it writes it to flash)")
    fp.add_argument("--retries", type=int, default=5, help="failed requests in a row before giving up (default 5)")
    kubectl_flags(fp)
    fp.set_defaults(func=cmd_firmware_push)
    fr = fws.add_parser("rollout", help="update every node to the staged image: workers one at a time, then the control plane")
    fr.add_argument("--sha256", help="refuse unless this is the staged image")
    fr.add_argument("--node-timeout", type=int, help="seconds a node has to switch to the image and confirm it (default 900)")
    fr.add_argument("--no-wait", action="store_true", help="start the rollout and return")
    fr.add_argument("--timeout", type=int, default=3600, help="seconds to wait for the rollout (default 3600)")
    fr.add_argument("--interval", type=float, default=5.0, help="seconds between status reads while waiting (default 5)")
    kubectl_flags(fr)
    fr.set_defaults(func=cmd_firmware_rollout)
    fs = fws.add_parser("status", help="the staged image, the rollout and each node's firmware")
    kubectl_flags(fs)
    fs.set_defaults(func=cmd_firmware_status)
    fa = fws.add_parser("abort", help="delete the rollout: nodes that have not switched yet keep their firmware")
    kubectl_flags(fa)
    fa.set_defaults(func=cmd_firmware_abort)
    fi = fws.add_parser("info", help="the version and SHA-256 of a firmware file")
    fi.add_argument("file")
    fi.set_defaults(func=cmd_firmware_info)
    fw.set_defaults(func=lambda args: (fw.print_help(), EXIT_USAGE)[1])

    dv = sub.add_parser("devices", help="list Devices with the latest reading of each driver (read from its node's pod server)")
    dv.add_argument("names", nargs="*", metavar="NAME", help="only these Devices")
    dv.add_argument("-n", "--namespace", default="default")
    dv.add_argument("-A", "--all-namespaces", action="store_true")
    dv.add_argument("--timeout", type=float, default=3.0, help="seconds to wait for each driver (default 3)")
    dv.add_argument("--activation-timeout", type=float, default=10.0,
                    help="seconds to wait for the drivers of Idle OnDemand Devices to start (default 10)")
    dv.add_argument("--no-activate", action="store_true", help="list without starting the drivers of Idle Devices (no use reported)")
    kubectl_flags(dv)
    dv.set_defaults(func=cmd_devices)

    def checkpoint_flags(sp, timeout):
        sp.add_argument("pod")
        sp.add_argument("-n", "--namespace", default="default")
        sp.add_argument("-c", "--container", help="the container (default: the pod's only one)")
        sp.add_argument("--no-wait", action="store_true", help="create the Checkpoint and return")
        sp.add_argument("--timeout", type=float, default=timeout, help="seconds to wait for the result (default %d)" % timeout)
        kubectl_flags(sp)

    ck = sub.add_parser("checkpoint", help="save a running pod's state in a Checkpoint (the pod keeps running)")
    checkpoint_flags(ck, 60)
    ck.set_defaults(func=cmd_checkpoint, action="Save")
    mv = sub.add_parser("move", help="move a running pod to another node with its state")
    checkpoint_flags(mv, 120)
    mv.add_argument("--to", help="the node (default: the one the scheduler picks)")
    mv.set_defaults(func=cmd_checkpoint, action="Move")
    cl = sub.add_parser("checkpoints", help="list Checkpoints")
    cl.add_argument("-n", "--namespace", default="default")
    cl.add_argument("-A", "--all-namespaces", action="store_true")
    kubectl_flags(cl)
    cl.set_defaults(func=cmd_checkpoints)

    fl = sub.add_parser("flash", help="write the release firmware (or an image file) to an ESP32-S3 board over USB")
    fl.add_argument("--port", help="serial port (default: KUBE32_SERIAL_PORT, or the one board connected)")
    fl.add_argument("--version", help="flash this release, X.Y.Z (default: the release of this kube32)")
    fl.add_argument("--file", help="flash this merged image at offset 0x0 instead of a release (erases the board's stored settings)")
    fl.add_argument("--erase-settings", action="store_true",
                    help="write the release's whole image from 0x0, erasing stored settings (Wi-Fi, node name, cluster); "
                         "by default only the bootloader, partition table and firmware are written")
    fl.add_argument("--baud", type=int, help="baud rate (default 921600, then 460800 when that fails)")
    fl.add_argument("--console", dest="console", action="store_true", default=None, help="open the serial console after flashing")
    fl.add_argument("--no-console", dest="console", action="store_false", help="do not offer the serial console after flashing")
    fl.set_defaults(func=cmd_flash)

    co = sub.add_parser("console", help="a board's serial console: prints what the board writes, sends the lines typed")
    co.add_argument("--port", help="serial port (default: KUBE32_SERIAL_PORT, or the one board connected)")
    co.set_defaults(func=cmd_console)

    gw = sub.add_parser("gateway", help="serve a board's API server and ingress on local ports over its USB console")
    gw.add_argument("--port", help="serial device (default: KUBE32_SERIAL_PORT, or the control plane among the boards connected)")
    gw.add_argument("--map", action="append", default=None, metavar="LOCAL:DEVICE",
                    help="local:device port mapping, repeatable (default 6443:6443 and 8080:80)")
    gw.set_defaults(func=cmd_gateway)

    si = sub.add_parser("sim", allow_abbrev=False, help="run the kube32-sim simulator: from PATH, its release, or --docker",
                        usage="%s sim [-h] [--docker] [--version X.Y.Z] [--] [kube32-sim arguments ...]" % prog,
                        description="Runs kube32-sim with the arguments kube32 does not take: kube32-sim from PATH, else the "
                                    "release's kube32-sim for this machine (downloaded once), or with --docker the release's "
                                    "container image. '%s sim -- --help' lists the simulator's options." % prog)
    si.add_argument("--docker", action="store_true", help="run the container image ghcr.io/arterialist/kube32-sim:<version>")
    si.add_argument("--version", help="run this release, X.Y.Z (default: kube32-sim on PATH, else the release of this kube32)")
    si.set_defaults(func=None)

    ve = sub.add_parser("version", help="this tool's version, and the cluster's when kubectl reaches it")
    ve.add_argument("--client", action="store_true", help="only this tool's version")
    kubectl_flags(ve)
    ve.set_defaults(func=cmd_version)
    return p


def main(argv=None, prog="kube32"):
    common.PROG = prog
    argv = sys.argv[1:] if argv is None else list(argv)
    parser = build_parser(prog)
    args, extra = parser.parse_known_args(argv)
    if args.command == "sim":
        from .sim import cmd_sim
        return cmd_sim(args, extra)
    if extra:
        parser.error("unrecognized arguments: %s" % " ".join(extra))
    if not args.command:
        parser.print_help()
        return EXIT_USAGE
    return args.func(args)


def main_cli(argv=None):
    """The kube32-cli name of the same command."""
    return main(argv, prog="kube32-cli")
