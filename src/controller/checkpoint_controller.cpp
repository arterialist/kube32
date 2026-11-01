#include "controller/checkpoint_controller.hpp"
#include "kubelet/kube_api_client.hpp"
#include "models/kube_models.hpp"
#include "models/migration.hpp"
#include "osal/socket.hpp"
#include "osal/system.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <strings.h>

namespace kube32::controller {

namespace {

using models::JsonValue;

constexpr const char* kPodPrefix = "/registry/pods/";
constexpr const char* kNodePrefix = "/registry/nodes/";

JsonValue jstr(const std::string& v) { return JsonValue{models::JsonType::String, false, 0, v, {}, {}}; }
JsonValue jnum(double v) { return JsonValue{models::JsonType::Number, false, v, "", {}, {}}; }
JsonValue jobj() { JsonValue o; o.type = models::JsonType::Object; return o; }

std::string field(const JsonValue& o, const char* part, const char* key, const char* def = "") {
    const JsonValue* p = o.get(part);
    return p ? p->get_string(key, def) : std::string(def);
}

std::string annotation(const JsonValue& o, const char* key) {
    const JsonValue* meta = o.get("metadata");
    const JsonValue* ann = meta ? meta->get("annotations") : nullptr;
    return ann ? ann->get_string(key, "") : std::string();
}

std::string seconds(uint64_t ms) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f s", static_cast<double>(ms) / 1000.0);
    return buf;
}

// Running with every container running, as the kubelet reports it
bool runs(const JsonValue& pod) {
    const JsonValue* st = pod.get("status");
    if (!st || st->get_string("phase", "") != "Running") return false;
    const JsonValue* css = st->get("containerStatuses");
    if (!css || !css->is_array() || css->arr_val.empty()) return false;
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
    if (st->get_string("phase", "") == "Failed") return "pod Failed: " + st->get_string("reason", "") + " " + st->get_string("message", "");
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
    if (models::parse_json(text, doc, err) && doc.is_object() && !doc.get_string("message", "").empty()) return doc.get_string("message", "");
    return text.substr(0, 200);
}

// A PUT whose body is written as it arrives: Content-Length when the length
// is known, chunked otherwise
class Upload {
public:
    bool begin(const std::string& host, uint16_t port, const std::string& token, const std::string& path, long length) {
        if (!sock_.create(osal::SocketType::Tcp)) return false;
        sock_.set_timeout(15000, 15000);
        sock_.set_tcp_nodelay(true);
        if (!sock_.connect(host.c_str(), port)) return false;
        chunked_ = length < 0;
        std::string head = "PUT " + path + " HTTP/1.1\r\nHost: " + host + "\r\nContent-Type: application/octet-stream\r\n";
        head += chunked_ ? std::string("Transfer-Encoding: chunked\r\n") : "Content-Length: " + std::to_string(length) + "\r\n";
        if (!token.empty()) head += "Authorization: Bearer " + token + "\r\n";
        head += "Connection: close\r\n\r\n";
        ok_ = sock_.send_all(head.data(), head.size());
        return ok_;
    }
    bool write(const char* p, size_t n) {
        if (!ok_ || n == 0) return ok_;
        if (chunked_) {
            char line[20];
            const int l = snprintf(line, sizeof(line), "%zx\r\n", n);
            ok_ = sock_.send_all(line, static_cast<size_t>(l)) && sock_.send_all(p, n) && sock_.send_all("\r\n", 2);
        } else {
            ok_ = sock_.send_all(p, n);
        }
        return ok_;
    }
    // The answer's status (0 without one), its body's start in `text`
    int finish(std::string& text) {
        if (chunked_ && ok_) sock_.send_all("0\r\n\r\n", 5);
        std::string in;
        char buf[256];
        while (in.size() < 1024) {
            const ssize_t n = sock_.recv(buf, sizeof(buf));
            if (n <= 0) break;
            in.append(buf, static_cast<size_t>(n));
        }
        if (in.rfind("HTTP/1.", 0) != 0) return 0;
        const size_t end = in.find("\r\n\r\n");
        if (end != std::string::npos) text = in.substr(end + 4);
        return std::atoi(in.c_str() + in.find(' ') + 1);
    }

private:
    osal::Socket sock_;
    bool chunked_{false};
    bool ok_{false};
};

// As the API's DELETE: graceful when a kubelet runs the pod
void delete_pod_gracefully(storage::Store& store, const std::string& key) {
    std::string json;
    JsonValue doc;
    std::string err;
    if (!store.get(key, json).ok() || !models::parse_json(json, doc, err) || !doc.is_object()) return;
    JsonValue* meta = doc.get_mut("metadata");
    const std::string now = models::now_rfc3339();
    if (!meta || !meta->get_string("deletionTimestamp", "").empty()) return;
    if (field(doc, "spec", "nodeName").empty() || now.empty()) {
        store.remove(key);
        return;
    }
    meta->set("deletionTimestamp", jstr(now));
    meta->set("deletionGracePeriodSeconds", jnum(30));
    store.put(key, doc.to_string(), true);
}

} // namespace

struct CheckpointController::Node {
    std::string name;
    std::string ip;
    uint16_t port{0};
    bool ready{false};
    bool unschedulable{false};
    bool pressure{false};
    double mops{0};
};

struct CheckpointController::Ckpt {
    std::string key, ns, name, uid, trigger;
    std::string action;       // spec.action
    std::string pod;          // spec.podName
    std::string container;    // spec.containerName
    std::string target;       // spec.target.nodeName
    std::string phase;        // status.phase ("" until the controller saw it)
    std::string node;         // status.nodeName: where the pod ran
    std::string new_pod;      // status.newPodName
    uint64_t version{0};
    JsonValue doc;
    bool active() const { return phase.empty() || phase == "Pending" || phase == "Saving" || phase == "Moving"; }
};

// The pods of a pass, each parsed when first asked for
struct CheckpointController::Pods {
    std::vector<std::string> keys, jsons;
    std::map<std::string, size_t> index;
    std::map<size_t, JsonValue> parsed;
    std::set<std::string> touched;   // written in this pass: what was read of them is stale
    const JsonValue* at(size_t i) {
        auto it = parsed.find(i);
        if (it == parsed.end()) {
            JsonValue doc;
            std::string err;
            if (!models::parse_json(jsons[i], doc, err) || !doc.is_object()) doc = JsonValue{};
            it = parsed.emplace(i, std::move(doc)).first;
        }
        return it->second.is_object() ? &it->second : nullptr;
    }
    const JsonValue* get(const std::string& key) {
        auto it = index.find(key);
        return it == index.end() ? nullptr : at(it->second);
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
    trigger_.watch_fields(kNodePrefix, [](const JsonValue& node) {
        std::string conds;
        if (const JsonValue* st = node.get("status")) {
            if (const JsonValue* cs = st->get("conditions"); cs && cs->is_array()) {
                for (const auto& c : cs->arr_val) {
                    const std::string type = c.get_string("type", "");
                    if (type == "Ready" || type == "MemoryPressure") conds += type + c.get_string("status", "");
                }
            }
        }
        const JsonValue* spec = node.get("spec");
        return ReconcileTrigger::hash({conds, spec && spec->get_bool("unschedulable", false) ? "u" : "", annotation(node, models::kAnnotationInterpMops)});
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
    store_.list(kNodePrefix, jsons);
    std::vector<Node> nodes;
    for (const auto& json : jsons) {
        JsonValue doc;
        std::string err;
        if (!models::parse_json(json, doc, err) || !doc.is_object()) continue;
        Node n;
        n.name = field(doc, "metadata", "name");
        if (n.name.empty()) continue;
        n.port = models::node_pod_port(doc);
        n.mops = std::strtod(annotation(doc, models::kAnnotationInterpMops).c_str(), nullptr);
        if (const JsonValue* spec = doc.get("spec")) n.unschedulable = spec->get_bool("unschedulable", false);
        if (const JsonValue* st = doc.get("status")) {
            if (const JsonValue* addrs = st->get("addresses"); addrs && addrs->is_array()) {
                for (const auto& a : addrs->arr_val) {
                    if (a.get_string("type", "") == "InternalIP") n.ip = a.get_string("address", "");
                }
            }
            if (const JsonValue* cs = st->get("conditions"); cs && cs->is_array()) {
                for (const auto& c : cs->arr_val) {
                    const bool on = c.get_string("status", "") == "True";
                    if (c.get_string("type", "") == "Ready") n.ready = on;
                    if (c.get_string("type", "") == "MemoryPressure") n.pressure = on;
                }
            }
        }
        nodes.push_back(std::move(n));
    }
    return nodes;
}

void CheckpointController::reconcile(uint64_t now) {
    if (now == 0) now = osal::System::get_uptime_ms();
    passes_.fetch_add(1);
    const std::vector<Node> nodes = load_nodes();
    Pods pods;
    store_.list_extended(kPodPrefix, pods.jsons, &pods.keys);
    for (size_t i = 0; i < pods.keys.size(); ++i) pods.index[pods.keys[i]] = i;

    std::vector<Ckpt> cks;
    {
        std::vector<std::string> jsons, keys;
        store_.list_extended(models::kCheckpointPrefix, jsons, &keys);
        for (size_t i = 0; i < jsons.size(); ++i) {
            Ckpt c;
            std::string err;
            if (!models::parse_json(jsons[i], c.doc, err) || !c.doc.is_object()) continue;
            c.key = keys[i];
            const JsonValue* meta = c.doc.get("metadata");
            const JsonValue* spec = c.doc.get("spec");
            if (!meta || !spec) continue;
            c.ns = meta->get_string("namespace", "default");
            c.name = meta->get_string("name", "");
            c.uid = meta->get_string("uid", "");
            c.version = std::strtoull(meta->get_string("resourceVersion", "0").c_str(), nullptr, 10);
            if (const JsonValue* labels = meta->get("labels")) c.trigger = labels->get_string(models::kLabelTrigger, "");
            c.action = spec->get_string("action", "Save");
            c.pod = spec->get_string("podName", "");
            c.container = spec->get_string("containerName", "");
            c.target = field(*spec, "target", "nodeName");
            c.phase = field(c.doc, "status", "phase");
            c.node = field(c.doc, "status", "nodeName");
            c.new_pod = field(c.doc, "status", "newPodName");
            cks.push_back(std::move(c));
        }
    }

    for (auto& ck : cks) {
        osal::Thread::yield();
        if (ck.phase == "Moving") {
            watch_move(ck, pods, nodes, now);
        } else if (ck.phase == "Saving") {
            // Only an earlier run leaves one: a pass ends each snapshot it starts
            if (ck.action == "Move") {
                const JsonValue* pod = pods.get(kPodPrefix + ck.ns + "/" + ck.pod);
                const Node* src = nullptr;
                for (const auto& n : nodes) {
                    if (n.name == ck.node) src = &n;
                }
                const JsonValue* cs = pod ? pod->get("spec") : nullptr;
                const JsonValue* containers = cs ? cs->get("containers") : nullptr;
                std::string container = ck.container;
                if (container.empty() && containers && containers->is_array() && !containers->arr_val.empty()) {
                    container = containers->arr_val[0].get_string("name", "");
                }
                thaw(src, ck.ns, ck.pod, container);
            }
            fail(ck, "interrupted: the control plane restarted while the snapshot was taken");
        }
    }
    finish_restores(pods, cks);
    policies(pods, nodes, cks, now);
    retention(pods, cks, now);

    // One snapshot per pass: the oldest asked for, those of users and
    // evictions before periodic ones. Its writes make the next pass due.
    std::set<std::string> busy;
    for (const auto& ck : cks) {
        if (ck.phase == "Saving" || ck.phase == "Moving") busy.insert(ck.ns + "/" + ck.pod);
    }
    Ckpt* next = nullptr;
    for (auto& ck : cks) {
        if (!(ck.phase.empty() || ck.phase == "Pending") || busy.count(ck.ns + "/" + ck.pod)) continue;
        const bool periodic = ck.trigger == "periodic";
        const bool next_periodic = next && next->trigger == "periodic";
        if (!next || (next_periodic && !periodic) || (next_periodic == periodic && ck.version < next->version)) next = &ck;
    }
    if (next) begin(*next, pods, nodes, now);
}

void CheckpointController::fail(Ckpt& ck, const std::string& message, const std::vector<std::string>& blockers) {
    JsonValue f = jobj();
    f.set("phase", jstr("Failed"));
    f.set("message", jstr(message));
    if (const uint64_t wall = wall_ms()) f.set("finishedAt", jstr(models::format_rfc3339(wall)));
    if (!blockers.empty()) {
        JsonValue list;
        list.type = models::JsonType::Array;
        for (const auto& b : blockers) list.arr_val.push_back(jstr(b));
        f.set("blockers", std::move(list));
    }
    update_status(ck.key, f);
    ck.phase = "Failed";
    printf("[CKPT] %s/%s failed: %s\n", ck.ns.c_str(), ck.name.c_str(), message.c_str());
}

void CheckpointController::begin(Ckpt& ck, Pods& pods, const std::vector<Node>& nodes, uint64_t now) {
    const std::string pod_key = kPodPrefix + ck.ns + "/" + ck.pod;
    const bool move = ck.action == "Move";
    // A move the eviction API asked for that cannot happen is an eviction
    auto give_up = [&](const std::string& message, const std::vector<std::string>& blockers = {}) {
        fail(ck, message, blockers);
        if (ck.trigger == "eviction") delete_pod_gracefully(store_, pod_key);
    };
    if (ck.action != "Save" && !move) return give_up("spec.action must be Save or Move");
    const JsonValue* pod = pods.get(pod_key);
    if (!pod) return give_up("pod " + ck.pod + " not found");
    const std::string source = field(*pod, "spec", "nodeName");
    const std::string phase = field(*pod, "status", "phase");
    if (source.empty() || phase != "Running" || !field(*pod, "metadata", "deletionTimestamp").empty()) {
        return give_up("pod " + ck.pod + " is not running" + (phase.empty() ? std::string() : " (phase " + phase + ")"));
    }
    std::string container = ck.container;
    if (container.empty()) {
        const JsonValue* cs = pod->get("spec")->get("containers");
        const size_t n = cs && cs->is_array() ? cs->arr_val.size() : 0;
        if (n != 1) return give_up("pod " + ck.pod + " has " + std::to_string(n) + " containers: set spec.containerName");
        container = cs->arr_val[0].get_string("name", "");
    }
    const std::vector<std::string> blockers = models::migration_blockers(*pod, move);
    if (!blockers.empty()) return give_up(move ? "the pod cannot move" : "the pod cannot be checkpointed", blockers);
    std::string target, why;
    if (move) {
        target = pick_target(ck, *pod, nodes, why);
        if (target.empty()) return give_up("no node fits: " + why);
    }
    const Node* src = nullptr;
    for (const auto& n : nodes) {
        if (n.name == source) src = &n;
    }
    if (!src || !src->port || src->ip.empty()) return give_up("node " + source + " reports no kubelet endpoint");
    {
        JsonValue f = jobj();
        f.set("phase", jstr("Saving"));
        f.set("nodeName", jstr(source));
        if (move) f.set("targetNode", jstr(target));
        if (!update_status(ck.key, f)) return;   // deleted meanwhile
        ck.phase = "Saving";
    }
    const std::string ns = ck.ns, name = field(*pod, "metadata", "name"), uid = field(*pod, "metadata", "uid");
    const uint64_t t0 = osal::System::get_uptime_ms();
    const Snapshot s = take_snapshot(*src, ck, ck.pod, container, move);
    const std::string took = seconds(osal::System::get_uptime_ms() - t0);
    if (!s.ok) {
        if (s.maybe_frozen) thaw(src, ns, name, container);
        std::string text = "checkpoint " + ck.name + ": " + s.error;
        for (size_t i = 0; i < s.blockers.size(); ++i) text += (i ? ", " : ": ") + s.blockers[i];
        pod_event(ns, name, uid, move ? "MoveFailed" : "CheckpointFailed", text, true);
        return give_up(s.error, s.blockers);
    }
    JsonValue f = jobj();
    f.set("sizeBytes", jnum(static_cast<double>(s.bytes)));
    if (!s.image_sha256.empty()) f.set("imageSha256", jstr(s.image_sha256));
    if (!s.snapshot_sha256.empty()) f.set("snapshotSha256", jstr(s.snapshot_sha256));
    if (!s.pod_time_ms.empty()) f.set("podTimeMs", jnum(static_cast<double>(std::strtoull(s.pod_time_ms.c_str(), nullptr, 10))));
    const uint64_t wall = wall_ms();
    if (wall) f.set("takenAt", jstr(models::format_rfc3339(wall)));
    const std::string size = std::to_string(s.bytes) + " bytes";
    if (!move) {
        f.set("phase", jstr("Saved"));
        if (wall) f.set("finishedAt", jstr(models::format_rfc3339(wall)));
        update_status(ck.key, f);
        ck.phase = "Saved";
        pod_event(ns, name, uid, "Checkpointed", "Saved checkpoint " + ck.name + " on node " + source + ": " + size + " in " + took, false);
        return;
    }
    std::string new_uid;
    if (!rebind(pod_key, target, ck.name, uid, &new_uid)) {
        thaw(src, ns, name, container);
        pod_event(ns, name, uid, "MoveFailed", "checkpoint " + ck.name + ": the pod changed while it was frozen, it stays on node " + source, true);
        return give_up("the pod changed while it was frozen: it stays on node " + source);
    }
    moving_since_[ck.uid] = now;
    f.set("phase", jstr("Moving"));
    f.set("newPodName", jstr(ck.pod));
    f.set("message", jstr("restoring on node " + target));
    update_status(ck.key, f);
    ck.phase = "Moving";
    pod_event(ns, name, new_uid, "Moving", "Moving from node " + source + " to node " + target + " with checkpoint " + ck.name + " (" + size +
                                           ", taken in " + took + ")", false);
}

void CheckpointController::watch_move(Ckpt& ck, Pods& pods, const std::vector<Node>& nodes, uint64_t now) {
    const uint64_t since = moving_since_.emplace(ck.uid, now).first->second;
    const std::string key = kPodPrefix + ck.ns + "/" + (ck.new_pod.empty() ? ck.pod : ck.new_pod);
    const JsonValue* pod = pods.get(key);
    pods.touched.insert(key);
    if (!pod || annotation(*pod, models::kAnnotationRestoreFrom) != ck.name) {
        fail(ck, pod ? "the pod was changed during the move" : "the pod was deleted during the move");
        return;
    }
    const std::string node = field(*pod, "spec", "nodeName");
    const std::string uid = field(*pod, "metadata", "uid");
    if (runs(*pod)) {
        strip_restore(key);
        const std::string message = "moved from node " + ck.node + " to node " + node + " in " + seconds(now - since);
        JsonValue f = jobj();
        f.set("phase", jstr("Completed"));
        f.set("targetNode", jstr(node));
        f.set("message", jstr(message));
        if (const uint64_t wall = wall_ms()) f.set("finishedAt", jstr(models::format_rfc3339(wall)));
        update_status(ck.key, f);
        ck.phase = "Completed";
        moving_since_.erase(ck.uid);
        pod_event(ck.ns, ck.pod, uid, "Moved", "Checkpoint " + ck.name + ": " + message, false);
        printf("[CKPT] %s/%s: %s\n", ck.ns.c_str(), ck.pod.c_str(), message.c_str());
        return;
    }
    std::string why = restore_failure(*pod);
    if (why.empty()) {
        if (now < since + config_.move_deadline_ms) {
            trigger_.wake_at(since + config_.move_deadline_ms);
            return;
        }
        why = "not running on node " + node + " after " + seconds(config_.move_deadline_ms);
    }
    // Back to the node it came from, from the same checkpoint
    bool source_ready = false;
    for (const auto& n : nodes) source_ready |= n.name == ck.node && n.ready;
    const std::string back = source_ready ? ck.node : std::string();
    std::string new_uid;
    const bool rebound = rebind(key, back, ck.name, uid, &new_uid);
    const std::string message = "restore on node " + node + " failed (" + why + "); " +
                                (!rebound ? std::string("the pod could not be put back")
                                          : back.empty() ? std::string("the pod waits for a node to restore it on")
                                                         : "restoring it on node " + back + " from the same checkpoint");
    fail(ck, message);
    moving_since_.erase(ck.uid);
    pod_event(ck.ns, ck.pod, rebound ? new_uid : uid, "MoveFailed", "Checkpoint " + ck.name + ": " + message, true);
}

void CheckpointController::finish_restores(Pods& pods, const std::vector<Ckpt>& cks) {
    const uint64_t wall = wall_ms();
    for (size_t i = 0; i < pods.jsons.size(); ++i) {
        if (pods.jsons[i].find(models::kAnnotationRestoreFrom) == std::string::npos || pods.touched.count(pods.keys[i])) continue;
        const JsonValue* pod = pods.at(i);
        const std::string from = pod ? annotation(*pod, models::kAnnotationRestoreFrom) : std::string();
        if (from.empty()) continue;
        const std::string ns = field(*pod, "metadata", "namespace", "default");
        const Ckpt* ck = nullptr;
        for (const auto& c : cks) {
            if (c.ns == ns && c.name == from) ck = &c;
        }
        if (ck && ck->phase == "Moving") continue;   // watch_move follows it
        const std::string name = field(*pod, "metadata", "name"), uid = field(*pod, "metadata", "uid");
        if (runs(*pod)) {
            if (!strip_restore(pods.keys[i]) || (ck && ck->phase == "Completed")) continue;
            // The checkpoint's age: from its takenAt, else from when this run saw it saved
            std::string age;
            uint64_t at = 0;
            const std::string taken = ck ? field(ck->doc, "status", "takenAt") : std::string();
            if (wall && !taken.empty() && models::parse_rfc3339(taken, at) && wall >= at) {
                age = ", taken " + seconds(wall - at) + " ago";
            } else if (ck && done_seen_.count(ck->uid)) {
                age = ", taken " + seconds(osal::System::get_uptime_ms() - done_seen_[ck->uid]) + " ago";
            }
            pod_event(ns, name, uid, "Restored", "Restored from checkpoint " + from + age + " on node " + field(*pod, "spec", "nodeName"), false);
        } else if (const std::string why = restore_failure(*pod); !why.empty()) {
            if (strip_restore(pods.keys[i])) {
                pod_event(ns, name, uid, "MoveFailed", "Restore from checkpoint " + from + " failed (" + why + "): the container starts afresh", true);
            }
        }
    }
}

std::string CheckpointController::pick_target(const Ckpt& ck, const JsonValue& pod, const std::vector<Node>& nodes, std::string& why) {
    const std::string source = field(pod, "spec", "nodeName");
    if (!scheduler_) {
        why = "no scheduler";
        return std::string();
    }
    JsonValue copy = pod;
    JsonValue* spec = copy.get_mut("spec");
    // spec.target.nodeSelector on top of the pod's own
    const JsonValue* cspec = ck.doc.get("spec");
    const JsonValue* target = cspec ? cspec->get("target") : nullptr;
    const JsonValue* sel = target ? target->get("nodeSelector") : nullptr;
    if (sel && sel->is_object()) {
        JsonValue merged = spec->get("nodeSelector") && spec->get("nodeSelector")->is_object() ? *spec->get("nodeSelector") : jobj();
        for (const auto& kv : sel->obj_val) merged.set(kv.first, kv.second);
        spec->set("nodeSelector", std::move(merged));
    }
    auto fits = [&](const std::string& node) {
        spec->set("nodeName", jstr(node));
        const scheduler::ScheduleResult r = scheduler_->pick_node(copy.to_string(), source);
        if (!r.scheduled && why.empty()) why = r.failure_reason;
        return r.scheduled ? r.target_node : std::string();
    };
    if (!ck.target.empty()) {
        if (ck.target != source) return fits(ck.target);
        why = "the target is the node the pod runs on";
        return std::string();
    }
    // kube32.io/placement: fastest goes to the fastest node that takes it
    if (models::migration_policy(pod) == models::MigrationPolicy::Auto && annotation(pod, models::kAnnotationPlacement) == "fastest") {
        std::vector<const Node*> by_speed;
        for (const auto& n : nodes) {
            if (n.mops > 0 && n.name != source) by_speed.push_back(&n);
        }
        std::sort(by_speed.begin(), by_speed.end(), [](const Node* a, const Node* b) { return a->mops > b->mops; });
        for (const Node* n : by_speed) {
            const std::string t = fits(n->name);
            if (!t.empty()) return t;
        }
        why.clear();
    }
    return fits(std::string());
}

void CheckpointController::policies(Pods& pods, const std::vector<Node>& nodes, const std::vector<Ckpt>& cks, uint64_t now) {
    std::set<std::string> busy;   // pods a checkpoint is under way for
    for (const auto& ck : cks) {
        if (ck.active()) busy.insert(ck.ns + "/" + ck.pod);
    }
    struct Pick {
        std::string ns, name;
        uint64_t bytes{0};
    };
    std::map<std::string, Pick> pressure;   // node -> the pod that frees the most
    for (size_t i = 0; i < pods.jsons.size(); ++i) {
        const std::string& j = pods.jsons[i];
        const bool periodic = j.find(models::kAnnotationCheckpointInterval) != std::string::npos;
        if (!periodic && j.find("\"Auto\"") == std::string::npos) continue;
        const JsonValue* pod = pods.touched.count(pods.keys[i]) ? nullptr : pods.at(i);
        if (!pod || !runs(*pod) || !field(*pod, "metadata", "deletionTimestamp").empty()) continue;
        const std::string ns = field(*pod, "metadata", "namespace", "default"), name = field(*pod, "metadata", "name");
        const std::string at = field(*pod, "spec", "nodeName");
        const std::string key = ns + "/" + name;
        const models::MigrationPolicy policy = models::migration_policy(*pod);
        if (busy.count(key) || policy == models::MigrationPolicy::Never || !models::migration_blockers(*pod, false).empty()) continue;
        if (periodic) {
            uint64_t every = models::parse_duration_ms(annotation(*pod, models::kAnnotationCheckpointInterval));
            if (every) {
                every = std::max<uint64_t>(every, config_.min_interval_ms);
                auto last = last_periodic_.find(key);
                if (last == last_periodic_.end() || now >= last->second + every) {
                    create(ns, name, "Save", "", "periodic");
                    last_periodic_[key] = now;
                    trigger_.wake_at(now + every);
                } else {
                    trigger_.wake_at(last->second + every);
                }
            }
        }
        if (policy != models::MigrationPolicy::Auto) continue;
        auto moved = auto_moved_.find(key);
        if (moved != auto_moved_.end() && now < moved->second + config_.auto_cooldown_ms) {
            trigger_.wake_at(moved->second + config_.auto_cooldown_ms);
            continue;
        }
        const Node* here = nullptr;
        for (const auto& n : nodes) {
            if (n.name == at) here = &n;
        }
        if (!here) continue;
        // A node at least twice as fast that takes the pod
        if (annotation(*pod, models::kAnnotationPlacement) == "fastest" && here->mops > 0 && scheduler_) {
            std::vector<const Node*> faster;
            for (const auto& n : nodes) {
                if (n.ready && !n.unschedulable && n.name != at && n.mops >= 2 * here->mops) faster.push_back(&n);
            }
            std::sort(faster.begin(), faster.end(), [](const Node* a, const Node* b) { return a->mops > b->mops; });
            bool placed = false;
            for (const Node* n : faster) {
                JsonValue copy = *pod;
                copy.get_mut("spec")->set("nodeName", jstr(n->name));
                if (!scheduler_->pick_node(copy.to_string(), at).scheduled) continue;
                create(ns, name, "Move", n->name, "placement");
                auto_moved_[key] = now;
                placed = true;
                break;
            }
            if (placed) continue;
        }
        if (here->pressure) {
            const scheduler::ResourceSpec req = scheduler::extract_pod_resource_requests(j);
            Pick& p = pressure[at];
            if (p.name.empty() || req.memory_bytes + req.psram_bytes > p.bytes) p = Pick{ns, name, req.memory_bytes + req.psram_bytes};
        }
    }
    for (const auto& kv : pressure) {
        auto last = pressure_moved_.find(kv.first);
        if (last != pressure_moved_.end() && now < last->second + config_.pressure_cooldown_ms) {
            trigger_.wake_at(last->second + config_.pressure_cooldown_ms);
            continue;
        }
        create(kv.second.ns, kv.second.name, "Move", "", "memory-pressure");
        pressure_moved_[kv.first] = now;
        auto_moved_[kv.second.ns + "/" + kv.second.name] = now;
    }
}

void CheckpointController::retention(Pods& pods, const std::vector<Ckpt>& cks, uint64_t now) {
    // Checkpoints a pod is to be restored from stay
    std::set<std::string> wanted;
    for (size_t i = 0; i < pods.jsons.size(); ++i) {
        if (pods.jsons[i].find(models::kAnnotationRestoreFrom) == std::string::npos) continue;
        if (const JsonValue* pod = pods.at(i)) {
            wanted.insert(field(*pod, "metadata", "namespace", "default") + "/" + annotation(*pod, models::kAnnotationRestoreFrom));
        }
    }
    // The latest Saved and Failed periodic checkpoint of each pod
    std::map<std::string, uint64_t> saved, failed;
    for (const auto& ck : cks) {
        if (ck.trigger != "periodic") continue;
        const std::string key = ck.ns + "/" + ck.pod;
        if (ck.phase == "Saved") saved[key] = std::max(saved[key], ck.version);
        if (ck.phase == "Failed") failed[key] = std::max(failed[key], ck.version);
    }
    const uint64_t wall = wall_ms();
    std::set<std::string> live;
    for (const auto& ck : cks) {
        live.insert(ck.uid);
        if (ck.active() && ck.phase != "Saved") continue;
        if (ck.phase != "Saved" && ck.phase != "Completed" && ck.phase != "Failed") continue;
        const uint64_t seen = done_seen_.emplace(ck.uid, now).first->second;
        if (wanted.count(ck.ns + "/" + ck.name)) continue;
        bool drop;
        if (ck.trigger == "periodic") {
            const std::string key = ck.ns + "/" + ck.pod;
            drop = !pods.get(kPodPrefix + key) || ck.version < saved[key] || (ck.phase != "Saved" && ck.version < failed[key]);
        } else {
            uint64_t age = now - seen, at = 0;
            const std::string finished = field(ck.doc, "status", "finishedAt");
            if (wall && !finished.empty() && models::parse_rfc3339(finished, at) && wall > at) age = std::max(age, wall - at);
            drop = age >= config_.keep_ms;
            if (!drop) trigger_.wake_at(now + (config_.keep_ms - age));
        }
        if (drop) store_.remove(ck.key);
    }
    for (auto* m : {&done_seen_, &moving_since_}) {
        for (auto it = m->begin(); it != m->end();) it = live.count(it->first) ? std::next(it) : m->erase(it);
    }
    for (auto* m : {&last_periodic_, &auto_moved_}) {
        for (auto it = m->begin(); it != m->end();) it = pods.index.count(kPodPrefix + it->first) ? std::next(it) : m->erase(it);
    }
}

CheckpointController::Snapshot CheckpointController::take_snapshot(const Node& node, const Ckpt& ck, const std::string& pod,
                                                                   const std::string& container, bool frozen) {
    Snapshot s;
    kubelet::HttpKubeClient kubelet(node.ip, node.port);
    kubelet.set_bearer_token(config_.token);
    kubelet.set_timeout_ms(config_.freeze_timeout_ms + 20000);
    const std::string path = "/checkpoint/" + ck.ns + "/" + pod + "/" + container + "?leave=" + (frozen ? "frozen" : "running") +
                             "&timeoutMs=" + std::to_string(config_.freeze_timeout_ms);
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
                         "/apis/kube32.io/v1/namespaces/" + ck.ns + "/checkpoints/" + ck.name + "/data", length)) {
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
            if (text.size() < 1024) text.append(p, std::min(n, 1024 - text.size()));
            return true;
        });
    if (status == 200) {
        s.maybe_frozen = frozen;
        if (!s.error.empty()) return s;
        std::string answer;
        const int put = up.finish(answer);
        if (length >= 0 && s.bytes != static_cast<uint64_t>(length)) {
            s.error = "the snapshot ended after " + std::to_string(s.bytes) + " of " + std::to_string(length) + " bytes";
        } else if (put < 200 || put >= 300) {
            s.error = "the API server did not store the checkpoint data (" + (put ? "HTTP " + std::to_string(put) + ": " + message_of(answer) : "no answer") + ")";
        } else {
            s.ok = true;
        }
        return s;
    }
    JsonValue body;
    std::string err;
    const std::string message = models::parse_json(text, body, err) && body.is_object() ? body.get_string("message", "") : std::string();
    if (status == 409) {
        if (const JsonValue* b = body.get("blockers"); b && b->is_array()) {
            for (const auto& x : b->arr_val) {
                if (x.is_string()) s.blockers.push_back(x.str_val);
            }
        }
        s.error = "the pod cannot move";
    } else if (status == 404) {
        s.error = "node " + node.name + " does not run container " + container + " of pod " + pod;
    } else if (status == 504) {
        s.error = message.empty() ? "the pod could not be frozen within " + std::to_string(config_.freeze_timeout_ms) + " ms" : message;
    } else if (status == 0) {
        s.error = "no answer from the kubelet of node " + node.name;
        s.maybe_frozen = frozen;
    } else {
        s.error = "the kubelet of node " + node.name + " answered HTTP " + std::to_string(status) + (message.empty() ? "" : ": " + message);
    }
    return s;
}

bool CheckpointController::thaw(const Node* node, const std::string& ns, const std::string& pod, const std::string& container) {
    if (!node || !node->port || node->ip.empty() || container.empty()) return false;
    kubelet::HttpKubeClient kubelet(node->ip, node->port);
    kubelet.set_bearer_token(config_.token);
    const int status = kubelet.request("POST", "/thaw/" + ns + "/" + pod + "/" + container, std::string_view(),
                                       [](int, std::string_view) { return true; }, [](const char*, size_t) { return true; });
    return status == 200;
}

bool CheckpointController::rebind(const std::string& key, const std::string& node, const std::string& restore_from, const std::string& uid,
                                  std::string* new_uid) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string json;
        uint64_t version = 0;
        JsonValue doc;
        std::string err;
        if (!store_.get_extended(key, json, &version).ok() || !models::parse_json(json, doc, err) || !doc.is_object()) return false;
        JsonValue* meta = doc.get_mut("metadata");
        JsonValue* spec = doc.get_mut("spec");
        if (!meta || !spec || meta->get_string("uid", "") != uid || !meta->get_string("deletionTimestamp", "").empty()) return false;
        const std::string fresh = models::generate_uid();
        meta->set("uid", jstr(fresh));
        const std::string now = models::now_rfc3339();
        if (!now.empty()) meta->set("creationTimestamp", jstr(now));
        JsonValue ann = meta->get("annotations") && meta->get("annotations")->is_object() ? *meta->get("annotations") : jobj();
        ann.set(models::kAnnotationRestoreFrom, jstr(restore_from));
        meta->set("annotations", std::move(ann));
        spec->set("nodeName", jstr(node));
        JsonValue st = jobj();
        st.set("phase", jstr("Pending"));
        if (!node.empty()) {
            JsonValue c = jobj();
            c.set("type", jstr("PodScheduled"));
            c.set("status", jstr("True"));
            if (!now.empty()) c.set("lastTransitionTime", jstr(now));
            JsonValue conds;
            conds.type = models::JsonType::Array;
            conds.arr_val.push_back(std::move(c));
            st.set("conditions", std::move(conds));
        }
        doc.set("status", std::move(st));
        const storage::Status ps = store_.put_extended(key, doc.to_string(), true, true, version, nullptr);
        if (ps.ok()) {
            if (new_uid) *new_uid = fresh;
            return true;
        }
        if (ps.code != storage::StatusCode::VersionConflict) return false;
    }
    return false;
}

bool CheckpointController::strip_restore(const std::string& key) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string json;
        uint64_t version = 0;
        JsonValue doc;
        std::string err;
        if (!store_.get_extended(key, json, &version).ok() || !models::parse_json(json, doc, err) || !doc.is_object()) return false;
        JsonValue* meta = doc.get_mut("metadata");
        JsonValue* ann = meta ? meta->get_mut("annotations") : nullptr;
        if (!ann || !ann->remove(models::kAnnotationRestoreFrom)) return true;
        const storage::Status ps = store_.put_extended(key, doc.to_string(), true, true, version, nullptr);
        if (ps.ok()) return true;
        if (ps.code != storage::StatusCode::VersionConflict) return false;
    }
    return false;
}

bool CheckpointController::update_status(const std::string& key, const JsonValue& fields) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string json;
        uint64_t version = 0;
        JsonValue doc;
        std::string err;
        if (!store_.get_extended(key, json, &version).ok() || !models::parse_json(json, doc, err) || !doc.is_object()) return false;
        JsonValue status = doc.get("status") && doc.get("status")->is_object() ? *doc.get("status") : jobj();
        for (const auto& kv : fields.obj_val) status.set(kv.first, kv.second);
        doc.set("status", std::move(status));
        const storage::Status ps = store_.put_extended(key, doc.to_string(), true, true, version, nullptr);
        if (ps.ok()) return true;
        if (ps.code != storage::StatusCode::VersionConflict) return false;
    }
    return false;
}

void CheckpointController::create(const std::string& ns, const std::string& pod, const std::string& action, const std::string& target,
                                  const std::string& trigger) {
    const std::string name = pod + "-" + models::generate_uid().substr(0, 5);
    store_.put(std::string(models::kCheckpointPrefix) + ns + "/" + name, models::checkpoint_json(ns, name, pod, action, target, trigger), false);
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
    store.list(std::string(models::kCheckpointPrefix) + ns + "/", jsons);
    uint64_t best = 0;
    for (const auto& json : jsons) {
        JsonValue doc;
        std::string err;
        if (json.find("\"periodic\"") == std::string::npos || !models::parse_json(json, doc, err) || !doc.is_object()) continue;
        const JsonValue* meta = doc.get("metadata");
        const JsonValue* labels = meta ? meta->get("labels") : nullptr;
        if (!labels || labels->get_string(models::kLabelTrigger, "") != "periodic" || field(doc, "spec", "podName") != pod ||
            field(doc, "status", "phase") != "Saved") {
            continue;
        }
        const uint64_t version = std::strtoull(meta->get_string("resourceVersion", "0").c_str(), nullptr, 10);
        if (version >= best) {
            best = version;
            name = meta->get_string("name", "");
        }
    }
    return !name.empty();
}

} // namespace kube32::controller
