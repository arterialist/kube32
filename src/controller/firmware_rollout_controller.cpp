#include "controller/firmware_rollout_controller.hpp"
#include "models/firmware.hpp"
#include "models/kube_models.hpp"
#include "osal/sha256.hpp"
#include "osal/system.hpp"

#include <cstdio>

namespace kube32::controller {

namespace {

constexpr const char* kRolloutPrefix = "/registry/firmwarerollouts/";
constexpr const char* kNodePrefix = "/registry/nodes/";

models::JsonValue jstr(const std::string& v) { return models::JsonValue{models::JsonType::String, false, 0, v, {}, {}}; }
models::JsonValue jnum(int64_t v) { return models::JsonValue{models::JsonType::Number, false, static_cast<double>(v), "", {}, {}}; }

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string short_sha(const std::string& sha) { return sha.substr(0, 12); }

} // namespace

FirmwareRolloutController::FirmwareRolloutController(storage::Store& store, std::function<api::StagedFirmware()> staged,
                                                     FirmwareRolloutConfig config)
    : store_(store), staged_(std::move(staged)), config_(config), events_(store, 64) {}

std::vector<FirmwareRolloutController::NodeView> FirmwareRolloutController::load_nodes() {
    std::vector<std::string> jsons;
    store_.list(kNodePrefix, jsons);
    std::vector<NodeView> nodes;
    for (const auto& json : jsons) {
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(json, doc, err) || !doc.is_object()) continue;
        const models::JsonValue* meta = doc.get("metadata");
        if (!meta) continue;
        NodeView n;
        n.name = meta->get_string("name", "");
        n.uid = meta->get_string("uid", "");
        if (const models::JsonValue* labels = meta->get("labels")) n.control_plane = labels->get("node-role.kubernetes.io/control-plane") != nullptr;
        if (const models::JsonValue* a = meta->get("annotations")) {
            n.target = lower(a->get_string(models::kAnnotationFirmwareTarget, ""));
            n.running = a->get_string(models::kAnnotationFirmwareSha256, "");
            n.version = a->get_string(models::kAnnotationFirmwareVersion, "");
            n.state = a->get_string(models::kAnnotationFirmwareState, "");
            n.failed = a->get_string(models::kAnnotationFirmwareFailed, "");
        }
        const models::JsonValue* st = doc.get("status");
        const models::JsonValue* conds = st ? st->get("conditions") : nullptr;
        if (conds && conds->is_array()) {
            for (const auto& c : conds->arr_val) {
                if (c.get_string("type", "") == models::kConditionFirmwareUpdate) n.condition = c.get_string("message", "");
            }
        }
        if (!n.name.empty()) nodes.push_back(std::move(n));
    }
    // Workers first, then the control plane; by name within each. The store
    // lists nodes by name already, and a cluster has a handful of them.
    std::vector<NodeView> ordered;
    ordered.reserve(nodes.size());
    for (int control_plane = 0; control_plane < 2; ++control_plane) {
        for (auto& n : nodes) {
            if (n.control_plane == (control_plane == 1)) ordered.push_back(std::move(n));
        }
    }
    return ordered;
}

bool FirmwareRolloutController::set_node_target(const std::string& node, const std::string& sha256) {
    const std::string key = kNodePrefix + node;
    std::string json;
    uint64_t version = 0;
    if (!store_.get_extended(key, json, &version).ok()) return false;
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) return false;
    models::JsonValue* meta = doc.get_mut("metadata");
    if (!meta) return false;
    models::JsonValue* ann = meta->get_mut("annotations");
    if (!ann) {
        if (sha256.empty()) return true;
        models::JsonValue obj;
        obj.type = models::JsonType::Object;
        meta->set("annotations", std::move(obj));
        ann = meta->get_mut("annotations");
    }
    if (sha256.empty()) ann->remove(models::kAnnotationFirmwareTarget);
    else ann->set(models::kAnnotationFirmwareTarget, jstr(sha256));
    // Only over the version read: a heartbeat written in between is kept,
    // and the next pass tries again
    return store_.put_extended(key, doc.to_string(), true, true, version, nullptr).ok();
}

bool FirmwareRolloutController::write_status(const std::string& key, const RolloutStatus& st) {
    std::string json;
    uint64_t version = 0;
    if (!store_.get_extended(key, json, &version).ok()) return false;
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) return false;
    models::JsonValue status;
    status.type = models::JsonType::Object;
    status.set("phase", jstr(st.phase));
    if (!st.version.empty()) status.set("version", jstr(st.version));
    if (!st.node.empty()) status.set("node", jstr(st.node));
    status.set("updatedNodes", jnum(st.updated));
    status.set("totalNodes", jnum(st.total));
    status.set("message", jstr(st.message));
    const models::JsonValue* old = doc.get("status");
    if (old && old->to_string() == status.to_string()) return true;
    doc.set("status", std::move(status));
    return store_.put_extended(key, doc.to_string(), true, true, version, nullptr).ok();
}

void FirmwareRolloutController::node_event(const NodeView& node, const char* reason, const std::string& message, bool warning) {
    api::EventSpec ev;
    ev.involved.kind = "Node";
    ev.involved.name = node.name;
    ev.involved.uid = node.uid;
    ev.reason = reason;
    ev.message = message;
    ev.warning = warning;
    ev.component = "firmware-controller";
    events_.record(ev);
}

void FirmwareRolloutController::rollout_event(const std::string& name, const std::string& uid, const char* reason,
                                              const std::string& message, bool warning) {
    api::EventSpec ev;
    ev.involved.kind = "FirmwareRollout";
    ev.involved.api_version = "kube32.io/v1";
    ev.involved.name = name;
    ev.involved.uid = uid;
    ev.reason = reason;
    ev.message = message;
    ev.warning = warning;
    ev.component = "firmware-controller";
    events_.record(ev);
    printf("[FIRMWARE] %s: %s\n", name.c_str(), message.c_str());
}

void FirmwareRolloutController::reconcile(uint64_t now) {
    if (now == 0) now = osal::System::get_uptime_ms();
    std::vector<std::string> jsons, keys;
    store_.list_extended(kRolloutPrefix, jsons, &keys);

    if (jsons.empty()) {
        if (!target_.empty()) {
            // The rollout was deleted: nodes that have not switched stop
            for (const auto& n : load_nodes()) {
                if (n.target == target_ && n.running != target_) set_node_target(n.name, std::string());
            }
            target_.clear();
            uid_.clear();
            started_ms_.clear();
        }
        return;
    }

    // One rollout at a time: the first by name
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(jsons[0], doc, err) || !doc.is_object()) return;
    const models::JsonValue* meta = doc.get("metadata");
    const models::JsonValue* spec = doc.get("spec");
    const models::JsonValue* old = doc.get("status");
    const std::string name = meta ? meta->get_string("name", "") : "";
    const std::string uid = meta ? meta->get_string("uid", "") : "";
    const std::string sha = lower(spec ? spec->get_string("sha256", "") : "");
    const int64_t timeout_s = spec ? spec->get_int("nodeTimeoutSeconds", 0) : 0;
    const uint64_t timeout_ms = timeout_s > 0 ? static_cast<uint64_t>(timeout_s) * 1000 : config_.node_timeout_ms;
    RolloutStatus st;
    st.phase = old ? old->get_string("phase", "") : "";
    st.version = old ? old->get_string("version", "") : "";
    st.node = old ? old->get_string("node", "") : "";
    for (size_t i = 1; i < jsons.size(); ++i) {
        RolloutStatus other;
        other.phase = "Failed";
        other.message = "FirmwareRollout " + name + " is being acted on: one rollout runs at a time";
        write_status(keys[i], other);
    }
    if (st.phase == "Complete" || st.phase == "Failed") return;

    auto fail = [&](const std::string& message) {
        st.phase = "Failed";
        st.message = message;
        write_status(keys[0], st);
        rollout_event(name, uid, "FirmwareRolloutFailed", message, true);
    };
    if (!osal::Sha256::valid_hex(sha)) {
        fail("spec.sha256 is not a SHA-256");
        return;
    }

    std::vector<NodeView> nodes = load_nodes();
    if (uid != uid_) {
        // A new rollout: nodes still pointed at another image by an earlier
        // one are released
        for (const auto& n : nodes) {
            if (!n.target.empty() && n.target != sha && n.target != n.running) set_node_target(n.name, std::string());
        }
        uid_ = uid;
        target_ = sha;
        started_ms_.clear();
        if (st.phase.empty()) {
            rollout_event(name, uid, "FirmwareRolloutStarted", "rolling out image " + short_sha(sha) + " to " +
                                                                std::to_string(nodes.size()) + " node(s)", false);
        }
    }
    const api::StagedFirmware staged = staged_ ? staged_() : api::StagedFirmware{};
    if (staged.phase == "Staged" && staged.sha256 == sha && !staged.version.empty()) st.version = staged.version;

    st.total = static_cast<int64_t>(nodes.size());
    st.updated = 0;
    for (const auto& n : nodes) {
        if (n.running == sha && n.state == "Confirmed") {
            st.updated++;
            if (st.version.empty()) st.version = n.version;
        }
    }

    for (const auto& n : nodes) {
        if (n.running == sha && n.state == "Confirmed") continue;
        st.phase = "Progressing";
        auto timed_out = [&]() {
            auto it = started_ms_.find(n.name);
            if (it == started_ms_.end()) {
                started_ms_[n.name] = now;   // after a control plane restart: from now
                return false;
            }
            return now - it->second > timeout_ms;
        };
        if (n.failed == sha) {
            if (st.node == n.name) {
                const std::string why = "node " + n.name + ": " + (n.condition.empty() ? "image " + short_sha(sha) + " failed" : n.condition);
                node_event(n, "FirmwareUpdateFailed", why, true);
                st.node = n.name;
                fail(why);
                return;
            }
            // A failure left from an earlier rollout of this image: the node
            // forgets it once its target is gone, then it is tried again
            if (!n.target.empty()) set_node_target(n.name, std::string());
            st.message = "waiting for node " + n.name + " to clear an earlier failure of image " + short_sha(sha);
            write_status(keys[0], st);
            return;
        }
        if (n.running == sha) {
            // Booted into the image, not confirmed yet
            st.node = n.name;
            if (timed_out()) {
                fail("node " + n.name + " runs image " + short_sha(sha) + " but did not confirm it within " + std::to_string(timeout_ms / 1000) + " s");
                return;
            }
            st.message = "node " + n.name + " runs " + (n.version.empty() ? short_sha(sha) : n.version) + ", waiting for it to confirm itself";
            write_status(keys[0], st);
            return;
        }
        if (n.target != sha) {
            if (staged.phase != "Staged" || staged.sha256 != sha) {
                fail("image " + short_sha(sha) + " is not staged on the control plane (staged: " +
                     (staged.sha256.empty() ? std::string("none") : short_sha(staged.sha256)) + ")");
                return;
            }
            // The status names the node before the node is told: a failure
            // the node reports is then known to belong to this rollout
            st.node = n.name;
            st.message = "updating node " + n.name;
            if (!write_status(keys[0], st)) return;
            if (!set_node_target(n.name, sha)) return;   // tried again next pass
            started_ms_[n.name] = now;
            node_event(n, "FirmwareUpdateStarted", "updating to " + (st.version.empty() ? std::string("image") : st.version) +
                                                   " (" + short_sha(sha) + ")", false);
            return;
        }
        st.node = n.name;
        if (timed_out()) {
            fail("node " + n.name + " did not switch to image " + short_sha(sha) + " within " + std::to_string(timeout_ms / 1000) + " s" +
                 (n.condition.empty() ? std::string() : " (" + n.condition + ")"));
            return;
        }
        st.message = "updating node " + n.name + (n.condition.empty() ? std::string() : ": " + n.condition);
        write_status(keys[0], st);
        return;
    }

    st.phase = "Complete";
    st.node.clear();
    st.message = std::to_string(st.total) + " node(s) run " + (st.version.empty() ? short_sha(sha) : st.version);
    write_status(keys[0], st);
    rollout_event(name, uid, "FirmwareRolloutComplete", st.message, false);
}

} // namespace kube32::controller
