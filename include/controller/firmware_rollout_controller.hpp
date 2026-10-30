#pragma once

// Rolls the staged firmware image out to the cluster, one node at a time
// (kube32.io/v1 FirmwareRollout, docs/firmware-updates.md).
//
// A FirmwareRollout names the image by its SHA-256 (spec.sha256); it must be
// the image staged on the control plane (api/firmware_api.hpp). Workers go
// first, in name order, then the control plane. For the first node that does
// not run the image confirmed, the controller sets the node annotation
// kube32.io/firmware to the SHA-256; the node's kubelet downloads the image,
// switches its boot slot, reboots, and reports the image it runs and whether
// it confirmed itself in its own annotations. The next node starts once the
// previous one runs the image confirmed. The control plane, last, boots the
// image straight from the slot it was staged in.
//
// The rollout stops (phase Failed) at the first node that reports the image
// failed (it rolled back, or the download or check failed) or that has not
// switched to the image confirmed within spec.nodeTimeoutSeconds. Failed and
// Complete are final: a new attempt is a new FirmwareRollout. Deleting a
// rollout removes kube32.io/firmware from the nodes that have not switched.
// Progress is in the rollout's status, in Events on the rollout and on the
// nodes, and in each node's FirmwareUpdate condition.
//
// reconcile() is one pass; the control plane calls it from its housekeeping
// loop, so the controller has no thread of its own.

#include "api/event_recorder.hpp"
#include "api/firmware_api.hpp"
#include "storage/store.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace kube32::controller {

struct FirmwareRolloutConfig {
    // Time a node has to switch to the image confirmed, when the rollout
    // does not set spec.nodeTimeoutSeconds
    uint32_t node_timeout_ms{900000};
};

class FirmwareRolloutController {
public:
    FirmwareRolloutController(storage::Store& store, std::function<api::StagedFirmware()> staged,
                              FirmwareRolloutConfig config = {});

    // One pass. `now_ms` is the monotonic clock (0: read it).
    void reconcile(uint64_t now_ms = 0);

private:
    struct NodeView {
        std::string name;
        std::string uid;
        bool control_plane{false};
        std::string target;     // kube32.io/firmware
        std::string running;    // kube32.io/firmware-sha256
        std::string version;    // kube32.io/firmware-version
        std::string state;      // kube32.io/firmware-state
        std::string failed;     // kube32.io/firmware-failed
        std::string condition;  // message of the FirmwareUpdate condition
    };
    struct RolloutStatus {
        std::string phase;
        std::string version;
        std::string node;
        int64_t updated{0};
        int64_t total{0};
        std::string message;
    };

    std::vector<NodeView> load_nodes();
    // Sets (or with an empty `sha256` removes) kube32.io/firmware on a node
    bool set_node_target(const std::string& node, const std::string& sha256);
    // Writes the rollout's status when it changed; false when the rollout is gone
    bool write_status(const std::string& key, const RolloutStatus& st);
    void node_event(const NodeView& node, const char* reason, const std::string& message, bool warning);
    void rollout_event(const std::string& name, const std::string& uid, const char* reason, const std::string& message, bool warning);

    storage::Store& store_;
    std::function<api::StagedFirmware()> staged_;
    FirmwareRolloutConfig config_;
    api::EventRecorder events_;
    std::string uid_;                            // the rollout acted on in the last pass
    std::string target_;                         // its image
    std::map<std::string, uint64_t> started_ms_; // node -> when this controller saw its update start
};

} // namespace kube32::controller
