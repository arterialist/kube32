#!/usr/bin/env python3
import subprocess
import os

PAGES = [
    ("page_01_index", "http://localhost:8000/kube32/"),
    ("page_02_quickstart", "http://localhost:8000/kube32/quickstart/"),
    ("page_03_hardware_spec", "http://localhost:8000/kube32/hardware-spec/"),
    ("page_04_flashing", "http://localhost:8000/kube32/flashing-troubleshooting/"),
    ("page_05_architecture", "http://localhost:8000/kube32/architecture/"),
    ("page_06_radix_store", "http://localhost:8000/kube32/architecture/radix-store/"),
    ("page_07_scheduler", "http://localhost:8000/kube32/architecture/scheduler/"),
    ("page_08_wasm_runtime", "http://localhost:8000/kube32/architecture/wasm-runtime/"),
    ("page_09_capability_firewall", "http://localhost:8000/kube32/architecture/capability-firewall/"),
    ("page_10_espnow_ipc", "http://localhost:8000/kube32/architecture/espnow-ipc/"),
    ("page_11_gpio_blinker", "http://localhost:8000/kube32/workloads/gpio-blinker/"),
    ("page_12_distributed_pipeline", "http://localhost:8000/kube32/workloads/distributed-pipeline/"),
    ("page_13_resource_limits", "http://localhost:8000/kube32/workloads/resource-limits/"),
    ("page_14_cli_reference", "http://localhost:8000/kube32/cli-reference/"),
    ("page_15_api_reference", "http://localhost:8000/kube32/api-reference/"),
]

OUT_DIR = "review-output"
CHROME_BIN = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

os.makedirs(OUT_DIR, exist_ok=True)

for name, url in PAGES:
    out_file = os.path.join(OUT_DIR, f"{name}.png")
    cmd = [
        CHROME_BIN,
        "--headless",
        "--disable-gpu",
        "--hide-scrollbars",
        f"--screenshot={out_file}",
        "--window-size=1280,1800",
        url
    ]
    print(f"Capturing {name} from {url}...")
    try:
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        size = os.path.getsize(out_file)
        print(f"  Saved {name}.png ({size} bytes)")
    except Exception as e:
        print(f"  Error on {name}: {e}")

print("Visual capture complete!")
