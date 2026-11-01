#pragma once

// Pod checkpoints on the node's kubelet endpoint (docs/migration.md). The
// pod HTTP server serves them; for the control plane's own node the node
// proxy answers them in-process. The API server reaches them as
//
//   /api/v1/nodes/<node>/proxy/checkpoint/...  ->  <node>:<kubelet port>/checkpoint/...
//
//   POST /checkpoint/<namespace>/<pod>/<container>?leave=running|frozen&timeoutMs=<n>
//        freezes the pod once its current call returns and answers its K32S
//        snapshot: 200 application/octet-stream with Content-Length and
//          X-Kube32-Image-Sha256      the module's SHA-256
//          X-Kube32-Snapshot-Sha256   the snapshot's trailer (SHA-256 of the
//                                     bytes before it)
//          X-Kube32-Pod-Time-Ms       the pod's clock at the snapshot
//        leave=running (the default) thaws the pod after the snapshot;
//        leave=frozen keeps it frozen for a move.
//        409 {"reason":"NotMovable","blockers":[...]}  the pod cannot move
//        504 {"reason":"Timeout","blockers":[...]}     a call outlasted
//            timeoutMs (default 10000); the pod runs on
//        404 no such pod or container
//   POST /thaw/<namespace>/<pod>/<container>
//        resumes a frozen pod: 200, or 404 when the pod is not frozen
//
// With a token set, requests carry "Authorization: Bearer <token>" (the API
// server forwards its own): a snapshot holds whatever the pod holds.

#include "api/http_server.hpp"
#include "kubelet/kube_api_client.hpp"
#include "runtime/app_pod.hpp"

#include <string>
#include <string_view>

namespace kube32::app {

// "/checkpoint/..." or "/thaw/..."
bool is_pod_checkpoint_path(std::string_view path);

// Answers one request for `path` on `conn`
bool serve_pod_checkpoint(api::HttpConnection& conn, std::string_view path, const std::string& token);

// The data of Checkpoint <ns>/<name>, streamed from the API server
// (kube32.io/restore-from): a restore source for DeploySpec::restore_from
runtime::SnapshotStream checkpoint_data_stream(kubelet::KubeApiClient& api, const std::string& ns, const std::string& name);

} // namespace kube32::app
