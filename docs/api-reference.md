# Kubernetes API matrix

<div class="blueprint-card">
  <strong>Target APIs</strong>: Kubernetes Core API <code>v1</code> and Apps API <code>apps/v1</code>.<br>
  <strong>Engine</strong>: Embedded HTTP server listening on port 6443.<br>
  <strong>Representations</strong>: Kubernetes <code>Table</code> format (<code>as=Table;v=v1;g=meta.k8s.io</code>) and JSON.
</div>

Standard Kubernetes tools (such as `kubectl` and the Go `client-go` library) communicate through REST endpoints including `/version`, `/api/v1/nodes`, `/api/v1/pods`, and `/apis/coordination.k8s.io/v1`. When you run `kubectl get nodes` or `kubectl get pods`, the client requests data formatted as a Kubernetes Table rather than a raw JSON list.

Kube32 implements these endpoints directly on Core 0. You can interact with the physical microcontroller cluster using standard `kubectl` commands over the Wi-Fi network without requiring translation proxies.

---

## Endpoint compliance matrix

| Resource category | HTTP method | Endpoint path | Support level |
| :--- | :--- | :--- | :--- |
| **Discovery** | `GET` | `/version` | Fully supported |
| **Discovery** | `GET` | `/api` | Fully supported |
| **Discovery** | `GET` | `/apis` | Fully supported |
| **Discovery** | `GET` | `/api/v1` | Fully supported |
| **Discovery** | `GET` | `/apis/apps/v1` | Fully supported |
| **Discovery** | `GET` | `/apis/coordination.k8s.io/v1` | Fully supported |
| **Nodes** | `GET` | `/api/v1/nodes` | Fully supported (Table and JSON list) |
| **Nodes** | `POST` | `/api/v1/nodes` | Fully supported (Node registration) |
| **Nodes** | `GET` | `/api/v1/nodes/{name}` | Fully supported |
| **Nodes** | `PATCH` | `/api/v1/nodes/{name}/status` | Fully supported (Heartbeat status) |
| **Pods** | `GET` | `/api/v1/pods` | Fully supported (Cluster-wide collection) |
| **Pods** | `GET` | `/api/v1/namespaces/{ns}/pods` | Fully supported (Namespace collection) |
| **Pods** | `POST` | `/api/v1/namespaces/{ns}/pods` | Fully supported (Pod creation and scheduling) |
| **Pods** | `GET` | `/api/v1/namespaces/{ns}/pods/{name}` | Fully supported |
| **Pods** | `DELETE` | `/api/v1/namespaces/{ns}/pods/{name}` | Fully supported (Pod termination and cleanup) |
| **Leases** | `GET`, `PUT`, `POST` | `/apis/coordination.k8s.io/v1/namespaces/{ns}/leases/{name}` | Fully supported (Node lease renewals) |

---

## Kubernetes `Table` format serialization

When `kubectl` requests human-readable lists, it sends a content negotiation header:

```http
GET /api/v1/nodes HTTP/1.1
Host: 192.168.4.1:6443
Accept: application/json;as=Table;v=v1;g=meta.k8s.io
```

Kube32 streams the Table representation directly from its bump-allocated memory arena:

```json
{
  "kind": "Table",
  "apiVersion": "meta.k8s.io/v1",
  "columnDefinitions": [
    {"name": "Name", "type": "string", "format": "name"},
    {"name": "Status", "type": "string"},
    {"name": "Roles", "type": "string"},
    {"name": "Age", "type": "string"},
    {"name": "Version", "type": "string"},
    {"name": "Internal-IP", "type": "string"},
    {"name": "OS-Image", "type": "string"}
  ],
  "rows": [
    {
      "cells": ["esp32-node-01", "Ready", "control-plane", "1h", "v1.33.0", "192.168.4.1", "ESP-IDF v5.5.3"],
      "object": {"kind": "Node", "apiVersion": "v1", "metadata": {"name": "esp32-node-01"}}
    },
    {
      "cells": ["esp32-node-02", "Ready", "worker", "1h", "v1.33.0", "192.168.4.2", "ESP-IDF v5.5.3"],
      "object": {"kind": "Node", "apiVersion": "v1", "metadata": {"name": "esp32-node-02"}}
    }
  ]
}
```

---

## Interacting with standard `kubectl`

When your machine is connected to the `Kube32-Cluster` Wi-Fi network, `kubectl` commands run against the cluster directly:

```bash
# Query nodes using standard kubectl
kubectl --server=http://192.168.4.1:6443 get nodes -o wide

# List pods across all namespaces
kubectl --server=http://192.168.4.1:6443 get pods

# Submit a workload manifest
kubectl --server=http://192.168.4.1:6443 apply -f examples/gpio-blinker.yaml
```
