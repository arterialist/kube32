#!/usr/bin/env python3
"""
Deploy and verify all 10 production real-world workloads in examples/workloads/
against both simulated and physical clusters.
"""

import os
import sys
import glob
import subprocess
import json
import time

WORKLOADS_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "examples", "workloads")

def run_cmd(cmd):
    res = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return res.returncode, res.stdout.strip(), res.stderr.strip()

def main():
    target_server = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8443"
    print("=" * 70)
    print(f" Deploying All Production Workloads to Kube32 Cluster ({target_server})")
    print("=" * 70)

    yaml_files = sorted(glob.glob(os.path.join(WORKLOADS_DIR, "*.yaml")))
    print(f"Found {len(yaml_files)} workload manifests in {WORKLOADS_DIR}:\n")

    results = []

    for yf in yaml_files:
        base = os.path.basename(yf)
        cmd_apply = f"kubectl --server={target_server} apply -f {yf} --validate=false"
        code, out, err = run_cmd(cmd_apply)
        success = (code == 0)
        results.append({
            "file": base,
            "success": success,
            "output": out if success else err
        })
        status = "PASSED" if success else "FAILED"
        print(f"  [{status}] {base:48s} -> {out if success else err}")

    print("\n" + "-" * 70)
    print(" Cluster Pod Statuses After Deployment:")
    print("-" * 70)
    code, out, err = run_cmd(f"kubectl --server={target_server} get pods -o wide")
    print(out if code == 0 else err)

    passed_count = sum(1 for r in results if r["success"])
    print("\n" + "=" * 70)
    print(f" Workload Deployment Summary: {passed_count}/{len(results)} Passed ({passed_count/len(results)*100:.1f}%)")
    print("=" * 70)

if __name__ == "__main__":
    main()
