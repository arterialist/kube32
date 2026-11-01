#pragma once

// Checkpoints and moves of running pods on the control plane
// (kube32.io/v1 Checkpoint, docs/migration.md).
//
// Save: the pod's kubelet freezes the pod between two host calls and
// answers POST /checkpoint/<ns>/<pod>/<container> with its state, which
// goes on as it arrives to the checkpoint's data (PUT
// /apis/kube32.io/v1/namespaces/<ns>/checkpoints/<name>/data on the API
// server). The kubelet thaws the pod and the phase turns Saved.
//
// Move: the same with the pod left frozen (?leave=frozen), then one store
// write replaces the pod: same name, spec, labels and owners, a new uid,
// spec.nodeName set to the target and the annotation kube32.io/restore-from
// naming the checkpoint. The source kubelet stops the frozen pod, which is
// no longer bound to it, and the target kubelet restores it from the data.
// A Deployment counts one pod all along: no pod is added and none goes
// terminating. The move is Completed once the replacement runs. A
// RestoreError, a Failed pod or no Running within move_deadline_ms puts the
// pod back on the source node, restored from the same checkpoint, and the
// move is Failed. A failure before the replacement thaws the pod where it
// is; a move the eviction API asked for then deletes the pod as an eviction
// does.
//
// Phases: Pending, Saving, then Saved (Save) or Moving and Completed (Move);
// Failed with a message, and the blockers when the pod cannot move. The
// target is spec.target.nodeName, or the node the scheduler picks with
// spec.target.nodeSelector added, never the source node and never one that
// is cordoned or not Ready. Events on the pod: Checkpointed, Moving, Moved,
// MoveFailed, CheckpointFailed, Restored.
//
// Policies (pod annotation kube32.io/migration): OnDrain (default) moves on
// eviction only; Never never moves and is never checkpointed by policy;
// Auto also moves the pod that frees the most memory off a node reporting
// MemoryPressure (one move per node per pressure_cooldown_ms), and with
// kube32.io/placement: fastest to the node with the highest
// kube32.io/interp-mops when it is at least twice as fast. A move of such a
// pod without a target (an eviction) goes to the fastest node that fits.
// Auto moves happen at most once per pod per auto_cooldown_ms.
//
// kube32.io/checkpoint-interval takes a Save checkpoint (label
// kube32.io/trigger: periodic) of a running pod at that interval (at least
// min_interval_ms). When the node lifecycle controller evicts the pods of a
// lost node, a pod with a Saved periodic checkpoint is rebound with
// kube32.io/restore-from (latest_periodic_checkpoint). Once a pod restored
// from a checkpoint runs the annotation is removed, so a later restart of
// the container starts it afresh.
//
// Retention: the latest Saved periodic checkpoint of each existing pod;
// other checkpoints keep_ms after they finished; never one a pod's
// kube32.io/restore-from names.
//
// start() runs a pass on a thread of its own after a Checkpoint changes, a
// pod with a migration annotation changes or what the controller reads of a
// Node changes, at the times it asked for (deadlines, intervals, cooldowns,
// expiries) and every resync_interval_ms; reconcile() is one pass. A pass
// takes at most one snapshot: the kubelet call blocks it.

#include "api/event_recorder.hpp"
#include "controller/reconcile_trigger.hpp"
#include "mem/function_ref.hpp"
#include "osal/thread.hpp"
#include "scheduler/scheduler.hpp"
#include "storage/store.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kube32::controller {

struct CheckpointControllerConfig {
    uint32_t reconcile_interval_ms{1000};    // shortest time between two passes
    uint32_t resync_interval_ms{30000};
    size_t stack_bytes{12288};
    bool stack_in_psram{false};
    int core_affinity{-1};
    // The API server that keeps the checkpoints' data, and the bearer token
    // it and the kubelets expect (empty: none)
    std::string api_host{"127.0.0.1"};
    uint16_t api_port{6443};
    std::string token;
    uint32_t freeze_timeout_ms{10000};       // ?timeoutMs= of the kubelet call
    uint32_t move_deadline_ms{60000};        // the replacement runs within this, or the move rolls back
    uint32_t keep_ms{3600000};               // finished Save, Move and Failed checkpoints are kept this long
    uint32_t auto_cooldown_ms{600000};
    uint32_t pressure_cooldown_ms{60000};
    uint32_t min_interval_ms{10000};
    // Wall clock in Unix milliseconds, 0 while unset. Empty: osal::System.
    std::function<uint64_t()> unix_time_ms;
};

class CheckpointController {
public:
    CheckpointController(storage::Store& store, scheduler::Scheduler* scheduler, CheckpointControllerConfig config = {});
    ~CheckpointController();

    bool start();
    void stop();

    // One pass. `now_ms` is the monotonic clock (0: read it).
    void reconcile(uint64_t now_ms = 0);
    uint64_t passes() const { return passes_.load(); }

private:
    struct Node;
    struct Ckpt;
    struct Pods;
    struct Snapshot;

    std::vector<Node> load_nodes();
    static const Node* node(const std::vector<Node>& nodes, const std::string& name);
    // A checkpoint of pod `ns/pod` under way (`started_only`: Saving or Moving)
    static bool busy(const std::vector<Ckpt>& cks, const std::string& ns, const std::string& pod, bool started_only);
    static std::string container_of(const Ckpt& ck, const models::JsonValue& pod);
    // What keeps the pod from moving: runtime::static_migration_blockers, and
    // the entry point of an embedded module
    static std::vector<std::string> blockers(const std::string& json, const models::JsonValue& pod);
    void begin(Ckpt& ck, Pods& pods, const std::vector<Node>& nodes, uint64_t now);
    void watch_move(Ckpt& ck, Pods& pods, const std::vector<Node>& nodes, uint64_t now);
    void fail(Ckpt& ck, const std::string& message, const std::vector<std::string>& blockers = {});
    // fail(), and for a move the eviction API asked for, the eviction
    void give_up(Ckpt& ck, const std::string& message, const std::vector<std::string>& blockers = {});
    void finish_restores(Pods& pods, const std::vector<Node>& nodes, const std::vector<Ckpt>& cks);
    void policies(Pods& pods, const std::vector<Node>& nodes, const std::vector<Ckpt>& cks, uint64_t now);
    void retention(Pods& pods, const std::vector<Ckpt>& cks, uint64_t now);
    std::string pick_target(const Ckpt& ck, const models::JsonValue& pod, const std::vector<Node>& nodes, std::string& why);
    // The pod's state from its kubelet into the checkpoint's data: POST
    // /checkpoint on the node, its answer streamed on to PUT .../data on the
    // API server. The one place the bytes pass through.
    Snapshot take_snapshot(const Node& node, const Ckpt& ck, const std::string& container, bool frozen);
    bool thaw(const Node* node, const std::string& ns, const std::string& pod, const std::string& container);
    bool edit(const std::string& key, mem::FunctionRef<bool(models::JsonValue&)> change);
    // Replaces the pod at `key` (uid `uid`): a new uid, bound to `node` (or
    // unbound), restored from checkpoint `restore_from` (empty: started afresh)
    bool rebind(const std::string& key, const std::string& node, const std::string& restore_from, const std::string& uid, std::string* new_uid);
    bool strip_restore(const std::string& key);
    // Sets the non-empty strings and the blockers in the status
    bool update_status(const std::string& key, std::initializer_list<std::pair<const char*, std::string_view>> strs,
                       const std::vector<std::string>* blockers = nullptr);
    void create(const std::string& ns, const std::string& pod, const char* action, const std::string& target, const char* trigger);
    void pod_event(const std::string& ns, const std::string& name, const std::string& uid, const char* reason, const std::string& message,
                   bool warning);
    uint64_t wall_ms() const;

    storage::Store& store_;
    scheduler::Scheduler* scheduler_;
    CheckpointControllerConfig config_;
    api::EventRecorder events_;
    std::map<std::string, uint64_t> moving_since_;    // checkpoint uid -> when its move began (monotonic)
    std::map<std::string, uint64_t> done_seen_;       // checkpoint uid -> when it was first seen finished
    std::map<std::string, uint64_t> last_periodic_;   // ns/pod -> when its last periodic checkpoint was asked for
    std::map<std::string, uint64_t> auto_moved_;      // ns/pod -> when an Auto policy last moved it
    std::map<std::string, uint64_t> pressure_moved_;  // node -> when memory pressure last moved a pod off it
    ReconcileTrigger trigger_;
    std::unique_ptr<osal::Thread> thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> passes_{0};
};

// The latest Saved periodic checkpoint of pod `ns/pod` in `name`; false
// when it has none. The node lifecycle controller restores evicted pods
// from it.
bool latest_periodic_checkpoint(storage::Store& store, const std::string& ns, const std::string& pod, std::string& name);

} // namespace kube32::controller
