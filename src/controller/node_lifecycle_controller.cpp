#include "controller/node_lifecycle_controller.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"

#include <iostream>
#include <vector>

namespace kube32::controller {

static const char* const kLeasePrefix = "/registry/leases/kube-node-lease/";
static const char* const kUnreachableTaint = "node.kubernetes.io/unreachable";

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

void NodeLifecycleController::reconcile_nodes(uint64_t now) {
    if (now == 0) now = osal::System::get_uptime_ms();
    const uint64_t eviction_delay = config_.eviction_threshold_ms > config_.not_ready_threshold_ms
                                        ? config_.eviction_threshold_ms - config_.not_ready_threshold_ms : 0;

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

        bool first_sight = false, renewed = false, evict = false;
        uint64_t not_ready_since = 0;
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
                    uint64_t since_ms = 0, now_unix = osal::System::get_unix_time_ms();
                    uint64_t elapsed = 0;
                    if (!since.empty() && now_unix > 0 && models::parse_rfc3339(since, since_ms) && now_unix > since_ms) {
                        elapsed = now_unix - since_ms;
                    }
                    if (elapsed > eviction_delay) elapsed = eviction_delay;
                    h.not_ready_since_ms = now > elapsed ? now - elapsed : 0;
                }
                it = nodes_.emplace(name, h).first;
            } else if (has_lease && marker != it->second.renew_marker) {
                it->second.renew_marker = marker;
                it->second.probe_ms = now;
                it->second.grace_ms = grace;
                if (it->second.not_ready) {
                    renewed = true;
                    it->second.not_ready = false;
                    it->second.evicted = false;
                }
            }
            NodeHealth& h = it->second;
            if (!h.not_ready && (h.probe_ms == 0 || now >= h.probe_ms + h.grace_ms)) {
                h.not_ready = true;
                h.not_ready_since_ms = (h.probe_ms == 0) ? now : h.probe_ms + h.grace_ms;
                become_not_ready = true;
            }
            if (h.not_ready && !h.evicted && now >= h.not_ready_since_ms + eviction_delay) {
                h.evicted = true;
                evict = true;
            }
            not_ready_since = h.not_ready_since_ms;
        }
        (void)first_sight;

        if (renewed) mark_recovered(name);
        if (become_not_ready) mark_not_ready(name, not_ready_since);
        if (evict) {
            update_node(name, true, true);
            evict_pods_from_node(name);
        }
    }

    // Forget nodes that were deleted
    osal::LockGuard guard(mutex_);
    for (auto it = nodes_.begin(); it != nodes_.end();) {
        bool present = false;
        for (const auto& n : seen) if (n == it->first) { present = true; break; }
        it = present ? std::next(it) : nodes_.erase(it);
    }
}

// Sets the Ready condition to Unknown (not_ready) and the unreachable taints.
// With not_ready false only the taints are removed: the kubelet reports Ready.
bool NodeLifecycleController::update_node(const std::string& node_name, bool not_ready, bool no_execute) {
    const std::string key = "/registry/nodes/" + node_name;
    std::string json;
    if (!store_.get(key, json).ok()) return false;
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) return false;
    const std::string now = models::now_rfc3339();

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
    if (const models::JsonValue* old = spec->get("taints"); old && old->is_array()) {
        for (const auto& t : old->arr_val) {
            if (t.get_string("key", "") == kUnreachableTaint) continue;
            taints.arr_val.push_back(t);
        }
    }
    auto add_taint = [&](const char* effect) {
        models::JsonValue t;
        t.type = models::JsonType::Object;
        t.set("key", jstr(kUnreachableTaint));
        t.set("effect", jstr(effect));
        if (!now.empty()) t.set("timeAdded", jstr(now));
        taints.arr_val.push_back(std::move(t));
    };
    if (not_ready) add_taint("NoSchedule");
    if (no_execute) add_taint("NoExecute");
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

void NodeLifecycleController::evict_pods_from_node(const std::string& node_name) {
    std::vector<std::string> pod_jsons;
    std::vector<std::string> pod_keys;
    store_.list_extended("/registry/pods/", pod_jsons, &pod_keys);

    for (size_t i = 0; i < pod_jsons.size(); ++i) {
        osal::Thread::yield();
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(pod_jsons[i], doc, err) || !doc.is_object()) continue;

        const models::JsonValue* spec = doc.get("spec");
        std::string assigned_node = spec ? spec->get_string("nodeName", "") : "";
        if (assigned_node != node_name) continue;

        // DaemonSet pods stay with their node, as in Kubernetes
        const models::JsonValue* meta = doc.get("metadata");
        const models::JsonValue* labels = meta ? meta->get("labels") : nullptr;
        if (models::has_owner(doc, "DaemonSet") || (labels && labels->get_string("kube32.io/daemonset", "") == "true")) {
            continue;
        }
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
        const std::string now = models::now_rfc3339();
        if (!now.empty()) disrupt.set("lastTransitionTime", jstr(now));
        conds.arr_val.push_back(std::move(disrupt));
        mutable_status->set("conditions", std::move(conds));

        store_.put(pod_keys[i], doc.to_string(), true);

        api::EventSpec ev;
        ev.involved.kind = "Pod";
        ev.involved.name = meta ? meta->get_string("name", "") : "";
        ev.involved.namespace_name = meta ? meta->get_string("namespace", "default") : "default";
        ev.involved.uid = meta ? meta->get_string("uid", "") : "";
        ev.reason = "Evicted";
        ev.message = "Pod evicted from node " + node_name + ": node lease expired, pending rescheduling";
        ev.warning = true;
        ev.component = "node-controller";
        events_.record(ev);
    }
}

} // namespace kube32::controller
