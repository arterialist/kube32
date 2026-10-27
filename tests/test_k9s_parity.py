import urllib.request
import urllib.error
import json
import sys

BASE_URL = "http://127.0.0.1:6443"

ENDPOINTS = [
    ("GET", "/version"),
    ("GET", "/api"),
    ("GET", "/api/v1"),
    ("GET", "/apis"),
    ("GET", "/api/v1/namespaces"),
    ("GET", "/api/v1/nodes"),
    ("GET", "/api/v1/pods"),
    ("GET", "/api/v1/services"),
    ("GET", "/api/v1/endpoints"),
    ("GET", "/api/v1/events"),
    ("GET", "/api/v1/configmaps"),
    ("GET", "/api/v1/secrets"),
    ("GET", "/apis/authorization.k8s.io/v1"),
    ("POST", "/apis/authorization.k8s.io/v1/selfsubjectaccessreviews", {"spec": {"resourceAttributes": {"verb": "get", "resource": "pods"}}}),
    ("POST", "/apis/authorization.k8s.io/v1/selfsubjectrulesreviews", {"spec": {"namespace": "default"}}),
    ("GET", "/apis/coordination.k8s.io/v1"),
    ("GET", "/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases"),
    ("GET", "/apis/apps/v1"),
    ("GET", "/apis/apps/v1/namespaces/default/deployments"),
    ("GET", "/apis/apiextensions.k8s.io/v1"),
    ("GET", "/apis/apiextensions.k8s.io/v1/customresourcedefinitions"),
    ("GET", "/openapi/v2"),
]

TABLE_ENDPOINTS = [
    "/api/v1/nodes",
    "/api/v1/pods",
    "/api/v1/namespaces",
    "/api/v1/services",
    "/api/v1/events"
]

def run_checks():
    print(f"--- Running K9s Kubernetes API Parity Checks against {BASE_URL} ---")
    passed = 0
    failed = 0
    
    for item in ENDPOINTS:
        method = item[0]
        path = item[1]
        data = item[2] if len(item) > 2 else None
        
        url = f"{BASE_URL}{path}"
        body_bytes = json.dumps(data).encode("utf-8") if data else None
        headers = {"Accept": "application/json"}
        if data:
            headers["Content-Type"] = "application/json"
            
        req = urllib.request.Request(url, data=body_bytes, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=3.0) as resp:
                code = resp.status
                body = resp.read().decode("utf-8")
                # Parse json
                try:
                    doc = json.loads(body)
                    kind = doc.get("kind", doc.get("swagger", "JSON"))
                    print(f"  [OK] {method:4} {path:50} -> {code} ({kind})")
                    passed += 1
                except Exception as je:
                    print(f"  [WARN] {method:4} {path:50} -> {code} (Non-JSON: {je})")
                    passed += 1
        except Exception as e:
            print(f"  [FAIL] {method:4} {path:50} -> {e}")
            failed += 1
            
    print(f"\n--- Checking Table Serializer Negotiation (as=Table) ---")
    for path in TABLE_ENDPOINTS:
        url = f"{BASE_URL}{path}"
        req = urllib.request.Request(url, headers={"Accept": "application/json;as=Table;v=v1;g=meta.k8s.io"}, method="GET")
        try:
            with urllib.request.urlopen(req, timeout=3.0) as resp:
                code = resp.status
                body = resp.read().decode("utf-8")
                doc = json.loads(body)
                if doc.get("kind") == "Table":
                    cols = len(doc.get("columnDefinitions", []))
                    rows = len(doc.get("rows", []))
                    print(f"  [OK] Table GET {path:44} -> {code} (Table: {cols} cols, {rows} rows)")
                    passed += 1
                else:
                    print(f"  [FAIL] Table GET {path:44} -> Expected kind=Table, got {doc.get('kind')}")
                    failed += 1
        except Exception as e:
            print(f"  [FAIL] Table GET {path:44} -> {e}")
            failed += 1

    print(f"\nResults: {passed} passed, {failed} failed")
    return failed == 0

if __name__ == "__main__":
    success = run_checks()
    sys.exit(0 if success else 1)
