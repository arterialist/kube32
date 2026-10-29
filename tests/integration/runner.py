#!/usr/bin/env python3
"""
Kube32 E2E Test Runner & CLI Harness
Executes the API, validation, scheduling, workload, cluster and federation suites against simulated and physical Kube32 clusters
using native kubectl commands and direct HTTP REST calls.
"""

import os
import sys
import time
import inspect
import logging
import argparse
import importlib
from typing import List, Tuple, Type

# Ensure project root is in sys.path
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
if PROJECT_ROOT not in sys.path:
    sys.path.insert(0, PROJECT_ROOT)

from tests.integration.framework import HTTPClient, KubectlClient, BaseTestCase

logger = logging.getLogger("kube32_test")


SUITE_MAPPING = {
    "api": [
        "tests.integration.api_resources.test_discovery",
        "tests.integration.api_resources.test_nodes",
        "tests.integration.api_resources.test_pods",
        "tests.integration.api_resources.test_deployments",
        "tests.integration.api_resources.test_tables",
    ],
    "validation": [
        "tests.integration.api_validation.test_empty_payloads",
        "tests.integration.api_validation.test_malformed_json",
        "tests.integration.api_validation.test_not_found",
        "tests.integration.api_validation.test_conflict",
        "tests.integration.api_validation.test_invalid_verbs",
    ],
    "resources": [
        "tests.integration.scheduling_resources.test_memory_limits",
        "tests.integration.scheduling_resources.test_peripherals",
        "tests.integration.scheduling_resources.test_concurrency",
    ],
    "workloads": [
        "tests.integration.workloads.test_microworkload",
    ],
    "cluster": [
        "tests.integration.cluster.test_node_lease",
        "tests.integration.cluster.test_self_healing",
        "tests.integration.cluster.test_channel_ipc",
        "tests.integration.cluster.test_reboot_recovery",
    ],
    "federation": [
        "tests.integration.federation.test_upstream_federation",
    ]
}


class TestResult:
    def __init__(self, class_name: str, test_name: str, passed: bool, duration: float, error: str = ""):
        self.class_name = class_name
        self.test_name = test_name
        self.passed = passed
        self.duration = duration
        self.error = error


def discover_test_classes(module_names: List[str]) -> List[Type[BaseTestCase]]:
    classes = []
    for mod_name in module_names:
        try:
            mod = importlib.import_module(mod_name)
            for attr_name in dir(mod):
                attr = getattr(mod, attr_name)
                if inspect.isclass(attr) and issubclass(attr, BaseTestCase) and attr is not BaseTestCase:
                    classes.append(attr)
        except Exception as e:
            print(f"Error importing module {mod_name}: {e}", file=sys.stderr)
            raise
    return classes


def run_test_suite(
    test_classes: List[Type[BaseTestCase]],
    http_client: HTTPClient,
    kubectl_client: KubectlClient,
    target: str,
    verbose: bool,
    filter_pattern: str = ""
) -> List[TestResult]:
    results = []
    for test_cls in test_classes:
        class_instance = test_cls(http_client, kubectl_client, target=target, verbose=verbose)
        test_methods = [
            m for m in dir(class_instance)
            if m.startswith("test_") and callable(getattr(class_instance, m))
        ]

        for method_name in sorted(test_methods):
            full_test_id = f"{test_cls.__name__}.{method_name}"
            if filter_pattern and filter_pattern.lower() not in full_test_id.lower():
                continue

            # Run setUp
            try:
                class_instance.setUp()
            except Exception as e:
                print(f"  [FAIL] {full_test_id} setUp error: {e}")
                results.append(TestResult(test_cls.__name__, method_name, False, 0.0, f"setUp failed: {e}"))
                continue

            test_func = getattr(class_instance, method_name)
            start_time = time.time()
            passed = False
            error_msg = ""
            try:
                test_func()
                passed = True
            except Exception as e:
                error_msg = str(e)
            finally:
                duration = time.time() - start_time
                try:
                    class_instance.tearDown()
                except Exception as te:
                    if passed:
                        passed = False
                        error_msg = f"tearDown failed: {te}"

            results.append(TestResult(test_cls.__name__, method_name, passed, duration, error_msg))

            status_tag = "\033[92mPASS\033[0m" if passed else "\033[91mFAIL\033[0m"
            print(f"  [{status_tag}] {full_test_id} ({duration:.3f}s)")
            if not passed and error_msg:
                print(f"         \033[91mError: {error_msg}\033[0m")

    return results


def print_summary(results: List[TestResult], total_elapsed: float):
    total = len(results)
    passed = sum(1 for r in results if r.passed)
    failed = total - passed
    pass_pct = (passed / total * 100.0) if total > 0 else 0.0

    print("\n" + "=" * 70)
    print("                    KUBE32 E2E TEST SUMMARY")
    print("=" * 70)
    print(f" Total Executed : {total}")
    print(f" Passed         : \033[92m{passed}\033[0m")
    print(f" Failed         : \033[91m{failed}\033[0m" if failed > 0 else f" Failed         : {failed}")
    print(f" Pass Rate      : {pass_pct:.1f}%")
    print(f" Execution Time : {total_elapsed:.3f} seconds")
    print("=" * 70)

    if failed > 0:
        print("\nFailed Tests:")
        for r in results:
            if not r.passed:
                print(f"  - {r.class_name}.{r.test_name}: {r.error}")
        print("")


def main():
    parser = argparse.ArgumentParser(
        description="Kube32 Automated E2E Test Suite Runner",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    parser.add_argument(
        "--target",
        choices=["sim", "hw"],
        default="sim",
        help="Test target environment: 'sim' (simulation cluster) or 'hw' (physical ESP32)"
    )
    parser.add_argument(
        "--server",
        default="http://127.0.0.1:6443",
        help="Base URL of the Kube32 API Server"
    )
    parser.add_argument(
        "--suite",
        choices=["api", "validation", "resources", "workloads", "cluster", "federation", "all"],
        default="all",
        help="Test suite to execute ('api', 'validation', 'resources', 'workloads', 'cluster', 'federation', or 'all')"
    )
    parser.add_argument(
        "-v", "--verbose", "--v",
        action="store_true",
        dest="verbose",
        help="Enable verbose output with request/response logs"
    )
    parser.add_argument(
        "--filter",
        default="",
        help="Optional substring filter for test names"
    )

    args = parser.parse_args()

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG, format="%(asctime)s [%(levelname)s] %(message)s")
    else:
        logging.basicConfig(level=logging.INFO, format="%(message)s")

    server_url = args.server
    if args.suite in ("cluster", "federation"):
        # Cluster and federation tests manage their own multi-node simulated clusters
        pass
    else:
        # Check if target server is reachable
        print(f"[INIT] Checking connection to Kube32 server at {server_url}...")
        test_client = HTTPClient(server_url, default_timeout=3.0, verbose=args.verbose)
        try:
            test_resp = test_client.get("/version")
            if test_resp.status_code != 200:
                print(f"[WARN] Server responded with status {test_resp.status_code} at /version")
            else:
                print(f"[INIT] Connected to Kube32 API Server: gitVersion={test_resp.json().get('gitVersion', 'unknown')}")
        except ConnectionError as ce:
            print(f"[ERROR] Could not connect to Kube32 server at {server_url}: {ce}", file=sys.stderr)
            print("[HINT] Start kube32-sim first, e.g. build/kube32-sim --role standalone --pod-port 17000", file=sys.stderr)
            sys.exit(3)

    # Determine modules to run
    if args.suite == "all":
        modules_to_run = (
            SUITE_MAPPING["api"] +
            SUITE_MAPPING["validation"] +
            SUITE_MAPPING["resources"] +
            SUITE_MAPPING["workloads"] +
            SUITE_MAPPING["cluster"] +
            SUITE_MAPPING["federation"]
        )
    else:
        modules_to_run = SUITE_MAPPING.get(args.suite, [])

    print("\n" + "=" * 70)
    print(f" Running Kube32 E2E Test Suite (Suite: {args.suite.upper()}, Target: {args.target})")
    print(f" Target Server : {server_url}")
    print(f" Modules Count : {len(modules_to_run)}")
    print("=" * 70 + "\n")

    http_client = HTTPClient(server_url, default_timeout=10.0, verbose=args.verbose)
    kubectl_client = KubectlClient(server_url, verbose=args.verbose)

    test_classes = discover_test_classes(modules_to_run)
    start_all = time.time()
    try:
        results = run_test_suite(
            test_classes,
            http_client,
            kubectl_client,
            target=args.target,
            verbose=args.verbose,
            filter_pattern=args.filter
        )
    finally:
        total_time = time.time() - start_all

    print_summary(results, total_time)

    # Return exit code
    all_passed = all(r.passed for r in results) if results else False
    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()
