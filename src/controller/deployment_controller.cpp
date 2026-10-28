#include "controller/deployment_controller.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"

#include <algorithm>
#include <iostream>
#include <set>

namespace kube32::controller {

static models::JsonValue jstr(const std::string& v) { return models::JsonValue{models::JsonType::String, false, 0, v, {}, {}}; }
static models::JsonValue jnum(double v) { return models::JsonValue{models::JsonType::Number, false, v, "", {}, {}}; }
static models::JsonValue jbool(bool v) { return models::JsonValue{models::JsonType::Boolean, v, 0, "", {}, {}}; }

DeploymentController::DeploymentController(storage::Store& store, scheduler::Scheduler* scheduler, DeploymentControllerConfig cfg)
    : store_(store), scheduler_(scheduler), cfg_(cfg), events_(store, 64) {}

DeploymentController::~DeploymentController() {
    stop();
}

bool DeploymentController::start() {
    if (running_.load()) return true;
    running_.store(true);
    thread_ = std::make_unique<osal::Thread>();
    osal::Thread::Config tc;
    tc.name = "k32_deploy";
    tc.stack_size = cfg_.stack_bytes;
    tc.priority = 4;
    tc.core_affinity = cfg_.core_affinity;
    tc.stack_in_psram = cfg_.stack_in_psram;
    if (!thread_->start(task_entry, this, tc)) {
        running_.store(false);
        return false;
    }
    return true;
}

void DeploymentController::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (thread_ && thread_->is_joinable()) {
        thread_->join();
        thread_.reset();
    }
}

void DeploymentController::task_entry(void* arg) {
    static_cast<DeploymentController*>(arg)->run_loop();
}

void DeploymentController::run_loop() {
    while (running_.load()) {
        try {
            reconcile();
        } catch (const std::exception& e) {
            std::cout << "[DEPLOY] pass skipped: " << e.what() << std::endl;
        }
        osal::System::feed_watchdog();
        osal::Thread::sleep_ms(cfg_.reconcile_interval_ms);
    }
}

std::string DeploymentController::template_hash(const std::string& template_json) {
    // FNV-1a 32-bit, rendered with kube's safe alphabet (no vowels, so no words)
    uint32_t h = 2166136261u;
    for (unsigned char c : template_json) { h ^= c; h *= 16777619u; }
    static const char* alphabet = "bcdfghjklmnpqrstvwxz2456789";
    std::string out;
    for (int i = 0; i < 8; ++i) { out += alphabet[h % 27]; h /= 27; if (h == 0) h = 2166136261u ^ static_cast<uint32_t>(i); }
    return out;
}

static std::string owner_uid_of(const models::JsonValue& pod) {
    const models::JsonValue* meta = pod.get("metadata");
    const models::JsonValue* refs = meta ? meta->get("ownerReferences") : nullptr;
    if (!refs || !refs->is_array()) return "";
    for (const auto& r : refs->arr_val) {
        if (r.get_string("kind", "") == "Deployment") return r.get_string("uid", "");
    }
    return "";
}

static bool pod_is_ready(const models::JsonValue& pod) {
    const models::JsonValue* st = pod.get("status");
    if (!st) return false;
    if (st->get_string("phase", "") != "Running") return false;
    const models::JsonValue* conds = st->get("conditions");
    if (conds && conds->is_array()) {
        for (const auto& c : conds->arr_val) {
            if (c.get_string("type", "") == "Ready") return c.get_string("status", "") == "True";
        }
    }
    return false;
}

void DeploymentController::delete_pod(const std::string& pod_key, const std::string& pod_json) {
    // Same semantics as the API's DELETE: graceful when a kubelet owns it
    models::JsonValue doc;
    std::string err;
    if (!models::parse_json(pod_json, doc, err) || !doc.is_object()) { store_.remove(pod_key); return; }
    const models::JsonValue* spec = doc.get("spec");
    std::string node = spec ? spec->get_string("nodeName", "") : "";
    models::JsonValue* meta = doc.get_mut("metadata");
    std::string now = models::now_rfc3339();
    if (!node.empty() && node != "<none>" && meta && meta->get_string("deletionTimestamp", "").empty() && !now.empty()) {
        meta->set("deletionTimestamp", jstr(now));
        meta->set("deletionGracePeriodSeconds", jnum(30));
        store_.put(pod_key, doc.to_string(), true);
    } else if (meta && !meta->get_string("deletionTimestamp", "").empty()) {
        // already terminating; the kubelet finalizes it
    } else {
        store_.remove(pod_key);
    }
}

void DeploymentController::reconcile() {
    std::vector<std::string> dep_jsons;
    std::vector<std::string> dep_keys;
    store_.list_extended("/registry/deployments/", dep_jsons, &dep_keys);

    std::vector<std::string> pod_jsons;
    std::vector<std::string> pod_keys;
    store_.list_extended("/registry/pods/", pod_jsons, &pod_keys);

    std::vector<std::string> uids;
    for (size_t i = 0; i < dep_jsons.size(); ++i) {
        osal::Thread::yield();
        models::JsonValue d;
        std::string err;
        if (!models::parse_json(dep_jsons[i], d, err) || !d.is_object()) continue;
        const models::JsonValue* meta = d.get("metadata");
        std::string uid = meta ? meta->get_string("uid", "") : "";
        if (!uid.empty()) uids.push_back(uid);
        reconcile_one(dep_keys[i], dep_jsons[i], pod_jsons, pod_keys);
    }
    collect_garbage(uids);
}

void DeploymentController::collect_garbage(const std::vector<std::string>& deployment_uids) {
    std::vector<std::string> pod_jsons;
    std::vector<std::string> pod_keys;
    store_.list_extended("/registry/pods/", pod_jsons, &pod_keys);
    std::set<std::string> live(deployment_uids.begin(), deployment_uids.end());
    for (size_t i = 0; i < pod_jsons.size(); ++i) {
        models::JsonValue p;
        std::string err;
        if (!models::parse_json(pod_jsons[i], p, err) || !p.is_object()) continue;
        std::string owner = owner_uid_of(p);
        if (owner.empty() || live.count(owner)) continue;
        const models::JsonValue* meta = p.get("metadata");
        if (meta && !meta->get_string("deletionTimestamp", "").empty()) continue;
        delete_pod(pod_keys[i], pod_jsons[i]);
    }
}

void DeploymentController::reconcile_one(const std::string& key, const std::string& json,
                                         const std::vector<std::string>& all_pod_jsons,
                                         const std::vector<std::string>& all_pod_keys) {
    models::JsonValue dep;
    std::string err;
    if (!models::parse_json(json, dep, err) || !dep.is_object()) return;
    const models::JsonValue* meta = dep.get("metadata");
    const models::JsonValue* spec = dep.get("spec");
    if (!meta || !spec || !spec->is_object()) return;
    std::string name = meta->get_string("name", "");
    std::string ns = meta->get_string("namespace", "default");
    std::string uid = meta->get_string("uid", "");
    if (name.empty() || uid.empty()) return;
    int64_t desired = spec->get_int("replicas", 1);
    if (desired < 0) desired = 0;
    const models::JsonValue* tmpl = spec->get("template");
    if (!tmpl || !tmpl->is_object()) return;
    std::string hash = template_hash(tmpl->to_string());

    // Owned pods in this namespace
    struct Owned { std::string key; std::string json; bool terminating; bool ready; bool current; };
    std::vector<Owned> owned;
    for (size_t i = 0; i < all_pod_jsons.size(); ++i) {
        if (all_pod_keys[i].compare(0, 16 + ns.size(), "/registry/pods/" + ns + "/") != 0) continue;
        models::JsonValue p;
        if (!models::parse_json(all_pod_jsons[i], p, err) || !p.is_object()) continue;
        if (owner_uid_of(p) != uid) continue;
        const models::JsonValue* pm = p.get("metadata");
        const models::JsonValue* labels = pm ? pm->get("labels") : nullptr;
        Owned o;
        o.key = all_pod_keys[i];
        o.json = all_pod_jsons[i];
        o.terminating = pm && !pm->get_string("deletionTimestamp", "").empty();
        o.ready = pod_is_ready(p);
        o.current = labels && labels->get_string("pod-template-hash", "") == hash;
        owned.push_back(std::move(o));
    }

    int64_t active = 0, ready = 0, updated = 0;
    for (const auto& o : owned) {
        if (o.terminating) continue;
        ++active;
        if (o.ready) ++ready;
        if (o.current) ++updated;
    }

    // Scale up: create missing pods from the template
    for (int64_t n = active; n < desired; ++n) {
        models::JsonValue pod;
        pod.type = models::JsonType::Object;
        pod.set("kind", jstr("Pod"));
        pod.set("apiVersion", jstr("v1"));
        models::JsonValue pmeta;
        pmeta.type = models::JsonType::Object;
        const models::JsonValue* tmeta = tmpl->get("metadata");
        if (tmeta && tmeta->is_object()) pmeta = *tmeta;
        std::string suffix = models::generate_uid().substr(0, 5);
        std::string pod_name = name + "-" + hash + "-" + suffix;
        pmeta.set("name", jstr(pod_name));
        pmeta.set("namespace", jstr(ns));
        pmeta.set("generateName", jstr(name + "-" + hash + "-"));
        pmeta.set("uid", jstr(models::generate_uid()));
        std::string now = models::now_rfc3339();
        if (!now.empty()) pmeta.set("creationTimestamp", jstr(now));
        models::JsonValue* labels = pmeta.get_mut("labels");
        if (!labels || !labels->is_object()) {
            models::JsonValue l;
            l.type = models::JsonType::Object;
            pmeta.set("labels", std::move(l));
            labels = pmeta.get_mut("labels");
        }
        labels->set("pod-template-hash", jstr(hash));
        models::JsonValue refs;
        refs.type = models::JsonType::Array;
        models::JsonValue ref;
        ref.type = models::JsonType::Object;
        ref.set("apiVersion", jstr("apps/v1"));
        ref.set("kind", jstr("Deployment"));
        ref.set("name", jstr(name));
        ref.set("uid", jstr(uid));
        ref.set("controller", jbool(true));
        ref.set("blockOwnerDeletion", jbool(true));
        refs.arr_val.push_back(std::move(ref));
        pmeta.set("ownerReferences", std::move(refs));
        pod.set("metadata", std::move(pmeta));
        const models::JsonValue* tspec = tmpl->get("spec");
        models::JsonValue pspec;
        if (tspec && tspec->is_object()) pspec = *tspec; else pspec.type = models::JsonType::Object;
        pod.set("spec", std::move(pspec));
        models::JsonValue st;
        st.type = models::JsonType::Object;
        st.set("phase", jstr("Pending"));
        pod.set("status", std::move(st));
        std::string pod_json = pod.to_string();
        if (scheduler_) scheduler_->schedule_pod_manifest(pod_json);
        store_.put("/registry/pods/" + ns + "/" + pod_name, pod_json, false);
        ++active;
        {
            api::EventSpec ev;
            ev.involved.kind = "Deployment";
            ev.involved.name = name;
            ev.involved.namespace_name = ns;
            ev.involved.uid = uid;
            ev.involved.api_version = "apps/v1";
            ev.reason = "SuccessfulCreate";
            ev.message = "Created pod: " + pod_name;
            ev.component = "deployment-controller";
            events_.record(ev);
        }
    }

    // Scale down: drop surplus (not-ready and stale first, then newest)
    if (active > desired) {
        std::vector<Owned*> candidates;
        for (auto& o : owned) if (!o.terminating) candidates.push_back(&o);
        std::stable_sort(candidates.begin(), candidates.end(), [](const Owned* a, const Owned* b) {
            if (a->ready != b->ready) return !a->ready;
            if (a->current != b->current) return !a->current;
            return a->key > b->key;
        });
        for (int64_t n = active; n > desired && !candidates.empty(); --n) {
            Owned* victim = candidates.front();
            candidates.erase(candidates.begin());
            delete_pod(victim->key, victim->json);
            size_t slash = victim->key.rfind('/');
            api::EventSpec ev;
            ev.involved.kind = "Deployment";
            ev.involved.name = name;
            ev.involved.namespace_name = ns;
            ev.involved.uid = uid;
            ev.involved.api_version = "apps/v1";
            ev.reason = "SuccessfulDelete";
            ev.message = "Deleted pod: " + victim->key.substr(slash + 1);
            ev.component = "deployment-controller";
            events_.record(ev);
        }
    } else if (updated < active) {
        // Rolling update: replace one stale pod per pass once the set is full
        for (auto& o : owned) {
            if (o.terminating || o.current) continue;
            delete_pod(o.key, o.json);
            break;
        }
    }

    // Status from what exists right now
    int64_t available = ready;
    int64_t unavailable = desired > available ? desired - available : 0;
    models::JsonValue status;
    const models::JsonValue* prev = dep.get("status");
    if (prev && prev->is_object()) status = *prev; else status.type = models::JsonType::Object;
    status.set("observedGeneration", jnum(static_cast<double>(meta->get_int("generation", 1))));
    status.set("replicas", jnum(static_cast<double>(active)));
    status.set("updatedReplicas", jnum(static_cast<double>(updated)));
    status.set("readyReplicas", jnum(static_cast<double>(ready)));
    status.set("availableReplicas", jnum(static_cast<double>(available)));
    if (unavailable > 0) status.set("unavailableReplicas", jnum(static_cast<double>(unavailable)));
    else status.remove("unavailableReplicas");
    models::JsonValue conds;
    conds.type = models::JsonType::Array;
    {
        models::JsonValue c;
        c.type = models::JsonType::Object;
        c.set("type", jstr("Available"));
        c.set("status", jstr(available >= desired && desired > 0 ? "True" : "False"));
        c.set("reason", jstr(available >= desired && desired > 0 ? "MinimumReplicasAvailable" : "MinimumReplicasUnavailable"));
        c.set("message", jstr(std::to_string(available) + " of " + std::to_string(desired) + " replicas available"));
        conds.arr_val.push_back(std::move(c));
        models::JsonValue p;
        p.type = models::JsonType::Object;
        p.set("type", jstr("Progressing"));
        bool complete = (updated == desired && available == desired);
        p.set("status", jstr("True"));
        p.set("reason", jstr(complete ? "NewReplicaSetAvailable" : "ReplicaSetUpdated"));
        p.set("message", jstr(complete ? "Deployment \"" + name + "\" has successfully progressed."
                                       : "Deployment \"" + name + "\" is progressing: " + std::to_string(updated) + " updated, " +
                                         std::to_string(available) + " available of " + std::to_string(desired)));
        conds.arr_val.push_back(std::move(p));
    }
    status.set("conditions", std::move(conds));
    std::string new_status = status.to_string();
    std::string old_status = (prev && prev->is_object()) ? prev->to_string() : "";
    if (new_status != old_status) {
        dep.set("status", std::move(status));
        store_.put(key, dep.to_string(), true);
    }
}

} // namespace kube32::controller
