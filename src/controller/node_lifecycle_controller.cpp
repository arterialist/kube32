#include "controller/node_lifecycle_controller.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"

#include <iostream>

namespace kube32::controller {

NodeLifecycleController::NodeLifecycleController(storage::Store& store, NodeLifecycleConfig config)
    : store_(store), config_(config), events_(store, 64) {}

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

void NodeLifecycleController::record_node_lease(const std::string& node_name, uint64_t uptime_ms) {
    bool recovered = false;
    {
        osal::LockGuard guard(lease_mutex_);
        if (uptime_ms == 0) {
            uptime_ms = osal::System::get_uptime_ms();
        }
        last_lease_update_ms_[node_name] = uptime_ms;
        recovered = not_ready_.erase(node_name) > 0;
    }
    if (recovered) {
        // The kubelet's next heartbeat patch restores the Ready condition; the
        // transition itself is what the event records.
        record_node_event(node_name, "NodeReady", "Node " + node_name + " status is now: NodeReady", false);
    }
}

uint64_t NodeLifecycleController::get_last_lease_time(const std::string& node_name) const {
    osal::LockGuard guard(lease_mutex_);
    auto it = last_lease_update_ms_.find(node_name);
    if (it != last_lease_update_ms_.end()) {
        return it->second;
    }
    return 0;
}

void NodeLifecycleController::reconcile_nodes(uint64_t mock_now) {
    uint64_t now = (mock_now > 0) ? mock_now : osal::System::get_uptime_ms();

    std::vector<std::string> node_jsons;
    std::vector<std::string> node_keys;
    store_.list_extended("/registry/nodes/", node_jsons, &node_keys);

    for (size_t i = 0; i < node_jsons.size(); ++i) {
        osal::Thread::yield();
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(node_jsons[i], doc, err) || !doc.is_object()) continue;

        const models::JsonValue* meta = doc.get("metadata");
        std::string node_name = meta ? meta->get_string("name", "") : "";
        if (node_name.empty()) continue;

        uint64_t last_update = get_last_lease_time(node_name);
        if (last_update == 0) {
            // First time seeing this node; check if it has a lease or initialize baseline
            std::string lease_json;
            if (store_.get("/registry/leases/kube-node-lease/" + node_name, lease_json).ok()) {
                record_node_lease(node_name, now);
                last_update = now;
            } else {
                record_node_lease(node_name, now);
                last_update = now;
            }
        }

        uint64_t delta_t = (now >= last_update) ? (now - last_update) : 0;

        if (delta_t >= config_.eviction_threshold_ms) {
            mark_node_unreachable_and_evict(node_name, node_jsons[i]);
        } else if (delta_t >= config_.not_ready_threshold_ms) {
            mark_node_not_ready(node_name, node_jsons[i]);
        }
    }
}

void NodeLifecycleController::mark_node_not_ready(const std::string& node_name, const std::string& current_node_json) {
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(current_node_json, doc, err) || !doc.is_object()) return;

    models::JsonValue* status = doc.get_mut("status");
    if (!status || !status->is_object()) return;

    models::JsonValue* conds = status->get_mut("conditions");
    if (!conds || !conds->is_array()) return;

    bool modified = false;
    for (auto& c : conds->arr_val) {
        if (c.is_object() && c.get_string("type", "") == "Ready") {
            if (c.get_string("status", "") != "False" || c.get_string("reason", "") != "NodeStatusUnknown") {
                c.set("status", models::JsonValue{models::JsonType::String, false, 0, "False", {}, {}});
                c.set("reason", models::JsonValue{models::JsonType::String, false, 0, "NodeStatusUnknown", {}, {}});
                c.set("message", models::JsonValue{models::JsonType::String, false, 0, "Kube32 node lease heartbeat expired", {}, {}});
                modified = true;
            }
            break;
        }
    }

    if (modified) {
        store_.put("/registry/nodes/" + node_name, doc.to_string(), true);
    }
    bool first = false;
    {
        osal::LockGuard guard(lease_mutex_);
        first = not_ready_.insert(node_name).second;
    }
    if (first) {
        record_node_event(node_name, "NodeNotReady", "Node " + node_name + " status is now: NodeNotReady (lease heartbeat expired)", true);
    }
}

void NodeLifecycleController::mark_node_unreachable_and_evict(const std::string& node_name, const std::string& current_node_json) {
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(current_node_json, doc, err) || !doc.is_object()) return;

    models::JsonValue* status = doc.get_mut("status");
    if (!status || !status->is_object()) return;

    models::JsonValue* conds = status->get_mut("conditions");
    if (!conds || !conds->is_array()) return;

    bool has_unreachable = false;
    for (auto& c : conds->arr_val) {
        if (c.is_object()) {
            if (c.get_string("type", "") == "Ready") {
                c.set("status", models::JsonValue{models::JsonType::String, false, 0, "False", {}, {}});
                c.set("reason", models::JsonValue{models::JsonType::String, false, 0, "NodeStatusUnknown", {}, {}});
            } else if (c.get_string("type", "") == "Unreachable") {
                has_unreachable = true;
                c.set("status", models::JsonValue{models::JsonType::String, false, 0, "True", {}, {}});
            }
        }
    }

    if (!has_unreachable) {
        models::JsonValue unreach;
        unreach.type = models::JsonType::Object;
        unreach.set("type", models::JsonValue{models::JsonType::String, false, 0, "Unreachable", {}, {}});
        unreach.set("status", models::JsonValue{models::JsonType::String, false, 0, "True", {}, {}});
        unreach.set("reason", models::JsonValue{models::JsonType::String, false, 0, "NodeLeaseExpired", {}, {}});
        unreach.set("message", models::JsonValue{models::JsonType::String, false, 0, "Node unreachable; lease expired", {}, {}});
        conds->arr_val.push_back(std::move(unreach));
    }

    store_.put("/registry/nodes/" + node_name, doc.to_string(), true);

    bool first = false;
    {
        osal::LockGuard guard(lease_mutex_);
        first = not_ready_.insert(node_name).second;
    }
    if (first) {
        record_node_event(node_name, "NodeNotReady", "Node " + node_name + " status is now: NodeNotReady (lease heartbeat expired)", true);
    }

    // Evict all non-daemon pods from dead node
    evict_pods_from_node(node_name);
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

        // Skip daemonset workloads
        const models::JsonValue* meta = doc.get("metadata");
        const models::JsonValue* labels = meta ? meta->get("labels") : nullptr;
        if (labels && labels->get_string("kube32.io/daemonset", "") == "true") {
            continue;
        }
        // A pod already being deleted has no kubelet left to finalize it
        if (meta && !meta->get_string("deletionTimestamp", "").empty()) {
            store_.remove(pod_keys[i]);
            continue;
        }

        // Mutate spec.nodeName = ""
        models::JsonValue* mutable_spec = doc.get_mut("spec");
        if (mutable_spec) {
            mutable_spec->set("nodeName", models::JsonValue{models::JsonType::String, false, 0, "", {}, {}});
        }

        // Mutate status: phase = Pending, reason = Evicted
        models::JsonValue* mutable_status = doc.get_mut("status");
        if (!mutable_status || !mutable_status->is_object()) {
            models::JsonValue new_status;
            new_status.type = models::JsonType::Object;
            doc.set("status", std::move(new_status));
            mutable_status = doc.get_mut("status");
        }
        mutable_status->set("phase", models::JsonValue{models::JsonType::String, false, 0, "Pending", {}, {}});
        mutable_status->set("reason", models::JsonValue{models::JsonType::String, false, 0, "Evicted", {}, {}});
        mutable_status->set("message", models::JsonValue{models::JsonType::String, false, 0, "Node " + node_name + " lease expired; evicted for rescheduling", {}, {}});

        // Append DisruptionTarget condition
        models::JsonValue* conds = mutable_status->get_mut("conditions");
        if (!conds || !conds->is_array()) {
            models::JsonValue new_conds;
            new_conds.type = models::JsonType::Array;
            mutable_status->set("conditions", std::move(new_conds));
            conds = mutable_status->get_mut("conditions");
        }
        models::JsonValue disrupt;
        disrupt.type = models::JsonType::Object;
        disrupt.set("type", models::JsonValue{models::JsonType::String, false, 0, "DisruptionTarget", {}, {}});
        disrupt.set("status", models::JsonValue{models::JsonType::String, false, 0, "True", {}, {}});
        disrupt.set("reason", models::JsonValue{models::JsonType::String, false, 0, "EvictionByNodeLifecycleController", {}, {}});
        conds->arr_val.push_back(std::move(disrupt));

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
