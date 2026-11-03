// What the control plane's API server takes from the heap per request when
// pods carry their module inline (kube32.io/wasm-base64, as kube32-cli deploy
// creates them), measured with the heap of a board.
//
// Every allocation of this process goes through the board model below: blocks
// of up to 4096 bytes come from internal RAM first and larger ones from PSRAM
// first (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL), each falling back to the other,
// and std::bad_alloc when neither has room, as heap_caps_malloc_default() does
// on the ESP32-S3 with CONFIG_SPIRAM_USE_MALLOC.

#include "api/http_server.hpp"
#include "api/watch_hub.hpp"
#include "app/ingress_controller.hpp"
#include "controller/deployment_controller.hpp"
#include "controller/device_controller.hpp"
#include "controller/node_lifecycle_controller.hpp"
#include "controller/workload_controller.hpp"
#include "kubelet/kube_api_client.hpp"
#include "models/kube_models.hpp"
#include "osal/sha256.hpp"
#include "osal/socket.hpp"
#include "osal/thread.hpp"
#include "scheduler/scheduler.hpp"
#include "storage/flash_log.hpp"
#include "storage/store.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <algorithm>
#include <iostream>
#include <mutex>
#include <new>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

// =============================================================================
// Board heap model
// =============================================================================
namespace board {

constexpr size_t kAlwaysInternal = 4096;
constexpr size_t kUnlimited = ~static_cast<size_t>(0);

struct Pool {
    size_t used{0};
    size_t peak{0};
    size_t cap{kUnlimited};
};

std::mutex& mu() {
    static std::mutex m;
    return m;
}
Pool internal_ram, psram;
size_t total_peak = 0;
size_t largest = 0;
std::atomic<uint64_t> refused{0};

// 16 bytes in front of every block: its size and pool
struct alignas(16) Header {
    size_t size;
    uint32_t pool;   // 0 internal, 1 PSRAM
};

bool take(Pool& p, size_t n) {
    if (p.cap != kUnlimited && p.used + n > p.cap) return false;
    p.used += n;
    if (p.used > p.peak) p.peak = p.used;
    return true;
}

void* allocate(size_t n) {
    uint32_t pool = 0;
    {
        std::lock_guard<std::mutex> lock(mu());
        const bool small = n <= kAlwaysInternal;
        Pool& first = small ? internal_ram : psram;
        Pool& second = small ? psram : internal_ram;
        if (take(first, n)) {
            pool = small ? 0 : 1;
        } else if (take(second, n)) {
            pool = small ? 1 : 0;
        } else {
            refused.fetch_add(1);
            throw std::bad_alloc();
        }
        const size_t total = internal_ram.used + psram.used;
        if (total > total_peak) total_peak = total;
        if (n > largest) largest = n;
    }
    void* raw = std::malloc(sizeof(Header) + n);
    if (!raw) throw std::bad_alloc();
    Header* h = static_cast<Header*>(raw);
    h->size = n;
    h->pool = pool;
    return h + 1;
}

void release(void* p) {
    if (!p) return;
    Header* h = static_cast<Header*>(p) - 1;
    {
        std::lock_guard<std::mutex> lock(mu());
        (h->pool ? psram : internal_ram).used -= h->size;
    }
    std::free(h);
}

struct Usage {
    size_t internal_ram;
    size_t psram;
    size_t total() const { return internal_ram + psram; }
};

Usage in_use() {
    std::lock_guard<std::mutex> lock(mu());
    return {internal_ram.used, psram.used};
}

// Starts a measurement: peaks from now on
Usage mark() {
    std::lock_guard<std::mutex> lock(mu());
    internal_ram.peak = internal_ram.used;
    psram.peak = psram.used;
    total_peak = internal_ram.used + psram.used;
    largest = 0;
    return {internal_ram.used, psram.used};
}

struct Window {
    size_t peak_total;    // highest use above the mark
    size_t peak_psram;
    size_t peak_internal;
    size_t retained;      // still in use above the mark at the end
    size_t largest;       // largest single block
};

Window since(const Usage& m) {
    std::lock_guard<std::mutex> lock(mu());
    auto above = [](size_t v, size_t base) { return v > base ? v - base : 0; };
    return {above(total_peak, m.total()), above(psram.peak, m.psram), above(internal_ram.peak, m.internal_ram),
            above(internal_ram.used + psram.used, m.total()), largest};
}

// Room left from now on: what a board has free in each pool
void limit(size_t internal_free, size_t psram_free) {
    std::lock_guard<std::mutex> lock(mu());
    internal_ram.cap = internal_free == kUnlimited ? kUnlimited : internal_ram.used + internal_free;
    psram.cap = psram_free == kUnlimited ? kUnlimited : psram.used + psram_free;
}

void unlimit() { limit(kUnlimited, kUnlimited); }

} // namespace board

void* operator new(size_t n) { return board::allocate(n ? n : 1); }
void* operator new[](size_t n) { return board::allocate(n ? n : 1); }
void* operator new(size_t n, const std::nothrow_t&) noexcept {
    try { return board::allocate(n ? n : 1); } catch (...) { return nullptr; }
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
    try { return board::allocate(n ? n : 1); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept { board::release(p); }
void operator delete[](void* p) noexcept { board::release(p); }
void operator delete(void* p, size_t) noexcept { board::release(p); }
void operator delete[](void* p, size_t) noexcept { board::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { board::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { board::release(p); }

using namespace kube32;

// =============================================================================
// Cluster pieces
// =============================================================================

static std::string base64(const std::vector<uint8_t>& in) {
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t n = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += a[n >> 18]; out += a[(n >> 12) & 63]; out += a[(n >> 6) & 63]; out += a[n & 63];
    }
    if (i < in.size()) {
        uint32_t n = in[i] << 16 | (i + 1 < in.size() ? in[i + 1] << 8 : 0);
        out += a[n >> 18]; out += a[(n >> 12) & 63];
        out += (i + 1 < in.size()) ? a[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

static std::vector<uint8_t> module_bytes(size_t n, int seed) {
    std::vector<uint8_t> m(n);
    uint32_t x = 2166136261u ^ static_cast<uint32_t>(seed);
    for (size_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        m[i] = static_cast<uint8_t>(x);
    }
    return m;
}

// The Pod kube32-cli deploy renders, as kubectl create sends it
static std::string inline_pod(const std::string& name, const std::vector<uint8_t>& module) {
    const std::string digest = osal::Sha256::hex(osal::Sha256::of(module.data(), module.size()));
    std::string annotations = module.empty() ? std::string()
        : "\"kube32.io/wasm-base64\":\"" + base64(module) + "\",\"kube32.io/wasm-sha256\":\"" + digest +
          "\",\"kube32.io/wasm-size\":\"" + std::to_string(module.size()) + "\"";
    return "{\"apiVersion\":\"v1\",\"kind\":\"Pod\",\"metadata\":{\"annotations\":{" + annotations + "},"
           "\"labels\":{\"app\":\"" + name + "\"},\"name\":\"" + name + "\",\"namespace\":\"default\"},"
           "\"spec\":{\"containers\":[{\"image\":\"wasm://inline/" + name + "-" + digest.substr(0, 12) + "\",\"name\":\"" + name + "\","
           "\"ports\":[{\"containerPort\":7000,\"name\":\"http\"}],"
           "\"resources\":{\"limits\":{\"memory\":\"64Ki\"},\"requests\":{\"memory\":\"64Ki\"}}}],"
           "\"nodeSelector\":{\"kubernetes.io/hostname\":\"n1\"},\"restartPolicy\":\"Always\"}}";
}

static const char* kNode =
    "{\"kind\":\"Node\",\"apiVersion\":\"v1\",\"metadata\":{\"name\":\"n1\",\"labels\":{\"kubernetes.io/hostname\":\"n1\"}},"
    "\"spec\":{},\"status\":{\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
    "\"allocatable\":{\"memory\":\"4Mi\",\"esp32.io/psram\":\"1Mi\"},\"capacity\":{\"memory\":\"4Mi\"}}}";

// One request; the answer is read into a buffer on the stack. Returns the
// status (0: no answer) and the first bytes of the body in `start`.
struct Reply {
    int status{0};
    size_t bytes{0};
    char start[256]{};
};

static Reply call(uint16_t port, const char* method, const char* path, const std::string* body = nullptr,
                  const char* content_type = "application/json", const char* accept = "application/json") {
    Reply r;
    osal::Socket s;
    if (!s.create(osal::SocketType::Tcp) || !s.connect("127.0.0.1", port, 2000)) return r;
    s.set_timeout(10000, 10000);
    char head[640];
    const int n = snprintf(head, sizeof head,
                           "%s %s HTTP/1.1\r\nHost: x\r\nAccept: %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                           method, path, accept, content_type, body ? body->size() : 0);
    if (!s.send_all(head, static_cast<size_t>(n))) return r;
    if (body && !body->empty() && !s.send_all(body->data(), body->size())) return r;
    char buf[2048];
    size_t at_body = 0;
    bool head_seen = false;
    char window[4] = {0, 0, 0, 0};
    for (;;) {
        ssize_t got = s.recv(buf, sizeof buf);
        if (got <= 0) break;
        for (ssize_t i = 0; i < got; ++i) {
            if (!head_seen) {
                window[0] = window[1]; window[1] = window[2]; window[2] = window[3]; window[3] = buf[i];
                if (r.bytes < 12) r.start[r.bytes] = buf[i];
                ++r.bytes;
                if (std::memcmp(window, "\r\n\r\n", 4) == 0) {
                    head_seen = true;
                    r.status = std::atoi(r.start + 9);
                    std::memset(r.start, 0, sizeof r.start);
                    r.bytes = 0;
                }
                continue;
            }
            if (at_body < sizeof r.start - 1) r.start[at_body++] = buf[i];
            ++r.bytes;
        }
    }
    return r;
}

// A watch client that reads its stream into a buffer on the stack and counts
// the event types it sees
class Watcher {
public:
    explicit Watcher(uint16_t port, const char* path = "/api/v1/pods?watch=true") {
        assert(sock_.create(osal::SocketType::Tcp) && sock_.connect("127.0.0.1", port, 2000));
        char head[256];
        const int n = snprintf(head, sizeof head, "GET %s HTTP/1.1\r\nHost: x\r\nAccept: application/json\r\n\r\n", path);
        assert(sock_.send_all(head, static_cast<size_t>(n)));
        thread_ = std::thread([this]() {
            char buf[4096];
            sock_.set_timeout(100, 1000);
            while (!stop_.load()) {
                ssize_t got = sock_.recv(buf, sizeof buf);
                if (got > 0) {
                    for (ssize_t i = 0; i < got; ++i) scan(buf[i]);
                } else if (got == 0) {
                    break;
                }
            }
            ended_.store(true);
        });
    }
    ~Watcher() {
        stop_.store(true);
        thread_.join();
        sock_.close();
    }
    size_t added() const { return counts_[0].load(); }
    size_t modified() const { return counts_[1].load(); }
    size_t deleted() const { return counts_[2].load(); }
    // The stream told its client to start over (410), or ended
    bool restarted() const { return counts_[3].load() > 0 || ended_.load(); }

private:
    void scan(char c) {
        static const char* const kTypes[] = {"\"type\":\"ADDED\"", "\"type\":\"MODIFIED\"", "\"type\":\"DELETED\"", "\"type\":\"ERROR\""};
        std::memmove(window_, window_ + 1, sizeof window_ - 1);
        window_[sizeof window_ - 1] = c;
        for (int t = 0; t < 4; ++t) {
            const size_t len = std::strlen(kTypes[t]);
            if (std::memcmp(window_ + sizeof window_ - len, kTypes[t], len) == 0) counts_[t].fetch_add(1);
        }
    }

    osal::Socket sock_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> ended_{false};
    char window_[24]{};
    std::atomic<size_t> counts_[4]{};
};

struct ControlPlane {
    std::string dir;
    std::unique_ptr<storage::FlashRegion> journal;
    std::unique_ptr<storage::Store> store;
    std::unique_ptr<scheduler::Scheduler> sched;
    std::unique_ptr<api::HttpServer> server;
    uint16_t port;

    explicit ControlPlane(uint16_t p) : port(p) {
        dir = "/tmp/kube32_test_api_memory_" + std::to_string(p);
        assert(std::system(("rm -rf " + dir + " && mkdir -p " + dir).c_str()) == 0);
        // The board's journal partition
        journal = storage::create_file_flash_region(dir + "/journal.img", 1024 * 1024);
        storage::StoreOptions so;
        so.base_path = dir;
        so.flash_region = journal.get();
        store = storage::create_radix_store(so);
        assert(store->recover().ok());
        store->put("/registry/nodes/n1", kNode, true);
        sched = scheduler::create_scheduler(*store);
        // As the board's API server: 2 workers, 4 KiB receive buffers, 8 streams
        api::HttpServerOptions o;
        o.host = "127.0.0.1";
        o.port = port;
        o.workers = 2;
        o.rx_buffer_size = 4096;
        o.worker_stack_bytes = 262144;
        o.watch_streams = 8;
        server = std::make_unique<api::HttpServer>(o, *store, sched.get());
        assert(server->start());
    }
    ~ControlPlane() {
        server->stop();
        if (std::system(("rm -rf " + dir).c_str()) != 0) std::printf("  %s was not removed\n", dir.c_str());
    }

    // `n` more streams in `out`, which holds every stream open: streams
    // closed before are gone from the hub first
    void watch(std::vector<std::unique_ptr<Watcher>>& out, int n) {
        while (server->watch_hub()->active_streams() != out.size()) osal::Thread::sleep_ms(5);
        for (int i = 0; i < n; ++i) out.push_back(std::make_unique<Watcher>(port));
        while (server->watch_hub()->active_streams() < out.size()) osal::Thread::sleep_ms(5);
    }

    // Until the hub has sent `frames` more frames (every watcher its event)
    void settle(uint64_t frames_before, uint64_t frames) {
        for (int i = 0; i < 400 && server->watch_hub()->frames_sent() < frames_before + frames; ++i) osal::Thread::sleep_ms(5);
        osal::Thread::sleep_ms(30);
    }
};

static std::string digest_of(const std::vector<uint8_t>& m) { return osal::Sha256::hex(osal::Sha256::of(m.data(), m.size())); }

static void row(const char* what, const board::Window& w) {
    std::printf("  %-28s peak %7zu (PSRAM %7zu, internal %6zu)  retained %7zu  largest block %7zu\n", what, w.peak_total, w.peak_psram,
                w.peak_internal, w.retained, w.largest);
}

// =============================================================================
// Per request type, per module size
// =============================================================================

struct Measured {
    std::vector<std::pair<std::string, board::Window>> rows;
    size_t ring_retained{0};
    const board::Window& operator[](const std::string& what) const {
        for (const auto& r : rows) if (r.first == what) return r.second;
        assert(false);
        return rows[0].second;
    }
};

static Measured measure(size_t module_size, uint16_t port, int watchers, int pods) {
    ControlPlane cp(port);
    std::vector<std::unique_ptr<Watcher>> watches;
    cp.watch(watches, watchers);

    std::vector<std::string> bodies;
    for (int i = 0; i < pods; ++i) bodies.push_back(inline_pod("app-" + std::to_string(i), module_bytes(module_size, i)));
    Measured s;
    auto record = [&](const char* what, const board::Window& w) {
        row(what, w);
        s.rows.emplace_back(what, w);
    };
    std::printf("module %zu bytes (pod manifest %zu bytes), %d watchers, %d pods\n", module_size, bodies[0].size(), watchers, pods);

    // Create the pods; the last create is the one measured
    for (int i = 0; i < pods; ++i) {
        const uint64_t f0 = cp.server->watch_hub()->frames_sent();
        board::Usage m = board::mark();
        Reply r = call(port, "POST", "/api/v1/namespaces/default/pods?fieldManager=kubectl-create&fieldValidation=Ignore", &bodies[i]);
        cp.settle(f0, watchers);
        board::Window w = board::since(m);
        assert(r.status == 201);
        if (i == pods - 1) record("create", w);
    }
    // Reads, as kubectl sends them
    {
        const char* table = "application/json;as=Table;v=v1;g=meta.k8s.io";
        struct Read { const char* what; const char* path; const char* accept; };
        const Read reads[] = {{"get", "/api/v1/namespaces/default/pods/app-0", "application/json"},
                              {"get (Table)", "/api/v1/namespaces/default/pods/app-0", table},
                              {"list", "/api/v1/namespaces/default/pods", "application/json"},
                              {"list (Table)", "/api/v1/namespaces/default/pods", table}};
        for (const Read& rd : reads) {
            board::Usage m = board::mark();
            Reply r = call(port, "GET", rd.path, nullptr, "application/json", rd.accept);
            board::Window w = board::since(m);
            assert(r.status == 200);
            record(rd.what, w);
        }
    }
    // The control plane's own kubelet: its sync pass, and the module of the pod it starts
    {
        kubelet::HttpKubeClient client("127.0.0.1", port);
        client.set_node_filter("n1");
        int seen = 0;
        board::Usage m = board::mark();
        assert(client.for_each_node_pod([&](const kubelet::PodListEntry&) { ++seen; }));
        board::Window w = board::since(m);
        assert(seen == pods);
        record("kubelet sync pass", w);
        if (module_size > 0) {
            runtime::ModuleBytes got;
            std::string why;
            const std::vector<uint8_t> want = module_bytes(module_size, 0);
            m = board::mark();
            const bool ok = client.get_image(digest_of(want), got, why);
            w = board::since(m);
            assert(ok && std::vector<uint8_t>(got.begin(), got.end()) == want);
            record("kubelet image fetch", w);   // the module goes to pod memory, outside this heap
        }
    }
    // A status update as the kubelet sends it, to every watcher
    {
        const std::string patch = "{\"status\":{\"phase\":\"Running\",\"podIP\":\"10.0.0.2\",\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}]}}";
        const uint64_t f0 = cp.server->watch_hub()->frames_sent();
        board::Usage m = board::mark();
        Reply r = call(port, "PATCH", "/api/v1/namespaces/default/pods/app-0/status", &patch, "application/merge-patch+json");
        cp.settle(f0, watchers);
        board::Window w = board::since(m);
        assert(r.status == 200);
        record("status patch", w);
    }
    // Controller passes over every pod
    {
        controller::DeploymentController deployments(*cp.store, cp.sched.get());
        controller::WorkloadController workloads(*cp.store, cp.sched.get());
        controller::NodeLifecycleController lifecycle(*cp.store);
        controller::DeviceController devices(*cp.store, cp.sched.get());
        app::IngressControllerOptions io;
        io.store = cp.store.get();
        app::IngressController ingress(io);
        auto pass = [&](const char* what, const std::function<void()>& fn) {
            board::Usage m = board::mark();
            fn();
            record(what, board::since(m));
        };
        pass("scheduler pass", [&] { cp.sched->reconcile_cluster(); });
        pass("deployment pass", [&] { deployments.reconcile(); });
        pass("workload pass", [&] { workloads.reconcile(); });
        pass("node lifecycle pass", [&] { lifecycle.reconcile_nodes(); });
        pass("device pass", [&] { devices.reconcile(); });
        pass("ingress rebuild", [&] { ingress.refresh_routes(); });
    }
    // kubectl delete (graceful), then the kubelet's final delete
    {
        uint64_t f0 = cp.server->watch_hub()->frames_sent();
        board::Usage m = board::mark();
        Reply r = call(port, "DELETE", "/api/v1/namespaces/default/pods/app-0");
        cp.settle(f0, watchers);
        board::Window w = board::since(m);
        assert(r.status == 200);
        record("delete (graceful)", w);
        f0 = cp.server->watch_hub()->frames_sent();
        m = board::mark();
        r = call(port, "DELETE", "/api/v1/namespaces/default/pods/app-0?gracePeriodSeconds=0");
        cp.settle(f0, watchers);
        w = board::since(m);
        assert(r.status == 200);
        record("delete (final)", w);
    }
    watches.clear();
    // What the watch hub keeps for resourceVersion watches
    {
        board::Usage before = board::in_use();
        cp.server->watch_hub()->stop();
        board::Usage after = board::in_use();
        s.ring_retained = before.total() > after.total() ? before.total() - after.total() : 0;
        std::printf("  %-28s %7zu\n", "watch hub replay ring", s.ring_retained);
    }
    return s;
}

// =============================================================================
// The control plane of a 2 MB devkit: kube32-cli deploy, three times
// =============================================================================

// Free at rest on the devkit's control plane with a handful of pods, Devices
// and a Deployment: internal RAM and PSRAM outside the pod pool
constexpr size_t kBoardInternalFree = 21 * 1024;
constexpr size_t kBoardPsramFree = 269 * 1024;

// Pods with `module_size` modules are deployed one after the other, each
// synced, started and reported Running by the control plane's own kubelet,
// with the controllers' passes in between and `watchers` watch streams
// open; then listed and deleted. Every request must succeed.
static bool deploy_on_board(size_t module_size, int watchers, int pods, uint16_t port) {
    ControlPlane cp(port);
    std::vector<std::unique_ptr<Watcher>> watches;
    cp.watch(watches, watchers);
    std::vector<std::string> bodies;
    std::vector<std::string> digests;
    for (int i = 0; i < pods; ++i) {
        const std::vector<uint8_t> m = module_bytes(module_size, i + 1);
        bodies.push_back(inline_pod("app-" + std::to_string(i), m));
        digests.push_back(digest_of(m));
    }
    const std::string running = "{\"status\":{\"phase\":\"Running\",\"podIP\":\"10.0.0.2\",\"conditions\":[{\"type\":\"Ready\",\"status\":\"True\"}],"
                                "\"containerStatuses\":[{\"name\":\"c\",\"ready\":true,\"restartCount\":0,\"state\":{\"running\":{}}}]}}";
    kubelet::HttpKubeClient kubelet("127.0.0.1", port);
    kubelet.set_node_filter("n1");
    controller::DeploymentController deployments(*cp.store, cp.sched.get());
    controller::WorkloadController workloads(*cp.store, cp.sched.get());
    controller::NodeLifecycleController lifecycle(*cp.store);
    controller::DeviceController devices(*cp.store, cp.sched.get());
    app::IngressControllerOptions io;
    io.store = cp.store.get();
    app::IngressController ingress(io);
    std::vector<std::string> paths, status_paths, final_paths;
    for (int i = 0; i < pods; ++i) {
        paths.push_back("/api/v1/namespaces/default/pods/app-" + std::to_string(i));
        status_paths.push_back(paths.back() + "/status");
        final_paths.push_back(paths.back() + "?gracePeriodSeconds=0");
    }
    auto passes = [&]() {
        try {
            cp.sched->reconcile_cluster();
            deployments.reconcile();
            workloads.reconcile();
            lifecycle.reconcile_nodes();
            devices.reconcile();
            ingress.refresh_routes();
        } catch (const std::bad_alloc&) {
            std::printf("    a controller pass ran out of memory\n");
        }
    };

    const uint64_t refused0 = board::refused.load();
    board::limit(kBoardInternalFree, kBoardPsramFree);
    bool ok = true;
    auto expect = [&](const char* what, int got, int want) {
        if (got == want) return;
        std::printf("    %s: %d, wanted %d\n", what, got, want);
        ok = false;
    };
    for (int i = 0; i < pods && ok; ++i) {
        expect("kubectl create", call(port, "POST", "/api/v1/namespaces/default/pods?fieldManager=kubectl-create", &bodies[i]).status, 201);
        // The kubelet's pass, the module it loads (the pod's own memory
        // afterwards), its status update; the controllers' passes
        int seen = 0;
        try {
            expect("kubelet sync pass", kubelet.for_each_node_pod([&](const kubelet::PodListEntry&) { ++seen; }) ? 1 : 0, 1);
            runtime::ModuleBytes module;
            std::string why;
            const bool pulled = kubelet.get_image(digests[i], module, why);
            if (!pulled) std::printf("    image: %s\n", why.c_str());
            expect("kubelet image pull", pulled ? 1 : 0, 1);
        } catch (const std::bad_alloc&) {
            expect("kubelet out of memory", 1, 0);
        }
        passes();
        expect("status update", call(port, "PATCH", status_paths[i].c_str(), &running, "application/merge-patch+json").status, 200);
        passes();
    }
    if (ok) {
        expect("kubectl get pods", call(port, "GET", "/api/v1/namespaces/default/pods").status, 200);
        expect("kubectl get pods (Table)", call(port, "GET", "/api/v1/pods", nullptr, "application/json", "application/json;as=Table;v=v1;g=meta.k8s.io").status, 200);
        for (int i = 0; i < pods; ++i) {
            expect("kubectl get pod -o yaml", call(port, "GET", paths[i].c_str()).status, 200);
            expect("kubectl delete pod", call(port, "DELETE", paths[i].c_str()).status, 200);
            passes();
            expect("final delete", call(port, "DELETE", final_paths[i].c_str()).status, 200);
        }
    }
    board::unlimit();
    const uint64_t refused = board::refused.load() - refused0;
    std::printf("  %zu-byte modules, %d pods, %d watchers: %s, %llu allocations refused\n", module_size, pods, watchers,
                ok ? "every request succeeded" : "requests failed", static_cast<unsigned long long>(refused));
    return ok && refused == 0;
}

// =============================================================================
// Out of memory anywhere in a request: the store and every watch stream agree
// afterwards, and the pod can still be updated and deleted
// =============================================================================

static void test_out_of_memory_leaves_nothing_half_done(uint16_t port) {
    std::cout << "[RUN] test_out_of_memory_leaves_nothing_half_done" << std::endl;
    ControlPlane cp(port);
    const std::vector<uint8_t> m = module_bytes(10 * 1024 + 400, 42);
    const std::string body = inline_pod("victim", m);
    const std::string running = "{\"status\":{\"phase\":\"Running\"}}";
    size_t failed = 0, consistent = 0;
    for (size_t budget = 1024; budget <= 80 * 1024; budget += 1536) {
        std::vector<std::unique_ptr<Watcher>> watches;
        cp.watch(watches, 2);
        // Room for `budget` bytes: a quarter internal, the rest PSRAM
        board::limit(budget / 4, budget - budget / 4);
        Reply r = call(port, "POST", "/api/v1/namespaces/default/pods", &body);
        board::unlimit();
        const bool exists = call(port, "GET", "/api/v1/namespaces/default/pods/victim").status == 200;
        // Past the hub's next pass: what the streams were sent has arrived
        auto delivered = [&]() {
            for (const auto& w : watches) if (!(w->added() >= 1 || w->restarted())) return false;
            return true;
        };
        for (int i = 0; i < 100 && exists && !delivered(); ++i) osal::Thread::sleep_ms(20);
        if (!exists) osal::Thread::sleep_ms(300);
        if (r.status != 201) ++failed;
        // An answered create was made; a created pod reached every stream
        // (or the stream was told to start over); a pod not made reached none
        assert(r.status != 201 || exists);
        for (const auto& w : watches) {
            if (exists) assert(w->added() >= 1 || w->restarted());
            else assert(w->added() == 0);
        }
        if (exists) {
            assert(call(port, "PATCH", "/api/v1/namespaces/default/pods/victim/status", &running, "application/merge-patch+json").status == 200);
            assert(call(port, "DELETE", "/api/v1/namespaces/default/pods/victim").status == 200);
            assert(call(port, "DELETE", "/api/v1/namespaces/default/pods/victim?gracePeriodSeconds=0").status == 200);
        }
        assert(call(port, "GET", "/api/v1/namespaces/default/pods/victim").status == 404);
        ++consistent;
    }
    std::printf("  %zu budgets, %zu creates refused for memory, all consistent\n", consistent, failed);
    assert(failed > 0 && failed < consistent);
    std::cout << "[PASS] test_out_of_memory_leaves_nothing_half_done" << std::endl;
}

// =============================================================================
// Lists under memory pressure: every answer is a complete list or a 503
// Status, and the server answers a full list again once memory is back
// =============================================================================

static void test_list_under_memory_pressure(uint16_t port) {
    std::cout << "[RUN] test_list_under_memory_pressure" << std::endl;
    ControlPlane cp(port);
    const std::vector<uint8_t> m = module_bytes(256, 7);
    for (int i = 0; i < 16; ++i) {
        const std::string body = inline_pod("list-" + std::to_string(i), m);
        assert(call(port, "POST", "/api/v1/namespaces/default/pods", &body).status == 201);
    }
    size_t complete = 0, refused = 0;
    for (size_t budget = 2048; budget <= 96 * 1024; budget += 2048) {
        board::limit(budget / 4, budget - budget / 4);
        Reply r = call(port, "GET", "/api/v1/pods");
        board::unlimit();
        if (r.status == 200) ++complete;
        else if (r.status == 503) {
            ++refused;
            assert(std::strstr(r.start, "node is out of memory") != nullptr);
        }
        // Never an answer other than a list or the out-of-memory Status
        assert(r.status == 200 || r.status == 503 || r.status == 0);
    }
    std::printf("  %zu budgets answered a list, %zu answered 503\n", complete, refused);
    assert(refused > 0 && complete > 0);
    assert(call(port, "GET", "/api/v1/pods").status == 200);
    std::cout << "[PASS] test_list_under_memory_pressure" << std::endl;
}

// =============================================================================
// Out of memory in a controller pass ends the pass, not the thread: with no
// room at all the loops keep running, and when memory is back they work
// =============================================================================

static void test_controller_loops_survive_exhaustion(uint16_t port) {
    std::cout << "[RUN] test_controller_loops_survive_exhaustion" << std::endl;
    ControlPlane cp(port);
    controller::DeploymentControllerConfig dcfg;
    dcfg.reconcile_interval_ms = 20;
    dcfg.resync_interval_ms = 40;
    controller::DeploymentController deployments(*cp.store, cp.sched.get(), dcfg);
    controller::WorkloadControllerConfig wcfg;
    wcfg.reconcile_interval_ms = 20;
    wcfg.resync_interval_ms = 40;
    controller::WorkloadController workloads(*cp.store, cp.sched.get(), wcfg);
    controller::NodeLifecycleConfig lcfg;
    lcfg.check_interval_ms = 20;
    controller::NodeLifecycleController lifecycle(*cp.store, lcfg);
    controller::DeviceControllerConfig ccfg;
    ccfg.reconcile_interval_ms = 20;
    ccfg.resync_interval_ms = 40;
    controller::DeviceController devices(*cp.store, cp.sched.get(), ccfg);
    assert(deployments.start() && workloads.start() && lifecycle.start() && devices.start());

    const uint64_t refused0 = board::refused.load();
    const uint64_t passes0 = deployments.passes();
    board::limit(0, 0);
    osal::Thread::sleep_ms(600);
    board::unlimit();
    // The passes ran into the limit: allocations were refused, and every
    // thread is still here
    assert(board::refused.load() > refused0);
    std::printf("  %llu allocations refused, deployment passes %llu -> %llu\n",
                static_cast<unsigned long long>(board::refused.load() - refused0), static_cast<unsigned long long>(passes0),
                static_cast<unsigned long long>(deployments.passes()));

    const std::string dep = "{\"apiVersion\":\"apps/v1\",\"kind\":\"Deployment\",\"metadata\":{\"name\":\"web\"},\"spec\":{\"replicas\":2,"
                            "\"selector\":{\"matchLabels\":{\"app\":\"web\"}},\"template\":{\"metadata\":{\"labels\":{\"app\":\"web\"}},"
                            "\"spec\":{\"containers\":[{\"name\":\"c\",\"image\":\"wasm://embedded/echo\"}]}}}}";
    assert(call(port, "POST", "/apis/apps/v1/namespaces/default/deployments", &dep).status == 201);
    bool created = false;
    for (int i = 0; i < 200 && !created; ++i) {
        std::vector<std::string> pods;
        cp.store->list("/registry/pods/", pods);
        created = pods.size() == 2;
        if (!created) osal::Thread::sleep_ms(20);
    }
    assert(created);
    deployments.stop();
    workloads.stop();
    lifecycle.stop();
    devices.stop();
    std::cout << "[PASS] test_controller_loops_survive_exhaustion" << std::endl;
}

int main() {
    std::cout << "=== API server memory with inline images ===" << std::endl;
    osal::Socket::init_networking();
    uint16_t port = 18760;

    // Peak heap per request type against module size: the same for a 64 KiB
    // module as for none. The kubelet reads a module into pod memory (the
    // pool block, then PSRAM above its reserve), not this heap: its fetch
    // takes as much for a 64 KiB module as for a 10 KiB one.
    std::vector<std::pair<size_t, Measured>> by_size;
    for (size_t kib : {0, 10, 32, 64}) {
        const size_t size = kib * 1024 + (kib ? 400 : 0);
        by_size.emplace_back(size, measure(size, port++, 3, 3));
    }
    const Measured& none = by_size.front().second;
    const Measured& small = by_size[1].second;
    for (const auto& [size, got] : by_size) {
        for (const auto& [what, w] : got.rows) {
            const board::Window& base = what == "kubelet image fetch" ? small[what] : none[what];
            assert(w.peak_total <= base.peak_total + 8 * 1024);
            assert(w.peak_internal <= base.peak_internal + 8 * 1024);
            assert(w.largest <= 16 * 1024);
        }
        assert(got.ring_retained <= none.ring_retained + 8 * 1024);
    }
    // Fan-out to more watchers: one copy of each event, whatever their number
    {
        Measured six = measure(64 * 1024 + 400, port++, 6, 2);
        assert(six["status patch"].peak_total <= none["status patch"].peak_total + 8 * 1024);
    }

    std::cout << "[RUN] deploys on a 2 MB control plane" << std::endl;
    for (size_t kib : {10, 32, 64}) {
        assert(deploy_on_board(kib * 1024 + 400, 4, 3, port++));
    }
    std::cout << "[PASS] deploys on a 2 MB control plane" << std::endl;

    test_out_of_memory_leaves_nothing_half_done(port++);
    test_list_under_memory_pressure(port++);
    test_controller_loops_survive_exhaustion(port++);
    std::cout << "All API memory tests passed" << std::endl;
    return 0;
}
