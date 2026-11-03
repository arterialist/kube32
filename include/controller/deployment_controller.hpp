#pragma once

// apps/v1 Deployment reconciliation on the control plane: keeps
// spec.replicas pods alive from spec.template (owned through
// ownerReferences), rolls pods whose template hash is stale one at a time,
// garbage-collects pods whose Deployment is gone, and writes a status that
// is computed from the pods that exist (readyReplicas = pods whose Ready
// condition is True, never the requested count).
//
// start() runs a pass after a Deployment or a pod a Deployment owns
// changes, at most every reconcile_interval_ms, and every
// resync_interval_ms without a change.

#include "storage/store.hpp"
#include "scheduler/scheduler.hpp"
#include "api/event_recorder.hpp"
#include "controller/reconcile_trigger.hpp"
#include "osal/thread.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace kube32::controller {

struct DeploymentControllerConfig {
    uint32_t reconcile_interval_ms{1000};   // shortest time between two passes
    uint32_t resync_interval_ms{30000};     // a pass at least this often
    size_t stack_bytes{16384};
    bool stack_in_psram{false};
    int core_affinity{-1};
};

class DeploymentController {
public:
    DeploymentController(storage::Store& store, scheduler::Scheduler* scheduler = nullptr, DeploymentControllerConfig cfg = {});
    ~DeploymentController();

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    // One reconciliation pass over every Deployment (public for tests).
    void reconcile();
    // Passes run so far
    uint64_t passes() const { return passes_.load(); }

    // Stable 8-character hash of a pod template (the pod-template-hash label).
    static std::string template_hash(const std::string& template_json);

private:
    struct PodView;
    static void task_entry(void* arg);
    void run_loop();
    void reconcile_one(const std::string& key, models::JsonValue& dep, const std::vector<PodView>& pods);
    void delete_pod(const std::string& pod_key);

    storage::Store& store_;
    scheduler::Scheduler* scheduler_;
    DeploymentControllerConfig cfg_;
    api::EventRecorder events_;
    ReconcileTrigger trigger_;
    std::unique_ptr<osal::Thread> thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> passes_{0};
};

} // namespace kube32::controller
