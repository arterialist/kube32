#!/usr/bin/env python3
"""Package an esp32s3 build as release assets.

    scripts/release/package-firmware.py --version 0.4.0 [--build-dir .pio/build/esp32s3] [--out dist]

Writes to the output directory, with a .sha256 file ("<sha256>  <file>", as
sha256sum writes it) next to each image:

  kube32-firmware-esp32s3-<version>.bin             every part merged into one image
                                                    written at 0x0, for a first install
  kube32-firmware-esp32s3-<version>-bootloader.bin  the parts, each written at its own
  kube32-firmware-esp32s3-<version>-partitions.bin  offset: they update a board and
  kube32-firmware-esp32s3-<version>-otadata.bin     keep its NVS
  kube32-firmware-esp32s3-<version>-app.bin         (the app alone: firmware rollouts)
  kube32-firmware-esp32s3-<version>-flash.json      the parts, offsets, sizes, SHA-256s
                                                    and flash settings
  manifest.json                                     ESP Web Tools manifest for the merged image

Offsets and flash settings come from the build's flash_args and are checked
against partitions.csv. The merged image runs from 0x0 to the end of the app
and fills the gaps between parts with 0xff, so it erases the NVS partition
(Wi-Fi credentials, node name, token, board profile). The parts leave NVS
alone; the journal and storage partitions lie past the end of either. Fails
when the app image does not carry <version>. Needs esptool 5.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHIP = "esp32s3"
VERSION_MARKER = b"kube32-version:"


def read_partitions():
    """{name: (type, subtype, offset, size)} from partitions.csv, in table order"""
    parts = {}
    with open(os.path.join(ROOT, "partitions.csv")) as f:
        for line in f:
            cols = [c.strip() for c in line.split("#", 1)[0].split(",")]
            if len(cols) >= 5 and cols[3]:
                parts[cols[0]] = (cols[1], cols[2], int(cols[3], 0), int(cols[4], 0))
    return parts


def read_flash_args(build_dir):
    """(flash settings, {role: offset}) from the build's flash_args"""
    with open(os.path.join(build_dir, "flash_args")) as f:
        lines = f.read().split("\n")
    words = lines[0].split()
    settings = {words[i].lstrip("-").replace("_", "-"): words[i + 1] for i in range(0, len(words) - 1, 2)}
    offsets = {}
    for line in lines[1:]:
        if not line.strip():
            continue
        offset, path = line.split()
        name = os.path.basename(path)
        role = ("bootloader" if name.startswith("bootloader") else
                "partitions" if name.startswith("partition") else
                "otadata" if name.startswith("ota_data") else "app")
        offsets[role] = int(offset, 0)
    return settings, offsets


def embedded_version(data):
    at = data.find(VERSION_MARKER)
    if at < 0:
        return None
    return data[at + len(VERSION_MARKER):data.find(b"\0", at)].decode("ascii", "replace")


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_sha256(path):
    digest = sha256_of(path)
    with open(path + ".sha256", "w") as f:
        f.write("%s  %s\n" % (digest, os.path.basename(path)))
    return digest


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--version", required=True, help="release version, without the v")
    ap.add_argument("--build-dir", default=os.path.join(ROOT, ".pio", "build", CHIP))
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    args = ap.parse_args()
    build = args.build_dir

    sources = {
        "bootloader": os.path.join(build, "bootloader.bin"),
        "partitions": os.path.join(build, "partitions.bin"),
        "app": os.path.join(build, "firmware.bin"),
    }
    for path in sources.values():
        if not os.path.isfile(path):
            sys.exit("missing %s: run pio run -e %s first" % (path, CHIP))
    with open(sources["app"], "rb") as f:
        found = embedded_version(f.read())
    if found != args.version:
        sys.exit("%s carries version %r, not %r: build with KUBE32_VERSION=%s" % (sources["app"], found, args.version, args.version))

    settings, offsets = read_flash_args(build)
    table = read_partitions()
    otadata = next(p for p in table.values() if p[0] == "data" and p[1] == "ota")
    first_app = next(p for p in table.values() if p[0] == "app")
    nvs = table["nvs"]
    if offsets.get("otadata") != otadata[2] or offsets.get("app") != first_app[2]:
        sys.exit("flash_args %s disagrees with partitions.csv (otadata 0x%x, first app slot 0x%x)" % (offsets, otadata[2], first_app[2]))
    if os.path.getsize(sources["app"]) > min(p[3] for p in table.values() if p[0] == "app"):
        sys.exit("%s is larger than the app slots" % sources["app"])

    os.makedirs(args.out, exist_ok=True)
    base = "kube32-firmware-%s-%s" % (CHIP, args.version)
    # Erased otadata: the bootloader starts the first app slot, where the app goes
    sources["otadata"] = os.path.join(args.out, base + "-otadata.bin")
    with open(sources["otadata"], "wb") as f:
        f.write(b"\xff" * otadata[3])

    order = sorted(offsets, key=offsets.get)
    for role in order:
        start, end = offsets[role], offsets[role] + os.path.getsize(sources[role])
        if start < nvs[2] + nvs[3] and nvs[2] < end:
            sys.exit("%s (0x%x to 0x%x) overlaps NVS" % (role, start, end))

    merged = os.path.join(args.out, base + ".bin")
    cmd = [sys.executable, "-m", "esptool", "--chip", CHIP, "merge-bin", "-o", merged,
           "--flash-mode", settings["flash-mode"], "--flash-freq", settings["flash-freq"], "--flash-size", settings["flash-size"]]
    for role in order:
        cmd += [hex(offsets[role]), sources[role]]
    subprocess.check_call(cmd)

    # Each part as the merged image holds it (esptool sets the bootloader's
    # flash settings), so the parts and the merged image write the same bytes
    with open(merged, "rb") as f:
        image = f.read()
    flash = {
        "chip": CHIP,
        "version": args.version,
        "flash_mode": settings["flash-mode"],
        "flash_freq": settings["flash-freq"],
        "flash_size": settings["flash-size"],
        "parts": [],
    }
    for role in order:
        path = os.path.join(args.out, "%s-%s.bin" % (base, role))
        size = os.path.getsize(sources[role])
        with open(path, "wb") as f:
            f.write(image[offsets[role]:offsets[role] + size])
        flash["parts"].append({"name": role, "offset": offsets[role], "size": size,
                               "file": os.path.basename(path), "sha256": write_sha256(path)})
    if image[nvs[2]:nvs[2] + nvs[3]] != b"\xff" * nvs[3]:
        sys.exit("the merged image does not erase NVS")
    flash["merged"] = {"file": os.path.basename(merged), "offset": 0, "size": len(image), "sha256": write_sha256(merged)}
    flash_json = os.path.join(args.out, base + "-flash.json")
    with open(flash_json, "w") as f:
        json.dump(flash, f, indent=2)
        f.write("\n")
    write_sha256(flash_json)

    manifest = {
        "name": "kube32",
        "version": args.version,
        "new_install_prompt_erase": True,
        # A first boot formats the node's storage before the console answers
        "new_install_improv_wait_time": 20,
        "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": os.path.basename(merged), "offset": 0}]}],
    }
    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print("merged image: 0x0 to 0x%x, NVS (0x%x to 0x%x) erased; parts: %s" % (
        len(image), nvs[2], nvs[2] + nvs[3], ", ".join("%s at 0x%x" % (r, offsets[r]) for r in order)))


if __name__ == "__main__":
    main()
