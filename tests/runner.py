#!/usr/bin/env python3
"""
Kube32 E2E Test Runner & CLI Harness
Executes Tiers 1-4 automated tests against simulated and physical Kube32 clusters
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
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
if PROJECT_ROOT not in sys.path:
    sys.path.insert(0, PROJECT_ROOT)

from tests.framework import HTTPClient, KubectlClient, BaseTestCase, MockKube32Server

logger = logging.getLogger("kube32_test")


TIER_MAPPING = {
    "1": [
        "tests.tier1_feature.test_discovery",
        "tests.tier1_feature.test_nodes",
        "tests.tier1_feature.test_pods",
        "tests.tier1_feature.test_deployments",
        "tests.tier1_feature.test_tables",
    ],
    "2": [
        "tests.tier2_boundary.test_empty_payloads",
        "tests.tier2_boundary.test_malformed_json",
        "tests.tier2_boundary.test_not_found",
        "tests.tier2_boundary.test_conflict",
        "tests.tier2_boundary.test_invalid_verbs",
    ],
    "3": [
        "tests.tier3_pairwise.test_memory_limits",
        "tests.tier3_pairwise.test_peripherals",
        "tests.tier3_pairwise.test_concurrency",
    ],
    "4": [
        "tests.tier4_application.test_microworkload",
        "tests.tier4_application.test_rescheduling",
        "tests.tier4_application.test_reboot",
        "tests.tier4_application.test_federation",
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
        choices=["sim", "hw", "mock"],
        default="sim",
        help="Test target environment: 'sim' (simulation cluster), 'hw' (physical ESP32), or 'mock' (reference server)"
    )
    parser.add_argument(
        "--server",
        default="http://127.0.0.1:6443",
        help="Base URL of the Kube32 API Server"
    )
    parser.add_argument(
        "--tier",
        choices=["1", "2", "3", "4", "all"],
        default="all",
        help="Test tier to execute ('1', '2', '3', '4', or 'all')"
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
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run self-test with built-in reference mock server"
    )

    args = parser.parse_args()

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG, format="%(asctime)s [%(levelname)s] %(message)s")
    else:
        logging.basicConfig(level=logging.INFO, format="%(message)s")

    server_url = args.server
    mock_server = None

    # Handle self-test or mock target
    if args.self_test or args.target == "mock":
        print("[INIT] Starting built-in MockKube32 reference server for self-test...")
        mock_server = MockKube32Server(host="127.0.0.1", port=0)
        mock_server.start()
        server_url = mock_server.url
        print(f"[INIT] MockKube32 running on {server_url}")
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
            print("[HINT] If Kube32 server is not running yet, use '--self-test' to verify test suites against reference mock.", file=sys.stderr)
            sys.exit(3)

    # Determine modules to run
    if args.tier == "all":
        modules_to_run = (
            TIER_MAPPING["1"] +
            TIER_MAPPING["2"] +
            TIER_MAPPING["3"] +
            TIER_MAPPING["4"]
        )
    else:
        modules_to_run = TIER_MAPPING.get(args.tier, [])

    print("\n" + "=" * 70)
    print(f" Running Kube32 E2E Test Suite (Tier: {args.tier.upper()}, Target: {args.target})")
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
        if mock_server:
            mock_server.stop()

    print_summary(results, total_time)

    # Return exit code
    all_passed = all(r.passed for r in results) if results else False
    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()
