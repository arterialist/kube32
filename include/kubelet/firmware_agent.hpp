#pragma once

// The kubelet's part of a firmware rollout (docs/firmware-updates.md).
//
// When the node annotation kube32.io/firmware names an image other than the
// one running, the agent streams that image from the control plane
// (api/firmware_api.hpp) into the passive slot, a range per kubelet loop
// pass through a small buffer, hashes it as it arrives, has the slot check
// it, switches the boot slot and reboots. The control plane takes the image
// from its own passive slot, where it was staged, instead.
//
// An image that boots unconfirmed confirms itself once the node has
// registered and its heartbeats have reported Ready for confirm_after_ms
// without a failure. An image that has not got there within
// confirm_timeout_ms rejects itself, and the bootloader brings the previous
// slot back; so does any reset before the confirmation. The image the node
// came back from is then reported failed, and is not tried again while the
// node's target still names it.
//
// The node reports the running image and the outcome in its annotations
// (models/firmware.hpp), progress in its FirmwareUpdate condition, and steps
// as Events.

#include "hal/firmware_slots.hpp"
#include "kubelet/kube_api_client.hpp"
#include "osal/mutex.hpp"
#include "osal/sha256.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace kube32::kubelet {

struct FirmwareAgentConfig {
    std::string node_name;
    uint32_t check_interval_ms{10000};     // how often the node's target is read
    uint32_t confirm_after_ms{30000};      // Ready heartbeats an image needs to confirm itself
    uint32_t confirm_timeout_ms{300000};   // an unconfirmed image rejects itself after this
    size_t range_bytes{32 * 1024};         // image bytes per request
    uint32_t stall_timeout_ms{120000};     // a download that makes no progress this long fails
    uint32_t reboot_delay_ms{3000};        // time for the last heartbeat before the restart
    // The control plane: the image is in its own passive slot already
    bool local_staging{false};
    // Runs before the restart (the control plane writes its store journal)
    std::function<void()> before_reboot;
};

// The FirmwareUpdate condition, in fixed storage: the heartbeat thread
// copies it while the kubelet thread changes it
struct FirmwareConditionView {
    bool updating{false};               // status True
    char reason[16]{"UpToDate"};
    char message[128]{""};
};

class FirmwareAgent {
public:
    FirmwareAgent(hal::FirmwareSlots& slots, KubeApiClient& api, FirmwareAgentConfig config);

    // The running image
    const hal::FirmwareInfo& running() const { return running_; }
    // The node's firmware annotations as this boot reports them
    std::map<std::string, std::string> annotations() const;

    // One step, from the kubelet loop (registered or not)
    void tick(uint64_t now_ms, bool registered);
    // A heartbeat went out (`ok`) reporting Ready (`ready`)
    void on_heartbeat(bool ok, bool ready, uint64_t now_ms);
    FirmwareConditionView condition() const;
    // A restart is due: the kubelet sends one more heartbeat
    bool heartbeat_wanted() { return heartbeat_wanted_.exchange(false); }
    // The annotations are sent again at the next tick
    void mark_annotations_dirty() { annotations_dirty_ = true; }

    enum class State { Idle, Verifying, Waiting, Downloading, Rebooting, Failed, RolledBack };
    State state() const { return state_; }

private:
    void set_condition(bool updating, const char* reason, const std::string& message);
    void event(const char* reason, const std::string& message, bool warning);
    void patch_annotations();
    void fail(const std::string& sha, const std::string& why, const char* reason);
    void start_update(const std::string& target, uint64_t now_ms);
    void download_step(uint64_t now_ms);
    void activate(uint64_t now_ms);
    void report_idle();

    hal::FirmwareSlots& slots_;
    KubeApiClient& api_;
    FirmwareAgentConfig config_;
    hal::FirmwareInfo running_;
    std::atomic<State> state_{State::Idle};

    mutable osal::Mutex mu_;
    FirmwareConditionView condition_;     // guarded by mu_

    // Confirmation of an unconfirmed image
    uint64_t boot_ms_{0};
    std::atomic<uint64_t> healthy_since_ms_{0};
    // The image being fetched
    std::string target_;
    size_t size_{0};
    size_t offset_{0};
    std::string version_;
    uint64_t progress_ms_{0};
    uint64_t last_check_ms_{0};
    uint64_t reboot_at_ms_{0};
    osal::Sha256 sha_;
    // The image that failed here (note "fail"), and the annotations last sent
    std::string failed_;
    std::map<std::string, std::string> sent_annotations_;
    std::atomic<bool> annotations_dirty_{true};
    std::vector<std::pair<std::string, std::pair<std::string, bool>>> pending_events_;   // reason, message, warning
    std::atomic<bool> heartbeat_wanted_{false};
#if defined(KUBE32_FIRMWARE_FAULT)
    uint64_t fault_at_ms_{0};
#endif
};

} // namespace kube32::kubelet
