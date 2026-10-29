#include "controller/workload_controller.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>

namespace kube32::controller {

static models::JsonValue jstr(const std::string& v) { return models::JsonValue{models::JsonType::String, false, 0, v, {}, {}}; }
static models::JsonValue jnum(double v) { return models::JsonValue{models::JsonType::Number, false, v, "", {}, {}}; }
static models::JsonValue jbool(bool v) { return models::JsonValue{models::JsonType::Boolean, v, 0, "", {}, {}}; }
static models::JsonValue jobj() { models::JsonValue o; o.type = models::JsonType::Object; return o; }

static const char* const kHostnameLabel = "kubernetes.io/hostname";
static const char* const kRevisionLabel = "controller-revision-hash";

// Revision of a DaemonSet's pod template (FNV-1a, kube's no-vowel alphabet)
static std::string template_revision(const std::string& template_json) {
    uint32_t h = 2166136261u;
    for (unsigned char c : template_json) { h ^= c; h *= 16777619u; }
    static const char* alphabet = "bcdfghjklmnpqrstvwxz2456789";
    std::string out;
    for (int i = 0; i < 10; ++i) { out += alphabet[h % 27]; h /= 27; if (h == 0) h = 2166136261u ^ static_cast<uint32_t>(i); }
    return out;
}

// One pod as the controllers need it, parsed once per pass.
struct WorkloadController::PodView {
    std::string key;
    std::string json;
    std::string ns;
    std::string owner_kind;
    std::string owner_uid;
    std::string node;          // spec.nodeName ("" while unbound)
    std::string target_node;   // the node a DaemonSet pod is meant for
    std::string phase;
    std::string revision;      // controller-revision-hash label
    bool terminating{false};
    bool ready{false};
};

static bool pod_ready(const models::JsonValue& pod) {
    const models::JsonValue* st = pod.get("status");
    if (!st || st->get_string("phase", "") != "Running") return false;
    const models::JsonValue* conds = st->get("conditions");
    if (conds && conds->is_array()) {
        for (const auto& c : conds->arr_val) {
            if (c.get_string("type", "") == "Ready") return c.get_string("status", "") == "True";
        }
    }
    return false;
}

static bool labels_match(const models::JsonValue* selector, const models::JsonValue* labels) {
    if (!selector || !selector->is_object()) return true;
    for (const auto& kv : selector->obj_val) {
        std::string want = kv.second.is_string() ? kv.second.str_val : "";
        std::string have = labels ? labels->get_string(kv.first, "") : "";
        if (!labels || !labels->get(kv.first) || have != want) return false;
    }
    return true;
}

WorkloadController::WorkloadController(storage::Store& store, scheduler::Scheduler* scheduler, WorkloadControllerConfig cfg)
    : store_(store), scheduler_(scheduler), cfg_(cfg), events_(store, 64) {}

WorkloadController::~WorkloadController() {
    stop();
}

bool WorkloadController::start() {
    if (running_.load()) return true;
    running_.store(true);
    thread_ = std::make_unique<osal::Thread>();
    osal::Thread::Config tc;
    tc.name = "k32_workload";
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

void WorkloadController::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (thread_ && thread_->is_joinable()) {
        thread_->join();
        thread_.reset();
    }
}

void WorkloadController::task_entry(void* arg) {
    static_cast<WorkloadController*>(arg)->run_loop();
}

void WorkloadController::run_loop() {
    while (running_.load()) {
        try {
            reconcile();
        } catch (const std::exception& e) {
            std::cout << "[WORKLOAD] pass skipped: " << e.what() << std::endl;
        }
        osal::System::feed_watchdog();
        osal::Thread::sleep_ms(cfg_.reconcile_interval_ms);
    }
}

void WorkloadController::record(const std::string& kind, const std::string& api, const std::string& name, const std::string& ns,
                                const std::string& uid, const char* reason, const std::string& message, bool warning) {
    api::EventSpec ev;
    ev.involved.kind = kind;
    ev.involved.name = name;
    ev.involved.namespace_name = ns;
    ev.involved.uid = uid;
    ev.involved.api_version = api;
    ev.reason = reason;
    ev.message = message;
    ev.warning = warning;
    ev.component = (kind == "Job") ? "job-controller" : "daemonset-controller";
    events_.record(ev);
}

void WorkloadController::delete_pod(const std::string& pod_key, const std::string& pod_json) {
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

std::string WorkloadController::create_pod(const std::string& owner_kind, const std::string& owner_api, const std::string& owner_name,
                                           const std::string& owner_uid, const std::string& ns, const std::string& template_json,
                                           const std::string& pin_node, const std::string& revision) {
    models::JsonValue tmpl;
    std::string err;
    if (!models::parse_json(template_json, tmpl, err) || !tmpl.is_object()) return "";
    models::JsonValue pod = jobj();
    pod.set("kind", jstr("Pod"));
    pod.set("apiVersion", jstr("v1"));
    models::JsonValue pmeta = jobj();
    const models::JsonValue* tmeta = tmpl.get("metadata");
    if (tmeta && tmeta->is_object()) pmeta = *tmeta;
    const std::string pod_name = owner_name + "-" + models::generate_uid().substr(0, 5);
    pmeta.set("name", jstr(pod_name));
    pmeta.set("generateName", jstr(owner_name + "-"));
    pmeta.set("namespace", jstr(ns));
    pmeta.set("uid", jstr(models::generate_uid()));
    std::string now = models::now_rfc3339();
    if (!now.empty()) pmeta.set("creationTimestamp", jstr(now));
    if (!revision.empty()) {
        models::JsonValue* labels = pmeta.get_mut("labels");
        if (!labels || !labels->is_object()) { pmeta.set("labels", jobj()); labels = pmeta.get_mut("labels"); }
        labels->set(kRevisionLabel, jstr(revision));
    }
    if (owner_kind == "Job") {
        models::JsonValue* labels = pmeta.get_mut("labels");
        if (!labels || !labels->is_object()) { pmeta.set("labels", jobj()); labels = pmeta.get_mut("labels"); }
        labels->set("job-name", jstr(owner_name));
        labels->set("batch.kubernetes.io/job-name", jstr(owner_name));
        labels->set("controller-uid", jstr(owner_uid));
    }
    models::JsonValue refs;
    refs.type = models::JsonType::Array;
    models::JsonValue ref = jobj();
    ref.set("apiVersion", jstr(owner_api));
    ref.set("kind", jstr(owner_kind));
    ref.set("name", jstr(owner_name));
    ref.set("uid", jstr(owner_uid));
    ref.set("controller", jbool(true));
    ref.set("blockOwnerDeletion", jbool(true));
    refs.arr_val.push_back(std::move(ref));
    pmeta.set("ownerReferences", std::move(refs));
    pod.set("metadata", std::move(pmeta));

    models::JsonValue pspec = jobj();
    const models::JsonValue* tspec = tmpl.get("spec");
    if (tspec && tspec->is_object()) pspec = *tspec;
    pspec.remove("nodeName");
    if (!pin_node.empty()) {
        // The scheduler binds a DaemonSet pod to its node, and only when the
        // node has room for it
        models::JsonValue* sel = pspec.get_mut("nodeSelector");
        if (!sel || !sel->is_object()) { pspec.set("nodeSelector", jobj()); sel = pspec.get_mut("nodeSelector"); }
        sel->set(kHostnameLabel, jstr(pin_node));
    }
    pod.set("spec", std::move(pspec));
    models::JsonValue st = jobj();
    st.set("phase", jstr("Pending"));
    pod.set("status", std::move(st));

    std::string pod_json = pod.to_string();
    if (scheduler_) scheduler_->schedule_pod_manifest(pod_json);
    store_.put("/registry/pods/" + ns + "/" + pod_name, pod_json, false);
    record(owner_kind, owner_api, owner_name, ns, owner_uid, "SuccessfulCreate", "Created pod: " + pod_name, false);
    return pod_name;
}

void WorkloadController::reconcile() {
    std::vector<std::string> pod_jsons, pod_keys;
    store_.list_extended("/registry/pods/", pod_jsons, &pod_keys);
    std::vector<PodView> pods;
    pods.reserve(pod_jsons.size());
    for (size_t i = 0; i < pod_jsons.size(); ++i) {
        models::JsonValue p;
        std::string err;
        if (!models::parse_json(pod_jsons[i], p, err) || !p.is_object()) continue;
        const models::JsonValue* meta = p.get("metadata");
        const models::JsonValue* refs = meta ? meta->get("ownerReferences") : nullptr;
        if (!refs || !refs->is_array()) continue;
        PodView v;
        for (const auto& r : refs->arr_val) {
            std::string kind = r.get_string("kind", "");
            if (kind == "DaemonSet" || kind == "Job") { v.owner_kind = kind; v.owner_uid = r.get_string("uid", ""); break; }
        }
        if (v.owner_kind.empty()) continue;
        v.key = pod_keys[i];
        v.json = pod_jsons[i];
        v.ns = meta->get_string("namespace", "default");
        v.terminating = !meta->get_string("deletionTimestamp", "").empty();
        const models::JsonValue* spec = p.get("spec");
        v.node = spec ? spec->get_string("nodeName", "") : "";
        const models::JsonValue* sel = spec ? spec->get("nodeSelector") : nullptr;
        v.target_node = sel ? sel->get_string(kHostnameLabel, "") : "";
        if (v.target_node.empty()) v.target_node = v.node;
        const models::JsonValue* st = p.get("status");
        v.phase = st ? st->get_string("phase", "") : "";
        v.ready = pod_ready(p);
        const models::JsonValue* plabels = meta->get("labels");
        v.revision = plabels ? plabels->get_string(kRevisionLabel, "") : "";
        pods.push_back(std::move(v));
    }

    std::set<std::string> live;
    std::vector<std::string> node_jsons;
    store_.list("/registry/nodes/", node_jsons);

    std::vector<std::string> jsons, keys;
    store_.list_extended("/registry/daemonsets/", jsons, &keys);
    for (size_t i = 0; i < jsons.size(); ++i) {
        osal::Thread::yield();
        models::JsonValue d;
        std::string err;
        if (!models::parse_json(jsons[i], d, err) || !d.is_object()) continue;
        const models::JsonValue* meta = d.get("metadata");
        if (meta) live.insert(meta->get_string("uid", ""));
        reconcile_daemonset(keys[i], jsons[i], node_jsons, pods);
    }
    jsons.clear();
    keys.clear();
    store_.list_extended("/registry/jobs/", jsons, &keys);
    for (size_t i = 0; i < jsons.size(); ++i) {
        osal::Thread::yield();
        models::JsonValue j;
        std::string err;
        if (!models::parse_json(jsons[i], j, err) || !j.is_object()) continue;
        const models::JsonValue* meta = j.get("metadata");
        if (meta) live.insert(meta->get_string("uid", ""));
        reconcile_job(keys[i], jsons[i], pods);
    }

    // Pods whose DaemonSet or Job is gone go with it
    for (const auto& p : pods) {
        if (p.terminating || p.owner_uid.empty() || live.count(p.owner_uid)) continue;
        delete_pod(p.key, p.json);
    }
}

void WorkloadController::reconcile_daemonset(const std::string& key, const std::string& json, const std::vector<std::string>& node_jsons,
                                             const std::vector<PodView>& pods) {
    models::JsonValue ds;
    std::string err;
    if (!models::parse_json(json, ds, err) || !ds.is_object()) return;
    const models::JsonValue* meta = ds.get("metadata");
    const models::JsonValue* spec = ds.get("spec");
    const models::JsonValue* tmpl = spec ? spec->get("template") : nullptr;
    if (!meta || !tmpl || !tmpl->is_object()) return;
    const std::string name = meta->get_string("name", "");
    const std::string ns = meta->get_string("namespace", "default");
    const std::string uid = meta->get_string("uid", "");
    if (name.empty() || uid.empty()) return;
    const models::JsonValue* tspec = tmpl->get("spec");
    const models::JsonValue* selector = tspec ? tspec->get("nodeSelector") : nullptr;

    // Nodes this DaemonSet should run on
    std::set<std::string> eligible;
    for (const auto& nj : node_jsons) {
        models::JsonValue n;
        if (!models::parse_json(nj, n, err) || !n.is_object()) continue;
        const models::JsonValue* nmeta = n.get("metadata");
        std::string nname = nmeta ? nmeta->get_string("name", "") : "";
        if (nname.empty()) continue;
        if (labels_match(selector, nmeta->get("labels"))) eligible.insert(nname);
    }

    const std::string revision = template_revision(tmpl->to_string());
    std::map<std::string, std::vector<const PodView*>> by_node;
    std::set<std::string> draining;   // nodes where a pod of this DaemonSet is still terminating
    for (const auto& p : pods) {
        if (p.owner_kind != "DaemonSet" || p.owner_uid != uid) continue;
        if (p.terminating) { draining.insert(p.target_node); continue; }
        by_node[p.target_node].push_back(&p);
    }

    int64_t scheduled = 0, ready = 0, misscheduled = 0, updated = 0, unavailable = 0;
    std::vector<const PodView*> stale;
    for (const auto& node : eligible) {
        auto it = by_node.find(node);
        if (it == by_node.end() || it->second.empty()) {
            // The replacement waits until the old pod has left the node
            if (!draining.count(node)) create_pod("DaemonSet", "apps/v1", name, uid, ns, tmpl->to_string(), node, revision);
            ++unavailable;
            continue;
        }
        // One pod per node: keep the first, remove duplicates
        for (size_t k = 1; k < it->second.size(); ++k) {
            delete_pod(it->second[k]->key, it->second[k]->json);
            record("DaemonSet", "apps/v1", name, ns, uid, "SuccessfulDelete", "Deleted pod: " + it->second[k]->key.substr(it->second[k]->key.rfind('/') + 1), false);
        }
        const PodView* p = it->second.front();
        if (p->node == node) ++scheduled;
        if (p->ready) ++ready; else ++unavailable;
        if (p->revision == revision) ++updated; else stale.push_back(p);
    }
    for (const auto& entry : by_node) {
        if (eligible.count(entry.first)) continue;
        for (const PodView* p : entry.second) {
            if (!p->node.empty()) ++misscheduled;
            delete_pod(p->key, p->json);
            record("DaemonSet", "apps/v1", name, ns, uid, "SuccessfulDelete", "Deleted pod: " + p->key.substr(p->key.rfind('/') + 1), false);
        }
    }

    // Rolling update: replace pods of an older template while at most
    // maxUnavailable (default 1) nodes are without a ready pod; OnDelete
    // leaves them to the user
    const models::JsonValue* strategy = spec->get("updateStrategy");
    const std::string stype = strategy ? strategy->get_string("type", "RollingUpdate") : "RollingUpdate";
    if (stype != "OnDelete" && !stale.empty()) {
        int64_t max_unavailable = 1;
        const models::JsonValue* ru = strategy ? strategy->get("rollingUpdate") : nullptr;
        const models::JsonValue* mu = ru ? ru->get("maxUnavailable") : nullptr;
        if (mu && mu->is_string() && !mu->str_val.empty() && mu->str_val.back() == '%') {
            const int64_t pct = std::atoi(mu->str_val.c_str());
            max_unavailable = (static_cast<int64_t>(eligible.size()) * pct + 99) / 100;
        } else if (mu && !mu->is_string()) {
            max_unavailable = ru->get_int("maxUnavailable", 1);
        }
        if (max_unavailable < 1) max_unavailable = 1;
        // Pods that are not ready go first: replacing them costs no availability
        std::stable_sort(stale.begin(), stale.end(), [](const PodView* a, const PodView* b) { return !a->ready && b->ready; });
        for (const PodView* p : stale) {
            if (p->ready && unavailable >= max_unavailable) break;
            delete_pod(p->key, p->json);
            record("DaemonSet", "apps/v1", name, ns, uid, "SuccessfulDelete",
                   "Deleted pod: " + p->key.substr(p->key.rfind('/') + 1) + " (template updated)", false);
            if (p->ready) ++unavailable;
        }
    }

    const int64_t desired = static_cast<int64_t>(eligible.size());
    models::JsonValue status = jobj();
    status.set("observedGeneration", jnum(static_cast<double>(meta->get_int("generation", 1))));
    status.set("desiredNumberScheduled", jnum(static_cast<double>(desired)));
    status.set("currentNumberScheduled", jnum(static_cast<double>(scheduled)));
    status.set("updatedNumberScheduled", jnum(static_cast<double>(updated)));
    status.set("numberMisscheduled", jnum(static_cast<double>(misscheduled)));
    status.set("numberReady", jnum(static_cast<double>(ready)));
    status.set("numberAvailable", jnum(static_cast<double>(ready)));
    if (desired > ready) status.set("numberUnavailable", jnum(static_cast<double>(desired - ready)));
    const models::JsonValue* prev = ds.get("status");
    if (!prev || prev->to_string() != status.to_string()) {
        ds.set("status", std::move(status));
        store_.put(key, ds.to_string(), true);
    }
}

void WorkloadController::reconcile_job(const std::string& key, const std::string& json, const std::vector<PodView>& pods) {
    models::JsonValue job;
    std::string err;
    if (!models::parse_json(json, job, err) || !job.is_object()) return;
    const models::JsonValue* meta = job.get("metadata");
    const models::JsonValue* spec = job.get("spec");
    const models::JsonValue* tmpl = spec ? spec->get("template") : nullptr;
    if (!meta || !tmpl || !tmpl->is_object()) return;
    const std::string name = meta->get_string("name", "");
    const std::string ns = meta->get_string("namespace", "default");
    const std::string uid = meta->get_string("uid", "");
    if (name.empty() || uid.empty()) return;
    int64_t completions = spec->get_int("completions", 1);
    int64_t parallelism = spec->get_int("parallelism", 1);
    int64_t backoff_limit = spec->get_int("backoffLimit", 6);
    if (completions < 1) completions = 1;
    if (parallelism < 0) parallelism = 0;

    std::vector<const PodView*> active;
    int64_t succeeded = 0, failed = 0, ready = 0;
    for (const auto& p : pods) {
        if (p.owner_kind != "Job" || p.owner_uid != uid) continue;
        if (p.phase == "Succeeded") ++succeeded;
        else if (p.phase == "Failed") ++failed;
        else if (!p.terminating) {
            active.push_back(&p);
            if (p.ready) ++ready;
        }
    }

    const models::JsonValue* prev = job.get("status");
    models::JsonValue status = jobj();
    if (prev && prev->is_object()) status = *prev;
    auto finished_as = [&](const char* type) {
        const models::JsonValue* conds = status.get("conditions");
        if (!conds || !conds->is_array()) return false;
        for (const auto& c : conds->arr_val) {
            if (c.get_string("type", "") == type && c.get_string("status", "") == "True") return true;
        }
        return false;
    };
    const std::string now = models::now_rfc3339();
    auto add_condition = [&](const char* type, const char* reason, const std::string& message) {
        models::JsonValue conds;
        conds.type = models::JsonType::Array;
        const models::JsonValue* old = status.get("conditions");
        if (old && old->is_array()) conds = *old;
        models::JsonValue c = jobj();
        c.set("type", jstr(type));
        c.set("status", jstr("True"));
        c.set("reason", jstr(reason));
        c.set("message", jstr(message));
        if (!now.empty()) { c.set("lastProbeTime", jstr(now)); c.set("lastTransitionTime", jstr(now)); }
        conds.arr_val.push_back(std::move(c));
        status.set("conditions", std::move(conds));
    };

    bool done = finished_as("Complete") || finished_as("Failed");
    if (!done) {
        if (!status.get("startTime") && !now.empty()) status.set("startTime", jstr(now));
        if (succeeded >= completions) {
            add_condition("Complete", "CompletionsReached", "Reached expected number of succeeded pods");
            if (!now.empty()) status.set("completionTime", jstr(now));
            record("Job", "batch/v1", name, ns, uid, "Completed", "Job completed", false);
            done = true;
        } else if (failed > backoff_limit) {
            add_condition("Failed", "BackoffLimitExceeded", "Job has reached the specified backoff limit");
            record("Job", "batch/v1", name, ns, uid, "BackoffLimitExceeded", "Job has reached the specified backoff limit", true);
            for (const PodView* p : active) delete_pod(p->key, p->json);
            active.clear();
            ready = 0;
            done = true;
        } else {
            int64_t want = std::min(parallelism, completions - succeeded);
            for (int64_t n = static_cast<int64_t>(active.size()); n < want; ++n) {
                create_pod("Job", "batch/v1", name, uid, ns, tmpl->to_string(), "", "");
            }
        }
    }

    std::vector<std::string> fresh_jsons, fresh_keys;
    int64_t active_now = static_cast<int64_t>(active.size());
    if (!done) {
        // Pods created above are active too
        store_.list_extended("/registry/pods/" + ns + "/", fresh_jsons, &fresh_keys);
        active_now = 0;
        for (const auto& pj : fresh_jsons) {
            models::JsonValue p;
            if (!models::parse_json(pj, p, err) || !p.is_object() || !models::has_owner(p, "Job", uid)) continue;
            const models::JsonValue* st = p.get("status");
            std::string phase = st ? st->get_string("phase", "") : "";
            const models::JsonValue* pm = p.get("metadata");
            bool terminating = pm && !pm->get_string("deletionTimestamp", "").empty();
            if (phase != "Succeeded" && phase != "Failed" && !terminating) ++active_now;
        }
    }
    status.set("active", jnum(static_cast<double>(active_now)));
    status.set("ready", jnum(static_cast<double>(ready)));
    status.set("succeeded", jnum(static_cast<double>(succeeded)));
    status.set("failed", jnum(static_cast<double>(failed)));
    if (!prev || prev->to_string() != status.to_string()) {
        job.set("status", std::move(status));
        store_.put(key, job.to_string(), true);
    }
}

} // namespace kube32::controller
