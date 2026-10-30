#pragma once

// Names shared by the firmware rollout controller, the kubelet's firmware
// agent and the API (docs/firmware-updates.md).

namespace kube32::models {

// Node annotation the rollout controller sets: the SHA-256 of the image the
// node is to run. The kubelet acts on it.
constexpr const char* kAnnotationFirmwareTarget = "kube32.io/firmware";

// Node annotations the kubelet keeps: the running image, its slot, whether
// it confirmed itself (Confirmed, Verifying), and the image that last failed
// on the node (rolled back, or failed to download or verify)
constexpr const char* kAnnotationFirmwareVersion = "kube32.io/firmware-version";
constexpr const char* kAnnotationFirmwareSha256 = "kube32.io/firmware-sha256";
constexpr const char* kAnnotationFirmwareSlot = "kube32.io/firmware-slot";
constexpr const char* kAnnotationFirmwareState = "kube32.io/firmware-state";
constexpr const char* kAnnotationFirmwareFailed = "kube32.io/firmware-failed";

// Node condition the kubelet reports with every heartbeat: True while an
// update is under way on the node
constexpr const char* kConditionFirmwareUpdate = "FirmwareUpdate";

} // namespace kube32::models
