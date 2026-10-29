#pragma once

// DaemonSet (apps/v1) and Job (batch/v1) reconciliation on the control plane.
//
// DaemonSet: one pod per node whose labels match the template's nodeSelector.
// Each pod carries a nodeSelector on kubernetes.io/hostname, so the scheduler
// binds it to its node and checks that the node has room. Pods on nodes that
// left (or stopped matching) are deleted. Pods carry the template's revision
// (label controller-revision-hash); after a template change they are
// replaced node by node, at most spec.updateStrategy.rollingUpdate.
// maxUnavailable (default 1) nodes at a time without a ready pod, and a node
// gets its new pod once the old one has terminated. updateStrategy OnDelete
// leaves the replacement to the user.
//
// Job: runs spec.template until spec.completions pods have Succeeded (default
// 1), with at most spec.parallelism (default 1) at a time. Pods that end
// Failed count against spec.backoffLimit (default 6); past it the Job fails
// and its running pods are deleted. Pods that restart in place
// (restartPolicy OnFailure) are not counted as failures.
//
// Status is computed from the pods that exist. Pods whose owner is gone are
// deleted.

#include "storage/store.hpp"
#include "scheduler/scheduler.hpp"
#include "api/event_recorder.hpp"
#include "osal/thread.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace kube32::controller {

struct WorkloadControllerConfig {
    uint32_t reconcile_interval_ms{1000};
    size_t stack_bytes{16384};
    bool stack_in_psram{false};
    int core_affinity{-1};
};

class WorkloadController {
public:
    WorkloadController(storage::Store& store, scheduler::Scheduler* scheduler = nullptr, WorkloadControllerConfig cfg = {});
    ~WorkloadController();

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    // One pass over every DaemonSet and Job (public for tests).
    void reconcile();

private:
    struct PodView;
    static void task_entry(void* arg);
    void run_loop();
    void reconcile_daemonset(const std::string& key, const std::string& json, const std::vector<std::string>& node_jsons,
                             const std::vector<PodView>& pods);
    void reconcile_job(const std::string& key, const std::string& json, const std::vector<PodView>& pods);
    std::string create_pod(const std::string& owner_kind, const std::string& owner_api, const std::string& owner_name,
                           const std::string& owner_uid, const std::string& ns, const std::string& template_json,
                           const std::string& pin_node, const std::string& revision);
    void delete_pod(const std::string& pod_key, const std::string& pod_json);
    void record(const std::string& kind, const std::string& api, const std::string& name, const std::string& ns,
                const std::string& uid, const char* reason, const std::string& message, bool warning);

    storage::Store& store_;
    scheduler::Scheduler* scheduler_;
    WorkloadControllerConfig cfg_;
    api::EventRecorder events_;
    std::unique_ptr<osal::Thread> thread_;
    std::atomic<bool> running_{false};
};

} // namespace kube32::controller
