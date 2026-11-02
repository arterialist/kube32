"""`kube32 deploy`, `embed`, `sd-push` and `push`: ship a module to the cluster or to a card."""
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

from .common import (EXIT_CHECK, EXIT_EXISTS, EXIT_INPUT, EXIT_KUBECTL, EXIT_OK, EXIT_USAGE, err_exit, find_checkout,
                     fmt_bytes, format_quantity, parse_quantity)
from .kube import kubectl, kubectl_base, kubectl_missing
from .wasm import (HARDWARE, HARDWARE_ORDER, SD_IMAGE_LIMIT_NO_PSRAM, SD_IMAGE_LIMIT_PSRAM, check_module, format_report,
                   hardware_for_name, read_wasm)


# ---------------------------------------------------------------------------
# deploy
# ---------------------------------------------------------------------------
_NAME_RE = re.compile(r"^[a-z0-9]([-a-z0-9]*[a-z0-9])?$")
_CLAIM_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,31}$")      # CapabilityFirewall::valid_sd_claim
_ENV_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def q(s):
    """A YAML scalar: JSON strings are valid YAML."""
    return json.dumps(s, ensure_ascii=False)


def render_manifest(name, namespace, node, data, kind, memory_bytes, psram_bytes, hardware, env, port,
                    image=None, sd_claim=None):
    """Pod manifest (YAML). Without `image` the module travels in the kube32.io/wasm-base64 annotation."""
    digest = hashlib.sha256(data).hexdigest()
    out = []
    a = out.append
    a("apiVersion: v1")
    a("kind: Pod")
    a("metadata:")
    a("  name: %s" % name)
    a("  namespace: %s" % namespace)
    a("  labels:")
    a("    app: %s" % name)
    a("  annotations:")
    if image is None:
        a("    kube32.io/wasm-base64: %s" % q(base64.b64encode(data).decode("ascii")))
    a("    kube32.io/wasm-sha256: %s" % q(digest))
    a("    kube32.io/wasm-size: %s" % q(str(len(data))))
    a("spec:")
    if node:
        a("  nodeSelector:")
        a("    kubernetes.io/hostname: %s" % node)
    a("  restartPolicy: Always")
    if "sd" in hardware:
        a("  volumes:")
        a("    - name: data")
        a("      persistentVolumeClaim:")
        a("        claimName: %s" % sd_claim)
    a("  containers:")
    a("    - name: %s" % name)
    # The kubelet tries its embedded registry before the annotation, keyed by
    # the last path component before ':' (ImageRegistry::normalize_key). With
    # the digest in that component an embedded image of the same name cannot
    # shadow the inline module.
    a("      image: %s" % q(image or "wasm://inline/%s-%s" % (name, digest[:12])))
    if kind == "request" and port:
        a("      ports:")
        a("        - containerPort: %d" % port)
        a("          name: http")
    if env:
        a("      env:")
        for k, v in env:
            a("        - name: %s" % k)
            a("          value: %s" % q(v))
    a("      resources:")
    a("        requests:")
    a("          memory: %s" % format_quantity(memory_bytes))
    if psram_bytes:
        a("          esp32.io/psram: %s" % format_quantity(psram_bytes))
    for hw in hardware:
        if HARDWARE[hw][0]:
            a("          %s: \"1\"" % HARDWARE[hw][0])
    a("        limits:")
    a("          memory: %s" % format_quantity(memory_bytes))
    if psram_bytes:
        a("          esp32.io/psram: %s" % format_quantity(psram_bytes))
    if hardware:
        a("      securityContext:")
        a("        capabilities:")
        a("          add: [%s]" % ", ".join(q(HARDWARE[hw][1]) for hw in hardware))
    return "\n".join(out) + "\n"


def parse_env(items):
    env = []
    for item in items or []:
        if "=" not in item:
            raise ValueError("--env expects KEY=VALUE, got '%s'" % item)
        k, v = item.split("=", 1)
        if not _ENV_RE.match(k):
            raise ValueError("invalid environment variable name '%s'" % k)
        env.append((k, v))
    return env


def parse_hardware(text):
    """--hardware: None (infer), or a list; '' and 'none' grant nothing."""
    if text is None:
        return None
    out = []
    for item in text.split(","):
        if not item.strip() or item.strip().lower() == "none":
            continue
        hw = hardware_for_name(item)
        if hw not in out:
            out.append(hw)
    return [h for h in HARDWARE_ORDER if h in out]


def cmd_deploy(args):
    try:
        data = read_wasm(args.file)
    except FileNotFoundError as e:
        return err_exit(str(e), EXIT_INPUT)
    name = args.name or os.path.splitext(os.path.basename(args.file))[0]
    if not _NAME_RE.match(name) or len(name) > 63:
        return err_exit("invalid pod name '%s' (lowercase letters, digits and '-', at most 63 characters; pass --name)" % name, EXIT_USAGE)
    try:
        memory_bytes = parse_quantity(args.memory)
        psram_bytes = parse_quantity(args.psram) if args.psram else 0
        env = parse_env(args.env)
        hardware = parse_hardware(args.hardware)
    except ValueError as e:
        return err_exit(str(e), EXIT_USAGE)
    if args.image is not None and not args.image.startswith("wasm://"):
        return err_exit("--image must be a wasm:// reference such as wasm://sd/%s or wasm://embedded/%s:v1" % (name, name), EXIT_USAGE)

    # kubelet.cpp: the sandbox is max(limits.memory, esp32.io/psram); under one
    # page it backs the first page partly
    sandbox = max(memory_bytes, psram_bytes)
    res = check_module(args.file, data, memory_limit=sandbox)
    if not res.ok:
        print(format_report(res))
        if res.memory_bytes > sandbox:
            print("hint: pass --memory %s, or --psram %s on a node with PSRAM" % (format_quantity(res.memory_bytes), format_quantity(res.memory_bytes)),
                  file=sys.stderr)
        return EXIT_CHECK
    if hardware is None:
        hardware = list(res.hardware)
    else:
        for hw in res.hardware:
            if hw not in hardware:
                print("warning: the module imports %s functions but --hardware does not grant '%s': those calls will return K32_ERR_PERM"
                      % (hw, hw), file=sys.stderr)
    sd_claim = None
    if "sd" in hardware:
        sd_claim = args.sd_claim or name
        if not _CLAIM_RE.match(sd_claim):
            return err_exit("invalid sd volume claim '%s' (1..32 characters of a-z, 0-9 and '-', not starting with '-'); pass --sd-claim" % sd_claim,
                            EXIT_USAGE)
    elif args.sd_claim:
        return err_exit("--sd-claim needs the sd hardware (--hardware ...,sd, or a module that imports k32_fs_*)", EXIT_USAGE)
    manifest = render_manifest(name, args.namespace, args.node, data, res.kind, memory_bytes, psram_bytes, hardware, env, args.port,
                               image=args.image, sd_claim=sd_claim)
    if args.dry_run:
        sys.stdout.write(manifest)
        return EXIT_OK

    base = kubectl_base(args)
    missing = kubectl_missing(base)
    if missing:
        return err_exit(missing, EXIT_INPUT)
    rc, out, errout = kubectl(base + ["get", "pod", name, "-n", args.namespace, "-o", "jsonpath={.metadata.annotations.kube32\\.io/wasm-sha256}"])
    if rc == 0:
        if not args.replace:
            have = out.strip()
            print("error: pod %s/%s already exists (module sha256 %s, this file %s); the kubelet does not restart a running pod "
                  "when its manifest changes: pass --replace to delete and recreate it"
                  % (args.namespace, name, (have[:12] or "unknown"), hashlib.sha256(data).hexdigest()[:12]), file=sys.stderr)
            return EXIT_EXISTS
        rc, out, errout = kubectl(base + ["delete", "pod", name, "-n", args.namespace, "--wait=true", "--timeout=%ds" % args.timeout])
        sys.stdout.write(out)
        if rc != 0:
            sys.stderr.write(errout)
            return err_exit("kubectl delete failed (exit %d)" % rc, EXIT_KUBECTL)
    elif "NotFound" not in errout and "not found" not in errout:
        sys.stderr.write(errout)
        return err_exit("kubectl get pod failed (exit %d)" % rc, EXIT_KUBECTL)

    # create, not apply: apply would copy the whole manifest, module included,
    # into the kubectl.kubernetes.io/last-applied-configuration annotation.
    # --validate=false: the Kube32 API server publishes no OpenAPI schema
    # (/openapi/v3 is an empty index, /openapi/v2 is absent), so kubectl's
    # client-side validation cannot run; this manifest is generated above.
    rc, out, errout = kubectl(base + ["create", "--validate=false", "-f", "-"], stdin=manifest)
    sys.stdout.write(out)
    if rc != 0:
        sys.stderr.write(errout)
        return err_exit("kubectl create failed (exit %d)" % rc, EXIT_KUBECTL)
    where = ("image %s" % args.image) if args.image else ("%s bytes inline" % fmt_bytes(len(data)))
    print("deployed %s/%s: %s, %s linear memory%s, hardware: %s"
          % (args.namespace, name, where, format_quantity(sandbox), " (PSRAM)" if psram_bytes > memory_bytes else "",
             ", ".join(hardware) or "none"))
    return EXIT_OK


# ---------------------------------------------------------------------------
# embed
# ---------------------------------------------------------------------------
_IMAGE_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")


def cmd_embed(args):
    try:
        data = read_wasm(args.file)
    except FileNotFoundError as e:
        return err_exit(str(e), EXIT_INPUT)
    name = args.name or os.path.splitext(os.path.basename(args.file))[0]
    if not _IMAGE_RE.match(name):
        return err_exit("invalid image name '%s' (lowercase letters, digits, '.', '_' and '-'; pass --name)" % name, EXIT_USAGE)
    res = check_module(args.file, data)
    if not res.ok:
        print(format_report(res))
        return EXIT_CHECK
    if args.images_dir:
        images_dir = os.path.abspath(args.images_dir)
    else:
        checkout = find_checkout()
        if not checkout:
            return err_exit("embed copies the module into the images/ directory of a kube32 source checkout: "
                            "run it inside one or pass --images-dir", EXIT_INPUT)
        images_dir = os.path.join(checkout, "images")
    if not os.path.isdir(images_dir):
        return err_exit("images directory does not exist: %s" % images_dir, EXIT_INPUT)
    dest = os.path.join(images_dir, name + ".wasm")
    if os.path.exists(dest) and not args.force:
        return err_exit("%s exists; pass --force to replace it" % dest, EXIT_INPUT)
    shutil.copyfile(args.file, dest)
    print("embedded %s (%s bytes)" % (dest, fmt_bytes(len(data))))
    print("image: wasm://embedded/%s:v1" % name)
    print("next: firmware: pio run -e esp32s3 (the pre-build step regenerates the image table)")
    print("      host simulator: cmake -S . -B build && cmake --build build (configure picks up new files in images/)")
    return EXIT_OK


# ---------------------------------------------------------------------------
# sd-push
# ---------------------------------------------------------------------------
_SD_TOKEN_RE = re.compile(r"^[A-Za-z0-9_-][A-Za-z0-9._-]*$")   # image_registry.cpp valid_image_token


# ---------------------------------------------------------------------------
# push: node storage through the API server's node proxy
# ---------------------------------------------------------------------------
_STORAGE_PATH_RE = re.compile(r"^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$")   # pod_fs.cpp validate_relative_path


def push_file(args, data, path):
    """PUT `data` to <node storage root>/kube32/<path> on args.node (kubectl replace --raw,
    so kubeconfig credentials apply). Returns (exit code, bytes the node wrote)."""
    if not _STORAGE_PATH_RE.match(path) or len(path) > 96 or any(c in (".", "..") for c in path.split("/")):
        return err_exit("invalid --path '%s' (components of A-Z, a-z, 0-9, '.', '_', '-', no '.' or '..', at most 96 bytes)" % path, EXIT_USAGE), 0
    base = kubectl_base(args)
    missing = kubectl_missing(base)
    if missing:
        return err_exit(missing, EXIT_INPUT), 0
    uri = "/api/v1/nodes/%s/proxy/storage/%s" % (args.node, path)
    with tempfile.NamedTemporaryFile(prefix="kube32-push-", delete=False) as f:
        f.write(data)
        tmp = f.name
    try:
        t0 = time.time()
        proc = subprocess.run(base + ["replace", "--raw", uri, "-f", tmp], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        took = time.time() - t0
    finally:
        os.unlink(tmp)
    out = proc.stdout.decode("utf-8", "replace")
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode("utf-8", "replace") or out)
        return err_exit("upload to node %s failed (kubectl exit %d)" % (args.node, proc.returncode), EXIT_KUBECTL), 0
    try:
        written = int(json.loads(out).get("bytes", -1))
    except (ValueError, AttributeError):
        written = -1
    if written != len(data):
        return err_exit("node %s answered %s" % (args.node, out.strip() or "nothing"), EXIT_KUBECTL), 0
    print("pushed %s bytes to %s:kube32/%s in %.1f s" % (fmt_bytes(len(data)), args.node, path, took))
    return EXIT_OK, written


def cmd_push(args):
    if os.path.isdir(args.file):
        return push_dir(args)
    try:
        with open(args.file, "rb") as f:
            data = f.read()
    except OSError as e:
        return err_exit("cannot read %s: %s" % (args.file, e.strerror or e), EXIT_INPUT)
    path = args.path or "images/" + os.path.basename(args.file)
    rc, _ = push_file(args, data, path)
    return rc


def push_dir(args):
    """Upload every regular file below args.file to <--path>/<relative path>, stopping at the first error."""
    root = os.path.normpath(args.file)
    dest = (args.path or "images").strip("/")
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            if os.path.isfile(full) and not os.path.islink(full):
                files.append(full)
    if not files:
        return err_exit("%s contains no files" % args.file, EXIT_INPUT)
    total = 0
    for full in files:
        rel = os.path.relpath(full, root).replace(os.sep, "/")
        try:
            with open(full, "rb") as f:
                data = f.read()
        except OSError as e:
            return err_exit("cannot read %s: %s" % (full, e.strerror or e), EXIT_INPUT)
        rc, written = push_file(args, data, dest + "/" + rel if dest else rel)
        if rc != EXIT_OK:
            return rc
        total += written
    print("pushed %d files, %s bytes to %s:kube32/%s" % (len(files), fmt_bytes(total), args.node, dest))
    return EXIT_OK


def cmd_sd_push(args):
    try:
        data = read_wasm(args.file)
    except FileNotFoundError as e:
        return err_exit(str(e), EXIT_INPUT)
    name = args.name or os.path.splitext(os.path.basename(args.file))[0]
    if not _SD_TOKEN_RE.match(name) or len(name) > 48:
        return err_exit("invalid image name '%s' (1..48 characters of A-Z, a-z, 0-9, '.', '_', '-', not starting with '.'; pass --name)" % name,
                        EXIT_USAGE)
    if args.tag is not None and (not _SD_TOKEN_RE.match(args.tag) or len(args.tag) > 24):
        return err_exit("invalid tag '%s' (1..24 characters of A-Z, a-z, 0-9, '.', '_', '-', not starting with '.')" % args.tag, EXIT_USAGE)
    if args.to is None and not args.node:
        return err_exit("pass --to <card mount point> for a card in this machine, or --node <node> for the card in a node", EXIT_USAGE)
    if args.to is not None and not os.path.isdir(args.to):
        return err_exit("%s is not a directory: mount the card and pass its mount point, e.g. --to /Volumes/CARD" % args.to, EXIT_INPUT)
    res = check_module(args.file, data)
    if not res.ok:
        print(format_report(res))
        return EXIT_CHECK
    if args.to is None:
        # The card is in a node: over the network
        fname = name + ("-" + args.tag if args.tag else "") + ".wasm"     # sd_image_relative_path()
        rc, _ = push_file(args, data, "images/" + fname)
        if rc == EXIT_OK:
            print("image: wasm://sd/%s%s" % (name, ":" + args.tag if args.tag else ""))
        return rc
    images_dir = os.path.join(args.to, "kube32", "images")
    try:
        os.makedirs(images_dir, exist_ok=True)
    except OSError as e:
        return err_exit("cannot create %s: %s" % (images_dir, e.strerror or e), EXIT_INPUT)
    fname = name + ("-" + args.tag if args.tag else "") + ".wasm"     # sd_image_relative_path()
    dest = os.path.join(images_dir, fname)
    previous = os.path.getsize(dest) if os.path.isfile(dest) else None
    tmp = dest + ".part"
    try:
        with open(tmp, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, dest)
    except OSError as e:
        return err_exit("cannot write %s: %s" % (dest, e.strerror or e), EXIT_INPUT)
    verb = "replaced" if previous is not None else "copied"
    print("%s %s (%s bytes%s)" % (verb, dest, fmt_bytes(len(data)), (", was %s" % fmt_bytes(previous)) if previous is not None else ""))
    print("image: wasm://sd/%s%s" % (name, ":" + args.tag if args.tag else ""))
    if len(data) > SD_IMAGE_LIMIT_PSRAM:
        print("warning: %s bytes is over the 2 MiB the kubelet pulls from a card on any node" % fmt_bytes(len(data)), file=sys.stderr)
    elif len(data) > SD_IMAGE_LIMIT_NO_PSRAM:
        print("warning: %s bytes is over the 256 KiB a node without PSRAM pulls from its card" % fmt_bytes(len(data)), file=sys.stderr)
    return EXIT_OK
