// Firmware rollouts on the simulator's slots (hal::FileFirmwareSlots):
//   - the slots' rollback state machine (NEW, pending verify, confirmed,
//     aborted, rejected) and the version read from an image
//   - staging an image on the API server in ranges, resuming, refusing a bad
//     image, reading ranges back
//   - a rollout over a control plane and two workers: one node at a time,
//     workers first, each node downloading through its kubelet, confirming
//     itself after healthy heartbeats; the control plane last, from its own
//     staged slot
//   - an image that resets before it confirms itself: the bootloader goes
//     back, the node reports it, the rollout stops there
//   - an image that never reaches the API server rejects itself in time

#include "api/firmware_api.hpp"
#include "api/http_server.hpp"
#include "controller/firmware_rollout_controller.hpp"
#include "hal/firmware_slots.hpp"
#include "kubelet/kubelet.hpp"
#include "kubelet/node_status_reporter.hpp"
#include "mem/fixed_text.hpp"
#include "models/firmware.hpp"
#include "models/kube_models.hpp"
#include "osal/sha256.hpp"
#include "osal/socket.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"
#include "runtime/wasm_runtime.hpp"
#include "storage/store.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <unistd.h>

// Largest single allocation while tracking is on, every thread: a download
// must never hold an image-sized buffer
static std::atomic<bool> g_track{false};
static std::atomic<size_t> g_max_alloc{0};
void* operator new(size_t n) {
    if (g_track.load(std::memory_order_relaxed)) {
        size_t m = g_max_alloc.load();
        while (n > m && !g_max_alloc.compare_exchange_weak(m, n)) {}
    }
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

using namespace kube32;

static const uint16_t kPort = 18720;
static const size_t kSlot = 1024 * 1024;

// An image as esptool writes it: magic byte, the version marker in its
// constant data, the SHA-256 of everything before it in the last 32 bytes
static std::vector<uint8_t> make_image(const std::string& version, size_t size, uint32_t seed, size_t tag_at = 4096) {
    std::vector<uint8_t> img(size);
    uint32_t x = seed * 2654435761u + 1;
    for (auto& b : img) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        b = static_cast<uint8_t>(x);
    }
    img[0] = hal::FileFirmwareSlots::kImageMagic;
    const std::string tag = "kube32-version:" + version;
    std::memcpy(img.data() + tag_at, tag.data(), tag.size());
    img[tag_at + tag.size()] = 0;
    const osal::Sha256::Digest d = osal::Sha256::of(img.data(), img.size() - 32);
    std::memcpy(img.data() + img.size() - 32, d.data(), 32);
    return img;
}

static std::string sha_of(const std::vector<uint8_t>& v) { return osal::Sha256::hex(osal::Sha256::of(v.data(), v.size())); }

static std::string temp_dir(const char* name) {
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/k32fw_%s_XXXXXX", name);
    char* d = mkdtemp(tmpl);
    assert(d);
    return d;
}

// One HTTP request to the test's API server: status and body
struct Reply {
    int status{0};
    std::string body;
};
static Reply http(const std::string& method, const std::string& path, const std::string& body = std::string(),
                  const std::string& content_type = "application/json") {
    osal::Socket s;
    Reply r;
    if (!s.create(osal::SocketType::Tcp) || !s.connect("127.0.0.1", kPort, 2000)) return r;
    s.set_timeout(10000, 10000);
    std::string head = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: " + content_type +
                       "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    if (!s.send_all(head.data(), head.size()) || (!body.empty() && !s.send_all(body.data(), body.size()))) return r;
    std::string raw;
    char buf[8192];
    ssize_t n;
    while ((n = s.recv(buf, sizeof buf)) > 0) raw.append(buf, static_cast<size_t>(n));
    const size_t sp = raw.find(' ');
    const size_t end = raw.find("\r\n\r\n");
    if (sp == std::string::npos || end == std::string::npos) return r;
    r.status = std::atoi(raw.c_str() + sp + 1);
    r.body = raw.substr(end + 4);
    return r;
}

static Reply put_range(const std::vector<uint8_t>& img, const std::string& sha, size_t offset, size_t len) {
    return http("PUT", "/apis/kube32.io/v1/firmware/image?offset=" + std::to_string(offset) + "&size=" + std::to_string(img.size()) +
                           "&sha256=" + sha, std::string(reinterpret_cast<const char*>(img.data()) + offset, len),
                "application/octet-stream");
}

static bool upload(const std::vector<uint8_t>& img, size_t chunk = 48 * 1024) {
    const std::string sha = sha_of(img);
    for (size_t off = 0; off < img.size(); off += chunk) {
        const Reply r = put_range(img, sha, off, std::min(chunk, img.size() - off));
        if (r.status != 200) {
            std::cout << "upload at " << off << ": " << r.status << " " << r.body << std::endl;
            return false;
        }
    }
    return true;
}

static models::JsonValue json(const std::string& text) {
    models::JsonValue v;
    std::string err;
    bool ok = models::parse_json(text, v, err);
    assert(ok);
    return v;
}

static std::string annotation(storage::Store& store, const std::string& node, const char* key) {
    std::string j;
    if (!store.get("/registry/nodes/" + node, j).ok()) return std::string();
    models::JsonValue d = json(j);
    const models::JsonValue* a = d.get("metadata") ? d.get("metadata")->get("annotations") : nullptr;
    return a ? a->get_string(key, "") : std::string();
}

static std::string condition(storage::Store& store, const std::string& node, const char* field) {
    std::string j;
    if (!store.get("/registry/nodes/" + node, j).ok()) return std::string();
    models::JsonValue d = json(j);
    const models::JsonValue* st = d.get("status");
    const models::JsonValue* conds = st ? st->get("conditions") : nullptr;
    if (!conds) return std::string();
    for (const auto& c : conds->arr_val) {
        if (c.get_string("type", "") == models::kConditionFirmwareUpdate) return c.get_string(field, "");
    }
    return std::string();
}

static bool has_event(storage::Store& store, const std::string& reason, const std::string& object) {
    std::vector<std::string> evs;
    store.list("/registry/events/", evs);
    for (const auto& e : evs) {
        models::JsonValue d = json(e);
        const models::JsonValue* inv = d.get("involvedObject");
        if (d.get_string("reason", "") == reason && inv && inv->get_string("name", "") == object) return true;
    }
    return false;
}

static void test_slots(const std::vector<uint8_t>& v1, const std::vector<uint8_t>& v2) {
    const std::string dir = temp_dir("slots");
    const hal::FirmwareInfo builtin{"v1", sha_of(v1), "", false};
    std::string why;
    {
        hal::FileFirmwareSlots s(dir, builtin, kSlot);
        hal::FirmwareInfo r = s.running();
        assert(r.version == "v1" && r.sha256 == sha_of(v1) && r.slot == "ota_0" && !r.pending_verify);
        assert(s.passive_slot() == "ota_1");
        // Not an image, or one whose appended SHA-256 does not match
        assert(s.begin(why));
        const uint8_t junk[4] = {1, 2, 3, 4};
        assert(!s.write(junk, sizeof junk, why) && why.find("not an ESP32 firmware image") != std::string::npos);
        assert(s.begin(why));
        std::vector<uint8_t> bad = v2;
        bad[100] ^= 1;
        assert(s.write(bad.data(), bad.size(), why) && !s.finish(why));
        // A good one, switched to
        assert(s.begin(why) && s.write(v2.data(), v2.size(), why) && s.finish(why));
        std::string hex;
        assert(s.passive_sha256(v2.size(), hex) && hex == sha_of(v2));
        assert(s.activate(why));
        s.set_note("k", "value");
    }
    {
        // The bootloader boots the new image unconfirmed
        hal::FileFirmwareSlots s(dir, builtin, kSlot);
        hal::FirmwareInfo r = s.running();
        assert(r.version == "v2" && r.sha256 == sha_of(v2) && r.slot == "ota_1" && r.pending_verify);
        assert(s.note("k") == "value");
        assert(!s.begin(why) && why.find("not confirmed itself") != std::string::npos);
        // It resets without confirming itself
    }
    {
        // ... so the bootloader goes back to the previous slot
        hal::FileFirmwareSlots s(dir, builtin, kSlot);
        hal::FirmwareInfo r = s.running();
        assert(r.version == "v1" && r.slot == "ota_0" && !r.pending_verify);
        assert(s.activate(why));   // ota_1 still holds v2
    }
    {
        hal::FileFirmwareSlots s(dir, builtin, kSlot);
        assert(s.running().pending_verify);
        s.confirm();
        assert(!s.running().pending_verify && !s.reject());
    }
    {
        hal::FileFirmwareSlots s(dir, builtin, kSlot);
        hal::FirmwareInfo r = s.running();
        assert(r.version == "v2" && r.slot == "ota_1" && !r.pending_verify);
        assert(s.begin(why) && s.write(v1.data(), v1.size(), why) && s.finish(why) && s.activate(why));
    }
    {
        // An image that rejects itself: back at once
        hal::FileFirmwareSlots s(dir, builtin, kSlot);
        assert(s.running().slot == "ota_0" && s.running().pending_verify);
        assert(s.reject() && s.reboot_requested());
    }
    {
        hal::FileFirmwareSlots s(dir, builtin, kSlot);
        assert(s.running().version == "v2" && s.running().slot == "ota_1" && !s.running().pending_verify);
    }
    // The version marker across the reader's window boundary
    const std::vector<uint8_t> edge = make_image("v9-edge", 8192, 9, 1015);
    const std::string v = hal::find_image_version(edge.size(), [&](size_t off, void* dst, size_t len) {
        std::memcpy(dst, edge.data() + off, len);
        return true;
    });
    assert(v == "v9-edge");
    std::cout << "[slots] rollback state machine and version marker ok" << std::endl;
}

// A node of the simulated cluster: its slots directory and kubelet. A
// reboot is a new FileFirmwareSlots (the bootloader) and a new kubelet.
struct Node {
    std::string name;
    std::string dir;
    bool control_plane{false};
    hal::FirmwareInfo builtin;
    std::shared_ptr<hal::FileFirmwareSlots> slots;
    std::unique_ptr<kubelet::Kubelet> kubelet;
    uint16_t api_port{kPort};
    uint32_t confirm_timeout_ms{20000};
    int boots{0};
};

static runtime::Runtime* g_runtime = nullptr;

static void boot(Node& n) {
    n.slots = std::make_shared<hal::FileFirmwareSlots>(n.dir, n.builtin, kSlot);
    kubelet::KubeletConfig cfg;
    cfg.node_name = n.name;
    cfg.api_server_host = "127.0.0.1";
    cfg.api_server_port = n.api_port;
    cfg.control_plane = n.control_plane;
    cfg.heartbeat_interval_ms = 200;
    cfg.sync_frequency_ms = 200;
    cfg.pod_status_frequency_ms = 500;
    cfg.task_stack_size = 256 * 1024;
    cfg.firmware_slots = n.slots;
    cfg.firmware.check_interval_ms = 100;
    cfg.firmware.confirm_after_ms = 600;
    cfg.firmware.confirm_timeout_ms = n.confirm_timeout_ms;
    cfg.firmware.range_bytes = 16384;
    cfg.firmware.reboot_delay_ms = 100;
    cfg.firmware.local_staging = n.control_plane;
    n.kubelet = std::make_unique<kubelet::Kubelet>(cfg, *g_runtime);
    assert(n.kubelet->start());
    n.boots++;
}

static void power_off(Node& n) {
    if (n.kubelet) n.kubelet->stop();
    n.kubelet.reset();
    n.slots.reset();
}

// The API server's handler, swapped when the control plane "reboots"
struct HandlerSlot : api::HttpHandler {
    std::atomic<api::HttpHandler*> target{nullptr};
    bool handle(api::HttpConnection& c) override { auto* h = target.load(); return h && h->handle(c); }
    bool streams_body(const api::HttpRequest& r) override { auto* h = target.load(); return h && h->streams_body(r); }
};

struct ControlPlane {
    std::unique_ptr<storage::Store> store;
    std::unique_ptr<api::HttpServer> server;
    HandlerSlot handler;
    std::vector<std::unique_ptr<api::FirmwareApi>> apis;   // every boot's, kept until the end
    std::unique_ptr<controller::FirmwareRolloutController> controller;
    Node* node{nullptr};

    // The control plane's own boot: its staging API and controller come up
    // with its slots
    void boot_services() {
        apis.push_back(std::make_unique<api::FirmwareApi>(*node->slots, *store, server->route_handler()));
        api::FirmwareApi* fw = apis.back().get();
        handler.target.store(fw);
        controller = std::make_unique<controller::FirmwareRolloutController>(*store, [fw] { return fw->staged(); });
    }
    std::string staged_phase() { return apis.back()->staged().phase; }
};

// Runs the cluster until `done` holds: the controller's passes and the
// reboots the nodes ask for, in the order they happen
static bool run_until(ControlPlane& cp, std::vector<Node*>& nodes, std::vector<std::string>& reboots, uint32_t timeout_ms,
                      const std::function<bool()>& done, const std::function<void(Node&)>& on_boot = nullptr) {
    const uint64_t t0 = osal::System::get_uptime_ms();
    while (osal::System::get_uptime_ms() - t0 < timeout_ms) {
        cp.controller->reconcile();
        for (Node* n : nodes) {
            if (!n->slots || !n->slots->reboot_requested()) continue;
            reboots.push_back(n->name);
            power_off(*n);
            boot(*n);
            if (n == cp.node) cp.boot_services();
            if (on_boot) on_boot(*n);
        }
        if (done()) return true;
        osal::Thread::sleep_ms(50);
    }
    return false;
}

static std::string rollout_status(storage::Store& store, const char* field) {
    std::string j;
    if (!store.get("/registry/firmwarerollouts/firmware", j).ok()) return std::string();
    models::JsonValue d = json(j);
    const models::JsonValue* st = d.get("status");
    return st ? st->get_string(field, "") : std::string();
}

static void create_rollout(const std::string& sha) {
    const Reply r = http("POST", "/apis/kube32.io/v1/firmwarerollouts",
                         "{\"apiVersion\":\"kube32.io/v1\",\"kind\":\"FirmwareRollout\",\"metadata\":{\"name\":\"firmware\"},"
                         "\"spec\":{\"sha256\":\"" + sha + "\",\"nodeTimeoutSeconds\":60}}");
    if (r.status != 201) std::cout << r.status << " " << r.body << std::endl;
    assert(r.status == 201);
}

// The Node object and the heartbeat of a board without PSRAM, with every
// firmware annotation and the longest condition message, still fit the
// kubelet's fixed buffers (2048 bytes for the Node, 1024 for a heartbeat)
static void test_node_documents_fit() {
    kubelet::NodeStatusReporter r("esp32-a1b2c3", "192.168.4.2", false);
    r.set_labels({{"kube32.io/board", "cardputer"}, {"i2c.kube32.io/0x76", "bus-0"}, {"i2c.kube32.io/0x28", "bus-0"}});
    r.set_kubelet_port(7000);
    r.set_firmware_version("v0.12.3-120-g1a2b3c4-dirty");
    const std::string sha(64, 'a');
    r.set_annotations({{models::kAnnotationFirmwareVersion, "v0.12.3-120-g1a2b3c4-dirty"}, {models::kAnnotationFirmwareSha256, sha},
                       {models::kAnnotationFirmwareSlot, "ota_1"}, {models::kAnnotationFirmwareState, "Verifying"},
                       {models::kAnnotationFirmwareFailed, sha}});
    kubelet::NodeTelemetry t = r.collect_telemetry();
    t.firmware_reported = true;
    t.firmware_updating = true;
    std::strcpy(t.firmware_reason, "Downloading");
    std::memset(t.firmware_message, 'x', sizeof(t.firmware_message) - 1);
    t.firmware_message[sizeof(t.firmware_message) - 1] = 0;
    char reg[2048], hb[1024];
    mem::FixedText reg_text(reg), hb_text(hb);
    const bool reg_fits = r.write_registration_json(reg_text, t);
    const bool hb_fits = r.write_heartbeat_patch_json(hb_text, t, false);
    std::cout << "[node] registration " << r.build_registration_json(t).size() << " bytes, heartbeat "
              << r.build_heartbeat_patch_json(t, false).size() << " bytes" << std::endl;
    assert(reg_fits && hb_fits);
    models::JsonValue doc = json(std::string(reg_text.view()));
    assert(doc.get("metadata")->get("annotations")->get_string(models::kAnnotationFirmwareState, "") == "Verifying");
}

int main() {
    test_node_documents_fit();
    const std::vector<uint8_t> v1 = make_image("v1", 96 * 1024, 1);
    const std::vector<uint8_t> v2 = make_image("v2", 300 * 1024 + 17, 2);
    const std::vector<uint8_t> v3 = make_image("v3-broken", 200 * 1024, 3);
    test_slots(v1, v2);

    auto runtime = runtime::create_wasm_runtime();
    g_runtime = runtime.get();

    ControlPlane cp;
    cp.store = storage::create_radix_store(storage::StoreOptions{"/tmp", false});
    api::HttpServerOptions o;
    o.port = kPort;
    o.workers = 3;
    o.worker_stack_bytes = 262144;
    o.watch_streams = 0;
    cp.server = std::make_unique<api::HttpServer>(o, *cp.store);
    cp.server->set_handler(&cp.handler);
    assert(cp.server->start());

    Node cpn{"cp", temp_dir("cp"), true, {"v1", sha_of(v1), "", false}};
    Node w1{"w1", temp_dir("w1"), false, {"v1", sha_of(v1), "", false}};
    Node w2{"w2", temp_dir("w2"), false, {"v1", sha_of(v1), "", false}};
    cp.node = &cpn;
    boot(cpn);
    cp.boot_services();

    // --- Staging over HTTP ------------------------------------------------
    {
        Reply r = http("GET", "/apis/kube32.io/v1/firmware");
        assert(r.status == 200 && json(r.body).get("status")->get_string("phase", "") == "Empty");
        // The discovery document lists the group and its resource
        r = http("GET", "/apis");
        assert(r.status == 200 && r.body.find("\"kube32.io\"") != std::string::npos);
        r = http("GET", "/apis/kube32.io/v1");
        assert(r.status == 200 && r.body.find("\"firmwarerollouts\"") != std::string::npos);
        // A rollout needs a SHA-256
        r = http("POST", "/apis/kube32.io/v1/firmwarerollouts", "{\"metadata\":{\"name\":\"x\"},\"spec\":{}}");
        assert(r.status == 422);

        const std::string sha2 = sha_of(v2);
        // Ranges in order; one out of order is refused with where to go on
        r = put_range(v2, sha2, 0, 40000);
        assert(r.status == 200);
        models::JsonValue st = json(r.body);
        assert(st.get("status")->get_string("phase", "") == "Receiving" && st.get("status")->get_int("received", 0) == 40000);
        r = put_range(v2, sha2, 50000, 1000);
        assert(r.status == 409 && r.body.find("continues at offset 40000") != std::string::npos);
        // Resumed where it stopped, in ranges of the gateway's size
        for (size_t off = 40000; off < v2.size(); off += 48 * 1024) {
            r = put_range(v2, sha2, off, std::min<size_t>(48 * 1024, v2.size() - off));
            assert(r.status == 200);
        }
        st = json(r.body);
        assert(st.get("status")->get_string("phase", "") == "Staged" && st.get("status")->get_string("version", "") == "v2");
        assert(st.get("status")->get_int("size", 0) == static_cast<int64_t>(v2.size()));
        // Ranges read back
        r = http("GET", "/apis/kube32.io/v1/firmware/image?sha256=" + sha2 + "&offset=1000&length=5000");
        assert(r.status == 200 && r.body.size() == 5000 && std::memcmp(r.body.data(), v2.data() + 1000, 5000) == 0);
        r = http("GET", "/apis/kube32.io/v1/firmware/image?sha256=" + sha_of(v1) + "&offset=0&length=10");
        assert(r.status == 409);
        r = http("GET", "/apis/kube32.io/v1/firmware/image?sha256=" + sha2 + "&offset=" + std::to_string(v2.size()));
        assert(r.status == 416);
        // An image whose bytes do not match the announced SHA-256
        std::vector<uint8_t> bad = v1;
        bad[200] ^= 0xff;
        const std::string claimed = sha_of(v1);
        r = put_range(bad, claimed, 0, bad.size());
        assert(r.status == 422 && r.body.find("SHA-256") != std::string::npos);
        assert(cp.staged_phase() == "Failed");
        assert(upload(v2));
        assert(cp.staged_phase() == "Staged");
        // What is staged survives a restart of the staging API (notes)
        api::FirmwareApi again(*cpn.slots, *cp.store, cp.server->route_handler());
        assert(again.staged().phase == "Staged" && again.staged().sha256 == sha2 && again.staged().version == "v2");
        std::cout << "[staging] ranges, resume, checks and read-back ok" << std::endl;
    }

    // --- Rollout of v2 over the control plane and two workers --------------
    boot(w1);
    boot(w2);
    std::vector<Node*> nodes{&cpn, &w1, &w2};
    std::vector<std::string> reboots;
    auto registered = [&] {
        for (Node* n : nodes) if (annotation(*cp.store, n->name, models::kAnnotationFirmwareState) != "Confirmed") return false;
        return true;
    };
    assert(run_until(cp, nodes, reboots, 15000, registered));
    assert(annotation(*cp.store, "w1", models::kAnnotationFirmwareVersion) == "v1");
    assert(condition(*cp.store, "w1", "reason") == "UpToDate");

    const std::string sha2 = sha_of(v2);
    create_rollout(sha2);
    // A new image cannot be staged while the rollout runs
    assert(put_range(v1, sha_of(v1), 0, 1000).status == 409);
    g_max_alloc = 0;
    g_track = true;
    const bool complete = run_until(cp, nodes, reboots, 60000, [&] { return rollout_status(*cp.store, "phase") == "Complete"; });
    g_track = false;
    if (!complete) {
        std::cout << "rollout: " << rollout_status(*cp.store, "phase") << " " << rollout_status(*cp.store, "message") << std::endl;
        for (Node* n : nodes) std::cout << n->name << ": " << condition(*cp.store, n->name, "message") << std::endl;
    }
    assert(complete);
    // One node at a time, workers first, the control plane last
    if (reboots != std::vector<std::string>{"w1", "w2", "cp"}) {
        std::cout << "reboots:";
        for (const auto& r : reboots) std::cout << " " << r;
        std::cout << std::endl;
        for (Node* n : nodes) std::cout << n->name << ": " << condition(*cp.store, n->name, "message") << std::endl;
    }
    assert((reboots == std::vector<std::string>{"w1", "w2", "cp"}));
    for (Node* n : nodes) {
        assert(annotation(*cp.store, n->name, models::kAnnotationFirmwareSha256) == sha2);
        assert(annotation(*cp.store, n->name, models::kAnnotationFirmwareVersion) == "v2");
        assert(annotation(*cp.store, n->name, models::kAnnotationFirmwareSlot) == "ota_1");
        assert(annotation(*cp.store, n->name, models::kAnnotationFirmwareState) == "Confirmed");
        assert(has_event(*cp.store, "FirmwareUpdated", n->name));
    }
    assert(has_event(*cp.store, "FirmwareRolloutComplete", "firmware"));
    // The control plane runs what it had staged: nothing is staged any more
    assert(cp.staged_phase() == "Empty");
    // kubectl get nodes -o wide: the firmware in OS-IMAGE
    {
        osal::Thread::sleep_ms(300);
        std::string j;
        assert(cp.store->get("/registry/nodes/w2", j).ok());
        assert(j.find("\"osImage\":\"kube32 v2 (") != std::string::npos);
    }
    std::cout << "[rollout] v2 on w1, w2, then cp; largest allocation " << g_max_alloc.load() << " bytes (image "
              << v2.size() << ")" << std::endl;
    assert(g_max_alloc.load() < 64 * 1024);

    // --- An image that resets before it confirms itself ---------------------
    assert(http("DELETE", "/apis/kube32.io/v1/firmwarerollouts/firmware").status == 200);
    assert(upload(v3));
    const std::string sha3 = sha_of(v3);
    reboots.clear();
    create_rollout(sha3);
    // The broken image: once w1 boots it, it resets (a panic) before its
    // heartbeats confirm it
    auto crash_v3 = [&](Node& n) {
        if (n.slots->running().version != "v3-broken") return;
        osal::Thread::sleep_ms(150);
        n.slots->reboot();
    };
    const bool failed = run_until(cp, nodes, reboots, 30000, [&] { return rollout_status(*cp.store, "phase") == "Failed"; }, crash_v3);
    assert(failed);
    // w1 is back on v2, says so, and the rollout went no further
    assert((reboots == std::vector<std::string>{"w1", "w1"}));
    assert(annotation(*cp.store, "w1", models::kAnnotationFirmwareSha256) == sha2);
    assert(annotation(*cp.store, "w1", models::kAnnotationFirmwareFailed) == sha3);
    assert(condition(*cp.store, "w1", "reason") == "RolledBack");
    assert(has_event(*cp.store, "FirmwareRolledBack", "w1"));
    assert(rollout_status(*cp.store, "message").find("w1") != std::string::npos);
    assert(annotation(*cp.store, "w2", models::kAnnotationFirmwareTarget) == sha2);
    assert(annotation(*cp.store, "cp", models::kAnnotationFirmwareTarget) == sha2);
    // Failed is final: w1 does not try the image again
    assert(!run_until(cp, nodes, reboots, 1500, [&] { return reboots.size() > 2; }));
    // Deleting the rollout releases w1, which forgets the failure
    assert(http("DELETE", "/apis/kube32.io/v1/firmwarerollouts/firmware").status == 200);
    assert(run_until(cp, nodes, reboots, 15000, [&] {
        return annotation(*cp.store, "w1", models::kAnnotationFirmwareFailed).empty() &&
               annotation(*cp.store, "w1", models::kAnnotationFirmwareTarget).empty();
    }));
    std::cout << "[rollback] v3 reset before confirming on w1; back on v2, rollout stopped" << std::endl;

    // --- An image that never reaches the API server -------------------------
    {
        power_off(w2);
        // w2's slot gets v3 and is switched to by hand; then it boots
        // pointed at an API server that does not answer
        {
            hal::FileFirmwareSlots s(w2.dir, w2.builtin, kSlot);
            std::string why;
            assert(s.begin(why) && s.write(v3.data(), v3.size(), why) && s.finish(why) && s.activate(why));
            s.set_note("try", sha3);
        }
        w2.api_port = 1;
        w2.confirm_timeout_ms = 1500;
        boot(w2);
        assert(w2.slots->running().version == "v3-broken" && w2.slots->running().pending_verify);
        const uint64_t t0 = osal::System::get_uptime_ms();
        while (!w2.slots->reboot_requested() && osal::System::get_uptime_ms() - t0 < 5000) osal::Thread::sleep_ms(50);
        assert(w2.slots->reboot_requested());
        power_off(w2);
        w2.api_port = kPort;
        w2.confirm_timeout_ms = 20000;
        boot(w2);
        assert(w2.slots->running().version == "v2" && !w2.slots->running().pending_verify);
        assert(w2.kubelet->firmware()->state() == kubelet::FirmwareAgent::State::RolledBack);
        assert(std::string(w2.kubelet->firmware()->condition().message).find("did not report Ready") != std::string::npos);
        assert(run_until(cp, nodes, reboots, 5000, [&] { return annotation(*cp.store, "w2", models::kAnnotationFirmwareFailed) == sha3; }));
        std::cout << "[timeout] an image without the API server rejected itself" << std::endl;
    }

    for (Node* n : nodes) power_off(*n);
    cp.server->stop();
    std::cout << "test_firmware_rollout: all passed" << std::endl;
    return 0;
}
