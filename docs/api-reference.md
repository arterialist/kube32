# Kubernetes API Matrix

<div class="blueprint-card">
  <strong>API Target</strong>: Kubernetes Core API <code>v1</code> &amp; Apps API <code>apps/v1</code><br>
  <strong>HTTP Engine</strong>: Embedded Micro-HTTP Server (Port 6443)<br>
  <strong>Serialization Formats</strong>: Kubernetes <code>Table</code> (as=Table v1 meta.k8s.io) &amp; Standard JSON
</div>

Kube32 implements an RFC-compliant subset of the Kubernetes REST API, allowing standard clients (`kubectl`, `kube32-cli`, Go client-go) to interact with the physical microcontroller cluster.

---

## Endpoint Compliance Matrix

| Resource Category | HTTP Verb | Endpoint Path | Implementation Status |
| :--- | :--- | :--- | :--- |
| **Discovery** | `GET` | `/version` | Fully Supported |
| **Discovery** | `GET` | `/api` | Fully Supported |
| **Discovery** | `GET` | `/apis` | Fully Supported |
| **Discovery** | `GET` | `/api/v1` | Fully Supported |
| **Discovery** | `GET` | `/apis/apps/v1` | Fully Supported |
| **Discovery** | `GET` | `/apis/coordination.k8s.io/v1` | Fully Supported |
| **Nodes** | `GET` | `/api/v1/nodes` | Fully Supported (Table &amp; JSON List) |
| **Nodes** | `POST` | `/api/v1/nodes` | Fully Supported (Node Registration) |
| **Nodes** | `GET` | `/api/v1/nodes/{name}` | Fully Supported |
| **Nodes** | `PATCH` | `/api/v1/nodes/{name}/status` | Fully Supported (Heartbeat Status) |
| **Pods** | `GET` | `/api/v1/pods` | Fully Supported (Cluster Collection) |
| **Pods** | `GET` | `/api/v1/namespaces/{ns}/pods` | Fully Supported (Namespace Collection) |
| **Pods** | `POST` | `/api/v1/namespaces/{ns}/pods` | Fully Supported (Pod Creation &amp; Scheduling) |
| **Pods** | `GET` | `/api/v1/namespaces/{ns}/pods/{name}` | Fully Supported |
| **Pods** | `DELETE` | `/api/v1/namespaces/{ns}/pods/{name}` | Fully Supported (Pod Eviction &amp; Cleanup) |
| **Leases** | `GET`, `PUT`, `POST` | `/apis/coordination.k8s.io/v1/namespaces/{ns}/leases/{name}` | Fully Supported (Heartbeat Renewals) |

---

## Kubernetes `Table` Format Serialization

When standard `kubectl` queries endpoints, it requests the Kubernetes `Table` representation using HTTP content negotiation:

```http
GET /api/v1/nodes HTTP/1.1
Host: 192.168.4.1:6443
Accept: application/json;as=Table;v=v1;g=meta.k8s.io
```

Kube32 streams the response using a compact Table serializer:

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

## Interacting via Standard `kubectl`

When associated with the `Kube32-Cluster` Wi-Fi network, standard `kubectl` commands work out-of-the-box:

```bash
# Query nodes using standard kubectl
kubectl --server=http://192.168.4.1:6443 get nodes -o wide

# Query pods across all namespaces
kubectl --server=http://192.168.4.1:6443 get pods

# Submit a workload
kubectl --server=http://192.168.4.1:6443 apply -f examples/gpio-blinker.yaml
```
