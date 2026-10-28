#pragma once

// apps/v1 Deployment reconciliation on the control plane: keeps
// spec.replicas pods alive from spec.template (owned through
// ownerReferences), rolls pods whose template hash is stale one at a time,
// garbage-collects pods whose Deployment is gone, and writes a status that
// is computed from the pods that exist (readyReplicas = pods whose Ready
// condition is True, never the requested count).

#include "storage/store.hpp"
#include "scheduler/scheduler.hpp"
#include "api/event_recorder.hpp"
#include "osal/thread.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace kube32::controller {

struct DeploymentControllerConfig {
    uint32_t reconcile_interval_ms{1000};
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

    // Stable 8-character hash of a pod template (the pod-template-hash label).
    static std::string template_hash(const std::string& template_json);

private:
    static void task_entry(void* arg);
    void run_loop();
    void reconcile_one(const std::string& key, const std::string& json, const std::vector<std::string>& ns_pod_jsons,
                       const std::vector<std::string>& ns_pod_keys);
    void collect_garbage(const std::vector<std::string>& deployment_uids);
    void delete_pod(const std::string& pod_key, const std::string& pod_json);

    storage::Store& store_;
    scheduler::Scheduler* scheduler_;
    DeploymentControllerConfig cfg_;
    api::EventRecorder events_;
    std::unique_ptr<osal::Thread> thread_;
    std::atomic<bool> running_{false};
};

} // namespace kube32::controller
