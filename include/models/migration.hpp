#pragma once

// Pod migration (docs/migration.md): the annotations that steer it and the
// Checkpoint objects
// (kube32.io/v1, /registry/checkpoints/<namespace>/<name>) that the eviction
// API and the Checkpoint controller create.

#include "models/kube_models.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace kube32::models {

// Pod annotations
constexpr const char* kAnnotationMigration = "kube32.io/migration";               // OnDrain (default), Never, Auto
constexpr const char* kAnnotationPlacement = "kube32.io/placement";               // "fastest" (with Auto)
constexpr const char* kAnnotationCheckpointInterval = "kube32.io/checkpoint-interval";   // "60s", at least 10 s
constexpr const char* kAnnotationRestoreFrom = "kube32.io/restore-from";          // checkpoint the kubelet restores the pod from
// Node annotation: millions of wasm3 operations per second the node measured at boot
constexpr const char* kAnnotationInterpMops = "kube32.io/interp-mops";
// Checkpoint label: what created it (periodic, eviction, memory-pressure,
// placement); absent on checkpoints a user created
constexpr const char* kLabelTrigger = "kube32.io/trigger";
constexpr const char* kCheckpointPrefix = "/registry/checkpoints/";

enum class MigrationPolicy { OnDrain, Never, Auto };

MigrationPolicy migration_policy(const JsonValue& pod);

// "60s", "2m", "1h", "500ms" or milliseconds; 0 when it is none of these
uint64_t parse_duration_ms(const std::string& text);

// A Checkpoint object, phase Pending. `action` is "Save" or "Move";
// `target_node` and `trigger` may be empty.
std::string checkpoint_json(const std::string& ns, const std::string& name, const std::string& pod, const std::string& action,
                            const std::string& target_node, const std::string& trigger);

} // namespace kube32::models
