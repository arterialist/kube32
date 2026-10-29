#pragma once

// Which store keys reach flash, and how soon.
//
//   Ephemeral  Leases and Events: rewritten every few seconds and rebuilt by
//              their writers after a restart; kept in RAM only.
//   Deferred   Nodes: the kubelet heartbeat rewrites status (allocatable
//              memory, heartbeat conditions) every few seconds. A change to
//              labels, annotations, taints or other spec fields, or to the
//              Ready condition, is written like any other object; heartbeat
//              churn is written at most once per deferred interval.
//   Immediate  Everything else: written within the flush delay (one second),
//              several changes to one key in that window coalesce.

#include <cstdint>
#include <string>

namespace kube32::storage {

enum class PersistClass { Ephemeral, Immediate, Deferred };

PersistClass persist_class(const std::string& key);

// For Deferred keys: a fingerprint of the parts of `json` that must not wait
// (metadata labels/annotations/uid, spec, the Ready condition's status).
uint64_t durable_fingerprint(const std::string& key, const std::string& json);

} // namespace kube32::storage
