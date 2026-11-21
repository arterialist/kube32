#pragma once

// Pod migration (docs/migration.md): the annotations that steer it and the
// Checkpoint objects
// (kube32.io/v1, /registry/checkpoints/<namespace>/<name>) that the eviction
// API and the Checkpoint controller create.

#include "hal/hal_types.hpp"
#include "models/kube_models.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kube32::models {

// Pod annotations
constexpr const char* kAnnotationMigration = "kube32.io/migration";               // OnDrain (default), Never, Auto
constexpr const char* kAnnotationPlacement = "kube32.io/placement";               // "fastest" or "hardware" (with Auto)
constexpr const char* kAnnotationCheckpointInterval = "kube32.io/checkpoint-interval";   // "60s", at least 10 s
constexpr const char* kAnnotationRestoreFrom = "kube32.io/restore-from";          // checkpoint the kubelet restores the pod from
// The devices the current step of a hardware-following pod needs, as the
// kubelet records them from k32_need: resource names, comma-separated
constexpr const char* kAnnotationNeeds = "kube32.io/needs";
// Nodes a placement prefers, best first: node names or <label>=<value>
constexpr const char* kAnnotationPreferNodes = "kube32.io/prefer-nodes";
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

// Hardware-following pods (docs/migration.md#hardware-following-pods).
// The on-board devices such a pod takes from node to node, as the
// capability firewall's KUBE32_CAP_* bits: their grants follow the pod,
// their state stays on the node.
constexpr uint32_t kFollowedDevices = hal::KUBE32_CAP_DISPLAY | hal::KUBE32_CAP_KEYBOARD | hal::KUBE32_CAP_SPEAKER |
                                      hal::KUBE32_CAP_MICROPHONE | hal::KUBE32_CAP_TOUCH | hal::KUBE32_CAP_LED | hal::KUBE32_CAP_CHIP_TEMP;

// The device a name grants: a resource ("hardware.kube32.io/tft-display"),
// the part after "hardware.kube32.io/" ("tft-display") or a capability
// ("DISPLAY", "TFT_DISPLAY"). 0 for any other name.
uint32_t device_bit(std::string_view name);
// The devices of a comma-separated list of such names, spaces around the
// names and empty entries skipped. False when an entry names no device.
bool parse_devices(std::string_view list, uint32_t& out);
// The resource names of `devices`, comma-separated in a fixed order: the
// value of kube32.io/needs
std::string devices_text(uint32_t devices);
// kube32.io/placement: hardware under the Auto policy
bool follows_hardware(const JsonValue& pod);
// The devices the pod's kube32.io/needs names
uint32_t pod_needs(const JsonValue& pod);
// The devices a Node offers pods: its allocatable (else capacity)
// hardware.kube32.io/* resources above 0
uint32_t node_devices(const JsonValue& node);
// The first entry of the pod's kube32.io/prefer-nodes the node matches
// (0 for the first), -1 when it matches none or the pod has none
int preferred_rank(const JsonValue& pod, const JsonValue& node);

} // namespace kube32::models
