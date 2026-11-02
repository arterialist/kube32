#!/usr/bin/env bash
# Start a kube32-sim build and check that it serves the Kubernetes API as
# release <version>, with every node Ready.
#
#   scripts/release/smoke-sim.sh <version> <command...>
#
# <command> starts the simulator with its API server on port 6443 of this
# host ($SMOKE_PORT when set), for example `dist/kube32-sim --workers "" --pod-port 17000` or
# `docker run --rm -p 6443:6443 kube32-sim`. It is stopped afterwards.
set -euo pipefail

VERSION="${1:?usage: scripts/release/smoke-sim.sh <version> <command...>}"
shift
PORT="${SMOKE_PORT:-6443}"
LOG="$(mktemp)"
"$@" > "$LOG" 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; rm -f "$LOG"' EXIT

check() {
    curl -sf "http://127.0.0.1:$PORT/version" > "$LOG.version" &&
    curl -sf "http://127.0.0.1:$PORT/api/v1/nodes" > "$LOG.nodes" &&
    python3 - "$VERSION" "$LOG.version" "$LOG.nodes" <<'EOF'
import json, sys
version, info, nodes = sys.argv[1], json.load(open(sys.argv[2])), json.load(open(sys.argv[3]))["items"]
ready = [n for n in nodes if any(c["type"] == "Ready" and c["status"] == "True" for c in n["status"]["conditions"])]
if info.get("kube32Version") != version:
    sys.exit("GET /version reports kube32Version %r, not %r" % (info.get("kube32Version"), version))
if not nodes or len(ready) != len(nodes):
    sys.exit("%d of %d nodes Ready" % (len(ready), len(nodes)))
for n in nodes:
    print("%s: %s" % (n["metadata"]["name"], n["status"]["nodeInfo"]["osImage"]))
EOF
}

for _ in $(seq 60); do
    if check 2>/dev/null; then
        rm -f "$LOG.version" "$LOG.nodes"
        echo "kube32-sim $VERSION serves the API"
        exit 0
    fi
    sleep 1
done
check || true
echo "kube32-sim did not come up as $VERSION; its output:" >&2
cat "$LOG" >&2
rm -f "$LOG.version" "$LOG.nodes"
exit 1
