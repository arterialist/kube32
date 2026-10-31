#pragma once

// Devices (kube32.io/v1 Device, docs/devices.md) on the control plane.
//
// For every Device the controller keeps two objects, both owned by it:
//
//   Pod "<device>-driver": the catalog driver (models/device.hpp) from
//     wasm://embedded/driver-<driver> (or spec.image), pinned to the Device's
//     node with a kubernetes.io/hostname nodeSelector, so the scheduler binds
//     it there and nowhere else. It carries the Device's labels plus
//     kube32.io/device and kube32.io/driver, the DEVICE_* / I2C_* / PIN_* /
//     CONFIG_* environment, kube32.io/tick-interval, a device grant
//     (kube32.io/i2c-device) for an I2C part or esp32.io/gpio-<n> claims for
//     its pins, and CHANNEL_IPC to publish readings on device.<device>. Its
//     container port "http" is the node's pod port.
//   Service "<device>": port 80 to the driver's "http" port, selecting
//     kube32.io/device=<device>.
//
// A Device names its node with spec.nodeName, or with spec.nodeSelector:
// the first Ready node that matches, boards before kube32-sim nodes
// (kube32.io/simulator), kept in status.nodeName while it still matches.
// A driver pod that no longer matches its Device (spec, node, image) is
// replaced; a changed label set is written onto it in place; a deleted one is
// created again; a Failed one is replaced after a minute, then after longer waits. The Device's status
// follows the pod: phase Pending, Running or Failed, with nodeName, podName,
// the pod's waiting or failure message, and chip (what discovery identified
// at the Device's address).
//
// Discovery: a node's kube32.io/i2c-devices annotation (hal/i2c_discovery.hpp)
// lists the parts its buses answer for. Each part identified as a catalog
// driver that no Device covers yet (same node, bus, multiplexer channel and
// address) gets a Device named <driver>-<node>-b<bus>[-m<mux>c<channel>]-<addr>
// in the configured namespace, labelled kube32.io/discovered=true and
// kube32.io/driver=<driver>. Labels added later stay. A discovered Device
// whose part the node no longer reports turns Absent: its driver pod goes,
// the Device and its Service stay until the part answers again.
//
// Pods and Services whose Device is gone are deleted. start() runs a pass
// every reconcile_interval_ms on a thread of its own; reconcile() is one
// pass (tests call it directly).

#include "api/event_recorder.hpp"
#include "models/device.hpp"
#include "scheduler/scheduler.hpp"
#include "osal/thread.hpp"
#include "storage/store.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace kube32::controller {

struct DeviceControllerConfig {
    uint32_t reconcile_interval_ms{2000};
    size_t stack_bytes{12288};
    bool stack_in_psram{false};
    int core_affinity{-1};
    // Namespace of the Devices created from discovery
    std::string discovered_namespace{"default"};
    // A Failed driver pod is replaced after this long, doubled for every
    // replacement that fails again, up to failed_retry_max_ms
    uint32_t failed_retry_ms{60000};
    uint32_t failed_retry_max_ms{600000};
};

class DeviceController {
public:
    explicit DeviceController(storage::Store& store, scheduler::Scheduler* scheduler = nullptr, DeviceControllerConfig config = {});
    ~DeviceController();

    bool start();
    void stop();

    // One pass. `now_ms` is the monotonic clock (0: read it).
    void reconcile(uint64_t now_ms = 0);

private:
    struct NodeView {
        std::string name;
        std::map<std::string, std::string> labels;
        bool ready{false};
        bool simulator{false};
        uint16_t pod_port{7000};
        bool reported{false};                       // carries a discovery report
        std::vector<models::DiscoveredPart> parts;
    };
    struct DeviceView;
    struct PodView {
        std::string key;
        std::string json;
        std::string owner_uid;
        std::string phase;
        std::string hash;          // kube32.io/device-spec annotation
        bool terminating{false};
        bool ready{false};
        std::string message;       // why it is not running
    };

    std::vector<NodeView> load_nodes();
    void discover(const std::vector<NodeView>& nodes, std::vector<std::string>& keys, std::vector<std::string>& jsons);
    void reconcile_device(const std::string& key, const std::string& json, const std::vector<NodeView>& nodes,
                          std::map<std::string, PodView>& pods, uint64_t now);
    std::string place(const models::DeviceSpec& spec, const std::string& current, const std::vector<NodeView>& nodes) const;
    // The driver pod for `d` on `node`; `out_hash` is what it was made from
    std::string pod_json(const DeviceView& d, const NodeView* node, std::string& out_hash) const;
    void ensure_service(const DeviceView& d, std::string& message);
    void delete_pod(const std::string& key, const std::string& json);
    bool update_pod_labels(const std::string& key, const models::JsonValue& labels);
    void write_status(const std::string& key, const models::JsonValue& status);
    void event(const DeviceView& d, const char* reason, const std::string& message, bool warning);

    storage::Store& store_;
    scheduler::Scheduler* scheduler_;
    DeviceControllerConfig config_;
    api::EventRecorder events_;
    // Failed driver pods by device uid: since when (0 while its replacement
    // has not failed yet) and the wait before the next replacement
    struct Retry {
        uint64_t since{0};
        uint64_t delay_ms{0};
    };
    std::map<std::string, Retry> retries_;
    std::unique_ptr<osal::Thread> thread_;
    std::atomic<bool> running_{false};
};

} // namespace kube32::controller
