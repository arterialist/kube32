#pragma once

// Taints and tolerations (core/v1) as Kubernetes matches them, and the
// tolerations pods get from the API server and the DaemonSet controller.

#include "models/kube_models.hpp"

#include <cstdint>

namespace kube32::models {

// Taints the node lifecycle controller sets
constexpr const char* kTaintNodeNotReady = "node.kubernetes.io/not-ready";
constexpr const char* kTaintNodeUnreachable = "node.kubernetes.io/unreachable";

// tolerationSeconds of the tolerations add_default_tolerations() adds
constexpr int64_t kDefaultTolerationSeconds = 300;

// no_execute_toleration_seconds() of a pod that tolerates a taint without limit
constexpr int64_t kTolerateForever = -1;

// Toleration.ToleratesTaint: the effects are equal or the toleration's effect
// is empty (all effects); the keys are equal or the toleration's key is empty
// (all keys); operator Exists matches any value, Equal (also when empty) the
// taint's value. Other operators match nothing.
bool tolerates_taint(const JsonValue& toleration, const JsonValue& taint);

// How long a pod with these spec.tolerations (null: none) stays on a node
// with the NoExecute `taint`, as the Kubernetes taint manager reads it: the
// first toleration that tolerates the taint decides. Its tolerationSeconds
// (0 for 0 or less), kTolerateForever without tolerationSeconds, and 0 when
// no toleration tolerates the taint.
int64_t no_execute_toleration_seconds(const JsonValue* tolerations, const JsonValue& taint);

// The DefaultTolerationSeconds admission plugin: unless the pod spec has a
// toleration for node.kubernetes.io/not-ready:NoExecute, it gets one with
// operator Exists and tolerationSeconds 300; the same for
// node.kubernetes.io/unreachable:NoExecute. A toleration with an empty key
// or effect counts as one for these taints.
void add_default_tolerations(JsonValue& pod_spec);

// DaemonSet pods tolerate node.kubernetes.io/not-ready:NoExecute and
// node.kubernetes.io/unreachable:NoExecute without tolerationSeconds, so
// those taints never evict them: the tolerations replace equal ones (same
// key, operator Exists, no value, effect NoExecute) or are added, as the
// DaemonSet controller does it.
void add_daemon_pod_tolerations(JsonValue& pod_spec);

} // namespace kube32::models
