// API server behaviour kubectl relies on: bearer-token authentication,
// ?dryRun=All, limit/continue pagination, patch media types, strict field
// validation of events.k8s.io Events, Endpoints and EndpointSlices computed
// from Services, metadata.generation, Deployment conditions, Lease renewTime
// stamping and timestamp backfill.

#include "api/route_handler.hpp"
#include "api/timestamp_backfill.hpp"
#include "controller/deployment_controller.hpp"
#include "mem/monotonic_arena.hpp"
#include "models/kube_models.hpp"
#include "storage/store.hpp"
#include "storage/flash_log.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace kube32;
using namespace kube32::api;

static HttpResponse call(RouteHandler& h, HttpMethod m, const std::string& path, const std::string& body = "",
                         const std::string& query = "", const std::string& auth = "", const std::string& content_type = "application/json",
                         bool table = false) {
    mem::MonotonicArena arena;
    HttpRequest req;
    req.method = m;
    req.path = path;
    req.query_string = query;
    req.body = body;
    req.authorization = auth;
    req.content_type = content_type;
    req.as_table = table;
    return h.handle_request(req, arena);
}

static models::JsonValue parse(const std::string& json) {
    models::JsonValue doc;
    std::string err;
    bool ok = models::parse_json(json, doc, err);
    assert(ok);
    return doc;
}

static std::unique_ptr<storage::Store> ram_store() {
    storage::StoreOptions o;
    o.enable_flash_persistence = false;
    return storage::create_radix_store(o);
}

static const char* kConfigMap = R"({"apiVersion":"v1","kind":"ConfigMap","metadata":{"name":"settings"},"data":{"a":"1"}})";

static void test_bearer_token_authentication() {
    std::cout << "[RUN] test_bearer_token_authentication" << std::endl;
    auto store = ram_store();
    RouteHandler h(*store);
    // Off by default: everything answers
    assert(call(h, HttpMethod::Get, "/api/v1/pods").status_code == 200);

    h.set_bearer_token("s3cret-token-0123456789");
    HttpResponse denied = call(h, HttpMethod::Get, "/api/v1/pods");
    assert(denied.status_code == 401);
    models::JsonValue st = parse(denied.body);
    assert(st.get_string("kind", "") == "Status" && st.get_string("reason", "") == "Unauthorized" && st.get_int("code", 0) == 401);
    assert(call(h, HttpMethod::Get, "/api/v1/pods", "", "", "Bearer wrong-token").status_code == 401);
    assert(call(h, HttpMethod::Get, "/api/v1/pods", "", "", "Bearer s3cret-token-0123456789x").status_code == 401);
    assert(call(h, HttpMethod::Get, "/api/v1/pods", "", "", "Basic czNjcmV0").status_code == 401);
    assert(call(h, HttpMethod::Get, "/api/v1/pods", "", "", "Bearer s3cret-token-0123456789").status_code == 200);
    assert(call(h, HttpMethod::Get, "/api/v1/pods", "", "", "bearer  s3cret-token-0123456789 ").status_code == 200);
    // Watches and writes go through the same check
    assert(call(h, HttpMethod::Get, "/api/v1/pods", "", "watch=true").status_code == 401);
    assert(call(h, HttpMethod::Post, "/api/v1/namespaces/default/configmaps", kConfigMap).status_code == 401);
    // Probes and /version stay open
    for (const char* p : {"/healthz", "/livez", "/readyz", "/readyz/ping", "/version"}) {
        assert(call(h, HttpMethod::Get, p).status_code == 200);
    }
    HttpResponse review = call(h, HttpMethod::Post, "/apis/authorization.k8s.io/v1/selfsubjectaccessreviews",
                               R"({"spec":{"resourceAttributes":{"verb":"list","resource":"pods"}}})", "", "Bearer s3cret-token-0123456789");
    assert(review.status_code == 201 && review.body.find("\"allowed\":true") != std::string::npos);
    assert(call(h, HttpMethod::Post, "/apis/authorization.k8s.io/v1/selfsubjectaccessreviews", "{}").status_code == 401);
    std::cout << "[PASS] test_bearer_token_authentication" << std::endl;
}

static void test_dry_run_stores_nothing() {
    std::cout << "[RUN] test_dry_run_stores_nothing" << std::endl;
    auto store = ram_store();
    RouteHandler h(*store);
    const uint64_t v0 = store->current_version();

    HttpResponse created = call(h, HttpMethod::Post, "/api/v1/namespaces/default/configmaps", kConfigMap, "dryRun=All");
    assert(created.status_code == 201);
    models::JsonValue doc = parse(created.body);
    assert(!doc.get("metadata")->get_string("uid", "").empty());
    std::string out;
    assert(!store->get("/registry/configmaps/default/settings", out).ok());
    assert(store->current_version() == v0);

    const char* pod = R"({"apiVersion":"v1","kind":"Pod","metadata":{"name":"p"},"spec":{"containers":[{"name":"c","image":"wasm://x"}]}})";
    assert(call(h, HttpMethod::Post, "/api/v1/namespaces/default/pods", pod, "dryRun=All").status_code == 201);
    assert(!store->get("/registry/pods/default/p", out).ok());
    assert(call(h, HttpMethod::Post, "/api/v1/namespaces/default/configmaps", kConfigMap, "dryRun=Some").status_code == 400);

    // Against a stored object: conflicts are still reported, nothing changes
    assert(call(h, HttpMethod::Post, "/api/v1/namespaces/default/configmaps", kConfigMap).status_code == 201);
    std::string before;
    store->get("/registry/configmaps/default/settings", before);
    assert(call(h, HttpMethod::Post, "/api/v1/namespaces/default/configmaps", kConfigMap, "dryRun=All").status_code == 409);
    HttpResponse patched = call(h, HttpMethod::Patch, "/api/v1/namespaces/default/configmaps/settings", R"({"data":{"a":"2"}})",
                                "dryRun=All", "", "application/merge-patch+json");
    assert(patched.status_code == 200 && patched.body.find("\"a\":\"2\"") != std::string::npos);
    assert(call(h, HttpMethod::Put, "/api/v1/namespaces/default/configmaps/settings",
                R"({"apiVersion":"v1","kind":"ConfigMap","metadata":{"name":"settings"},"data":{"a":"3"}})", "dryRun=All").status_code == 200);
    assert(call(h, HttpMethod::Delete, "/api/v1/namespaces/default/configmaps/settings", "", "dryRun=All").status_code == 200);
    // kubectl delete --dry-run=server puts it in DeleteOptions
    assert(call(h, HttpMethod::Delete, "/api/v1/namespaces/default/configmaps/settings",
                R"({"kind":"DeleteOptions","apiVersion":"v1","dryRun":["All"]})").status_code == 200);
    std::string after;
    assert(store->get("/registry/configmaps/default/settings", after).ok());
    assert(after == before);
    std::cout << "[PASS] test_dry_run_stores_nothing" << std::endl;
}

static void test_list_pagination() {
    std::cout << "[RUN] test_list_pagination" << std::endl;
    auto store = ram_store();
    RouteHandler h(*store);
    for (int i = 0; i < 7; ++i) {
        std::string cm = R"({"apiVersion":"v1","kind":"ConfigMap","metadata":{"name":"cm-)" + std::to_string(i) + R"("}})";
        assert(call(h, HttpMethod::Post, "/api/v1/namespaces/default/configmaps", cm).status_code == 201);
    }
    std::vector<std::string> names;
    std::string cont;
    int pages = 0;
    do {
        HttpResponse r = call(h, HttpMethod::Get, "/api/v1/namespaces/default/configmaps", "",
                              "limit=3" + (cont.empty() ? std::string() : "&continue=" + cont));
        assert(r.status_code == 200);
        models::JsonValue list = parse(r.body);
        const models::JsonValue* items = list.get("items");
        assert(items->arr_val.size() <= 3);
        for (const auto& it : items->arr_val) names.push_back(it.get("metadata")->get_string("name", ""));
        cont = list.get("metadata")->get_string("continue", "");
        if (!cont.empty()) assert(list.get("metadata")->get_int("remainingItemCount", 0) == 7 - static_cast<int64_t>(names.size()));
        ++pages;
    } while (!cont.empty() && pages < 10);
    assert(pages == 3);
    assert(names.size() == 7);
    for (size_t i = 1; i < names.size(); ++i) assert(names[i - 1] < names[i]);
    // A limit above the size returns everything without a token
    HttpResponse all = call(h, HttpMethod::Get, "/api/v1/namespaces/default/configmaps", "", "limit=500");
    assert(parse(all.body).get("metadata")->get_string("continue", "").empty());
    assert(call(h, HttpMethod::Get, "/api/v1/namespaces/default/configmaps", "", "limit=2&continue=%%%").status_code == 400);
    // Tables carry the token too (kubectl get --chunk-size)
    HttpResponse table = call(h, HttpMethod::Get, "/api/v1/namespaces/default/configmaps", "", "limit=2", "", "application/json", true);
    assert(table.body.find("\"kind\":\"Table\"") != std::string::npos && table.body.find("\"continue\":\"") != std::string::npos);
    std::cout << "[PASS] test_list_pagination" << std::endl;
}

static void test_patch_media_types() {
    std::cout << "[RUN] test_patch_media_types" << std::endl;
    auto store = ram_store();
    RouteHandler h(*store);
    assert(call(h, HttpMethod::Post, "/api/v1/namespaces/default/configmaps", kConfigMap).status_code == 201);
    HttpResponse ssa = call(h, HttpMethod::Patch, "/api/v1/namespaces/default/configmaps/settings", "apiVersion: v1\nkind: ConfigMap\n",
                            "fieldManager=kubectl&force=false", "", "application/apply-patch+yaml");
    assert(ssa.status_code == 415);
    assert(ssa.body.find("server-side apply is not supported") != std::string::npos);
    assert(call(h, HttpMethod::Patch, "/api/v1/namespaces/default/configmaps/settings", R"([{"op":"remove","path":"/data"}])", "", "",
                "application/json-patch+json").status_code == 415);
    assert(call(h, HttpMethod::Patch, "/api/v1/namespaces/default/configmaps/settings", R"({"data":{"b":"2"}})", "", "",
                "application/strategic-merge-patch+json").status_code == 200);
    std::cout << "[PASS] test_patch_media_types" << std::endl;
}

static void test_events_v1_strict_fields() {
    std::cout << "[RUN] test_events_v1_strict_fields" << std::endl;
    auto store = ram_store();
    RouteHandler h(*store);
    const std::string ev = R"({"apiVersion":"events.k8s.io/v1","kind":"Event","metadata":{"name":"e1","namespace":"default"},
        "regarding":{"kind":"Pod","name":"web","namespace":"default"},"note":"pulled","reason":"Pulled","type":"Normal",
        "action":"Pull","reportingController":"kubelet","reportingInstance":"node-a","eventTime":"2026-10-28T10:00:00.000000Z"})";
    HttpResponse r = call(h, HttpMethod::Post, "/apis/events.k8s.io/v1/namespaces/default/events", ev, "fieldValidation=Strict");
    assert(r.status_code == 201);
    models::JsonValue back = parse(r.body);
    assert(back.get_string("note", "") == "pulled" && back.get_string("action", "") == "Pull");
    assert(back.get("regarding")->get_string("name", "") == "web");
    std::string bad = ev;
    bad.insert(bad.size() - 1, R"(,"bogus":1)");
    HttpResponse rej = call(h, HttpMethod::Post, "/apis/events.k8s.io/v1/namespaces/default/events", bad, "fieldValidation=Strict");
    assert(rej.status_code == 400 && rej.body.find("bogus") != std::string::npos);
    // core/v1 Events keep their own field set
    assert(call(h, HttpMethod::Post, "/api/v1/namespaces/default/events", ev, "fieldValidation=Strict").status_code == 400);
    std::cout << "[PASS] test_events_v1_strict_fields" << std::endl;
}

static void test_endpoints_from_services() {
    std::cout << "[RUN] test_endpoints_from_services" << std::endl;
    auto store = ram_store();
    RouteHandler h(*store);
    assert(call(h, HttpMethod::Post, "/api/v1/namespaces/default/services",
                R"({"apiVersion":"v1","kind":"Service","metadata":{"name":"web","labels":{"tier":"front"}},
                    "spec":{"selector":{"app":"web"},"ports":[{"name":"http","port":80,"targetPort":"http"}]}})").status_code == 201);
    store->put("/registry/pods/default/web-1", R"({"kind":"Pod","metadata":{"name":"web-1","namespace":"default","uid":"u1","labels":{"app":"web"}},
        "spec":{"nodeName":"node-a","containers":[{"name":"c","ports":[{"name":"http","containerPort":8080}]}]},
        "status":{"phase":"Running","podIP":"10.0.0.5","conditions":[{"type":"Ready","status":"True"}]}})", false);
    store->put("/registry/pods/default/web-2", R"({"kind":"Pod","metadata":{"name":"web-2","namespace":"default","labels":{"app":"web"}},
        "spec":{"nodeName":"node-b","containers":[{"name":"c","ports":[{"name":"http","containerPort":8080}]}]},
        "status":{"phase":"Pending","podIP":"10.0.0.6"}})", false);
    store->put("/registry/pods/default/other", R"({"kind":"Pod","metadata":{"name":"other","namespace":"default","labels":{"app":"db"}},
        "spec":{"containers":[{"name":"c"}]},"status":{"phase":"Running","podIP":"10.0.0.7"}})", false);

    HttpResponse ep = call(h, HttpMethod::Get, "/api/v1/namespaces/default/endpoints/web");
    assert(ep.status_code == 200);
    models::JsonValue e = parse(ep.body);
    const models::JsonValue& sub = e.get("subsets")->arr_val.at(0);
    assert(sub.get("addresses")->arr_val.size() == 1 && sub.get("addresses")->arr_val[0].get_string("ip", "") == "10.0.0.5");
    assert(sub.get("addresses")->arr_val[0].get("targetRef")->get_string("name", "") == "web-1");
    assert(sub.get("notReadyAddresses")->arr_val.size() == 1);
    assert(sub.get("ports")->arr_val[0].get_int("port", 0) == 8080);
    assert(e.get("metadata")->get("labels")->get_string("tier", "") == "front");
    assert(call(h, HttpMethod::Get, "/api/v1/namespaces/default/endpoints/nothing").status_code == 404);
    assert(call(h, HttpMethod::Delete, "/api/v1/namespaces/default/endpoints/web").status_code == 405);

    HttpResponse slices = call(h, HttpMethod::Get, "/apis/discovery.k8s.io/v1/namespaces/default/endpointslices", "",
                               "labelSelector=kubernetes.io%2Fservice-name%3Dweb");
    assert(slices.status_code == 200);
    models::JsonValue sl = parse(slices.body);
    assert(sl.get_string("kind", "") == "EndpointSliceList" && sl.get("items")->arr_val.size() == 1);
    const models::JsonValue& slice = sl.get("items")->arr_val[0];
    assert(slice.get_string("addressType", "") == "IPv4" && slice.get("endpoints")->arr_val.size() == 2);
    assert(slice.get("ports")->arr_val[0].get_int("port", 0) == 8080);

    HttpResponse table = call(h, HttpMethod::Get, "/api/v1/namespaces/default/endpoints", "", "", "", "application/json", true);
    assert(table.body.find("\"kind\":\"Table\"") != std::string::npos && table.body.find("10.0.0.5:8080") != std::string::npos);
    assert(call(h, HttpMethod::Get, "/api/v1").body.find("\"name\":\"endpoints\"") != std::string::npos);
    assert(call(h, HttpMethod::Get, "/apis").body.find("discovery.k8s.io") != std::string::npos);
    std::cout << "[PASS] test_endpoints_from_services" << std::endl;
}

static void test_generation_and_deployment_conditions() {
    std::cout << "[RUN] test_generation_and_deployment_conditions" << std::endl;
    auto store = ram_store();
    RouteHandler h(*store);
    const char* dep = R"({"apiVersion":"apps/v1","kind":"Deployment","metadata":{"name":"web"},
        "spec":{"replicas":2,"selector":{"matchLabels":{"app":"web"}},"template":{"metadata":{"labels":{"app":"web"}},
        "spec":{"containers":[{"name":"c","image":"wasm://embedded/echo:v1"}]}}}})";
    HttpResponse created = call(h, HttpMethod::Post, "/apis/apps/v1/namespaces/default/deployments", dep);
    assert(created.status_code == 201 && parse(created.body).get("metadata")->get_int("generation", 0) == 1);

    controller::DeploymentController dc(*store, nullptr);
    dc.reconcile();
    std::string stored;
    store->get("/registry/deployments/default/web", stored);
    models::JsonValue d = parse(stored);
    assert(stored.find("ReplicaSet \"") == std::string::npos);
    const models::JsonValue* conds = d.get("status")->get("conditions");
    bool saw_available = false;
    for (const auto& c : conds->arr_val) {
        if (c.get_string("type", "") == "Available") {
            saw_available = true;
            assert(c.get_string("status", "") == "False" && c.get_string("reason", "") == "MinimumReplicasUnavailable");
        }
        if (c.get_string("type", "") == "Progressing") assert(c.get_string("reason", "") != "NewReplicaSetAvailable");
    }
    assert(saw_available);

    // Pods become ready: the rollout is complete
    std::vector<std::string> pods, keys;
    store->list_extended("/registry/pods/", pods, &keys);
    assert(pods.size() == 2);
    for (size_t i = 0; i < pods.size(); ++i) {
        models::JsonValue p = parse(pods[i]);
        models::JsonValue st = parse(R"({"phase":"Running","conditions":[{"type":"Ready","status":"True"}]})");
        p.set("status", st);
        store->put(keys[i], p.to_string(), true);
    }
    dc.reconcile();
    store->get("/registry/deployments/default/web", stored);
    assert(stored.find("\"reason\":\"MinimumReplicasAvailable\"") != std::string::npos);
    assert(stored.find("\"reason\":\"NewReplicaSetAvailable\"") != std::string::npos);
    assert(stored.find("\"observedGeneration\":1") != std::string::npos);

    // Spec changes move generation; metadata-only changes and status do not
    HttpResponse p1 = call(h, HttpMethod::Patch, "/apis/apps/v1/namespaces/default/deployments/web", R"({"spec":{"replicas":3}})");
    assert(parse(p1.body).get("metadata")->get_int("generation", 0) == 2);
    HttpResponse p2 = call(h, HttpMethod::Patch, "/apis/apps/v1/namespaces/default/deployments/web", R"({"metadata":{"labels":{"x":"y"}}})");
    assert(parse(p2.body).get("metadata")->get_int("generation", 0) == 2);
    HttpResponse put = call(h, HttpMethod::Put, "/apis/apps/v1/namespaces/default/deployments/web",
                            R"({"apiVersion":"apps/v1","kind":"Deployment","metadata":{"name":"web"},
        "spec":{"replicas":3,"selector":{"matchLabels":{"app":"web"}},"template":{"metadata":{"labels":{"app":"web"}},
        "spec":{"containers":[{"name":"c","image":"wasm://embedded/echo:v2"}]}}}})");
    models::JsonValue pd = parse(put.body);
    assert(pd.get("metadata")->get_int("generation", 0) == 3);
    assert(!pd.get("metadata")->get_string("uid", "").empty());
    assert(pd.get("status") && pd.get("status")->get("conditions"));   // kept across a PUT without status
    std::cout << "[PASS] test_generation_and_deployment_conditions" << std::endl;
}

static void test_lease_renew_time_and_backfill() {
    std::cout << "[RUN] test_lease_renew_time_and_backfill" << std::endl;
    auto store = ram_store();
    RouteHandler h(*store);
    HttpResponse r = call(h, HttpMethod::Put, "/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/node-a",
                          R"({"apiVersion":"coordination.k8s.io/v1","kind":"Lease","metadata":{"name":"node-a"},
                              "spec":{"holderIdentity":"node-a","leaseDurationSeconds":15}})");
    assert(r.status_code == 200);
    std::string renew = parse(r.body).get("spec")->get_string("renewTime", "");
    assert(renew.size() == 27 && renew.back() == 'Z' && renew[19] == '.');   // MicroTime, stamped on arrival
    assert(call(h, HttpMethod::Patch, "/apis/coordination.k8s.io/v1/namespaces/kube-node-lease/leases/none", "{}").status_code == 404);

    store->put("/registry/configmaps/default/early", R"({"kind":"ConfigMap","metadata":{"name":"early","namespace":"default"}})", false);
    store->put_extended("/registry/events/default/early.1", R"({"kind":"Event","metadata":{"name":"early.1","namespace":"default"},"reason":"X"})",
                        false, false);
    assert(backfill_timestamps(*store) == 2);
    std::string cm, ev;
    store->get("/registry/configmaps/default/early", cm);
    store->get("/registry/events/default/early.1", ev);
    assert(cm.find("\"creationTimestamp\"") != std::string::npos);
    assert(ev.find("\"lastTimestamp\"") != std::string::npos && ev.find("\"firstTimestamp\"") != std::string::npos);
    assert(backfill_timestamps(*store) == 0);
    std::cout << "[PASS] test_lease_renew_time_and_backfill" << std::endl;
}

// Creates are refused once the journal is full; the API says so instead of
// answering 201 for an object that was never stored.
static void test_full_store_refuses_creates() {
    std::cout << "[RUN] test_full_store_refuses_creates" << std::endl;
    storage::RamFlashRegion flash(40 * 4096);
    storage::StoreOptions o;
    o.enable_flash_persistence = true;
    o.flash_region = &flash;
    o.flush_delay_ms = 0;
    o.max_value_size_bytes = 4096;
    auto store = storage::create_radix_store(o);
    assert(store->recover().ok());
    RouteHandler h(*store);
    const std::string pad(2500, 'x');
    int status = 0;
    for (int i = 0; i < 200; ++i) {
        std::string cm = R"({"apiVersion":"v1","kind":"ConfigMap","metadata":{"name":"fill-)" + std::to_string(i) + R"("},"data":{"x":")" + pad + R"("}})";
        status = call(h, HttpMethod::Post, "/api/v1/namespaces/default/configmaps", cm).status_code;
        store->flush(false);
        if (status != 201) break;
    }
    assert(status == 500);
    HttpResponse pod = call(h, HttpMethod::Post, "/api/v1/namespaces/default/pods",
                            R"({"apiVersion":"v1","kind":"Pod","metadata":{"name":"late"},"spec":{"containers":[{"name":"c","image":"wasm://x","env":[{"name":"P","value":")" + pad + R"("}]}]}})");
    assert(pod.status_code == 500 && pod.body.find("object store is full") != std::string::npos);
    std::string out;
    assert(!store->get("/registry/pods/default/late", out).ok());
    HttpResponse dep = call(h, HttpMethod::Post, "/apis/apps/v1/namespaces/default/deployments",
                            R"({"apiVersion":"apps/v1","kind":"Deployment","metadata":{"name":"late","annotations":{"a":")" + pad + R"("}},
                                "spec":{"replicas":1,"template":{"spec":{"containers":[{"name":"c","image":"wasm://x"}]}}}})");
    assert(dep.status_code == 500);
    assert(!store->get("/registry/deployments/default/late", out).ok());
    // Every kind, not only pods and deployments
    assert(call(h, HttpMethod::Post, "/api/v1/namespaces/default/services",
                R"({"apiVersion":"v1","kind":"Service","metadata":{"name":"late","annotations":{"a":")" + pad + R"("}},"spec":{"ports":[{"port":80}]}})").status_code == 500);
    assert(call(h, HttpMethod::Post, "/api/v1/namespaces",
                R"({"apiVersion":"v1","kind":"Namespace","metadata":{"name":"late","annotations":{"a":")" + pad + R"("}}})").status_code == 500);
    // An update that grows an object is refused and leaves it as it was;
    // one that shrinks it goes through
    std::string before;
    assert(store->get("/registry/configmaps/default/fill-0", before).ok());
    HttpResponse grow = call(h, HttpMethod::Patch, "/api/v1/namespaces/default/configmaps/fill-0",
                             R"({"data":{"more":")" + pad + R"("}})");
    assert(grow.status_code == 500 && grow.body.find("object store is full") != std::string::npos);
    std::string after;
    assert(store->get("/registry/configmaps/default/fill-0", after).ok() && after == before);
    assert(call(h, HttpMethod::Put, "/api/v1/namespaces/default/configmaps/fill-0",
                R"({"apiVersion":"v1","kind":"ConfigMap","metadata":{"name":"fill-0"},"data":{"x":"small"}})").status_code == 200);
    // What the API accepted is exactly what the journal holds
    assert(store->flush(true).ok());
    storage::PersistenceStats ps;
    assert(store->persistence_stats(ps) && ps.write_errors == 0 && ps.pending_keys == 0);
    auto reopened = storage::create_radix_store(o);
    assert(reopened->recover().ok());
    std::vector<std::string> live, recovered;
    store->list("/registry/configmaps/", live);
    reopened->list("/registry/configmaps/", recovered);
    assert(live.size() == recovered.size() && !live.empty());
    std::string small;
    assert(reopened->get("/registry/configmaps/default/fill-0", small).ok() && small.find("\"small\"") != std::string::npos);
    std::cout << "[PASS] test_full_store_refuses_creates" << std::endl;
}

int main() {
    test_bearer_token_authentication();
    test_dry_run_stores_nothing();
    test_list_pagination();
    test_patch_media_types();
    test_events_v1_strict_fields();
    test_endpoints_from_services();
    test_generation_and_deployment_conditions();
    test_lease_renew_time_and_backfill();
    test_full_store_refuses_creates();
    std::cout << "All control plane API tests passed" << std::endl;
    return 0;
}
