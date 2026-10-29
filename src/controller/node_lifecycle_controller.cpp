#include "controller/node_lifecycle_controller.hpp"
#include "models/kube_models.hpp"
#include "models/tolerations.hpp"
#include "osal/system.hpp"

#include <iostream>
#include <vector>

namespace kube32::controller {

static const char* const kLeasePrefix = "/registry/leases/kube-node-lease/";
static const char* const kUnreachableTaint = models::kTaintNodeUnreachable;

static models::JsonValue jstr(const std::string& v) { return models::JsonValue{models::JsonType::String, false, 0, v, {}, {}}; }

NodeLifecycleController::NodeLifecycleController(storage::Store& store, NodeLifecycleConfig config)
    : store_(store), config_(config), events_(store, 64) {}

NodeLifecycleController::~NodeLifecycleController() {
    stop();
}

bool NodeLifecycleController::start() {
    if (running_.load()) return true;
    running_.store(true);

    thread_ = std::make_unique<osal::Thread>();
    osal::Thread::Config cfg;
    cfg.name = "node_lifecycle";
    cfg.stack_size = 8192;
    return thread_->start(task_entry, this, cfg);
}

void NodeLifecycleController::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (thread_ && thread_->is_joinable()) {
        thread_->join();
        thread_.reset();
    }
}

void NodeLifecycleController::task_entry(void* arg) {
    static_cast<NodeLifecycleController*>(arg)->run_loop();
}

void NodeLifecycleController::run_loop() {
    while (running_.load()) {
        try {
            reconcile_nodes();
        } catch (const std::exception& e) {
            std::cout << "[LIFECYCLE] pass skipped: " << e.what() << std::endl;
        }
        osal::Thread::sleep_ms(config_.check_interval_ms);
    }
}

void NodeLifecycleController::record_node_event(const std::string& node_name, const char* reason, const std::string& message, bool warning) {
    api::EventSpec ev;
    ev.involved.kind = "Node";
    ev.involved.name = node_name;
    std::string node_json;
    if (store_.get("/registry/nodes/" + node_name, node_json).ok()) {
        models::JsonValue doc;
        std::string err;
        if (models::parse_json(node_json, doc, err) && doc.is_object()) {
            const models::JsonValue* meta = doc.get("metadata");
            if (meta) ev.involved.uid = meta->get_string("uid", "");
        }
    }
    ev.reason = reason;
    ev.message = message;
    ev.warning = warning;
    ev.component = "node-controller";
    events_.record(ev);
}

uint64_t NodeLifecycleController::wall_clock_ms() const {
    return config_.unix_time_ms ? config_.unix_time_ms() : osal::System::get_unix_time_ms();
}

uint64_t NodeLifecycleController::last_renewal_seen(const std::string& node_name) const {
    osal::LockGuard guard(mutex_);
    auto it = nodes_.find(node_name);
    return it == nodes_.end() ? 0 : it->second.probe_ms;
}

// Ready condition of a stored Node: status ("" without one) and lastTransitionTime.
static void ready_condition(const models::JsonValue& node, std::string& status, std::string& reason, std::string& since) {
    status.clear();
    reason.clear();
    since.clear();
    const models::JsonValue* st = node.get("status");
    const models::JsonValue* conds = st ? st->get("conditions") : nullptr;
    if (!conds || !conds->is_array()) return;
    for (const auto& c : conds->arr_val) {
        if (c.is_object() && c.get_string("type", "") == "Ready") {
            status = c.get_string("status", "");
            reason = c.get_string("reason", "");
            since = c.get_string("lastTransitionTime", "");
            return;
        }
    }
}

// The node's node.kubernetes.io/unreachable:NoExecute taint, or null.
static const models::JsonValue* unreachable_no_execute(const models::JsonValue& node) {
    const models::JsonValue* spec = node.get("spec");
    const models::JsonValue* taints = spec ? spec->get("taints") : nullptr;
    if (!taints || !taints->is_array()) return nullptr;
    for (const auto& t : taints->arr_val) {
        if (t.is_object() && t.get_string("key", "") == kUnreachableTaint && t.get_string("effect", "") == "NoExecute") return &t;
    }
    return nullptr;
}

// Milliseconds from an RFC 3339 time to `wall_ms`; 0 when either is unknown.
static uint64_t ms_since(const std::string& rfc3339, uint64_t wall_ms) {
    uint64_t at = 0;
    if (rfc3339.empty() || wall_ms == 0 || !models::parse_rfc3339(rfc3339, at) || wall_ms <= at) return 0;
    return wall_ms - at;
}

void NodeLifecycleController::reconcile_nodes(uint64_t now) {
    if (now == 0) now = osal::System::get_uptime_ms();
    const uint64_t eviction_delay = config_.eviction_threshold_ms > config_.not_ready_threshold_ms
                                        ? config_.eviction_threshold_ms - config_.not_ready_threshold_ms : 0;
    const uint64_t wall = wall_clock_ms();

    std::vector<std::string> node_jsons;
    store_.list("/registry/nodes/", node_jsons);
    std::vector<std::string> lease_jsons, lease_keys;
    store_.list_extended(kLeasePrefix, lease_jsons, &lease_keys);
    std::map<std::string, models::JsonValue> leases;
    for (size_t i = 0; i < lease_jsons.size(); ++i) {
        models::JsonValue l;
        std::string err;
        if (models::parse_json(lease_jsons[i], l, err) && l.is_object()) {
            leases[lease_keys[i].substr(std::string(kLeasePrefix).size())] = std::move(l);
        }
    }

    std::vector<std::string> seen;
    std::map<std::string, uint64_t> tainted;   // node -> how long it has had the NoExecute taint
    for (const auto& nj : node_jsons) {
        osal::Thread::yield();
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(nj, doc, err) || !doc.is_object()) continue;
        const models::JsonValue* meta = doc.get("metadata");
        const std::string name = meta ? meta->get_string("name", "") : "";
        if (name.empty()) continue;
        seen.push_back(name);

        // What the Lease says
        bool has_lease = false;
        std::string marker;
        uint64_t grace = config_.not_ready_threshold_ms;
        auto lit = leases.find(name);
        if (lit != leases.end()) {
            has_lease = true;
            const models::JsonValue* spec = lit->second.get("spec");
            const models::JsonValue* lmeta = lit->second.get("metadata");
            marker = spec ? spec->get_string("renewTime", "") : "";
            if (marker.empty() && lmeta) marker = "rv:" + lmeta->get_string("resourceVersion", "");
            int64_t duration_s = spec ? spec->get_int("leaseDurationSeconds", 0) : 0;
            if (duration_s > 0) grace = static_cast<uint64_t>(duration_s) * 1000ULL;
        }

        const models::JsonValue* stored_taint = unreachable_no_execute(doc);
        const std::string taint_added = stored_taint ? stored_taint->get_string("timeAdded", "") : "";

        bool first_sight = false, renewed = false, add_no_execute = false, no_execute = false;
        uint64_t not_ready_since = 0, no_execute_for = 0;
        bool become_not_ready = false;
        {
            osal::LockGuard guard(mutex_);
            auto it = nodes_.find(name);
            if (it == nodes_.end()) {
                first_sight = true;
                NodeHealth h;
                h.grace_ms = grace;
                std::string status, reason, since;
                ready_condition(doc, status, reason, since);
                if (has_lease) {
                    h.renew_marker = marker;
                    h.probe_ms = now;
                } else if (status == "True" || status.empty()) {
                    // No control plane to renew against until now: a full lease from here
                    h.probe_ms = now;
                } else {
                    // Already NotReady before this controller started
                    h.probe_ms = 0;
                    h.not_ready = true;
                    uint64_t elapsed = ms_since(since, wall);
                    if (elapsed > eviction_delay) elapsed = eviction_delay;
                    h.not_ready_since_ms = now > elapsed ? now - elapsed : 0;
                    if (stored_taint) {
                        // Tolerations keep running from the taint's timeAdded
                        h.no_execute = true;
                        h.no_execute_seen_ms = now;
                        h.no_execute_age_ms = ms_since(taint_added, wall);
                    }
                }
                it = nodes_.emplace(name, h).first;
            } else if (has_lease && marker != it->second.renew_marker) {
                it->second.renew_marker = marker;
                it->second.probe_ms = now;
                it->second.grace_ms = grace;
                if (it->second.not_ready) {
                    renewed = true;
                    it->second.not_ready = false;
                    it->second.no_execute = false;
                }
            }
            NodeHealth& h = it->second;
            if (!h.not_ready && (h.probe_ms == 0 || now >= h.probe_ms + h.grace_ms)) {
                h.not_ready = true;
                h.not_ready_since_ms = (h.probe_ms == 0) ? now : h.probe_ms + h.grace_ms;
                become_not_ready = true;
            }
            if (h.not_ready && !h.no_execute && now >= h.not_ready_since_ms + eviction_delay) {
                h.no_execute = true;
                h.no_execute_seen_ms = now;
                h.no_execute_age_ms = 0;
                add_no_execute = true;
            }
            not_ready_since = h.not_ready_since_ms;
            no_execute = h.no_execute;
            no_execute_for = (now > h.no_execute_seen_ms ? now - h.no_execute_seen_ms : 0) + h.no_execute_age_ms;
        }
        (void)first_sight;

        if (renewed) mark_recovered(name);
        if (become_not_ready) mark_not_ready(name, not_ready_since);
        if (no_execute) {
            // Written when added, when removed from the node by hand, and when
            // timeAdded is missing (no wall clock then) and can be set now
            if (add_no_execute || !stored_taint || (taint_added.empty() && wall > 0)) update_node(name, true, true, no_execute_for);
            tainted[name] = no_execute_for;
        }
    }
    if (!tainted.empty()) evict_pods(tainted);

    // Forget nodes that were deleted
    osal::LockGuard guard(mutex_);
    for (auto it = nodes_.begin(); it != nodes_.end();) {
        bool present = false;
        for (const auto& n : seen) if (n == it->first) { present = true; break; }
        it = present ? std::next(it) : nodes_.erase(it);
    }
}

// Sets the Ready condition to Unknown (not_ready) and the unreachable taints:
// NoSchedule with not_ready, NoExecute with no_execute. A taint that is
// already on the node stays as it is, except that a NoExecute taint without
// timeAdded gets the wall-clock time `no_execute_for_ms` ago. With not_ready
// false only the taints are removed: the kubelet reports Ready.
bool NodeLifecycleController::update_node(const std::string& node_name, bool not_ready, bool no_execute, uint64_t no_execute_for_ms) {
    const std::string key = "/registry/nodes/" + node_name;
    std::string json;
    if (!store_.get(key, json).ok()) return false;
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) return false;
    const uint64_t wall = wall_clock_ms();
    const std::string now = wall ? models::format_rfc3339(wall) : std::string();

    if (not_ready) {
        models::JsonValue* status = doc.get_mut("status");
        if (!status || !status->is_object()) {
            models::JsonValue s;
            s.type = models::JsonType::Object;
            doc.set("status", std::move(s));
            status = doc.get_mut("status");
        }
        models::JsonValue* conds = status->get_mut("conditions");
        if (!conds || !conds->is_array()) {
            models::JsonValue c;
            c.type = models::JsonType::Array;
            status->set("conditions", std::move(c));
            conds = status->get_mut("conditions");
        }
        models::JsonValue* ready = nullptr;
        for (auto& c : conds->arr_val) {
            if (c.is_object() && c.get_string("type", "") == "Ready") { ready = &c; break; }
        }
        if (!ready) {
            models::JsonValue c;
            c.type = models::JsonType::Object;
            c.set("type", jstr("Ready"));
            conds->arr_val.push_back(std::move(c));
            ready = &conds->arr_val.back();
        }
        if (ready->get_string("status", "") != "Unknown") {
            ready->set("status", jstr("Unknown"));
            ready->set("reason", jstr("NodeStatusUnknown"));
            ready->set("message", jstr("Kubelet stopped posting node status."));
            if (!now.empty()) ready->set("lastTransitionTime", jstr(now));
        }
    }

    models::JsonValue* spec = doc.get_mut("spec");
    if (!spec || !spec->is_object()) {
        models::JsonValue s;
        s.type = models::JsonType::Object;
        doc.set("spec", std::move(s));
        spec = doc.get_mut("spec");
    }
    models::JsonValue taints;
    taints.type = models::JsonType::Array;
    models::JsonValue had_no_schedule, had_no_execute;
    if (const models::JsonValue* old = spec->get("taints"); old && old->is_array()) {
        for (const auto& t : old->arr_val) {
            if (t.get_string("key", "") != kUnreachableTaint) {
                taints.arr_val.push_back(t);
                continue;
            }
            const std::string effect = t.get_string("effect", "");
            if (effect == "NoSchedule") had_no_schedule = t;
            else if (effect == "NoExecute") had_no_execute = t;
        }
    }
    auto add_taint = [&](const char* effect, models::JsonValue t, const std::string& added) {
        if (!t.is_object()) {
            t.type = models::JsonType::Object;
            t.set("key", jstr(kUnreachableTaint));
            t.set("effect", jstr(effect));
        }
        if (t.get_string("timeAdded", "").empty() && !added.empty()) t.set("timeAdded", jstr(added));
        taints.arr_val.push_back(std::move(t));
    };
    if (not_ready) add_taint("NoSchedule", had_no_schedule, had_no_schedule.is_object() ? std::string() : now);
    if (no_execute) add_taint("NoExecute", had_no_execute, wall > no_execute_for_ms ? models::format_rfc3339(wall - no_execute_for_ms) : std::string());
    if (taints.arr_val.empty()) spec->remove("taints"); else spec->set("taints", std::move(taints));

    const std::string updated = doc.to_string();
    if (updated == json) return false;
    store_.put(key, updated, true);
    return true;
}

void NodeLifecycleController::mark_not_ready(const std::string& node_name, uint64_t since_ms) {
    (void)since_ms;
    update_node(node_name, true, false);
    record_node_event(node_name, "NodeNotReady", "Node " + node_name + " status is now: NodeNotReady (lease not renewed)", true);
}

void NodeLifecycleController::mark_recovered(const std::string& node_name) {
    update_node(node_name, false, false);
    record_node_event(node_name, "NodeReady", "Node " + node_name + " status is now: NodeReady", false);
}

// The taint manager's pass over the pods of tainted nodes. A pod leaves the
// node when no toleration of it matches the unreachable NoExecute taint, or
// when the node has had the taint for the matching toleration's
// tolerationSeconds; a toleration without tolerationSeconds keeps it there.
void NodeLifecycleController::evict_pods(const std::map<std::string, uint64_t>& tainted) {
    models::JsonValue taint;
    taint.type = models::JsonType::Object;
    taint.set("key", jstr(kUnreachableTaint));
    taint.set("effect", jstr("NoExecute"));

    std::vector<std::string> pod_jsons;
    std::vector<std::string> pod_keys;
    store_.list_extended("/registry/pods/", pod_jsons, &pod_keys);

    for (size_t i = 0; i < pod_jsons.size(); ++i) {
        osal::Thread::yield();
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(pod_jsons[i], doc, err) || !doc.is_object()) continue;

        const models::JsonValue* spec = doc.get("spec");
        const std::string node_name = spec ? spec->get_string("nodeName", "") : "";
        auto node = tainted.find(node_name);
        if (node == tainted.end()) continue;

        // DaemonSet pods stay with their node, as in Kubernetes
        const models::JsonValue* meta = doc.get("metadata");
        const models::JsonValue* labels = meta ? meta->get("labels") : nullptr;
        if (models::has_owner(doc, "DaemonSet") || (labels && labels->get_string("kube32.io/daemonset", "") == "true")) {
            continue;
        }
        const int64_t tolerated_s = models::no_execute_toleration_seconds(spec->get("tolerations"), taint);
        if (tolerated_s == models::kTolerateForever) continue;
        if (node->second < static_cast<uint64_t>(tolerated_s) * 1000ULL) continue;

        // A pod already being deleted has no kubelet left to finalize it
        if (meta && !meta->get_string("deletionTimestamp", "").empty()) {
            store_.remove(pod_keys[i]);
            continue;
        }
        // Finished pods stay where they ran
        const models::JsonValue* st = doc.get("status");
        const std::string phase = st ? st->get_string("phase", "") : "";
        if (phase == "Succeeded" || phase == "Failed") continue;

        // Back to the scheduler: unbound, Pending, marked as evicted
        models::JsonValue* mutable_spec = doc.get_mut("spec");
        if (mutable_spec) mutable_spec->set("nodeName", jstr(""));
        models::JsonValue* mutable_status = doc.get_mut("status");
        if (!mutable_status || !mutable_status->is_object()) {
            models::JsonValue new_status;
            new_status.type = models::JsonType::Object;
            doc.set("status", std::move(new_status));
            mutable_status = doc.get_mut("status");
        }
        mutable_status->set("phase", jstr("Pending"));
        mutable_status->set("reason", jstr("Evicted"));
        mutable_status->set("message", jstr("Node " + node_name + " lease expired; evicted for rescheduling"));
        mutable_status->remove("podIP");
        mutable_status->remove("podIPs");
        mutable_status->remove("containerStatuses");

        models::JsonValue conds;
        conds.type = models::JsonType::Array;
        models::JsonValue disrupt;
        disrupt.type = models::JsonType::Object;
        disrupt.set("type", jstr("DisruptionTarget"));
        disrupt.set("status", jstr("True"));
        disrupt.set("reason", jstr("EvictionByNodeLifecycleController"));
        const uint64_t wall = wall_clock_ms();
        if (wall) disrupt.set("lastTransitionTime", jstr(models::format_rfc3339(wall)));
        conds.arr_val.push_back(std::move(disrupt));
        mutable_status->set("conditions", std::move(conds));

        store_.put(pod_keys[i], doc.to_string(), true);

        api::EventSpec ev;
        ev.involved.kind = "Pod";
        ev.involved.name = meta ? meta->get_string("name", "") : "";
        ev.involved.namespace_name = meta ? meta->get_string("namespace", "default") : "default";
        ev.involved.uid = meta ? meta->get_string("uid", "") : "";
        ev.reason = "Evicted";
        ev.message = "Pod evicted from node " + node_name + ": node lease expired and " +
                     (tolerated_s > 0 ? "the pod's " + std::to_string(tolerated_s) + " s toleration of " + kUnreachableTaint + ":NoExecute ran out"
                                      : "the pod does not tolerate " + std::string(kUnreachableTaint) + ":NoExecute") +
                     ", pending rescheduling";
        ev.warning = true;
        ev.component = "node-controller";
        events_.record(ev);
    }
}

} // namespace kube32::controller
