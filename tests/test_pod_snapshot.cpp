// Pod snapshots (docs/migration.md): a request pod frozen between host calls,
// its K32S snapshot, and a restore into a new instance of the same module
// that continues where the first one stopped. Covers globals, tables,
// memory.grow, memory caps below one page, the clock, held events,
// channels, refused snapshots, every blocker, the kubelet endpoints, the
// Checkpoint data on the API server, and a snapshot taken on an ESP32-S3.

#include "pod_test_util.hpp"
#include "api/checkpoint_api.hpp"
#include "api/http_server.hpp"
#include "app/pod_checkpoint_api.hpp"
#include "app/pod_server.hpp"
#include "hal/node_storage.hpp"
#include "ipc/channel.hpp"
#include "osal/sha256.hpp"
#include "osal/thread.hpp"
#include "runtime/pod_snapshot.hpp"
#include "storage/store.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>

using namespace kube32;
using runtime::AppPodHost;

namespace {

constexpr int32_t kAllState = 0x7FF;        // check() with memory grown
constexpr int32_t kStateNoGrow = 0x67F;     // ... without

std::vector<uint8_t> probe() {
    std::vector<uint8_t> b = k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/tests/data/snapshot-probe.wasm");
    assert(!b.empty() && "tests/data/snapshot-probe.wasm (python3 tests/data/snapshot_probe.py)");
    return b;
}

runtime::DeploySpec probe_spec(const std::string& container = "c") {
    runtime::DeploySpec spec;
    spec.container_name = container;
    spec.event_topics = {"probe.in"};
    return spec;
}

// A snapshot of a frozen pod into memory
std::vector<uint8_t> take(const std::string& pod, runtime::SnapshotSummary* out = nullptr) {
    std::vector<uint8_t> bytes;
    runtime::SnapshotSummary sum;
    std::string why;
    const bool ok = AppPodHost::instance().snapshot(pod, [&](const uint8_t* d, size_t n) { bytes.insert(bytes.end(), d, d + n); return true; },
                                                    sum, why);
    if (!ok) std::cout << "snapshot " << pod << ": " << why << std::endl;
    assert(ok && bytes.size() == sum.bytes);
    if (out) *out = sum;
    return bytes;
}

// Restores `bytes` into a new pod, handed over in pieces of `piece` bytes as
// a network stream would; returns the deploy error ("" on success)
std::string restore(runtime::WasmRuntime& rt, const std::string& pod, const std::vector<uint8_t>& wasm, const std::vector<uint8_t>& bytes,
                    uint32_t memory = 128 * 1024, size_t piece = 1000) {
    runtime::DeploySpec spec = probe_spec();
    spec.memory_limit_bytes = memory;
    spec.stack_size_bytes = 4096;
    spec.restore_from = [&](mem::FunctionRef<bool(const uint8_t*, size_t)> feed, std::string&) {
        for (size_t off = 0; off < bytes.size(); off += piece) {
            if (!feed(bytes.data() + off, std::min(piece, bytes.size() - off))) return true;
        }
        return true;
    };
    std::string err;
    hal::CapabilityFirewall::instance().unregister_pod(pod);
    std::string fw_err;
    hal::CapabilityFirewall::instance().register_pod(pod, "{\"metadata\":{\"name\":\"x\"},\"spec\":{\"containers\":[{\"name\":\"c\",\"image\":\"wasm://x\","
                                                          "\"securityContext\":{\"capabilities\":{\"add\":[\"CHANNEL_IPC\"]}}}]}}", fw_err);
    if (!rt.deploy_workload_ex(pod, wasm.data(), wasm.size(), spec, err)) {
        std::cout << "restore " << pod << ": " << err << std::endl;
        return err.empty() ? "failed" : err;
    }
    assert(rt.start(pod));
    return "";
}

int32_t call(runtime::WasmRuntime& rt, const std::string& pod, const char* fn, std::vector<int32_t> args = {}) {
    return rt.execute_function(pod, fn, args);
}

std::string count_of(const std::string& pod) {
    k32test::Collect c = k32test::call_pod(pod, "/count");
    assert(c.status == 200);
    return c.body;
}

bool wait_for(const std::function<bool()>& cond, uint32_t ms = 3000) {
    for (uint32_t t = 0; t < ms; t += 10) {
        if (cond()) return true;
        osal::Thread::sleep_ms(10);
    }
    return cond();
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) if (x == s) return true;
    std::cout << "missing blocker \"" << s << "\" in:";
    for (const auto& x : v) std::cout << " [" << x << "]";
    std::cout << std::endl;
    return false;
}

} // namespace

int main() {
    std::cout << "=== pod snapshots ===" << std::endl;
    osal::Socket::init_networking();
    ipc::ChannelManager::instance().init(0, 18740);
    auto rt = runtime::create_wasm_runtime();
    const std::vector<uint8_t> wasm = probe();
    AppPodHost& host = AppPodHost::instance();

    // --- Round trip: globals, tables, memory.grow, clock, events, channels ---
    {
        k32test::deploy(*rt, "default/probe-a", wasm, 128 * 1024, "\"CHANNEL_IPC\"", probe_spec());
        k32test::Collect c = k32test::call_pod("default/probe-a", "/setup");
        assert(c.status == 200 && c.body == "1");
        assert(count_of("default/probe-a") == "2" && count_of("default/probe-a") == "3");
        assert(call(*rt, "default/probe-a", "check") == kAllState);
        const int32_t handle = call(*rt, "default/probe-a", "peek", {12});
        assert(handle > 0);
        // An event delivered while the pod runs
        const uint8_t e0[] = {'a'};
        assert(host.event("default/probe-a", "probe.in", e0, 1) == 0);
        assert(call(*rt, "default/probe-a", "peek", {4}) == 1);

        std::string why;
        assert(host.freeze("default/probe-a", 1000, why) == 0 && host.is_frozen("default/probe-a"));
        const int32_t t_frozen = call(*rt, "default/probe-a", "time");
        // A frozen pod answers 503 and holds events
        c = k32test::call_pod("default/probe-a", "/count");
        assert(c.status == 503 && c.headers.find("Retry-After: 1") != std::string::npos);
        const uint8_t e1[] = {'b', 'x'}, e2[] = {'c'};
        assert(host.event("default/probe-a", "probe.in", e1, 2) == 0 && host.event("default/probe-a", "probe.in", e2, 1) == 0);
        assert(call(*rt, "default/probe-a", "peek", {4}) == 1);

        runtime::SnapshotSummary sum;
        const std::vector<uint8_t> snap = take("default/probe-a", &sum);
        // The same frozen pod gives the same bytes
        assert(take("default/probe-a") == snap);
        runtime::SnapshotHeader h;
        assert(runtime::parse_snapshot_header(snap.data(), snap.size(), h, why));
        std::cout << "[round trip] " << snap.size() << " bytes, memory " << h.memory_bytes << " bytes in " << h.memory_pages << " pages, "
                  << h.sections << " sections, pod time " << h.pod_time_ms << " ms" << std::endl;
        assert(h.version == 1 && h.memory_pages == 2 && h.memory_bytes == 2 * 65536 && h.sections == 5);
        assert(std::memcmp(snap.data() + snap.size() - 32, sum.digest, 32) == 0);
        assert(h.pod_time_ms >= static_cast<uint64_t>(t_frozen) && h.pod_time_ms <= static_cast<uint64_t>(t_frozen) + 50);

        // The move: the old pod goes, the new one restores 300 ms later
        assert(rt->terminate("default/probe-a"));
        osal::Thread::sleep_ms(300);
        assert(restore(*rt, "default/probe-a", wasm, snap, 128 * 1024, 777).empty());
        assert(call(*rt, "default/probe-a", "check") == kAllState);
        assert(call(*rt, "default/probe-a", "peek", {16}) == 1);   // k32_restored ran once
        // The clock continues from the snapshot, the 300 ms gap not counted
        const int32_t t_restored = call(*rt, "default/probe-a", "time");
        std::cout << "[clock] frozen at " << t_frozen << " ms, restored reads " << t_restored << " ms" << std::endl;
        assert(t_restored >= static_cast<int32_t>(h.pod_time_ms) && t_restored < static_cast<int32_t>(h.pod_time_ms) + 250);
        // The request count continues
        assert(count_of("default/probe-a") == "4");
        // The held events arrive after the restore, in order
        assert(wait_for([&] { return call(*rt, "default/probe-a", "peek", {4}) == 3; }));
        assert(call(*rt, "default/probe-a", "peek", {8}) == 1);
        assert(call(*rt, "default/probe-a", "peek", {20}) == ('a' * 31 + 'b') * 31 + 'c');
        // The channel is open again under the same handle
        assert(call(*rt, "default/probe-a", "peek", {12}) == handle);
        auto open = ipc::ChannelManager::instance().handles_of("default/probe-a");
        assert(open.size() == 1 && open[0].first == handle && open[0].second == "pod://snapshot-probe/in");
        assert(rt->terminate("default/probe-a"));
    }

    // --- Thaw: a checkpoint that leaves the pod running ---
    {
        k32test::deploy(*rt, "default/probe-t", wasm, 64 * 1024, "", probe_spec());
        std::string why;
        assert(host.freeze("default/probe-t", 1000, why) == 0);
        const uint8_t e[] = {'z'};
        host.event("default/probe-t", "probe.in", e, 1);
        assert(host.thaw("default/probe-t") && !host.is_frozen("default/probe-t") && !host.thaw("default/probe-t"));
        assert(count_of("default/probe-t") == "1");
        assert(wait_for([&] { return call(*rt, "default/probe-t", "peek", {4}) == 1; }));
        assert(rt->terminate("default/probe-t"));
    }

    // --- A memory limit below one page: 32 KiB of the page are backed ---
    std::vector<uint8_t> small;
    {
        k32test::deploy(*rt, "default/probe-s", wasm, 32 * 1024, "", probe_spec());
        assert(call(*rt, "default/probe-s", "setup", {1}) == 0);   // memory.grow is refused at the limit
        assert(call(*rt, "default/probe-s", "check") == kStateNoGrow);
        assert(count_of("default/probe-s") == "1");
        std::string why;
        assert(host.freeze("default/probe-s", 1000, why) == 0);
        small = take("default/probe-s");
        runtime::SnapshotHeader h;
        assert(runtime::parse_snapshot_header(small.data(), small.size(), h, why));
        assert(h.memory_pages == 1 && h.memory_bytes == 32 * 1024);
        assert(rt->terminate("default/probe-s"));
        assert(restore(*rt, "default/probe-s", wasm, small, 32 * 1024).empty());
        assert(call(*rt, "default/probe-s", "check") == kStateNoGrow && count_of("default/probe-s") == "2");
        assert(rt->terminate("default/probe-s"));
        // A smaller limit cannot hold it
        const std::string err = restore(*rt, "default/probe-s", wasm, small, 16 * 1024);
        std::cout << "[16 KiB] " << err << std::endl;
        assert(runtime::deploy_error_is_restore(err) && err.find("gives 16384 bytes") != std::string::npos);
        assert(!rt->is_deployed("default/probe-s"));
    }

    // --- Refused snapshots ---
    {
        // Another module: the image digest differs
        const std::vector<uint8_t> echo = k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/images/echo.wasm");
        std::string err = restore(*rt, "default/other", echo, small);
        std::cout << "[digest] " << err << std::endl;
        assert(runtime::deploy_error_is_restore(err) && err.find("image sha256") != std::string::npos);
        // A damaged byte in the memory section
        std::vector<uint8_t> bad = small;
        bad[runtime::kSnapshotHeaderBytes + 6 + 2000] ^= 0x40;
        err = restore(*rt, "default/probe-s", wasm, bad, 32 * 1024);
        std::cout << "[trailer] " << err << std::endl;
        assert(runtime::deploy_error_is_restore(err) && err.find("trailer") != std::string::npos);
        // A version this node does not read
        bad = small;
        bad[4] = 2;
        err = restore(*rt, "default/probe-s", wasm, bad, 32 * 1024);
        assert(runtime::deploy_error_is_restore(err) && err.find("version 2") != std::string::npos);
        // Cut short
        bad.assign(small.begin(), small.begin() + 5000);
        err = restore(*rt, "default/probe-s", wasm, bad, 32 * 1024);
        assert(runtime::deploy_error_is_restore(err) && err.find("ended early") != std::string::npos);
        // Not a snapshot
        bad.assign(100, 'x');
        err = restore(*rt, "default/probe-s", wasm, bad, 32 * 1024);
        assert(runtime::deploy_error_is_restore(err) && err.find("K32S") != std::string::npos);
        assert(!rt->is_deployed("default/probe-s"));
    }

    // --- Blockers ---
    {
        const std::string spec_blocked =
            "{\"metadata\":{\"name\":\"d\",\"annotations\":{\"kube32.io/i2c-device\":\"bus=0,addr=0x76\",\"kube32.io/migration\":\"Never\"}},"
            "\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"hardware.kube32.io/ipc\":\"1\",\"esp32.io/gpio-4\":\"1\","
            "\"memory\":\"64Ki\"},\"limits\":{\"hardware.kube32.io/tft-display\":\"1\",\"esp32.io/gpio-4\":\"1\"}},"
            "\"securityContext\":{\"capabilities\":{\"add\":[\"SERVICE_CALL\",\"KEYBOARD\"]}}}],"
            "\"volumes\":[{\"name\":\"data\",\"persistentVolumeClaim\":{\"claimName\":\"notes\"}}]}}";
        const auto b = runtime::static_migration_blockers(spec_blocked);
        assert(b.size() == 6);
        assert(has(b, "grant esp32.io/gpio-4") && has(b, "grant hardware.kube32.io/tft-display") && has(b, "grant capability KEYBOARD") &&
               has(b, "grant kube32.io/i2c-device") && has(b, "volume claim notes on node storage") &&
               has(b, "annotation kube32.io/migration: Never"));
        assert(runtime::static_migration_blockers("{\"metadata\":{\"name\":\"e\"},\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":"
                                                  "{\"requests\":{\"kube32.io/ipc\":\"1\",\"memory\":\"64Ki\"}}}]}}").empty());

        // An entry point, found in the module and by the node
        static const uint8_t kStart[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03, 0x02, 0x01,
                                         0x00, 0x07, 0x0a, 0x01, 0x06, '_',  's',  't',  'a',  'r',  't',  0x00, 0x00, 0x0a, 0x04, 0x01,
                                         0x02, 0x00, 0x0b};
        assert(std::string(runtime::module_entry_point(kStart, sizeof kStart)) == "_start");
        assert(runtime::module_entry_point(wasm.data(), wasm.size()) == nullptr);
        k32test::deploy(*rt, "default/main", std::vector<uint8_t>(kStart, kStart + sizeof kStart), 64 * 1024, "", probe_spec());
        assert(has(host.migration_blockers("default/main"), "entry point _start runs on its own thread"));
        assert(rt->terminate("default/main"));

        // An open file
        const std::string root = "/tmp/kube32_test_pod_snapshot";
        std::system(("rm -rf " + root).c_str());
        assert(hal::node_storage_mount(false, root));
        const std::string vol = "{\"metadata\":{\"name\":\"f\"},\"spec\":{\"containers\":[{\"name\":\"c\",\"image\":\"wasm://x\"}],"
                                "\"volumes\":[{\"name\":\"data\",\"persistentVolumeClaim\":{\"claimName\":\"notes\"}}]}}";
        std::string err;
        hal::CapabilityFirewall::instance().unregister_pod("default/files");
        assert(hal::CapabilityFirewall::instance().register_pod("default/files", vol, err));
        runtime::DeploySpec spec = probe_spec();
        spec.migration_blockers = runtime::static_migration_blockers(vol);
        spec.memory_limit_bytes = 64 * 1024;
        assert(rt->deploy_workload_ex("default/files", wasm.data(), wasm.size(), spec, err) && rt->start("default/files"));
        assert(call(*rt, "default/files", "open_file") == 0);
        const auto fb = host.migration_blockers("default/files");
        assert(fb.size() == 2 && has(fb, "volume claim notes on node storage") && has(fb, "open file notes.txt (descriptor 0)"));
        assert(rt->terminate("default/files"));

        // A call that outlasts the freeze: the pod is not frozen
        runtime::DeploySpec slow = probe_spec();
        slow.gas_budget_per_call = 0;
        k32test::deploy(*rt, "default/slow", wasm, 64 * 1024, "", slow);
        std::thread busy([] { k32test::call_pod("default/slow", "/wait"); });
        // The oldest call in flight is this pod's: another pod's call would let freeze find the pod idle
        assert(wait_for([] { uint32_t age = 0; return AppPodHost::instance().in_flight(age).find("default/slow") != std::string::npos; }));
        std::string why;
        assert(host.freeze("default/slow", 50, why) == -1 && !host.is_frozen("default/slow"));
        std::cout << "[timeout] " << why << std::endl;
        assert(why == "call k32_handle still running after 50 ms");
        busy.join();
        assert(count_of("default/slow") == "2");
        assert(rt->terminate("default/slow"));
    }

    // --- The kubelet endpoints, on the pod HTTP server ---
    const std::string token = "node-token-0123456789";
    const std::string auth = "Authorization: Bearer " + token + "\r\n";
    std::vector<uint8_t> served;
    {
        app::PodServerOptions po;
        po.port = 18741;
        po.workers = 1;
        po.worker_stack = 262144;
        po.token = token;
        app::PodServer node(po);
        assert(node.start());
        k32test::deploy(*rt, "default/web", wasm, 64 * 1024, "", probe_spec());
        assert(k32test::call_pod("default/web", "/setup").body == "1");

        k32test::HttpReply r = k32test::http(po.port, "POST", "/checkpoint/default/web/c");
        assert(r.status == 401);
        r = k32test::http(po.port, "POST", "/checkpoint/default/web/other", auth);
        assert(r.status == 404);
        r = k32test::http(po.port, "POST", "/checkpoint/default/nosuch/c", auth);
        assert(r.status == 404);
        r = k32test::http(po.port, "GET", "/checkpoint/default/web/c", auth);
        assert(r.status == 405);
        r = k32test::http(po.port, "POST", "/checkpoint/default/web/c?leave=away", auth);
        assert(r.status == 400);
        // leave=running: the snapshot, and the pod serves on
        r = k32test::http(po.port, "POST", "/checkpoint/default/web/c", auth);
        std::cout << "[endpoint] " << r.status << " " << r.body.size() << " bytes" << std::endl;
        assert(r.status == 200 && r.has("application/octet-stream") && !host.is_frozen("default/web"));
        const std::string trailer = osal::Sha256::hex(osal::Sha256::of(r.body.data(), r.body.size() - 32));
        assert(r.has("X-Kube32-Snapshot-Sha256: " + trailer) && r.has("X-Kube32-Pod-Time-Ms: "));
        assert(r.has("X-Kube32-Image-Sha256: " + osal::Sha256::hex(osal::Sha256::of(wasm.data(), wasm.size()))));
        assert(count_of("default/web") == "2");
        // leave=frozen, then a thaw
        r = k32test::http(po.port, "POST", "/checkpoint/default/web/c?leave=frozen&timeoutMs=2000", auth);
        assert(r.status == 200 && host.is_frozen("default/web"));
        served.assign(r.body.begin(), r.body.end());
        r = k32test::http(po.port, "POST", "/thaw/default/web/c", auth);
        assert(r.status == 200 && !host.is_frozen("default/web"));
        r = k32test::http(po.port, "POST", "/thaw/default/web/c", auth);
        assert(r.status == 404);
        // A pod that cannot move is refused with every reason
        runtime::DeploySpec blocked = probe_spec();
        blocked.migration_blockers = {"grant esp32.io/gpio-4", "annotation kube32.io/migration: Never"};
        k32test::deploy(*rt, "default/pinned", wasm, 64 * 1024, "", blocked);
        r = k32test::http(po.port, "POST", "/checkpoint/default/pinned/c", auth);
        std::cout << "[blocked] " << r.status << " " << r.body << std::endl;
        assert(r.status == 409 && r.body == "{\"reason\":\"NotMovable\",\"blockers\":[\"grant esp32.io/gpio-4\",\"annotation kube32.io/migration: Never\"]}");
        assert(!host.is_frozen("default/pinned"));
        assert(rt->terminate("default/pinned") && rt->terminate("default/web"));
        node.stop();
    }
    // What the endpoint sent restores
    assert(restore(*rt, "default/web", wasm, served, 64 * 1024).empty());
    assert(count_of("default/web") == "3" && call(*rt, "default/web", "check") == kStateNoGrow);
    assert(rt->terminate("default/web"));

    // --- Checkpoint data on the API server ---
    {
        auto store = storage::create_radix_store(storage::StoreOptions{"/tmp/kube32_test_checkpoint_data", false});
        api::HttpServerOptions ao;
        ao.port = 18742;
        ao.worker_stack_bytes = 262144;
        ao.watch_streams = 0;
        ao.max_body_size = 16 * 1024;   // data uploads stream past it
        api::HttpServer apiserver(ao, *store);
        // Room for two snapshots of this size, not three
        api::CheckpointApi data(*store, apiserver.route_handler(), served.size() * 2 + 100);
        apiserver.set_handler(&data);
        assert(apiserver.start());
        const std::string body(served.begin(), served.end());
        const std::string base = "/apis/kube32.io/v1/namespaces/default/checkpoints";
        auto create = [&](const std::string& name) {
            return k32test::http(ao.port, "POST", base, "Content-Type: application/json\r\n",
                                 "{\"apiVersion\":\"kube32.io/v1\",\"kind\":\"Checkpoint\",\"metadata\":{\"name\":\"" + name +
                                     "\"},\"spec\":{\"podName\":\"web\",\"action\":\"Save\"},\"status\":{\"phase\":\"Saved\"}}");
        };
        k32test::HttpReply r = create("one");
        assert(r.status == 201 && r.body.find("\"status\"") == std::string::npos);
        r = k32test::http(ao.port, "POST", base, "", "{\"metadata\":{\"name\":\"bad\"},\"spec\":{}}");
        assert(r.status == 422);
        r = k32test::http(ao.port, "GET", base + "/one/data");
        assert(r.status == 404);
        r = k32test::http(ao.port, "PUT", base + "/nosuch/data", "", body);
        assert(r.status == 404);
        std::string damaged = body;
        damaged[200] ^= 1;
        r = k32test::http(ao.port, "PUT", base + "/one/data", "", damaged);
        assert(r.status == 422 && data.count() == 0);
        r = k32test::http(ao.port, "PUT", base + "/one/data", "", body);
        std::cout << "[data put] " << r.status << " " << r.body << std::endl;
        assert(r.status == 200 && r.body.find("\"sizeBytes\":" + std::to_string(body.size())) != std::string::npos);
        assert(r.body.find("\"snapshotSha256\":\"" + osal::Sha256::hex(osal::Sha256::of(body.data(), body.size() - 32)) + "\"") != std::string::npos);
        r = k32test::http(ao.port, "GET", base + "/one/data");
        assert(r.status == 200 && r.body == body);
        // kubectl get checkpoints
        r = k32test::http(ao.port, "GET", base, "Accept: application/json;as=Table;v=v1;g=meta.k8s.io\r\n");
        assert(r.status == 200 && r.body.find("\"POD\"") != std::string::npos && r.body.find("\"SIZE\"") != std::string::npos);
        assert(r.body.find("\"" + std::to_string((body.size() + 1023) / 1024) + "Ki\"") != std::string::npos);
        // The cap: a third snapshot drops the oldest
        assert(create("two").status == 201 && create("three").status == 201);
        assert(k32test::http(ao.port, "PUT", base + "/two/data", "", body).status == 200);
        assert(k32test::http(ao.port, "PUT", base + "/three/data", "", body).status == 200);
        assert(data.count() == 2 && k32test::http(ao.port, "GET", base + "/one/data").status == 404);
        // Larger than the cap
        assert(k32test::http(ao.port, "PUT", base + "/one/data", "", body + body + body).status == 413);
        // Deleting the Checkpoint drops its data
        assert(k32test::http(ao.port, "DELETE", base + "/two").status == 200);
        assert(data.count() == 1 && data.used_bytes() == body.size());
        apiserver.stop();
    }

    // --- A snapshot taken on an ESP32-S3 board restores here ---
    // tests/data/snapshot-probe-esp32s3.k32s: the probe on a Cardputer (no
    // PSRAM, memory limit 8Ki, IPC granted) after GET /setup and two GET
    // /count, taken through the kubelet endpoint with leave=running
    {
        const std::vector<uint8_t> board = k32test::read_file(std::string(KUBE32_SOURCE_DIR) + "/tests/data/snapshot-probe-esp32s3.k32s");
        assert(!board.empty());
        runtime::SnapshotHeader h;
        std::string why;
        assert(runtime::parse_snapshot_header(board.data(), board.size(), h, why));
        std::cout << "[board] " << board.size() << " bytes, " << h.memory_bytes << " bytes of memory, " << h.sections << " sections, pod time "
                  << h.pod_time_ms << " ms" << std::endl;
        assert(h.version == 1 && h.memory_pages == 1 && h.memory_bytes == 8 * 1024 && h.unix_ms > 0);
        assert(restore(*rt, "default/snapshot-probe", wasm, board, 8 * 1024).empty());
        // The board's state: globals, tables and channel from setup, three requests
        assert(call(*rt, "default/snapshot-probe", "check") == kStateNoGrow);
        assert(count_of("default/snapshot-probe") == "4");
        assert(call(*rt, "default/snapshot-probe", "time") >= static_cast<int32_t>(h.pod_time_ms));
        const int32_t handle = call(*rt, "default/snapshot-probe", "peek", {12});
        auto open = ipc::ChannelManager::instance().handles_of("default/snapshot-probe");
        assert(handle > 0 && open.size() == 1 && open[0].first == handle && open[0].second == "pod://snapshot-probe/in");
        assert(rt->terminate("default/snapshot-probe"));
    }

    ipc::ChannelManager::instance().shutdown();
    std::cout << "PASS" << std::endl;
    return 0;
}
