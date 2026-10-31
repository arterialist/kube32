#include "controller/device_controller.hpp"
#include "models/kube_models.hpp"
#include "models/tolerations.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

namespace kube32::controller {

namespace {

constexpr const char* kDevicePrefix = "/registry/devices/";
constexpr const char* kPodPrefix = "/registry/pods/";
constexpr const char* kServicePrefix = "/registry/services/";
constexpr const char* kNodePrefix = "/registry/nodes/";
constexpr const char* kHostnameLabel = "kubernetes.io/hostname";
// Hash of what the driver pod was made from: a pod with another one is replaced
constexpr const char* kSpecAnnotation = "kube32.io/device-spec";
constexpr const char* kPodSuffix = "-driver";

models::JsonValue jstr(const std::string& v) { return models::JsonValue{models::JsonType::String, false, 0, v, {}, {}}; }
models::JsonValue jnum(double v) { return models::JsonValue{models::JsonType::Number, false, v, "", {}, {}}; }
models::JsonValue jbool(bool v) { return models::JsonValue{models::JsonType::Boolean, v, 0, "", {}, {}}; }
models::JsonValue jobj() { models::JsonValue o; o.type = models::JsonType::Object; return o; }
models::JsonValue jarr() { models::JsonValue a; a.type = models::JsonType::Array; return a; }

// Members set out of line: a call at each site instead of a JsonValue
// built and torn down there
void set_str(models::JsonValue& o, const char* key, const std::string& v) { o.set(key, jstr(v)); }
void set_num(models::JsonValue& o, const char* key, double v) { o.set(key, jnum(v)); }
void set_val(models::JsonValue& o, const char* key, models::JsonValue&& v) { o.set(key, std::move(v)); }

std::string quantity(uint32_t bytes) { return bytes % 1024 == 0 ? std::to_string(bytes / 1024) + "Ki" : std::to_string(bytes); }

// PIN_<NAME>, CONFIG_<KEY>: upper case, '-' and '.' as '_'
std::string env_name(const char* prefix, const std::string& key) {
    std::string out = prefix;
    for (char c : key) {
        if (c >= 'a' && c <= 'z') out += static_cast<char>(c - 'a' + 'A');
        else if (c == '-' || c == '.') out += '_';
        else out += c;
    }
    return out;
}

std::string fnv_hex(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    char buf[12];
    snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(h));
    return buf;
}

models::JsonValue owner_refs(const std::string& name, const std::string& uid) {
    models::JsonValue ref = jobj();
    set_str(ref, "apiVersion", "kube32.io/v1");
    set_str(ref, "kind", "Device");
    set_str(ref, "name", name);
    set_str(ref, "uid", uid);
    ref.set("controller", jbool(true));
    ref.set("blockOwnerDeletion", jbool(true));
    models::JsonValue refs = jarr();
    refs.arr_val.push_back(std::move(ref));
    return refs;
}

std::string device_owner(const models::JsonValue& obj) {
    const models::JsonValue* meta = obj.get("metadata");
    const models::JsonValue* refs = meta ? meta->get("ownerReferences") : nullptr;
    if (!refs || !refs->is_array()) return std::string();
    for (const auto& r : refs->arr_val) {
        if (r.get_string("kind", "") == "Device" && r.get_string("apiVersion", "").rfind("kube32.io/", 0) == 0) return r.get_string("uid", "");
    }
    return std::string();
}

// What keeps a pod from running: its container's waiting or terminated
// reason and message, the scheduler's, or the pod's own
std::string pod_message(const models::JsonValue& pod) {
    const models::JsonValue* st = pod.get("status");
    if (!st) return std::string();
    if (const models::JsonValue* css = st->get("containerStatuses"); css && css->is_array()) {
        for (const auto& cs : css->arr_val) {
            const models::JsonValue* state = cs.get("state");
            for (const char* k : {"waiting", "terminated"}) {
                const models::JsonValue* w = state ? state->get(k) : nullptr;
                if (!w) continue;
                const std::string reason = w->get_string("reason", ""), message = w->get_string("message", "");
                if (!reason.empty() || !message.empty()) return reason + (message.empty() ? "" : ": " + message);
            }
        }
    }
    if (const models::JsonValue* conds = st->get("conditions"); conds && conds->is_array()) {
        for (const auto& c : conds->arr_val) {
            if (c.get_string("type", "") == "PodScheduled" && c.get_string("status", "") == "False") {
                return c.get_string("reason", "") + ": " + c.get_string("message", "");
            }
        }
    }
    const std::string reason = st->get_string("reason", ""), message = st->get_string("message", "");
    return reason + (!reason.empty() && !message.empty() ? ": " : "") + message;
}

bool pod_ready(const models::JsonValue& pod) {
    const models::JsonValue* st = pod.get("status");
    if (!st || st->get_string("phase", "") != "Running") return false;
    const models::JsonValue* conds = st->get("conditions");
    if (conds && conds->is_array()) {
        for (const auto& c : conds->arr_val) {
            if (c.get_string("type", "") == "Ready") return c.get_string("status", "") == "True";
        }
    }
    return true;
}

// The part a node reports at `where`
const models::DiscoveredPart* reported_part(const std::vector<models::DiscoveredPart>& parts, const models::I2cLocation& where) {
    for (const auto& p : parts) {
        if (p.where == where) return &p;
    }
    return nullptr;
}

} // namespace

struct DeviceController::DeviceView {
    std::string key;
    std::string name;
    std::string ns;
    std::string uid;
    models::JsonValue labels;   // the Device's
    models::DeviceSpec spec;
    bool discovered{false};
    std::string node;           // where its driver runs, "" when no node fits
};

DeviceController::DeviceController(storage::Store& store, scheduler::Scheduler* scheduler, DeviceControllerConfig config)
    : store_(store), scheduler_(scheduler), config_(std::move(config)), events_(store, 64) {}

DeviceController::~DeviceController() {
    stop();
}

bool DeviceController::start() {
    if (running_.load()) return true;
    running_.store(true);
    thread_ = std::make_unique<osal::Thread>();
    osal::Thread::Config tc;
    tc.name = "k32_devices";
    tc.stack_size = config_.stack_bytes;
    tc.priority = 4;
    tc.core_affinity = config_.core_affinity;
    tc.stack_in_psram = config_.stack_in_psram;
    const bool ok = thread_->start([](void* arg) {
        auto* self = static_cast<DeviceController*>(arg);
        while (self->running_.load()) {
            try {
                self->reconcile();
            } catch (const std::exception& e) {
                printf("[DEVICE] pass skipped: %s\n", e.what());
            }
            osal::System::feed_watchdog();
            osal::Thread::sleep_ms(self->config_.reconcile_interval_ms);
        }
    }, this, tc);
    if (!ok) running_.store(false);
    return ok;
}

void DeviceController::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (thread_ && thread_->is_joinable()) thread_->join();
    thread_.reset();
}

std::vector<DeviceController::NodeView> DeviceController::load_nodes() {
    std::vector<std::string> jsons;
    store_.list(kNodePrefix, jsons);
    std::vector<NodeView> nodes;
    for (const auto& json : jsons) {
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(json, doc, err) || !doc.is_object()) continue;
        const models::JsonValue* meta = doc.get("metadata");
        NodeView n;
        n.name = meta ? meta->get_string("name", "") : "";
        if (n.name.empty()) continue;
        if (const models::JsonValue* labels = meta->get("labels"); labels && labels->is_object()) {
            for (const auto& kv : labels->obj_val) {
                if (kv.second.is_string()) n.labels[kv.first] = kv.second.str_val;
            }
        }
        n.simulator = n.labels.count(models::kLabelSimulator) && n.labels[models::kLabelSimulator] == "true";
        if (const models::JsonValue* a = meta->get("annotations"); a && a->is_object()) {
            if (const models::JsonValue* report = a->get(models::kAnnotationI2cDevices); report && report->is_string()) {
                n.reported = models::parse_discovered_parts(report->str_val, n.parts);
            }
        }
        if (const models::JsonValue* st = doc.get("status")) {
            if (const models::JsonValue* conds = st->get("conditions"); conds && conds->is_array()) {
                for (const auto& c : conds->arr_val) {
                    if (c.get_string("type", "") == "Ready") n.ready = c.get_string("status", "") == "True";
                }
            }
            const models::JsonValue* de = st->get("daemonEndpoints");
            const models::JsonValue* ke = de ? de->get("kubeletEndpoint") : nullptr;
            const int64_t port = ke ? ke->get_int("Port", 0) : 0;
            if (port > 0 && port <= 65535) n.pod_port = static_cast<uint16_t>(port);
        }
        nodes.push_back(std::move(n));
    }
    std::sort(nodes.begin(), nodes.end(), [](const NodeView& a, const NodeView& b) { return a.name < b.name; });
    return nodes;
}

void DeviceController::reconcile(uint64_t now) {
    if (now == 0) now = osal::System::get_uptime_ms();
    const std::vector<NodeView> nodes = load_nodes();
    std::vector<std::string> jsons, keys;
    store_.list_extended(kDevicePrefix, jsons, &keys);
    discover(nodes, keys, jsons);

    // Driver pods by key: only pods named <device>-driver are parsed
    std::map<std::string, PodView> pods;
    {
        std::vector<std::string> pod_jsons, pod_keys;
        store_.list_extended(kPodPrefix, pod_jsons, &pod_keys);
        for (size_t i = 0; i < pod_keys.size(); ++i) {
            const std::string& k = pod_keys[i];
            if (k.size() < 8 || k.compare(k.size() - 7, 7, kPodSuffix) != 0) continue;
            models::JsonValue p;
            std::string err;
            if (!models::parse_json(pod_jsons[i], p, err) || !p.is_object()) continue;
            PodView v;
            v.owner_uid = device_owner(p);
            if (v.owner_uid.empty()) continue;
            v.key = k;
            v.json = std::move(pod_jsons[i]);
            const models::JsonValue* meta = p.get("metadata");
            v.terminating = meta && !meta->get_string("deletionTimestamp", "").empty();
            const models::JsonValue* ann = meta ? meta->get("annotations") : nullptr;
            v.hash = ann ? ann->get_string(kSpecAnnotation, "") : "";
            const models::JsonValue* st = p.get("status");
            v.phase = st ? st->get_string("phase", "Pending") : "Pending";
            v.ready = pod_ready(p);
            v.message = pod_message(p);
            pods[k.substr(std::string(kPodPrefix).size())] = std::move(v);
        }
    }

    std::set<std::string> live;
    for (size_t i = 0; i < jsons.size(); ++i) {
        osal::Thread::yield();
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(jsons[i], doc, err) || !doc.is_object()) continue;
        const models::JsonValue* meta = doc.get("metadata");
        if (meta) live.insert(meta->get_string("uid", ""));
        reconcile_device(keys[i], jsons[i], nodes, pods, now);
    }

    // Pods and Services whose Device is gone go with it
    for (const auto& kv : pods) {
        if (!live.count(kv.second.owner_uid) && !kv.second.terminating) delete_pod(kv.second.key, kv.second.json);
    }
    std::vector<std::string> svc_jsons, svc_keys;
    store_.list_extended(kServicePrefix, svc_jsons, &svc_keys);
    for (size_t i = 0; i < svc_jsons.size(); ++i) {
        if (svc_jsons[i].find("\"Device\"") == std::string::npos) continue;
        models::JsonValue s;
        std::string err;
        if (!models::parse_json(svc_jsons[i], s, err) || !s.is_object()) continue;
        const std::string owner = device_owner(s);
        if (!owner.empty() && !live.count(owner)) store_.remove(svc_keys[i]);
    }
    for (auto it = retries_.begin(); it != retries_.end();) {
        it = live.count(it->first) ? std::next(it) : retries_.erase(it);
    }
}

void DeviceController::discover(const std::vector<NodeView>& nodes, std::vector<std::string>& keys, std::vector<std::string>& jsons) {
    // What the Devices cover: (node, I2C location), hand-written ones first
    std::vector<std::pair<std::string, models::I2cLocation>> covered;
    std::vector<size_t> replaced;   // indexes of discovered Devices a hand-written one replaces
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < jsons.size(); ++i) {
            models::JsonValue doc;
            std::string err, why;
            models::DeviceSpec spec;
            if (!models::parse_json(jsons[i], doc, err) || !models::parse_device_spec(doc.get("spec"), spec, why) || !spec.has_i2c) continue;
            const models::JsonValue* meta = doc.get("metadata");
            const models::JsonValue* labels = meta ? meta->get("labels") : nullptr;
            const bool auto_made = labels && labels->get_string(models::kLabelDiscovered, "") == "true";
            if (auto_made != (pass == 1)) continue;
            const models::JsonValue* st = doc.get("status");
            const std::string node = !spec.node_name.empty() ? spec.node_name : (st ? st->get_string("nodeName", "") : "");
            const bool known = std::any_of(covered.begin(), covered.end(), [&](const auto& c) { return c.first == node && c.second == spec.i2c; });
            // A hand-written Device takes the place of the one discovery made for its part
            if (auto_made && known) {
                store_.remove(keys[i]);
                printf("[DEVICE] %s: replaced by a Device written for the same part\n", keys[i].c_str());
                replaced.push_back(i);
                continue;
            }
            covered.emplace_back(node, spec.i2c);
        }
    }
    for (auto it = replaced.rbegin(); it != replaced.rend(); ++it) {
        keys.erase(keys.begin() + static_cast<long>(*it));
        jsons.erase(jsons.begin() + static_cast<long>(*it));
    }
    for (const auto& n : nodes) {
        for (const auto& part : n.parts) {
            const models::DeviceDriver* driver = models::find_device_driver(part.driver);
            if (!driver) continue;
            const bool known = std::any_of(covered.begin(), covered.end(), [&](const auto& c) { return c.first == n.name && c.second == part.where; });
            if (known) continue;
            const std::string name = models::discovered_device_name(driver->name, n.name, part.where);
            models::JsonValue doc = jobj();
            set_str(doc, "apiVersion", "kube32.io/v1");
            set_str(doc, "kind", "Device");
            models::JsonValue meta = jobj();
            set_str(meta, "name", name);
            set_str(meta, "namespace", config_.discovered_namespace);
            set_str(meta, "uid", models::generate_uid());
            set_num(meta, "generation", 1);
            const std::string now = models::now_rfc3339();
            if (!now.empty()) set_str(meta, "creationTimestamp", now);
            models::JsonValue labels = jobj();
            set_str(labels, models::kLabelDiscovered, "true");
            set_str(labels, models::kLabelDriver, driver->name);
            set_val(meta, "labels", std::move(labels));
            set_val(doc, "metadata", std::move(meta));
            models::JsonValue spec = jobj();
            set_str(spec, "driver", driver->name);
            set_str(spec, "nodeName", n.name);
            models::JsonValue i2c = jobj();
            set_num(i2c, "bus", part.where.bus);
            set_str(i2c, "address", models::hex_byte(part.where.address));
            if (part.where.mux) {
                models::JsonValue mux = jobj();
                set_str(mux, "address", models::hex_byte(part.where.mux));
                set_num(mux, "channel", part.where.channel);
                set_val(i2c, "mux", std::move(mux));
            }
            set_val(spec, "i2c", std::move(i2c));
            set_val(doc, "spec", std::move(spec));
            const std::string key = kDevicePrefix + config_.discovered_namespace + "/" + name;
            const std::string json = doc.to_string();
            if (!store_.put(key, json, false).ok()) continue;   // there already, or no room
            covered.emplace_back(n.name, part.where);
            keys.push_back(key);
            jsons.push_back(json);
            DeviceView d;
            d.name = name;
            d.ns = config_.discovered_namespace;
            d.uid = doc.get("metadata")->get_string("uid", "");
            event(d, "Discovered", (part.chip.empty() ? std::string(driver->name) : part.chip) + " found on node " + n.name + " at " +
                                   models::i2c_location_text(part.where), false);
            printf("[DEVICE] %s/%s: %s on %s at %s\n", d.ns.c_str(), name.c_str(), part.chip.c_str(), n.name.c_str(),
                   models::i2c_location_text(part.where).c_str());
        }
    }
}

std::string DeviceController::place(const models::DeviceSpec& spec, const std::string& current, const std::vector<NodeView>& nodes) const {
    if (!spec.node_name.empty()) return spec.node_name;
    auto matches = [&](const NodeView& n) {
        for (const auto& kv : spec.node_selector) {
            auto it = n.labels.find(kv.first);
            if (it == n.labels.end() || it->second != kv.second) return false;
        }
        return true;
    };
    for (const auto& n : nodes) {
        if (n.name == current && matches(n)) return current;
    }
    // Boards first: a kube32-sim node only simulates the parts
    for (int sim = 0; sim < 2; ++sim) {
        for (const auto& n : nodes) {
            if (n.ready && n.simulator == (sim == 1) && matches(n)) return n.name;
        }
    }
    return std::string();
}

void DeviceController::reconcile_device(const std::string& key, const std::string& json, const std::vector<NodeView>& nodes,
                                        std::map<std::string, PodView>& pods, uint64_t now) {
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) return;
    const models::JsonValue* meta = doc.get("metadata");
    if (!meta) return;
    DeviceView d;
    d.key = key;
    d.name = meta->get_string("name", "");
    d.ns = meta->get_string("namespace", "default");
    d.uid = meta->get_string("uid", "");
    if (d.name.empty() || d.uid.empty()) return;
    d.labels = jobj();
    if (const models::JsonValue* labels = meta->get("labels"); labels && labels->is_object()) d.labels = *labels;
    d.discovered = d.labels.get_string(models::kLabelDiscovered, "") == "true";
    const models::JsonValue* old = doc.get("status");
    const std::string old_phase = old ? old->get_string("phase", "") : "";

    const std::string pod_name = d.name + kPodSuffix;
    auto pit = pods.find(d.ns + "/" + pod_name);
    PodView* pod = nullptr;
    std::string conflict;
    if (pit != pods.end()) {
        if (pit->second.owner_uid == d.uid) pod = &pit->second;
        else conflict = "pod " + pod_name + " belongs to another Device";
    }

    models::JsonValue status = jobj();
    bool created = false;   // the driver pod was made in this pass
    auto finish = [&](const std::string& phase, const std::string& message) {
        set_str(status, "phase", phase);
        if (!d.node.empty()) set_str(status, "nodeName", d.node);
        if (((pod && !pod->terminating) || created) && phase != "Absent") set_str(status, "podName", pod_name);
        set_str(status, "message", message);
        set_num(status, "observedGeneration", static_cast<double>(meta->get_int("generation", 1)));
        write_status(key, status);
    };

    std::string why;
    if (!models::parse_device_spec(doc.get("spec"), d.spec, why)) {
        if (pod && !pod->terminating) delete_pod(pod->key, pod->json);
        finish("Failed", why);
        return;
    }
    d.node = place(d.spec, old ? old->get_string("nodeName", "") : "", nodes);
    const NodeView* node = nullptr;
    for (const auto& n : nodes) {
        if (n.name == d.node) node = &n;
    }
    // What discovery found at the Device's address
    const models::DiscoveredPart* part = (node && d.spec.has_i2c) ? reported_part(node->parts, d.spec.i2c) : nullptr;
    if (part && !part->chip.empty()) set_str(status, "chip", part->chip);
    else if (old && !old->get_string("chip", "").empty()) set_str(status, "chip", old->get_string("chip", ""));

    // A discovered part the node no longer finds: no driver until it is back
    const bool gone = part == nullptr || (!part->driver.empty() && part->driver != d.spec.driver->name);
    if (d.discovered && d.spec.has_i2c && node && node->reported && gone) {
        if (pod && !pod->terminating) delete_pod(pod->key, pod->json);
        const std::string message = "node " + d.node + " no longer finds the part at " + models::i2c_location_text(d.spec.i2c);
        if (old_phase != "Absent") event(d, "Absent", message, true);
        finish("Absent", message);
        return;
    }
    if (old_phase == "Absent") event(d, "Present", "node " + d.node + " finds the part at " + models::i2c_location_text(d.spec.i2c) + " again", false);
    if (d.node.empty()) {
        finish("Pending", "no Ready node matches spec.nodeSelector");
        return;
    }
    if (!conflict.empty()) {
        finish("Failed", conflict);
        return;
    }

    std::string message;
    ensure_service(d, message);
    std::string hash;
    const std::string desired = pod_json(d, node, hash);
    if (!pod) {
        std::string bound = desired;
        if (scheduler_) scheduler_->schedule_pod_manifest(bound);
        if (store_.put(kPodPrefix + d.ns + "/" + pod_name, bound, false).ok()) {
            event(d, "SuccessfulCreate", "Created driver pod: " + pod_name, false);
            created = true;
        }
        finish("Pending", message.empty() ? "driver pod " + pod_name + " created" : message);
        return;
    }
    if (pod->terminating) {
        finish("Pending", "waiting for driver pod " + pod_name + " to terminate");
        return;
    }
    if (pod->hash != hash) {
        delete_pod(pod->key, pod->json);
        event(d, "Replacing", "Device changed: replacing driver pod " + pod_name, false);
        finish("Pending", "replacing driver pod " + pod_name);
        return;
    }
    // Labels follow the Device's without a new pod
    {
        models::JsonValue p;
        models::parse_json(pod->json, p, err);
        const models::JsonValue* pmeta = p.get("metadata");
        const models::JsonValue* plabels = pmeta ? pmeta->get("labels") : nullptr;
        models::JsonValue want = d.labels;
        set_str(want, models::kLabelDevice, d.name);
        set_str(want, models::kLabelDriver, d.spec.driver->name);
        if (!plabels || plabels->to_string() != want.to_string()) update_pod_labels(pod->key, want);
    }
    if (pod->phase == "Failed" || pod->phase == "Succeeded") {
        Retry& r = retries_[d.uid];
        if (r.delay_ms == 0) r.delay_ms = config_.failed_retry_ms;
        if (r.since == 0) r.since = now;
        if (now - r.since >= r.delay_ms) {
            delete_pod(pod->key, pod->json);
            event(d, "Replacing", "replacing failed driver pod " + pod_name, true);
            r.since = 0;
            r.delay_ms = std::min<uint64_t>(r.delay_ms * 2, config_.failed_retry_max_ms);
        }
        finish("Failed", pod->message.empty() ? "driver pod " + pod_name + " failed" : pod->message);
        return;
    }
    if (pod->phase == "Running") retries_.erase(d.uid);
    if (pod->phase == "Running" && pod->ready) {
        finish("Running", message);
        return;
    }
    finish("Pending", pod->message.empty() ? message : pod->message);
}

std::string DeviceController::pod_json(const DeviceView& d, const NodeView* node, std::string& out_hash) const {
    const models::DeviceDriver& drv = *d.spec.driver;
    models::JsonValue env = jarr();
    auto add_env = [&](const std::string& name, const std::string& value) {
        models::JsonValue e = jobj();
        set_str(e, "name", name);
        set_str(e, "value", value);
        env.arr_val.push_back(std::move(e));
    };
    add_env("DEVICE_NAME", d.name);
    add_env("DEVICE_NAMESPACE", d.ns);
    add_env("DEVICE_DRIVER", drv.name);
    {
        models::JsonValue e = jobj();
        set_str(e, "name", "NODE_NAME");
        models::JsonValue fr = jobj();
        set_str(fr, "fieldPath", "spec.nodeName");
        models::JsonValue from = jobj();
        set_val(from, "fieldRef", std::move(fr));
        set_val(e, "valueFrom", std::move(from));
        env.arr_val.push_back(std::move(e));
    }
    add_env("DEVICE_TOPIC", "device." + d.name);
    add_env("DEVICE_INTERVAL_MS", std::to_string(d.spec.interval_ms));
    if (d.spec.has_i2c) {
        add_env("I2C_BUS", std::to_string(d.spec.i2c.bus));
        add_env("I2C_ADDR", models::hex_byte(d.spec.i2c.address));
    }
    for (const auto& p : d.spec.pins) add_env(env_name("PIN_", p.first), std::to_string(p.second));
    for (const auto& c : d.spec.config) add_env(env_name("CONFIG_", c.first), c.second);

    models::JsonValue requests = jobj();
    set_str(requests, "memory", quantity(drv.memory_bytes));
    set_str(requests, "hardware.kube32.io/ipc", "1");
    for (const auto& p : d.spec.pins) {
        requests.set((drv.input_pins ? "esp32.io/gpio-input-" : "esp32.io/gpio-") + std::to_string(p.second), jstr("1"));
    }
    if (drv.resource) set_str(requests, drv.resource, "1");
    models::JsonValue limits = jobj();
    set_str(limits, "memory", quantity(drv.memory_bytes));
    models::JsonValue resources = jobj();
    set_val(resources, "requests", std::move(requests));
    set_val(resources, "limits", std::move(limits));

    models::JsonValue port = jobj();
    set_str(port, "name", "http");
    set_num(port, "containerPort", node ? node->pod_port : 7000);
    models::JsonValue ports = jarr();
    ports.arr_val.push_back(std::move(port));
    models::JsonValue caps = jobj();
    models::JsonValue add = jarr();
    add.arr_val.push_back(jstr("CHANNEL_IPC"));
    set_val(caps, "add", std::move(add));
    models::JsonValue sc = jobj();
    set_val(sc, "capabilities", std::move(caps));

    models::JsonValue container = jobj();
    set_str(container, "name", "driver");
    set_str(container, "image", d.spec.image);
    set_val(container, "ports", std::move(ports));
    set_val(container, "env", std::move(env));
    set_val(container, "resources", std::move(resources));
    set_val(container, "securityContext", std::move(sc));
    models::JsonValue containers = jarr();
    containers.arr_val.push_back(std::move(container));

    models::JsonValue spec = jobj();
    models::JsonValue sel = jobj();
    set_str(sel, kHostnameLabel, d.node);
    set_val(spec, "nodeSelector", std::move(sel));
    set_str(spec, "restartPolicy", "Always");
    set_val(spec, "containers", std::move(containers));
    models::add_default_tolerations(spec);

    models::JsonValue ann = jobj();
    set_str(ann, "kube32.io/tick-interval", std::to_string(d.spec.interval_ms) + "ms");
    if (d.spec.has_i2c) set_str(ann, models::kAnnotationI2cDevice, models::i2c_device_annotation(d.spec.i2c));
    out_hash = fnv_hex(spec.to_string() + ann.to_string());
    set_str(ann, kSpecAnnotation, out_hash);

    models::JsonValue labels = d.labels;
    set_str(labels, models::kLabelDevice, d.name);
    set_str(labels, models::kLabelDriver, drv.name);
    models::JsonValue meta = jobj();
    set_str(meta, "name", d.name + kPodSuffix);
    set_str(meta, "namespace", d.ns);
    set_str(meta, "uid", models::generate_uid());
    const std::string now = models::now_rfc3339();
    if (!now.empty()) set_str(meta, "creationTimestamp", now);
    set_val(meta, "labels", std::move(labels));
    set_val(meta, "annotations", std::move(ann));
    set_val(meta, "ownerReferences", owner_refs(d.name, d.uid));

    models::JsonValue pod = jobj();
    set_str(pod, "apiVersion", "v1");
    set_str(pod, "kind", "Pod");
    set_val(pod, "metadata", std::move(meta));
    set_val(pod, "spec", std::move(spec));
    models::JsonValue st = jobj();
    set_str(st, "phase", "Pending");
    set_val(pod, "status", std::move(st));
    return pod.to_string();
}

void DeviceController::ensure_service(const DeviceView& d, std::string& message) {
    const std::string key = kServicePrefix + d.ns + "/" + d.name;
    models::JsonValue spec = jobj();
    models::JsonValue sel = jobj();
    set_str(sel, models::kLabelDevice, d.name);
    set_val(spec, "selector", std::move(sel));
    models::JsonValue port = jobj();
    set_str(port, "name", "http");
    set_num(port, "port", 80);
    set_str(port, "targetPort", "http");
    set_str(port, "protocol", "TCP");
    models::JsonValue ports = jarr();
    ports.arr_val.push_back(std::move(port));
    set_val(spec, "ports", std::move(ports));
    // The defaults the API server gives a Service
    set_str(spec, "type", "ClusterIP");
    set_str(spec, "sessionAffinity", "None");
    set_str(spec, "internalTrafficPolicy", "Cluster");
    set_str(spec, "ipFamilyPolicy", "SingleStack");
    models::JsonValue fam = jarr();
    fam.arr_val.push_back(jstr("IPv4"));
    set_val(spec, "ipFamilies", std::move(fam));

    std::string existing;
    uint64_t version = 0;
    models::JsonValue svc;
    std::string err;
    if (store_.get_extended(key, existing, &version).ok() && models::parse_json(existing, svc, err) && svc.is_object()) {
        if (device_owner(svc) != d.uid) {
            message = "service " + d.name + " exists and does not belong to the Device";
            return;
        }
        const models::JsonValue* old = svc.get("spec");
        if (old && old->to_string() == spec.to_string()) return;
        set_val(svc, "spec", std::move(spec));
        store_.put_extended(key, svc.to_string(), true, true, version, nullptr);
        return;
    }
    svc = jobj();
    set_str(svc, "apiVersion", "v1");
    set_str(svc, "kind", "Service");
    models::JsonValue meta = jobj();
    set_str(meta, "name", d.name);
    set_str(meta, "namespace", d.ns);
    set_str(meta, "uid", models::generate_uid());
    const std::string now = models::now_rfc3339();
    if (!now.empty()) set_str(meta, "creationTimestamp", now);
    models::JsonValue labels = jobj();
    set_str(labels, models::kLabelDevice, d.name);
    set_str(labels, models::kLabelDriver, d.spec.driver->name);
    set_val(meta, "labels", std::move(labels));
    set_val(meta, "ownerReferences", owner_refs(d.name, d.uid));
    set_val(svc, "metadata", std::move(meta));
    set_val(svc, "spec", std::move(spec));
    if (store_.put(key, svc.to_string(), false).ok()) {
        event(d, "SuccessfulCreate", "Created service: " + d.name, false);
    }
}

void DeviceController::delete_pod(const std::string& key, const std::string& json) {
    // As the API's DELETE: graceful when a kubelet runs it
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) { store_.remove(key); return; }
    const models::JsonValue* spec = doc.get("spec");
    const std::string node = spec ? spec->get_string("nodeName", "") : "";
    models::JsonValue* meta = doc.get_mut("metadata");
    const std::string now = models::now_rfc3339();
    if (!node.empty() && meta && meta->get_string("deletionTimestamp", "").empty() && !now.empty()) {
        set_str(*meta, "deletionTimestamp", now);
        set_num(*meta, "deletionGracePeriodSeconds", 30);
        store_.put(key, doc.to_string(), true);
    } else if (!meta || meta->get_string("deletionTimestamp", "").empty()) {
        store_.remove(key);
    }
}

bool DeviceController::update_pod_labels(const std::string& key, const models::JsonValue& labels) {
    std::string json;
    uint64_t version = 0;
    if (!store_.get_extended(key, json, &version).ok()) return false;
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) return false;
    models::JsonValue* meta = doc.get_mut("metadata");
    if (!meta) return false;
    meta->set("labels", labels);
    // Over the version read: a status the kubelet wrote meanwhile is kept, the next pass tries again
    return store_.put_extended(key, doc.to_string(), true, true, version, nullptr).ok();
}

void DeviceController::write_status(const std::string& key, const models::JsonValue& status) {
    std::string json;
    uint64_t version = 0;
    if (!store_.get_extended(key, json, &version).ok()) return;
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(json, doc, err) || !doc.is_object()) return;
    const models::JsonValue* old = doc.get("status");
    if (old && old->to_string() == status.to_string()) return;
    doc.set("status", status);
    store_.put_extended(key, doc.to_string(), true, true, version, nullptr);
}

void DeviceController::event(const DeviceView& d, const char* reason, const std::string& message, bool warning) {
    api::EventSpec ev;
    ev.involved.kind = "Device";
    ev.involved.api_version = "kube32.io/v1";
    ev.involved.name = d.name;
    ev.involved.namespace_name = d.ns;
    ev.involved.uid = d.uid;
    ev.reason = reason;
    ev.message = message;
    ev.warning = warning;
    ev.component = "device-controller";
    events_.record(ev);
}

} // namespace kube32::controller
