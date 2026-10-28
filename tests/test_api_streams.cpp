// Watch framing, pod log rings, events, metrics and selectors.
#include "api/watch_hub.hpp"
#include "api/selectors.hpp"
#include "api/event_recorder.hpp"
#include "api/metrics_registry.hpp"
#include "api/route_handler.hpp"
#include "runtime/pod_log.hpp"
#include "storage/store.hpp"
#include "mem/monotonic_arena.hpp"
#include "osal/socket.hpp"
#include "osal/thread.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

using namespace kube32;

static std::unique_ptr<storage::Store> mem_store() {
    storage::StoreOptions o;
    o.enable_flash_persistence = false;
    return storage::create_radix_store(o);
}

// Reads from fd until `needle` appears or the deadline passes.
static std::string read_until(int fd, const std::string& needle, uint32_t timeout_ms) {
    std::string acc;
    uint64_t t0 = osal::Thread::get_time_ms();
    char buf[1024];
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (osal::Thread::get_time_ms() - t0 < timeout_ms) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) acc.append(buf, static_cast<size_t>(n));
        else if (n == 0) break;
        if (acc.find(needle) != std::string::npos) break;
    }
    return acc;
}

void test_inject_resource_version() {
    std::cout << "[RUN] test_inject_resource_version" << std::endl;
    assert(storage::inject_resource_version("{\"kind\":\"Pod\",\"metadata\":{\"name\":\"a\"}}", 7) ==
           "{\"kind\":\"Pod\",\"metadata\":{\"resourceVersion\":\"7\",\"name\":\"a\"}}");
    assert(storage::inject_resource_version("{\"metadata\":{\"name\":\"a\",\"resourceVersion\":\"3\",\"labels\":{\"resourceVersion\":\"x\"}}}", 9) ==
           "{\"metadata\":{\"name\":\"a\",\"resourceVersion\":\"9\",\"labels\":{\"resourceVersion\":\"x\"}}}");
    assert(storage::inject_resource_version("{\"metadata\":{}}", 2) == "{\"metadata\":{\"resourceVersion\":\"2\"}}");
    assert(storage::inject_resource_version("{\"no\":\"meta\"}", 2) == "{\"no\":\"meta\"}");
    auto store = mem_store();
    store->put("/registry/pods/default/a", "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"a\"}}", true);
    std::string v;
    uint64_t ver = 0;
    store->get_extended("/registry/pods/default/a", v, &ver);
    assert(v.find("\"resourceVersion\":\"" + std::to_string(ver) + "\"") != std::string::npos);
    std::cout << "[PASS] test_inject_resource_version" << std::endl;
}

void test_selectors() {
    std::cout << "[RUN] test_selectors" << std::endl;
    assert(api::url_decode("spec.nodeName%3Dn1%2Cmetadata.name!%3Dx") == "spec.nodeName=n1,metadata.name!=x");
    std::string v;
    assert(api::query_param("watch=true&resourceVersion=42&fieldSelector=a%3Db", "resourceVersion", v) && v == "42");
    assert(api::query_flag("watch=1", "watch"));
    assert(!api::query_flag("watch=false", "watch"));
    api::ListSelector sel;
    std::string err;
    assert(api::ListSelector::parse("spec.nodeName=n1,status.phase!=Failed", "app in (web,api),tier!=db,!legacy,owner", sel, err));
    const char* pod = "{\"metadata\":{\"name\":\"p\",\"labels\":{\"app\":\"web\",\"tier\":\"fe\",\"owner\":\"me\"}},"
                      "\"spec\":{\"nodeName\":\"n1\"},\"status\":{\"phase\":\"Running\"}}";
    assert(sel.matches_json(pod));
    api::ListSelector sel2;
    assert(api::ListSelector::parse("spec.nodeName=n2", "", sel2, err));
    assert(!sel2.matches_json(pod));
    api::ListSelector sel3;
    assert(api::ListSelector::parse("", "tier=db", sel3, err));
    assert(!sel3.matches_json(pod));
    api::ListSelector sel4;
    assert(api::ListSelector::parse("involvedObject.name=p,involvedObject.uid=", "", sel4, err));
    assert(sel4.matches_json("{\"involvedObject\":{\"name\":\"p\"}}"));
    api::ListSelector bad;
    assert(!api::ListSelector::parse("garbage", "", bad, err));
    std::cout << "[PASS] test_selectors" << std::endl;
}

void test_log_ring() {
    std::cout << "[RUN] test_log_ring" << std::endl;
    runtime::PodLogRing ring(256 + 3 * sizeof(runtime::PodLogRecord));
    for (int i = 0; i < 50; ++i) ring.append("line-" + std::to_string(i) + std::string(60, 'x'), 1000 + i, 10 + i);
    assert(ring.bytes_used() <= ring.capacity());
    assert(ring.dropped() > 0);
    assert(ring.last_seq() == 50);
    std::vector<runtime::PodLogRecord> recs;
    ring.read_after(0, recs);
    assert(!recs.empty() && recs.back().text.rfind("line-49", 0) == 0);
    for (size_t i = 1; i < recs.size(); ++i) assert(recs[i].seq == recs[i - 1].seq + 1);
    std::vector<runtime::PodLogRecord> t;
    ring.tail(1, t);
    assert(t.size() == 1 && t[0].seq == 50);
    // Oversized line keeps its tail
    runtime::PodLogRing small(64 + sizeof(runtime::PodLogRecord));
    small.append(std::string(200, 'a') + "END", 0, 0);
    std::vector<runtime::PodLogRecord> s1;
    small.read_after(0, s1);
    assert(s1.size() == 1 && s1[0].text.size() <= 64 && s1[0].text.substr(s1[0].text.size() - 3) == "END");

    auto& mirror = runtime::PodLogStore::apiserver();
    mirror.clear();
    runtime::PodLogRecord r;
    r.seq = 1; r.unix_ms = 1700000000123ULL; r.uptime_ms = 5; r.text = "hello";
    assert(mirror.accept_shipped("default/p", 77, r));
    assert(!mirror.accept_shipped("default/p", 77, r));     // duplicate
    r.seq = 2; r.text = "world";
    assert(mirror.accept_shipped("default/p", 77, r));
    r.seq = 1; r.text = "rebooted";
    assert(mirror.accept_shipped("default/p", 78, r));      // new epoch resets and clears
    std::vector<runtime::PodLogRecord> m;
    assert(mirror.tail("default/p", 0, m));
    assert(m.size() == 1 && m[0].text == "rebooted");
    runtime::PodLogRecord ts;
    ts.unix_ms = 1700000000123ULL; ts.text = "x";
    assert(runtime::format_log_line(ts, true) == "2023-11-14T22:13:20.123Z x\n");
    ts.unix_ms = 0; ts.uptime_ms = 1500;
    assert(runtime::format_log_line(ts, true) == "+1.500s x\n");
    assert(runtime::format_log_line(ts, false) == "x\n");
    mirror.clear();
    std::cout << "[PASS] test_log_ring" << std::endl;
}

void test_events() {
    std::cout << "[RUN] test_events" << std::endl;
    auto store = mem_store();
    api::EventRecorder rec(*store, 4);
    api::EventSpec e;
    e.involved.kind = "Pod";
    e.involved.name = "p";
    e.involved.namespace_name = "default";
    e.involved.uid = "u-1";
    e.reason = "Started";
    e.message = "Started container c";
    e.component = "kubelet";
    e.host = "n1";
    std::string j1 = rec.record(e);
    assert(j1.find("\"reason\":\"Started\"") != std::string::npos);
    assert(j1.find("\"count\":1") != std::string::npos);
    assert(j1.find("\"uid\":\"u-1\"") != std::string::npos);
    std::string j2 = rec.record(e);   // identical: merged
    assert(j2.find("\"count\":2") != std::string::npos);
    assert(rec.count() == 1);
    for (int i = 0; i < 6; ++i) {
        e.reason = "R" + std::to_string(i);
        rec.record(e);
    }
    assert(rec.count() == 4);         // bounded, oldest dropped
    std::vector<std::string> left;
    store->list("/registry/events/", left);
    bool has_started = false;
    for (auto& j : left) if (j.find("\"reason\":\"Started\"") != std::string::npos) has_started = true;
    assert(!has_started);
    std::string stored, err;
    assert(rec.admit("{\"kind\":\"Event\",\"apiVersion\":\"v1\",\"metadata\":{\"namespace\":\"default\"},"
                     "\"involvedObject\":{\"kind\":\"Node\",\"name\":\"n1\"},\"reason\":\"NodeReady\",\"message\":\"ok\","
                     "\"type\":\"Normal\",\"reportingComponent\":\"node-controller\"}", stored, err));
    assert(stored.find("\"name\":\"n1.") != std::string::npos);
    assert(!rec.admit("{\"kind\":\"Event\"}", stored, err));
    std::string v1 = api::EventRecorder::to_events_v1(j2);
    assert(v1.find("\"apiVersion\":\"events.k8s.io/v1\"") != std::string::npos);
    assert(v1.find("\"regarding\":{") != std::string::npos);
    assert(v1.find("\"note\":\"Started container c\"") != std::string::npos);
    assert(v1.find("\"series\":{\"count\":2") != std::string::npos);

    // describe-style field selector through the router
    api::RouteHandler handler(*store);
    mem::MonotonicArena arena;
    api::HttpRequest req;
    req.method = api::HttpMethod::Get;
    req.path = "/api/v1/namespaces/default/events";
    req.query_string = "fieldSelector=involvedObject.name%3Dp%2CinvolvedObject.kind%3DPod";
    api::HttpResponse resp = handler.handle_request(req, arena);
    assert(resp.status_code == 200);
    assert(resp.body.find("\"kind\":\"EventList\"") != std::string::npos);
    assert(resp.body.find("\"involvedObject\":{\"kind\":\"Pod\"") != std::string::npos);
    assert(resp.body.find("\"NodeReady\"") == std::string::npos);
    std::cout << "[PASS] test_events" << std::endl;
}

void test_metrics_registry() {
    std::cout << "[RUN] test_metrics_registry" << std::endl;
    api::MetricsRegistry reg(1000, 2);
    std::string err;
    assert(!reg.ingest("{\"usage\":{}}", err));
    assert(reg.ingest("{\"node\":\"n1\",\"window_ms\":5000,\"timestamp\":\"2026-10-28T10:00:00Z\",\"usage\":{\"memory_bytes\":180224},"
                      "\"pods\":[{\"namespace\":\"default\",\"name\":\"p\",\"containers\":[{\"name\":\"c\",\"memory_bytes\":71667}]}]}", err));
    api::NodeUsageSample n;
    assert(reg.node("n1", n));
    assert(n.has_memory && !n.has_cpu);
    std::string nj = api::MetricsRegistry::node_metrics_json(n);
    assert(nj.find("\"memory\":\"176Ki\"") != std::string::npos);
    assert(nj.find("\"cpu\"") == std::string::npos);          // never a fabricated zero
    assert(nj.find("\"window\":\"5.000s\"") != std::string::npos);
    api::PodUsageSample p;
    assert(reg.pod("default", "p", p));
    std::string pj = api::MetricsRegistry::pod_metrics_json(p);
    assert(pj.find("\"memory\":\"71667\"") != std::string::npos);
    assert(pj.find("\"cpu\"") == std::string::npos);
    assert(reg.ingest("{\"node\":\"n1\",\"window_ms\":5000,\"usage\":{\"cpu_nanocores\":3095224,\"memory_bytes\":1}}", err));
    assert(reg.node("n1", n) && n.has_cpu && n.cpu_nanocores == 3095224);
    assert(api::MetricsRegistry::node_metrics_json(n).find("\"cpu\":\"3095224n\"") != std::string::npos);
    osal::Thread::sleep_ms(1100);
    assert(!reg.node("n1", n));                                   // stale samples are not served

    // Router: 404 for a node without a fresh sample, discovery lists the group
    auto store = mem_store();
    api::RouteHandler handler(*store);
    mem::MonotonicArena arena;
    api::HttpRequest req;
    req.method = api::HttpMethod::Get;
    req.path = "/apis/metrics.k8s.io/v1beta1/nodes/ghost";
    assert(handler.handle_request(req, arena).status_code == 404);
    req.path = "/apis/metrics.k8s.io/v1beta1";
    api::HttpResponse d = handler.handle_request(req, arena);
    assert(d.status_code == 200 && d.body.find("\"kind\":\"NodeMetrics\"") != std::string::npos);
    req.path = "/apis";
    assert(handler.handle_request(req, arena).body.find("metrics.k8s.io") != std::string::npos);
    req.method = api::HttpMethod::Post;
    req.path = "/apis/metrics.k8s.io/v1beta1/nodes/n1";
    req.body = "{\"node\":\"n1\",\"window_ms\":100,\"usage\":{\"memory_bytes\":5}}";
    assert(handler.handle_request(req, arena).status_code == 200);
    req.method = api::HttpMethod::Get;
    req.path = "/apis/metrics.k8s.io/v1beta1/nodes";
    assert(handler.handle_request(req, arena).body.find("\"name\":\"n1\"") != std::string::npos);
    std::cout << "[PASS] test_metrics_registry" << std::endl;
}

void test_watch_hub_framing() {
    std::cout << "[RUN] test_watch_hub_framing" << std::endl;
    auto store = mem_store();
    store->put("/registry/pods/default/a", "{\"kind\":\"Pod\",\"apiVersion\":\"v1\",\"metadata\":{\"name\":\"a\",\"namespace\":\"default\",\"labels\":{\"app\":\"x\"}},\"spec\":{\"nodeName\":\"n1\"}}", true);
    store->put("/registry/pods/other/b", "{\"kind\":\"Pod\",\"apiVersion\":\"v1\",\"metadata\":{\"name\":\"b\",\"namespace\":\"other\"},\"spec\":{}}", true);
    api::WatchHubOptions wo;
    wo.max_streams = 2;
    wo.queue_depth = 2;
    wo.bookmark_interval_ms = 300;
    wo.poll_interval_ms = 50;
    api::WatchHub hub(*store, wo);
    assert(hub.start());

    // 1. Initial ADDED for the namespace, then MODIFIED / DELETED, then a BOOKMARK
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    api::WatchSpec spec;
    spec.prefix = "/registry/pods/default/";
    spec.kind = "Pod";
    spec.api_version = "v1";
    spec.bookmarks = true;
    assert(hub.add_watch(osal::Socket(sv[0]), spec));
    std::string got = read_until(sv[1], "\"type\":\"ADDED\"", 2000);
    assert(got.find("{\"type\":\"ADDED\",\"object\":{\"kind\":\"Pod\"") != std::string::npos);
    assert(got.find("\"name\":\"b\"") == std::string::npos);       // other namespace filtered by prefix
    // Chunk framing: hex size line, payload ending in \n, CRLF
    size_t crlf = got.find("\r\n");
    assert(crlf != std::string::npos && crlf > 0);
    size_t len = std::strtoul(got.substr(0, crlf).c_str(), nullptr, 16);
    assert(got.size() >= crlf + 2 + len + 2);
    assert(got[crlf + 2 + len - 1] == '\n');
    assert(got.compare(crlf + 2 + len, 2, "\r\n") == 0);

    store->put("/registry/pods/default/a", "{\"kind\":\"Pod\",\"apiVersion\":\"v1\",\"metadata\":{\"name\":\"a\",\"namespace\":\"default\"},\"spec\":{\"nodeName\":\"n2\"}}", true);
    got = read_until(sv[1], "\"type\":\"MODIFIED\"", 2000);
    assert(got.find("\"type\":\"MODIFIED\"") != std::string::npos && got.find("\"nodeName\":\"n2\"") != std::string::npos);
    store->remove("/registry/pods/default/a");
    got = read_until(sv[1], "\"type\":\"DELETED\"", 2000);
    assert(got.find("\"type\":\"DELETED\"") != std::string::npos);
    got = read_until(sv[1], "\"type\":\"BOOKMARK\"", 2000);
    assert(got.find("{\"type\":\"BOOKMARK\",\"object\":{\"kind\":\"Pod\",\"apiVersion\":\"v1\",\"metadata\":{\"resourceVersion\":\"") != std::string::npos);
    assert(hub.active_streams() == 1);

    // 2. Capacity: a third stream is refused; a hung-up client is reaped
    int sv2[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv2) == 0);
    api::WatchSpec spec2 = spec;
    spec2.bookmarks = false;
    std::string err;
    assert(api::ListSelector::parse("spec.nodeName=n1", "", spec2.selector, err));
    assert(hub.add_watch(osal::Socket(sv2[0]), spec2));
    assert(!hub.has_capacity());
    int sv3[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv3) == 0);
    assert(!hub.add_watch(osal::Socket(sv3[0]), spec));
    ::close(sv3[1]);
    ::close(sv[1]);                                                   // client 1 hangs up
    for (int i = 0; i < 40 && hub.active_streams() != 1; ++i) osal::Thread::sleep_ms(50);
    assert(hub.active_streams() == 1);

    // 3. Field selector on the stream: only pods on n1 come through
    store->put("/registry/pods/default/c", "{\"kind\":\"Pod\",\"apiVersion\":\"v1\",\"metadata\":{\"name\":\"c\",\"namespace\":\"default\"},\"spec\":{\"nodeName\":\"n2\"}}", true);
    store->put("/registry/pods/default/d", "{\"kind\":\"Pod\",\"apiVersion\":\"v1\",\"metadata\":{\"name\":\"d\",\"namespace\":\"default\"},\"spec\":{\"nodeName\":\"n1\"}}", true);
    got = read_until(sv2[1], "\"name\":\"d\"", 2000);
    assert(got.find("\"name\":\"d\"") != std::string::npos);
    assert(got.find("\"name\":\"c\"") == std::string::npos);
    ::close(sv2[1]);
    for (int i = 0; i < 40 && hub.active_streams() != 0; ++i) osal::Thread::sleep_ms(50);

    // 4. resourceVersion replay and 410 for a version older than the ring
    uint64_t rv_before = store->current_version();
    store->put("/registry/pods/default/e", "{\"kind\":\"Pod\",\"apiVersion\":\"v1\",\"metadata\":{\"name\":\"e\",\"namespace\":\"default\"},\"spec\":{}}", true);
    int sv4[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv4) == 0);
    api::WatchSpec spec4 = spec;
    spec4.bookmarks = false;
    spec4.since_version = rv_before;
    assert(hub.add_watch(osal::Socket(sv4[0]), spec4));
    got = read_until(sv4[1], "\"name\":\"e\"", 2000);
    assert(got.find("\"type\":\"ADDED\"") != std::string::npos && got.find("\"name\":\"e\"") != std::string::npos);
    assert(got.find("\"name\":\"d\"") == std::string::npos);        // older than rv_before: not replayed
    ::close(sv4[1]);
    for (int i = 0; i < 40 && hub.active_streams() != 0; ++i) osal::Thread::sleep_ms(50);
    for (int i = 0; i < 40; ++i) store->put("/registry/pods/default/spam", "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"spam\"}}", true);
    int sv5[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv5) == 0);
    api::WatchSpec spec5 = spec4;                                     // rv_before is now older than the ring
    assert(hub.add_watch(osal::Socket(sv5[0]), spec5));
    got = read_until(sv5[1], "0\r\n\r\n", 2000);
    assert(got.find("{\"type\":\"ERROR\",\"object\":{\"kind\":\"Status\"") != std::string::npos);
    assert(got.find("\"code\":410") != std::string::npos);
    assert(got.find("\"reason\":\"Expired\"") != std::string::npos);
    assert(got.find("0\r\n\r\n") != std::string::npos);              // stream terminated
    ::close(sv5[1]);

    // 5. Log follow: initial records after a seq, then new ones, closes when the pod is gone
    auto& mirror = runtime::PodLogStore::apiserver();
    mirror.clear();
    store->put("/registry/pods/default/lp", "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"lp\",\"namespace\":\"default\"}}", true);
    runtime::PodLogRecord r;
    r.seq = 1; r.text = "first"; r.unix_ms = 0; r.uptime_ms = 10;
    mirror.accept_shipped("default/lp", 5, r);
    int sv6[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv6) == 0);
    api::LogFollowSpec lf;
    lf.pod_key = "default/lp";
    lf.after_seq = 0;
    assert(hub.add_log_follow(osal::Socket(sv6[0]), lf));
    got = read_until(sv6[1], "first\n", 2000);
    assert(got.find("first\n") != std::string::npos);
    r.seq = 2; r.text = "second";
    mirror.accept_shipped("default/lp", 5, r);
    got = read_until(sv6[1], "second\n", 2000);
    assert(got.find("second\n") != std::string::npos);
    r.seq = 3; r.text = "last";
    mirror.accept_shipped("default/lp", 5, r);
    store->remove("/registry/pods/default/lp");
    got = read_until(sv6[1], "0\r\n\r\n", 3000);
    assert(got.find("last\n") != std::string::npos);                 // drained before closing
    assert(got.find("0\r\n\r\n") != std::string::npos);
    ::close(sv6[1]);
    hub.stop();
    mirror.clear();
    std::cout << "[PASS] test_watch_hub_framing" << std::endl;
}

void test_router_watch_and_log_paths() {
    std::cout << "[RUN] test_router_watch_and_log_paths" << std::endl;
    auto store = mem_store();
    api::RouteHandler handler(*store);
    mem::MonotonicArena arena;
    api::HttpRequest req;
    req.method = api::HttpMethod::Get;
    req.path = "/api/v1/pods";
    req.query_string = "watch=true";
    assert(handler.handle_request(req, arena).status_code == 501);   // no hub installed
    api::WatchHubOptions wo;
    wo.max_streams = 1;
    api::WatchHub hub(*store, wo);
    assert(hub.start());
    handler.set_watch_hub(&hub);
    assert(handler.handle_request(req, arena).status_code == 400);   // no streaming socket
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    osal::Socket client(sv[0]);
    req.accept = "application/json;as=Table;v=v1;g=meta.k8s.io";
    req.as_table = true;
    api::HttpResponse r = handler.handle_request(req, arena, &client);
    assert(r.socket_taken && r.status_code == 200);
    assert(!client.is_valid());                                       // moved to the hub
    std::string head = read_until(sv[1], "\r\n\r\n", 1000);
    assert(head.find("HTTP/1.1 200 OK\r\n") == 0);
    assert(head.find("Transfer-Encoding: chunked") != std::string::npos);
    assert(head.find("as=Table") != std::string::npos);
    store->put("/registry/pods/default/t", "{\"kind\":\"Pod\",\"apiVersion\":\"v1\",\"metadata\":{\"name\":\"t\",\"namespace\":\"default\"},\"spec\":{\"nodeName\":\"n1\"},\"status\":{\"phase\":\"Running\"}}", true);
    std::string got = read_until(sv[1], "\"rows\":[", 2000);
    assert(got.find("{\"type\":\"ADDED\",\"object\":{\"kind\":\"Table\"") != std::string::npos);
    int sv2[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv2) == 0);
    osal::Socket client2(sv2[0]);
    assert(handler.handle_request(req, arena, &client2).status_code == 429);  // cap reached
    ::close(sv[1]);
    ::close(sv2[1]);

    // Pod log: 404 unknown pod, 400 while waiting to start, text once lines are shipped
    req.query_string.clear();
    req.as_table = false;
    req.path = "/api/v1/namespaces/default/pods/nope/log";
    assert(handler.handle_request(req, arena).status_code == 404);
    store->put("/registry/pods/default/w", "{\"kind\":\"Pod\",\"metadata\":{\"name\":\"w\",\"namespace\":\"default\"},\"spec\":{\"containers\":[{\"name\":\"c\"}]},\"status\":{\"phase\":\"Pending\"}}", true);
    req.path = "/api/v1/namespaces/default/pods/w/log";
    assert(handler.handle_request(req, arena).status_code == 400);
    req.query_string = "container=zzz";
    assert(handler.handle_request(req, arena).status_code == 400);
    req.query_string.clear();
    api::HttpRequest ship;
    ship.method = api::HttpMethod::Post;
    ship.path = "/api/v1/namespaces/default/pods/w/log";
    ship.body = "{\"epoch\":1,\"seq\":1,\"t\":1700000000000,\"u\":1,\"m\":\"alpha\"}\n{\"epoch\":1,\"seq\":2,\"t\":1700000001000,\"u\":2,\"m\":\"beta\"}\n";
    api::HttpResponse sr = handler.handle_request(ship, arena);
    assert(sr.status_code == 200 && sr.body.find("\"accepted\":2") != std::string::npos);
    api::HttpResponse lr = handler.handle_request(req, arena);
    assert(lr.status_code == 200 && lr.content_type == "text/plain" && lr.body == "alpha\nbeta\n");
    req.query_string = "tailLines=1&timestamps=true";
    lr = handler.handle_request(req, arena);
    assert(lr.body == "2023-11-14T22:13:21.000Z beta\n");
    req.query_string = "follow=true";
    int sv3[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv3) == 0);
    osal::Socket client3(sv3[0]);
    api::HttpResponse fr = handler.handle_request(req, arena, &client3);
    assert(fr.socket_taken && fr.status_code == 200);
    got = read_until(sv3[1], "beta\n", 2000);
    assert(got.find("Content-Type: text/plain") != std::string::npos && got.find("alpha\nbeta\n") != std::string::npos);
    ship.body = "{\"epoch\":1,\"seq\":3,\"t\":0,\"u\":3,\"m\":\"gamma\"}\n";
    handler.handle_request(ship, arena);
    got = read_until(sv3[1], "gamma\n", 2000);
    assert(got.find("gamma\n") != std::string::npos);
    ::close(sv3[1]);
    handler.set_watch_hub(nullptr);
    hub.stop();
    runtime::PodLogStore::apiserver().clear();
    std::cout << "[PASS] test_router_watch_and_log_paths" << std::endl;
}

int main() {
    std::cout << "=== Kube32 API streams: watch, logs, events, metrics ===" << std::endl;
    test_inject_resource_version();
    test_selectors();
    test_log_ring();
    test_events();
    test_metrics_registry();
    test_watch_hub_framing();
    test_router_watch_and_log_paths();
    std::cout << "=== All API stream tests passed ===" << std::endl;
    return 0;
}
