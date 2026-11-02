#!/usr/bin/env python3
"""Print the CHANGELOG.md section of one release, for its GitHub Release notes.

    scripts/release/release-notes.py 0.4.0 [--fallback-unreleased]

Prints what follows the "## [0.4.0]" heading up to the next "## [" heading.
Fails when the section is missing or empty; --fallback-unreleased prints the
[Unreleased] section instead (dry runs before the section exists).
"""
import argparse
import os
import sys

CHANGELOG = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "CHANGELOG.md")


def section(lines, name):
    out = None
    for line in lines:
        if line.startswith("## ["):
            if out is not None:
                break
            if line[4:].split("]", 1)[0] == name:
                out = []
            continue
        if out is not None:
            out.append(line)
    return "".join(out).strip() if out is not None else ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("version")
    ap.add_argument("--fallback-unreleased", action="store_true")
    ap.add_argument("--changelog", default=CHANGELOG)
    args = ap.parse_args()
    with open(args.changelog, encoding="utf-8") as f:
        lines = f.readlines()
    notes = section(lines, args.version)
    if not notes and args.fallback_unreleased:
        notes = section(lines, "Unreleased")
    if not notes:
        sys.exit("%s has no section \"## [%s]\"" % (os.path.basename(args.changelog), args.version))
    print(notes)


if __name__ == "__main__":
    main()
