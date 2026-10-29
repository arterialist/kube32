# Cluster access

The API server listens on port 6443. Port 6443 serves both TLS and plain HTTP on the same port by inspecting the first bytes of each connection, and supports HTTP/1.1 keep-alive over TLS (up to 4 idle TLS sessions within 16 total idle connections, closing the oldest first). Bearer token authentication is off by default.

## Plain HTTP kubeconfig

When authentication is disabled, configure `kubectl` to use `http://` with no credentials:

```sh
kubectl config set-cluster kube32 --server=http://<control-plane-ip>:6443
kubectl config set-context kube32 --cluster=kube32
kubectl config use-context kube32
```

## Bearer token authentication

To enable authentication, set the same bearer token on the control plane and on every worker node over the USB serial console at 115200 baud:

```
K32_CMD:TOKEN:<token>
```

Tokens must be 16 to 128 printable ASCII characters without spaces. The command stores the token in the `k32` NVS namespace, returns `K32_OK token stored, rebooting`, and reboots the board.

Control plane and worker nodes read the `token` NVS key at boot. The API server checks `Authorization: Bearer <token>` on incoming requests, and each kubelet sends that header when talking to the API server.

To disable authentication on a board, run:

```
K32_CMD:TOKEN:off
```

On the host simulator, pass `--token <token>` or set the `KUBE32_API_TOKEN` environment variable.

### Exempt endpoints

The following paths answer without credentials even when a token is configured:

- `/healthz`, `/livez`, `/readyz`, and subpaths such as `/readyz/ping`
- `/version`

All other endpoints return `401 Unauthorized` with a Kubernetes `Status` body when the token is missing or does not match.

## TLS on the API port

`kubectl` only sends bearer tokens over `https://` connections.

At first boot, a control plane board generates an ECDSA P-256 key and a self-signed X.509 certificate, and stores them in NVS under `tls_key` and `tls_crt`. The certificate has Subject `CN=kube32-apiserver,O=kube32` and Subject Alternative Names for `kube32-apiserver`, `localhost`, the node name, `127.0.0.1`, and the node IP address at generation time.

To export the certificate, send `K32_CMD:CERT` over the control plane's serial console:

```
K32_CMD:CERT
```

The console prints each line prefixed with `K32_CERT `, followed by `K32_OK cert`:

```
K32_CERT -----BEGIN CERTIFICATE-----
K32_CERT ...
K32_CERT -----END CERTIFICATE-----
K32_OK cert
```

Strip the `K32_CERT ` prefix and save the PEM block to a file such as `kube32-ca.pem`.

On the host simulator, pass `kube32-sim --tls` to enable TLS on port 6443 (compiled in by default when OpenSSL 3 is found on the host). The simulator stores its key and self-signed certificate under `<data-dir>/kube32/tls/`.

### Configuring kubeconfig for TLS and token auth

Configure `kubectl` to verify the certificate and present the bearer token. Including `--tls-server-name=kube32-apiserver` ensures verification succeeds even if the board's IP address changes after the certificate was generated:

```sh
kubectl config set-cluster kube32 \
  --server=https://<control-plane-ip>:6443 \
  --tls-server-name=kube32-apiserver \
  --certificate-authority=<pem file> \
  --embed-certs
kubectl config set-credentials kube32 --token=<token>
kubectl config set-context kube32 --cluster=kube32 --user=kube32
kubectl config use-context kube32
```

To skip certificate verification instead of exporting the CA certificate, pass `--insecure-skip-tls-verify`:

```sh
kubectl config set-cluster kube32 \
  --server=https://<control-plane-ip>:6443 \
  --insecure-skip-tls-verify
kubectl config set-credentials kube32 --token=<token>
kubectl config set-context kube32 --cluster=kube32 --user=kube32
kubectl config use-context kube32
```

Check `K32_CMD:STATUS` on the serial console to verify that `auth=token` and `tls=on` are active.

## Using k9s

`k9s` reads the active context from `KUBECONFIG` or `~/.kube/config` and uses the same cluster URL, TLS settings, and bearer token:

```sh
k9s --context kube32
```

When authenticated, `SelfSubjectAccessReview` and `SelfSubjectRulesReview` requests made by `k9s` and `kubectl auth can-i` always return allowed.

## Limitations

- Worker kubelets send the bearer token to the API server over plain HTTP on the LAN.
- One bearer token applies to the whole cluster, with no per-user or ServiceAccount tokens.
- Without RBAC, authenticated callers are allowed every action on every resource.
