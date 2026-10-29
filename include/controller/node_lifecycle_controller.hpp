#pragma once

// Node health from Lease objects (coordination.k8s.io/v1, namespace
// kube-node-lease), as kube-controller-manager's node lifecycle controller
// derives it.
//
// A kubelet renews its Lease every heartbeat. The controller notes when it
// sees spec.renewTime change (the arrival time on this node's monotonic clock,
// so the boards' wall clocks need not agree) and marks the node NotReady
// (Ready=Unknown, reason NodeStatusUnknown, taint
// node.kubernetes.io/unreachable:NoSchedule) once no renewal has been seen
// for spec.leaseDurationSeconds. After a further eviction delay its pods are
// evicted for rescheduling (DaemonSet pods stay) and the NoExecute taint is
// added. A renewal clears both taints; the kubelet's next status update
// restores Ready.
//
// Leases are not persisted. After a control-plane restart a node whose
// stored Ready condition was True gets a full lease duration from the moment
// the controller first sees it: it had no control plane to renew against. A
// node that was already NotReady stays NotReady, and its eviction delay runs
// from the condition's lastTransitionTime when the wall clock allows,
// otherwise from the restart.

#include "storage/store.hpp"
#include "api/event_recorder.hpp"
#include "osal/thread.hpp"
#include "osal/mutex.hpp"

#include <string>
#include <map>
#include <atomic>
#include <memory>
#include <cstdint>

namespace kube32::controller {

struct NodeLifecycleConfig {
    uint32_t check_interval_ms{1000};
    // Lease duration assumed when a Lease does not state one, and for a node
    // that has not renewed since this controller started
    uint32_t not_ready_threshold_ms{15000};
    // Time since the last renewal after which pods are evicted (the eviction
    // delay is this minus the lease duration, at least 0)
    uint32_t eviction_threshold_ms{30000};
};

class NodeLifecycleController {
public:
    explicit NodeLifecycleController(storage::Store& store, NodeLifecycleConfig config = {});
    ~NodeLifecycleController();

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    // One pass over every Node. `now_ms` is the monotonic clock (0: read it).
    void reconcile_nodes(uint64_t now_ms = 0);

    // Monotonic time at which the controller last saw the node's lease
    // renewed (0 when it has not seen one).
    uint64_t last_renewal_seen(const std::string& node_name) const;

private:
    struct NodeHealth {
        std::string renew_marker;     // spec.renewTime (or resourceVersion without one) last seen
        uint64_t probe_ms{0};         // when the marker last changed; 0: expired already
        uint64_t grace_ms{0};         // lease duration
        bool not_ready{false};
        uint64_t not_ready_since_ms{0};
        bool evicted{false};
    };

    static void task_entry(void* arg);
    void run_loop();

    void mark_not_ready(const std::string& node_name, uint64_t since_ms);
    void mark_recovered(const std::string& node_name);
    void evict_pods_from_node(const std::string& node_name);
    bool update_node(const std::string& node_name, bool not_ready, bool no_execute);
    void record_node_event(const std::string& node_name, const char* reason, const std::string& message, bool warning);

    storage::Store& store_;
    NodeLifecycleConfig config_;
    std::unique_ptr<osal::Thread> thread_;
    std::atomic<bool> running_{false};
    mutable osal::Mutex mutex_;
    std::map<std::string, NodeHealth> nodes_;
    api::EventRecorder events_;
};

} // namespace kube32::controller
