#include "controller/checkpoint_controller.hpp"
#include "kubelet/kube_api_client.hpp"
#include "models/kube_models.hpp"
#include "models/migration.hpp"
#include "osal/socket.hpp"
#include "osal/system.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <strings.h>

namespace kube32::controller {

namespace {

using models::JsonValue;
using models::str_cat;

constexpr const char* kPodPrefix = "/registry/pods/";

// Built out of line: a call at each site instead of a JsonValue made and
// torn down there
void set_str(JsonValue& o, const char* key, std::string_view v) { o.set(key, JsonValue{models::JsonType::String, false, 0, std::string(v), {}, {}}); }
JsonValue object() {
    JsonValue o;
    o.type = models::JsonType::Object;
    return o;
}
// The object at `key` of `o`, made an empty one when it is not an object
JsonValue& child(JsonValue& o, const char* key) {
    JsonValue* c = o.get_mut(key);
    if (!c || !c->is_object()) {
        o.set(key, object());
        c = o.get_mut(key);
    }
    return *c;
}

// String fields by reference: no copy at the call sites
const std::string& str_of(const JsonValue* v) {
    static const std::string empty;
    return v && v->is_string() ? v->str_val : empty;
}

const std::string& field(const JsonValue& o, const char* part, const char* key) {
    const JsonValue* p = o.get(part);
    return str_of(p ? p->get(key) : nullptr);
}

const std::string& annotation(const JsonValue& o, const char* key) {
    const JsonValue* meta = o.get("metadata");
    const JsonValue* ann = meta ? meta->get("annotations") : nullptr;
    return str_of(ann ? ann->get(key) : nullptr);
}

std::string seconds(uint64_t ms) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f s", static_cast<double>(ms) / 1000.0);
    return buf;
}

std::string now_text(uint64_t wall) { return wall ? models::format_rfc3339(wall) : std::string(); }

// Running with every container running, as the kubelet reports it
bool runs(const JsonValue& pod) {
    const JsonValue* st = pod.get("status");
    const JsonValue* css = st ? st->get("containerStatuses") : nullptr;
    if (!css || !css->is_array() || css->arr_val.empty() || st->get_string("phase", "") != "Running") return false;
    for (const auto& cs : css->arr_val) {
        const JsonValue* state = cs.get("state");
        if (!state || !state->get("running")) return false;
    }
    return true;
}

// Why a pod restored from a checkpoint does not run: a container's
// RestoreError, or the pod Failed. Empty while neither happened.
std::string restore_failure(const JsonValue& pod) {
    const JsonValue* st = pod.get("status");
    if (!st) return std::string();
    if (const JsonValue* css = st->get("containerStatuses"); css && css->is_array()) {
        for (const auto& cs : css->arr_val) {
            for (const char* which : {"state", "lastState"}) {
                const JsonValue* s = cs.get(which);
                for (const char* kind : {"waiting", "terminated"}) {
                    const JsonValue* x = s ? s->get(kind) : nullptr;
                    if (x && x->get_string("reason", "") == "RestoreError") return "RestoreError: " + x->get_string("message", "");
                }
            }
        }
    }
    if (st->get_string("phase", "") == "Failed") return str_cat({"pod Failed: ", st->get_string("reason", ""), " ", st->get_string("message", "")});
    return std::string();
}

// The value of header `name` in a response head, empty when absent
std::string header(std::string_view head, std::string_view name) {
    for (size_t line = head.find('\n'); line != std::string_view::npos; line = head.find('\n', line + 1)) {
        std::string_view rest = head.substr(line + 1);
        if (rest.size() <= name.size() || rest[name.size()] != ':' || strncasecmp(rest.data(), name.data(), name.size()) != 0) continue;
        rest.remove_prefix(name.size() + 1);
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        return std::string(rest.substr(0, rest.find_first_of("\r\n")));
    }
    return std::string();
}

// The "message" of a JSON answer, or the start of the text
std::string message_of(const std::string& text) {
    JsonValue doc;
    std::string err;
    if (models::parse_json(text, doc, err) && doc.is_object() && doc.get("message")) return doc.get_string("message", "");
    return text.substr(0, 200);
}

// A PUT whose body is written as it arrives: Content-Length when the length
// is known, chunked otherwise
class Upload {
public:
    bool begin(const std::string& host, uint16_t port, const std::string& token, const std::string& path, long length) {
        chunked_ = length < 0;
        const std::string head = str_cat({"PUT ", path, " HTTP/1.1\r\nHost: ", host, "\r\nContent-Type: application/octet-stream\r\n",
                                          chunked_ ? "Transfer-Encoding: chunked" : "Content-Length: ", chunked_ ? "" : std::to_string(length),
                                          "\r\n", token.empty() ? "" : "Authorization: Bearer ", token, token.empty() ? "" : "\r\n",
                                          "Connection: close\r\n\r\n"});
        ok_ = sock_.create(osal::SocketType::Tcp) && sock_.set_timeout(15000, 15000) && sock_.connect(host.c_str(), port) &&
              sock_.send_all(head.data(), head.size());
        return ok_;
    }
    bool write(const char* p, size_t n) {
        if (!ok_ || n == 0) return ok_;
        char line[20];
        const int l = snprintf(line, sizeof(line), "%zx\r\n", n);
        ok_ = (!chunked_ || sock_.send_all(line, static_cast<size_t>(l))) && sock_.send_all(p, n) && (!chunked_ || sock_.send_all("\r\n", 2));
        return ok_;
    }
    // The answer's status (0 without one), its body's start in `text`
    int finish(std::string& text) {
        if (chunked_ && ok_) sock_.send_all("0\r\n\r\n", 5);
        std::string in;
        char buf[256];
        for (ssize_t n; in.size() < 1024 && (n = sock_.recv(buf, sizeof(buf))) > 0;) in.append(buf, static_cast<size_t>(n));
        const size_t end = in.find("\r\n\r\n");
        if (in.rfind("HTTP/1.", 0) != 0 || end == std::string::npos) return 0;
        text = in.substr(end + 4);
        return std::atoi(in.c_str() + 9);
    }

private:
    osal::Socket sock_;
    bool chunked_{false};
    bool ok_{false};
};

} // namespace

struct CheckpointController::Node {
    std::string name, ip;
    uint16_t port{0};
    bool ready{false}, unschedulable{false}, pressure{false};
    double mops{0};
};

struct CheckpointController::Ckpt {
    std::string key, ns, name, uid, trigger;
    std::string action, pod, container, target;   // spec: action, podName, containerName, target.nodeName
    std::string phase, node, new_pod;             // status: phase, nodeName (where the pod ran), newPodName
    uint64_t version{0};
    JsonValue doc;
    bool active() const { return phase.empty() || phase == "Pending" || phase == "Saving" || phase == "Moving"; }
};

// The pods of a pass, each parsed when first asked for
struct CheckpointController::Pods {
    std::vector<std::string> keys, jsons;
    std::vector<JsonValue> parsed;
    std::vector<std::string> touched;   // written in this pass: what was read of them is stale
    const JsonValue* at(size_t i) {
        if (parsed.size() != jsons.size()) parsed.resize(jsons.size());
        JsonValue& d = parsed[i];
        if (d.is_null()) {
            std::string err;
            if (!models::parse_json(jsons[i], d, err) || !d.is_object()) d.type = models::JsonType::Boolean;
        }
        return d.is_object() ? &d : nullptr;
    }
    // The pod at `key`, null when it is gone or was written in this pass
    const JsonValue* get(const std::string& key) {
        if (std::find(touched.begin(), touched.end(), key) != touched.end()) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) return at(i);
        }
        return nullptr;
    }
};

struct CheckpointController::Snapshot {
    bool ok{false};
    bool maybe_frozen{false};   // the node may hold the pod frozen still
    uint64_t bytes{0};
    std::string image_sha256, snapshot_sha256, pod_time_ms, error;
    std::vector<std::string> blockers;
};

CheckpointController::CheckpointController(storage::Store& store, scheduler::Scheduler* scheduler, CheckpointControllerConfig config)
    : store_(store), scheduler_(scheduler), config_(std::move(config)), events_(store, 64),
      trigger_(store, ReconcileTriggerConfig{config_.reconcile_interval_ms, config_.resync_interval_ms}) {
    trigger_.watch(models::kCheckpointPrefix);
    trigger_.watch(kPodPrefix, [](const storage::WatchEvent& ev) {
        const std::string& j = ev.json;
        return j.find(models::kAnnotationRestoreFrom) != std::string::npos || j.find(models::kAnnotationMigration) != std::string::npos ||
               j.find(models::kAnnotationCheckpointInterval) != std::string::npos;
    });
    // What load_nodes reads of a Node
    trigger_.watch_fields("/registry/nodes/", [](const JsonValue& node) {
        std::string seen = annotation(node, models::kAnnotationInterpMops);
        if (const JsonValue* spec = node.get("spec"); spec && spec->get_bool("unschedulable", false)) seen += 'u';
        const JsonValue* st = node.get("status");
        if (const JsonValue* cs = st ? st->get("conditions") : nullptr; cs && cs->is_array()) {
            for (const auto& c : cs->arr_val) seen += str_cat({c.get_string("type", ""), c.get_string("status", "")});
        }
        return ReconcileTrigger::hash({seen});
    });
}

CheckpointController::~CheckpointController() {
    stop();
}

bool CheckpointController::start() {
    if (running_.load()) return true;
    running_.store(true);
    thread_ = std::make_unique<osal::Thread>();
    osal::Thread::Config tc;
    tc.name = "k32_ckpt";
    tc.stack_size = config_.stack_bytes;
    tc.priority = 4;
    tc.core_affinity = config_.core_affinity;
    tc.stack_in_psram = config_.stack_in_psram;
    const bool ok = thread_->start([](void* arg) {
        auto* self = static_cast<CheckpointController*>(arg);
        while (self->running_.load()) {
            try {
                if (self->trigger_.due(osal::System::get_uptime_ms())) self->reconcile();
            } catch (const std::exception& e) {
                printf("[CKPT] pass skipped: %s\n", e.what());
            }
            osal::System::feed_watchdog();
            self->trigger_.wait(osal::System::get_uptime_ms(), self->config_.resync_interval_ms);
        }
    }, this, tc);
    if (!ok) running_.store(false);
    return ok;
}

void CheckpointController::stop() {
    if (!running_.load()) return;
    running_.store(false);
    trigger_.interrupt();
    if (thread_ && thread_->is_joinable()) thread_->join();
    thread_.reset();
}

uint64_t CheckpointController::wall_ms() const {
    return config_.unix_time_ms ? config_.unix_time_ms() : osal::System::get_unix_time_ms();
}

std::vector<CheckpointController::Node> CheckpointController::load_nodes() {
    std::vector<std::string> jsons;
    store_.list("/registry/nodes/", jsons);
    std::vector<Node> nodes;
    for (const auto& json : jsons) {
        JsonValue doc;
        std::string err;
        if (!models::parse_json(json, doc, err) || !doc.is_object()) continue;
        Node n;
        n.name = field(doc, "metadata", "name");
        n.port = models::node_pod_port(doc);
        n.mops = std::strtod(annotation(doc, models::kAnnotationInterpMops).c_str(), nullptr);
        if (const JsonValue* spec = doc.get("spec")) n.unschedulable = spec->get_bool("unschedulable", false);
        const JsonValue* st = doc.get("status");
        for (const char* list : {"addresses", "conditions"}) {
            const JsonValue* items = st ? st->get(list) : nullptr;
            for (size_t i = 0; items && items->is_array() && i < items->arr_val.size(); ++i) {
                const JsonValue& c = items->arr_val[i];
                const std::string type = c.get_string("type", "");
                const bool on = c.get_string("status", "") == "True";
                if (type == "InternalIP") n.ip = c.get_string("address", "");
                if (type == "Ready") n.ready = on;
                if (type == "MemoryPressure") n.pressure = on;
            }
        }
        nodes.push_back(std::move(n));
    }
    return nodes;
}

const CheckpointController::Node* CheckpointController::node(const std::vector<Node>& nodes, const std::string& name) {
    for (const auto& n : nodes) {
        if (n.name == name) return &n;
    }
    return nullptr;
}

void CheckpointController::reconcile(uint64_t now) {
    if (now == 0) now = osal::System::get_uptime_ms();
    passes_.fetch_add(1);
    const std::vector<Node> nodes = load_nodes();
    Pods pods;
    store_.list_extended(kPodPrefix, pods.jsons, &pods.keys);

    std::vector<Ckpt> cks;
    {
        std::vector<std::string> jsons, keys;
        store_.list_extended(models::kCheckpointPrefix, jsons, &keys);
        for (size_t i = 0; i < jsons.size(); ++i) {
            Ckpt c;
            std::string err;
            if (!models::parse_json(jsons[i], c.doc, err) || !c.doc.is_object() || !c.doc.get("metadata") || !c.doc.get("spec")) continue;
            const JsonValue& meta = *c.doc.get("metadata");
            const JsonValue& spec = *c.doc.get("spec");
            c.key = keys[i];
            c.ns = meta.get_string("namespace", "default");
            c.name = meta.get_string("name", "");
            c.uid = meta.get_string("uid", "");
            c.version = std::strtoull(meta.get_string("resourceVersion", "0").c_str(), nullptr, 10);
            c.trigger = field(meta, "labels", models::kLabelTrigger);
            c.action = spec.get_string("action", "Save");
            c.pod = spec.get_string("podName", "");
            c.container = spec.get_string("containerName", "");
            c.target = field(spec, "target", "nodeName");
            c.phase = field(c.doc, "status", "phase");
            c.node = field(c.doc, "status", "nodeName");
            c.new_pod = field(c.doc, "status", "newPodName");
            cks.push_back(std::move(c));
        }
    }

    for (auto& ck : cks) {
        if (ck.phase == "Moving") watch_move(ck, pods, nodes, now);
        // Only an earlier run leaves one: a pass ends each snapshot it starts
        if (ck.phase != "Saving") continue;
        const JsonValue* pod = pods.get(kPodPrefix + ck.ns + "/" + ck.pod);
        if (ck.action == "Move" && pod) thaw(node(nodes, ck.node), ck.ns, ck.pod, container_of(ck, *pod));
        fail(ck, "interrupted: the control plane restarted while the snapshot was taken");
    }
    finish_restores(pods, cks);
    policies(pods, nodes, cks, now);
    retention(pods, cks, now);

    // One snapshot per pass: the oldest asked for, those of users and
    // evictions before periodic ones. Its writes make the next pass due.
    Ckpt* next = nullptr;
    for (auto& ck : cks) {
        if (!(ck.phase.empty() || ck.phase == "Pending") || busy(cks, ck.ns, ck.pod, true)) continue;
        const bool periodic = ck.trigger == "periodic", next_periodic = next && next->trigger == "periodic";
        if (!next || (next_periodic && !periodic) || (next_periodic == periodic && ck.version < next->version)) next = &ck;
    }
    if (next) begin(*next, pods, nodes, now);
}

bool CheckpointController::busy(const std::vector<Ckpt>& cks, const std::string& ns, const std::string& pod, bool started_only) {
    for (const auto& ck : cks) {
        if (ck.ns == ns && ck.pod == pod && (started_only ? ck.phase == "Saving" || ck.phase == "Moving" : ck.active())) return true;
    }
    return false;
}

std::string CheckpointController::container_of(const Ckpt& ck, const JsonValue& pod) {
    if (!ck.container.empty()) return ck.container;
    const JsonValue* spec = pod.get("spec");
    const JsonValue* cs = spec ? spec->get("containers") : nullptr;
    return cs && cs->is_array() && cs->arr_val.size() == 1 ? cs->arr_val[0].get_string("name", "") : std::string();
}

void CheckpointController::fail(Ckpt& ck, const std::string& message, const std::vector<std::string>& blockers) {
    update_status(ck.key, {{"phase", "Failed"}, {"message", message}, {"finishedAt", now_text(wall_ms())}}, {}, &blockers);
    ck.phase = "Failed";
    printf("[CKPT] %s/%s failed: %s\n", ck.ns.c_str(), ck.name.c_str(), message.c_str());
}

void CheckpointController::give_up(Ckpt& ck, const std::string& message, const std::vector<std::string>& blockers) {
    fail(ck, message, blockers);
    if (ck.trigger != "eviction") return;
    // A move the eviction API asked for that cannot happen is an eviction:
    // deleted as DELETE does it, graceful when a kubelet runs the pod
    const std::string key = kPodPrefix + ck.ns + "/" + ck.pod;
    std::string json, err;
    JsonValue doc;
    if (!store_.get(key, json).ok() || !models::parse_json(json, doc, err) || !doc.is_object()) return;
    JsonValue& meta = child(doc, "metadata");
    const std::string now = models::now_rfc3339();
    if (!meta.get_string("deletionTimestamp", "").empty()) return;
    if (field(doc, "spec", "nodeName").empty() || now.empty()) {
        store_.remove(key);
        return;
    }
    set_str(meta, "deletionTimestamp", now);
    meta.set("deletionGracePeriodSeconds", JsonValue{models::JsonType::Number, false, 30, "", {}, {}});
    store_.put(key, doc.to_string(), true);
}

void CheckpointController::begin(Ckpt& ck, Pods& pods, const std::vector<Node>& nodes, uint64_t now) {
    const std::string pod_key = kPodPrefix + ck.ns + "/" + ck.pod;
    const bool move = ck.action == "Move";
    if (ck.action != "Save" && !move) return give_up(ck, "spec.action must be Save or Move");
    const JsonValue* pod = pods.get(pod_key);
    if (!pod) return give_up(ck, str_cat({"pod ", ck.pod, " not found"}));
    const std::string& source = field(*pod, "spec", "nodeName");
    const std::string& phase = field(*pod, "status", "phase");
    const std::string uid = field(*pod, "metadata", "uid");
    if (source.empty() || phase != "Running" || !field(*pod, "metadata", "deletionTimestamp").empty()) {
        return give_up(ck, str_cat({"pod ", ck.pod, " is not running (phase ", phase.empty() ? "unknown" : phase, ")"}));
    }
    const std::string container = container_of(ck, *pod);
    if (container.empty()) return give_up(ck, str_cat({"pod ", ck.pod, " has several containers: set spec.containerName"}));
    const std::vector<std::string> blockers = models::migration_blockers(*pod, move);
    if (!blockers.empty()) return give_up(ck, move ? "the pod cannot move" : "the pod cannot be checkpointed", blockers);
    std::string target, why;
    if (move && (target = pick_target(ck, *pod, nodes, why)).empty()) return give_up(ck, "no node fits: " + why);
    const Node* src = node(nodes, source);
    if (!src || !src->port || src->ip.empty()) return give_up(ck, str_cat({"node ", source, " reports no kubelet endpoint"}));
    if (!update_status(ck.key, {{"phase", "Saving"}, {"nodeName", source}, {"targetNode", target}})) return;   // deleted meanwhile
    ck.phase = "Saving";

    const uint64_t t0 = osal::System::get_uptime_ms();
    const Snapshot s = take_snapshot(*src, ck, container, move);
    const std::string took = seconds(osal::System::get_uptime_ms() - t0);
    if (!s.ok) {
        if (s.maybe_frozen) thaw(src, ck.ns, ck.pod, container);
        std::string text = str_cat({"checkpoint ", ck.name, ": ", s.error});
        for (const auto& b : s.blockers) text += str_cat({"; ", b});
        pod_event(ck.ns, ck.pod, uid, move ? "MoveFailed" : "CheckpointFailed", text, true);
        return give_up(ck, s.error, s.blockers);
    }
    const std::string taken = now_text(wall_ms()), size = std::to_string(s.bytes);
    const std::initializer_list<std::pair<const char*, double>> nums = {
        {"sizeBytes", static_cast<double>(s.bytes)}, {"podTimeMs", static_cast<double>(std::strtoull(s.pod_time_ms.c_str(), nullptr, 10))}};
    if (!move) {
        update_status(ck.key, {{"phase", "Saved"}, {"imageSha256", s.image_sha256}, {"snapshotSha256", s.snapshot_sha256}, {"takenAt", taken},
                               {"finishedAt", taken}}, nums);
        ck.phase = "Saved";
        pod_event(ck.ns, ck.pod, uid, "Checkpointed", str_cat({"Saved checkpoint ", ck.name, " on node ", source, ": ", size, " bytes in ", took}), false);
        return;
    }
    std::string new_uid;
    if (!rebind(pod_key, target, ck.name, uid, &new_uid)) {
        thaw(src, ck.ns, ck.pod, container);
        const std::string message = "the pod changed while it was frozen: it stays on node " + source;
        pod_event(ck.ns, ck.pod, uid, "MoveFailed", str_cat({"checkpoint ", ck.name, ": ", message}), true);
        return give_up(ck, message);
    }
    pods.touched.push_back(pod_key);
    moving_since_[ck.uid] = now;
    update_status(ck.key, {{"phase", "Moving"}, {"imageSha256", s.image_sha256}, {"snapshotSha256", s.snapshot_sha256}, {"takenAt", taken},
                           {"newPodName", ck.pod}, {"message", "restoring on node " + target}}, nums);
    ck.phase = "Moving";
    pod_event(ck.ns, ck.pod, new_uid, "Moving",
              str_cat({"Moving from node ", source, " to node ", target, " with checkpoint ", ck.name, " (", size, " bytes, taken in ", took, ")"}), false);
}

void CheckpointController::watch_move(Ckpt& ck, Pods& pods, const std::vector<Node>& nodes, uint64_t now) {
    const uint64_t since = moving_since_.emplace(ck.uid, now).first->second;
    const std::string key = kPodPrefix + ck.ns + "/" + (ck.new_pod.empty() ? ck.pod : ck.new_pod);
    const JsonValue* pod = pods.get(key);
    pods.touched.push_back(key);
    if (!pod || annotation(*pod, models::kAnnotationRestoreFrom) != ck.name) {
        return fail(ck, pod ? "the pod was changed during the move" : "the pod was deleted during the move");
    }
    const std::string& at = field(*pod, "spec", "nodeName");
    const std::string& uid = field(*pod, "metadata", "uid");
    if (runs(*pod)) {
        strip_restore(key);
        const std::string message = str_cat({"moved from node ", ck.node, " to node ", at, " in ", seconds(now - since)});
        update_status(ck.key, {{"phase", "Completed"}, {"targetNode", at}, {"message", message}, {"finishedAt", now_text(wall_ms())}});
        ck.phase = "Completed";
        moving_since_.erase(ck.uid);
        pod_event(ck.ns, ck.pod, uid, "Moved", str_cat({"Checkpoint ", ck.name, ": ", message}), false);
        printf("[CKPT] %s/%s: %s\n", ck.ns.c_str(), ck.pod.c_str(), message.c_str());
        return;
    }
    std::string why = restore_failure(*pod);
    if (why.empty() && now < since + config_.move_deadline_ms) return trigger_.wake_at(since + config_.move_deadline_ms);
    if (why.empty()) why = str_cat({"not running on node ", at, " after ", seconds(config_.move_deadline_ms)});
    // Back to the node it came from, from the same checkpoint
    const Node* src = node(nodes, ck.node);
    const std::string back = src && src->ready ? ck.node : std::string();
    std::string new_uid;
    const bool rebound = rebind(key, back, ck.name, uid, &new_uid);
    const std::string message = str_cat({"restore on node ", at, " failed (", why, "); ",
                                         !rebound ? "the pod could not be put back"
                                                  : back.empty() ? "the pod waits for a node to restore it on"
                                                                 : str_cat({"restoring it on node ", back, " from the same checkpoint"})});
    fail(ck, message);
    moving_since_.erase(ck.uid);
    pod_event(ck.ns, ck.pod, rebound ? new_uid : uid, "MoveFailed", str_cat({"Checkpoint ", ck.name, ": ", message}), true);
}

void CheckpointController::finish_restores(Pods& pods, const std::vector<Ckpt>& cks) {
    const uint64_t wall = wall_ms();
    for (size_t i = 0; i < pods.jsons.size(); ++i) {
        if (pods.jsons[i].find(models::kAnnotationRestoreFrom) == std::string::npos) continue;
        const JsonValue* pod = pods.get(pods.keys[i]);
        if (!pod || annotation(*pod, models::kAnnotationRestoreFrom).empty()) continue;
        const std::string& from = annotation(*pod, models::kAnnotationRestoreFrom);
        const std::string& ns = field(*pod, "metadata", "namespace");
        const std::string& name = field(*pod, "metadata", "name");
        const std::string& uid = field(*pod, "metadata", "uid");
        const Ckpt* ck = nullptr;
        for (const auto& c : cks) {
            if (c.ns == ns && c.name == from) ck = &c;
        }
        if (ck && ck->phase == "Moving") continue;   // watch_move follows it
        if (runs(*pod)) {
            if (!strip_restore(pods.keys[i]) || (ck && ck->phase == "Completed")) continue;
            // The checkpoint's age: from its takenAt, else from when this run saw it finished
            std::string age;
            uint64_t at = 0;
            auto seen = ck ? done_seen_.find(ck->uid) : done_seen_.end();
            if (ck && wall && models::parse_rfc3339(field(ck->doc, "status", "takenAt"), at) && wall >= at) {
                age = str_cat({", taken ", seconds(wall - at), " ago"});
            } else if (seen != done_seen_.end()) {
                age = str_cat({", taken ", seconds(osal::System::get_uptime_ms() - seen->second), " ago"});
            }
            pod_event(ns, name, uid, "Restored", str_cat({"Restored from checkpoint ", from, age, " on node ", field(*pod, "spec", "nodeName")}), false);
        } else if (const std::string why = restore_failure(*pod); !why.empty() && strip_restore(pods.keys[i])) {
            pod_event(ns, name, uid, "MoveFailed", str_cat({"Restore from checkpoint ", from, " failed (", why, "): the container starts afresh"}), true);
        }
    }
}

std::string CheckpointController::pick_target(const Ckpt& ck, const JsonValue& pod, const std::vector<Node>& nodes, std::string& why) {
    const std::string& source = field(pod, "spec", "nodeName");
    if (!scheduler_) return std::string();
    JsonValue copy = pod;
    JsonValue& spec = child(copy, "spec");
    // spec.target.nodeSelector on top of the pod's own
    const JsonValue* target = ck.doc.get("spec")->get("target");
    const JsonValue* sel = target ? target->get("nodeSelector") : nullptr;
    for (size_t i = 0; sel && sel->is_object() && i < sel->obj_val.size(); ++i) child(spec, "nodeSelector").set(sel->obj_val[i].first, sel->obj_val[i].second);
    if (ck.target == source && !source.empty()) {
        why = "the target is the node the pod runs on";
        return std::string();
    }
    std::vector<std::string> tries;
    if (!ck.target.empty()) {
        tries.push_back(ck.target);
    } else {
        // kube32.io/placement: fastest goes to the fastest node that takes it
        if (models::migration_policy(pod) == models::MigrationPolicy::Auto && annotation(pod, models::kAnnotationPlacement) == "fastest") {
            std::vector<const Node*> by_speed;
            for (const auto& n : nodes) {
                if (n.mops > 0) by_speed.push_back(&n);
            }
            std::sort(by_speed.begin(), by_speed.end(), [](const Node* a, const Node* b) { return a->mops > b->mops; });
            for (const Node* n : by_speed) tries.push_back(n->name);
        }
        tries.emplace_back();   // the scheduler's choice
    }
    for (const auto& t : tries) {
        set_str(spec, "nodeName", t);
        const scheduler::ScheduleResult r = scheduler_->pick_node(copy.to_string(), source);
        if (r.scheduled) return r.target_node;
        why = r.failure_reason;
    }
    return std::string();
}

void CheckpointController::policies(Pods& pods, const std::vector<Node>& nodes, const std::vector<Ckpt>& cks, uint64_t now) {
    struct Pick {
        std::string node, ns, name;
        uint64_t bytes{0};
    };
    std::vector<Pick> pressure;   // per node, the pod that frees the most
    for (size_t i = 0; i < pods.jsons.size(); ++i) {
        const std::string& j = pods.jsons[i];
        const bool periodic = j.find(models::kAnnotationCheckpointInterval) != std::string::npos;
        if (!periodic && j.find("\"Auto\"") == std::string::npos) continue;
        const JsonValue* pod = pods.get(pods.keys[i]);
        if (!pod || !runs(*pod) || !field(*pod, "metadata", "deletionTimestamp").empty()) continue;
        const std::string& ns = field(*pod, "metadata", "namespace");
        const std::string& name = field(*pod, "metadata", "name");
        const std::string& at = field(*pod, "spec", "nodeName");
        const std::string key = str_cat({ns, "/", name});
        const models::MigrationPolicy policy = models::migration_policy(*pod);
        if (busy(cks, ns, name, false) || policy == models::MigrationPolicy::Never || !models::migration_blockers(*pod, false).empty()) continue;
        const uint64_t interval = models::parse_duration_ms(annotation(*pod, models::kAnnotationCheckpointInterval));
        if (const uint64_t every = std::max<uint64_t>(interval, config_.min_interval_ms); interval) {
            auto last = last_periodic_.find(key);
            if (last == last_periodic_.end() || now >= last->second + every) {
                create(ns, name, "Save", "", "periodic");
                last_periodic_[key] = now;
            }
            trigger_.wake_at(last_periodic_[key] + every);
        }
        if (policy != models::MigrationPolicy::Auto) continue;
        auto moved = auto_moved_.find(key);
        if (moved != auto_moved_.end() && now < moved->second + config_.auto_cooldown_ms) {
            trigger_.wake_at(moved->second + config_.auto_cooldown_ms);
            continue;
        }
        const Node* here = node(nodes, at);
        if (!here) continue;
        // A node at least twice as fast that takes the pod
        if (annotation(*pod, models::kAnnotationPlacement) == "fastest" && here->mops > 0 && scheduler_) {
            const Node* best = nullptr;
            JsonValue copy = *pod;
            for (const auto& n : nodes) {
                if (!n.ready || n.unschedulable || n.mops < 2 * here->mops || (best && best->mops >= n.mops)) continue;
                set_str(child(copy, "spec"), "nodeName", n.name);
                if (scheduler_->pick_node(copy.to_string(), at).scheduled) best = &n;
            }
            if (best) {
                create(ns, name, "Move", best->name, "placement");
                auto_moved_[key] = now;
                continue;
            }
        }
        if (here->pressure) {
            const scheduler::ResourceSpec req = scheduler::extract_pod_resource_requests(j);
            auto p = std::find_if(pressure.begin(), pressure.end(), [&](const Pick& x) { return x.node == at; });
            if (p == pressure.end()) p = pressure.insert(pressure.end(), Pick{at, "", "", 0});
            if (p->name.empty() || req.memory_bytes + req.psram_bytes > p->bytes) *p = Pick{at, ns, name, req.memory_bytes + req.psram_bytes};
        }
    }
    for (const auto& p : pressure) {
        auto last = pressure_moved_.find(p.node);
        if (last != pressure_moved_.end() && now < last->second + config_.pressure_cooldown_ms) {
            trigger_.wake_at(last->second + config_.pressure_cooldown_ms);
            continue;
        }
        create(p.ns, p.name, "Move", "", "memory-pressure");
        pressure_moved_[p.node] = now;
        auto_moved_[p.ns + "/" + p.name] = now;
    }
}

void CheckpointController::retention(Pods& pods, const std::vector<Ckpt>& cks, uint64_t now) {
    // Checkpoints a pod is to be restored from stay
    std::vector<std::string> wanted;
    for (size_t i = 0; i < pods.jsons.size(); ++i) {
        if (pods.jsons[i].find(models::kAnnotationRestoreFrom) == std::string::npos) continue;
        if (const JsonValue* pod = pods.at(i)) wanted.push_back(field(*pod, "metadata", "namespace") + "/" + annotation(*pod, models::kAnnotationRestoreFrom));
    }
    const uint64_t wall = wall_ms();
    std::map<std::string, uint64_t> live;
    for (const auto& ck : cks) {
        live[ck.uid] = 0;
        if (ck.active() || std::find(wanted.begin(), wanted.end(), ck.ns + "/" + ck.name) != wanted.end()) continue;
        const uint64_t seen = done_seen_.emplace(ck.uid, now).first->second;
        bool drop = false;
        if (ck.trigger == "periodic") {
            // Only the latest Saved one of a pod that exists, and a Failed one newer than it
            drop = std::find(pods.keys.begin(), pods.keys.end(), str_cat({kPodPrefix, ck.ns, "/", ck.pod})) == pods.keys.end();
            for (const auto& o : cks) {
                const bool newer = o.trigger == "periodic" && o.ns == ck.ns && o.pod == ck.pod && o.version > ck.version;
                drop |= newer && (o.phase == "Saved" || (o.phase == "Failed" && ck.phase == "Failed"));
            }
        } else {
            uint64_t age = now - seen, at = 0;
            if (wall && models::parse_rfc3339(field(ck.doc, "status", "finishedAt"), at) && wall > at) age = std::max(age, wall - at);
            drop = age >= config_.keep_ms;
            if (!drop) trigger_.wake_at(now + (config_.keep_ms - age));
        }
        if (drop) store_.remove(ck.key);
    }
    for (auto* m : {&done_seen_, &moving_since_}) {
        for (auto it = m->begin(); it != m->end();) it = live.count(it->first) ? std::next(it) : m->erase(it);
    }
    for (auto* m : {&last_periodic_, &auto_moved_}) {
        for (auto it = m->begin(); it != m->end();) it = std::find(pods.keys.begin(), pods.keys.end(), kPodPrefix + it->first) != pods.keys.end() ? std::next(it) : m->erase(it);
    }
}

CheckpointController::Snapshot CheckpointController::take_snapshot(const Node& node, const Ckpt& ck, const std::string& container, bool frozen) {
    Snapshot s;
    kubelet::HttpKubeClient kubelet(node.ip, node.port);
    kubelet.set_bearer_token(config_.token);
    kubelet.set_timeout_ms(config_.freeze_timeout_ms + 20000);
    const std::string path = str_cat({"/checkpoint/", ck.ns, "/", ck.pod, "/", container, "?leave=", frozen ? "frozen" : "running", "&timeoutMs=",
                                      std::to_string(config_.freeze_timeout_ms)});
    Upload up;
    int code = 0;
    long length = -1;
    std::string text;   // the body of a refusal
    const int status = kubelet.request("POST", path, std::string_view(),
        [&](int st, std::string_view head) {
            code = st;
            if (st != 200) return true;
            s.image_sha256 = header(head, "X-Kube32-Image-Sha256");
            s.snapshot_sha256 = header(head, "X-Kube32-Snapshot-Sha256");
            s.pod_time_ms = header(head, "X-Kube32-Pod-Time-Ms");
            const std::string cl = header(head, "Content-Length");
            if (!cl.empty()) length = std::strtol(cl.c_str(), nullptr, 10);
            if (up.begin(config_.api_host, config_.api_port, config_.token,
                         str_cat({"/apis/kube32.io/v1/namespaces/", ck.ns, "/checkpoints/", ck.name, "/data"}), length)) {
                return true;
            }
            s.error = "the API server cannot be reached to store the checkpoint";
            return false;
        },
        [&](const char* p, size_t n) {
            if (code == 200) {
                s.bytes += n;
                return up.write(p, n);
            }
            text.append(p, std::min(n, 1024 - std::min<size_t>(text.size(), 1024)));
            return true;
        });
    s.maybe_frozen = frozen && (status == 200 || status == 0);
    if (status == 200) {
        std::string answer;
        const int put = s.error.empty() ? up.finish(answer) : 0;
        if (!s.error.empty()) return s;
        if (length >= 0 && s.bytes != static_cast<uint64_t>(length)) {
            s.error = str_cat({"the snapshot ended after ", std::to_string(s.bytes), " of ", std::to_string(length), " bytes"});
        } else if (put < 200 || put >= 300) {
            s.error = str_cat({"the API server did not store the checkpoint data (", put ? str_cat({"HTTP ", std::to_string(put), ": ", message_of(answer)}) : "no answer", ")"});
        }
        s.ok = s.error.empty();
        return s;
    }
    JsonValue body;
    std::string err;
    models::parse_json(text, body, err);
    const std::string message = body.is_object() ? body.get_string("message", "") : std::string();
    const JsonValue* b = body.is_object() ? body.get("blockers") : nullptr;
    for (size_t i = 0; b && b->is_array() && i < b->arr_val.size(); ++i) s.blockers.push_back(b->arr_val[i].str_val);
    s.error = status == 409   ? std::string("the pod cannot move")
              : status == 404 ? str_cat({"node ", node.name, " does not run container ", container, " of pod ", ck.pod})
              : status == 0   ? "no answer from the kubelet of node " + node.name
                              : str_cat({"the kubelet of node ", node.name, " answered HTTP ", std::to_string(status), message.empty() ? "" : ": ", message});
    return s;
}

bool CheckpointController::thaw(const Node* node, const std::string& ns, const std::string& pod, const std::string& container) {
    if (!node || !node->port || node->ip.empty() || container.empty()) return false;
    kubelet::HttpKubeClient kubelet(node->ip, node->port);
    kubelet.set_bearer_token(config_.token);
    auto head = [](int, std::string_view) { return true; };
    auto data = [](const char*, size_t) { return true; };
    return kubelet.request("POST", str_cat({"/thaw/", ns, "/", pod, "/", container}), std::string_view(), head, data) == 200;
}

// Reads the object at `key`, lets `change` edit it (false: leave it) and
// writes it over the version read, three times at most on a conflict
bool CheckpointController::edit(const std::string& key, mem::FunctionRef<bool(JsonValue&)> change) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string json, err;
        uint64_t version = 0;
        JsonValue doc;
        if (!store_.get_extended(key, json, &version).ok() || !models::parse_json(json, doc, err) || !doc.is_object() || !change(doc)) return false;
        const storage::Status ps = store_.put_extended(key, doc.to_string(), true, true, version, nullptr);
        if (ps.ok()) return true;
        if (ps.code != storage::StatusCode::VersionConflict) return false;
    }
    return false;
}

bool CheckpointController::rebind(const std::string& key, const std::string& node, const std::string& restore_from, const std::string& uid,
                                  std::string* new_uid) {
    return edit(key, [&](JsonValue& doc) {
        JsonValue& meta = child(doc, "metadata");
        if (meta.get_string("uid", "") != uid || !meta.get_string("deletionTimestamp", "").empty()) return false;
        *new_uid = models::generate_uid();
        set_str(meta, "uid", *new_uid);
        const std::string now = models::now_rfc3339();
        if (!now.empty()) set_str(meta, "creationTimestamp", now);
        set_str(child(meta, "annotations"), models::kAnnotationRestoreFrom, restore_from);
        set_str(child(doc, "spec"), "nodeName", node);
        // Pending, as the scheduler leaves a pod it bound
        std::string err;
        JsonValue st;
        models::parse_json(node.empty() ? R"({"phase":"Pending"})" : R"({"phase":"Pending","conditions":[{"type":"PodScheduled","status":"True"}]})", st, err);
        doc.set("status", std::move(st));
        return true;
    });
}

bool CheckpointController::strip_restore(const std::string& key) {
    return edit(key, [](JsonValue& doc) { return child(child(doc, "metadata"), "annotations").remove(models::kAnnotationRestoreFrom); });
}

bool CheckpointController::update_status(const std::string& key, std::initializer_list<std::pair<const char*, std::string_view>> strs,
                                         std::initializer_list<std::pair<const char*, double>> nums, const std::vector<std::string>* list) {
    return edit(key, [&](JsonValue& doc) {
        JsonValue& st = child(doc, "status");
        for (const auto& kv : strs) {
            if (!kv.second.empty()) set_str(st, kv.first, kv.second);
        }
        for (const auto& kv : nums) st.set(kv.first, JsonValue{models::JsonType::Number, false, kv.second, "", {}, {}});
        if (list && !list->empty()) {
            JsonValue arr;
            arr.type = models::JsonType::Array;
            for (const auto& b : *list) arr.arr_val.push_back(JsonValue{models::JsonType::String, false, 0, b, {}, {}});
            st.set("blockers", std::move(arr));
        }
        return true;
    });
}

void CheckpointController::create(const std::string& ns, const std::string& pod, const char* action, const std::string& target, const char* trigger) {
    const std::string name = pod + "-" + models::generate_uid().substr(0, 5);
    store_.put(str_cat({models::kCheckpointPrefix, ns, "/", name}), models::checkpoint_json(ns, name, pod, action, target, trigger), false);
}

void CheckpointController::pod_event(const std::string& ns, const std::string& name, const std::string& uid, const char* reason,
                                     const std::string& message, bool warning) {
    api::EventSpec ev;
    ev.involved.kind = "Pod";
    ev.involved.name = name;
    ev.involved.namespace_name = ns;
    ev.involved.uid = uid;
    ev.reason = reason;
    ev.message = message;
    ev.warning = warning;
    ev.component = "checkpoint-controller";
    events_.record(ev);
}

bool latest_periodic_checkpoint(storage::Store& store, const std::string& ns, const std::string& pod, std::string& name) {
    std::vector<std::string> jsons;
    store.list(str_cat({models::kCheckpointPrefix, ns, "/"}), jsons);
    uint64_t best = 0;
    for (const auto& json : jsons) {
        JsonValue doc;
        std::string err;
        if (json.find("\"periodic\"") == std::string::npos || !models::parse_json(json, doc, err) || !doc.is_object() || !doc.get("metadata")) continue;
        const JsonValue& meta = *doc.get("metadata");
        const uint64_t version = std::strtoull(meta.get_string("resourceVersion", "0").c_str(), nullptr, 10);
        if (field(meta, "labels", models::kLabelTrigger) == "periodic" && field(doc, "spec", "podName") == pod &&
            field(doc, "status", "phase") == "Saved" && version >= best) {
            best = version;
            name = meta.get_string("name", "");
        }
    }
    return !name.empty();
}

} // namespace kube32::controller
