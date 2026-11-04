// Blob images (wasm://blob/<name>): the kubelet parses and compiles a
// module in place from the node's mapped flash blob store, as kube32-sim
// maps its region file. The reference parser; a pod that runs from a blob
// with no copy of its bytes and the blob held open while it runs; the blob
// removed or replaced meanwhile (the pod keeps the old bytes, the next
// start sees the change); a blob that is missing, or in a store that is not
// mapped; the API server's Pod Overhead, which does not count the module's
// bytes for a blob image; and the kubelet's own admission estimate.
#include "hal/capability_firewall.hpp"
#include "runtime/k32_app_abi.h"
#include "runtime/image_registry.hpp"
#include "runtime/wasm_runtime.hpp"
#include "runtime/app_pod.hpp"
#include "kubelet/kubelet.hpp"
#include "kubelet/kube_api_client.hpp"
#include "storage/blob_store.hpp"
#include "models/kube_models.hpp"
#include "embedded/embedded_blobs.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

using namespace kube32;

static const runtime::EmbeddedImage* embedded_image(const char* name) {
    size_t n = 0;
    const auto* imgs = embedded::images(n);
    for (size_t i = 0; i < n; ++i) if (std::strcmp(imgs[i].name, name) == 0) return &imgs[i];
    return nullptr;
}

static void put_blob(storage::BlobStore& store, const char* name, const uint8_t* data, size_t len) {
    int32_t err = 0;
    std::string why;
    auto w = store.create(name, len, err, why);
    assert(w && w->write(data, len, why) == K32_OK && w->commit(why) == K32_OK);
}

// A region whose bytes are not in the address space (a file on a board's card)
class UnmappedRegion : public storage::BlobRegion {
public:
    explicit UnmappedRegion(size_t size) : ram_(storage::create_ram_blob_region(size)) {}
    size_t size() const override { return ram_->size(); }
    bool read(size_t o, void* d, size_t n) override { return ram_->read(o, d, n); }
    bool write(size_t o, const void* s, size_t n) override { return ram_->write(o, s, n); }
    bool erase(size_t o, size_t n) override { return ram_->erase(o, n); }
    const uint8_t* data() const override { return nullptr; }
    std::string name() const override { return "unmapped"; }

private:
    std::unique_ptr<storage::BlobRegion> ram_;
};

class RecordingClient : public kubelet::KubeApiClient {
public:
    std::vector<std::pair<models::KubePod, std::string>> pods;
    std::vector<std::string> events;
    std::string last_status;
    bool register_node(std::string_view, bool& conflict) override { conflict = false; return true; }
    bool patch_node_status(const std::string&, std::string_view) override { return true; }
    bool list_pods(std::vector<models::KubePod>& out) override { for (auto& p : pods) out.push_back(p.first); return true; }
    bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out) override { out = pods; return true; }
    bool patch_pod_status(const std::string&, const std::string&, const std::string& patch) override { last_status = patch; return true; }
    bool create_event(const std::string&, const std::string& ev) override { events.push_back(ev); return true; }
    bool has_event(const char* reason, const char* text) const {
        for (const auto& e : events)
            if (e.find(std::string("\"reason\":\"") + reason + "\"") != std::string::npos && e.find(text) != std::string::npos) return true;
        return false;
    }
};

static models::KubePod make_pod(const std::string& name) {
    models::KubePod pod;
    pod.name = name;
    pod.namespace_name = "default";
    pod.node_name = "test-node-01";
    pod.status = "Pending";
    return pod;
}
static std::string plain_manifest(const std::string& name, const std::string& image) {
    return "{\"metadata\":{\"name\":\"" + name + "\",\"namespace\":\"default\"},"
           "\"spec\":{\"nodeName\":\"test-node-01\",\"containers\":[{\"name\":\"c\",\"image\":\"" + image + "\"}]}}";
}

static void test_blob_image_refs() {
    std::string name;
    assert(runtime::parse_blob_image_ref("wasm://blob/doom-sim", name) && name == "doom-sim");
    assert(runtime::parse_blob_image_ref("blob/a.b_c-1", name) && name == "a.b_c-1");
    assert(!runtime::parse_blob_image_ref("wasm://blob/", name));
    assert(!runtime::parse_blob_image_ref("wasm://blob/a/b", name));
    assert(!runtime::parse_blob_image_ref("wasm://blob/..", name));
    assert(!runtime::parse_blob_image_ref("wasm://blob/x.part", name));
    assert(!runtime::parse_blob_image_ref("wasm://sd/echo", name));
    assert(!runtime::parse_blob_image_ref("wasm://embedded/echo", name));
    assert(!runtime::parse_blob_image_ref("wasm://blob/" + std::string(48, 'a'), name));
    std::cout << "[PASS] test_blob_image_refs" << std::endl;
}

static std::string g_dir;

static void test_kubelet_runs_module_from_blob() {
    hal::CapabilityFirewall::instance().reset();
    const runtime::EmbeddedImage* echo = embedded_image("echo");
    assert(echo && "images/echo.wasm must be embedded in the host build");
    // the simulator's flash region: a file, mapped
    storage::FlashBlobStore flash(storage::create_file_blob_region(g_dir + "/blobs.img", 1024 * 1024), false);
    put_blob(flash, "echo-mod", echo->data, echo->len);
    storage::set_node_blob_stores(&flash, nullptr);

    kubelet::KubeletConfig cfg;
    cfg.node_name = "test-node-01";
    auto rt = runtime::create_wasm_runtime();
    auto client = std::make_unique<RecordingClient>();
    RecordingClient* c = client.get();
    kubelet::Kubelet kl(cfg, *rt, std::move(client));

    // 1. the pod runs from the blob: no copy of the module, the blob held open
    c->pods.emplace_back(make_pod("from-blob"), plain_manifest("from-blob", "wasm://blob/echo-mod"));
    kl.sync_assigned_pods();
    kubelet::LocalPodRecord rec;
    assert(kl.get_pod_record("default/from-blob", rec));
    assert(rec.execution_phase == kubelet::PodExecutionPhase::Running);
    assert(c->has_event("Pulled", "flash blob"));
    assert(c->last_status.find("\"phase\":\"Running\"") != std::string::npos);
    assert(runtime::AppPodHost::instance().pod_handles_requests("default/from-blob"));
    assert(rt->get_status("default/from-blob").bytecode_bytes == 0);
    assert(flash.open_readers() == 1);

    // 2. the blob is removed and replaced while the pod runs: the pod keeps
    // its bytes, and a second pod from the new blob runs beside it
    std::string why;
    assert(flash.remove("echo-mod", why) == K32_OK);
    assert(runtime::AppPodHost::instance().pod_handles_requests("default/from-blob"));
    assert(flash.open_readers() == 1);
    put_blob(flash, "echo-mod", echo->data, echo->len);
    c->pods.emplace_back(make_pod("second"), plain_manifest("second", "wasm://blob/echo-mod"));
    kl.sync_assigned_pods();
    assert(kl.get_pod_record("default/second", rec) && rec.execution_phase == kubelet::PodExecutionPhase::Running);
    assert(flash.open_readers() == 2);

    // 3. a missing blob fails the pod with ErrImagePull and says why
    c->events.clear();
    c->pods.emplace_back(make_pod("absent"), plain_manifest("absent", "wasm://blob/absent"));
    kl.sync_assigned_pods();
    assert(kl.get_pod_record("default/absent", rec) && rec.execution_phase == kubelet::PodExecutionPhase::Failed);
    assert(rec.last_error_message.find("blob absent is not in the node's blob store") != std::string::npos);
    assert(rec.last_error_message.find("firmware update") != std::string::npos);
    assert(c->last_status.find("ErrImagePull") != std::string::npos);
    assert(c->has_event("Failed", "not in the node's blob store"));

    // 4. stopping the pods lets the blobs go
    c->pods.clear();
    kl.sync_assigned_pods();
    assert(kl.get_active_pod_keys().empty());
    assert(flash.open_readers() == 0);

    // 5. the blob gone before a new start: the pod does not start
    assert(flash.remove("echo-mod", why) == K32_OK);
    c->pods.emplace_back(make_pod("again"), plain_manifest("again", "wasm://blob/echo-mod"));
    kl.sync_assigned_pods();
    assert(kl.get_pod_record("default/again", rec) && rec.execution_phase == kubelet::PodExecutionPhase::Failed);
    assert(c->last_status.find("ErrImagePull") != std::string::npos);
    c->pods.clear();
    kl.sync_assigned_pods();

    // 6. a store that is not mapped (a card) cannot run a module in place
    storage::FlashBlobStore unmapped(std::make_unique<UnmappedRegion>(256 * 1024), false);
    put_blob(unmapped, "echo-mod", echo->data, echo->len);
    storage::set_node_blob_stores(&unmapped, nullptr);
    c->pods.emplace_back(make_pod("unmapped"), plain_manifest("unmapped", "wasm://blob/echo-mod"));
    kl.sync_assigned_pods();
    assert(kl.get_pod_record("default/unmapped", rec) && rec.execution_phase == kubelet::PodExecutionPhase::Failed);
    assert(rec.last_error_message.find("is not mapped") != std::string::npos);
    c->pods.clear();
    kl.sync_assigned_pods();
    storage::set_node_blob_stores(nullptr, nullptr);
    hal::CapabilityFirewall::instance().reset();
    std::cout << "[PASS] test_kubelet_runs_module_from_blob" << std::endl;
}

// The API server's Pod Overhead: a blob image's bytes are not held, so with
// kube32.io/wasm-size it is the estimate of an image of that size without
// the module's bytes; without the size it is nothing (the kubelet's
// admission, which sees the blob, counts it)
static void test_pod_overhead_of_blob_image() {
    auto parse = [](const std::string& json) {
        models::JsonValue doc;
        std::string err;
        bool ok = models::parse_json(json, doc, err);
        assert(ok);
        return doc;
    };
    auto kib_up = [](size_t bytes) { return (bytes + 1023) / 1024 * 1024; };
    models::JsonValue blob = parse(R"({"metadata":{"name":"b","annotations":{"kube32.io/wasm-size":"20000","kube32.io/wasm-sha256":"ab"}},)"
                                   R"("spec":{"containers":[{"name":"c","image":"wasm://blob/b"}]}})");
    models::JsonValue held = parse(R"({"metadata":{"name":"s","annotations":{"kube32.io/wasm-size":"20000","kube32.io/wasm-sha256":"ab"}},)"
                                   R"("spec":{"containers":[{"name":"c","image":"registry.local/s:1"}]}})");
    size_t linear = 0;
    const size_t need = runtime::estimate_pod_memory(nullptr, 20000, 64 * 1024, 4 * 1024, true, runtime::EstimateSizes::BoardHeld, &linear, false);
    assert(runtime::set_pod_overhead(blob) == kib_up(need - linear));
    assert(runtime::set_pod_overhead(held) == kib_up(need + 20008 - linear));
    models::JsonValue bare = parse(R"({"metadata":{"name":"x"},"spec":{"overhead":{"memory":"99Ki"},"containers":[{"name":"c","image":"wasm://blob/x"}]}})");
    assert(runtime::set_pod_overhead(bare) == 0 && !bare.get("spec")->get("overhead"));
    std::cout << "[PASS] test_pod_overhead_of_blob_image" << std::endl;
}

int main() {
    char tmpl[] = "/tmp/kube32-blobimg-XXXXXX";
    char* p = mkdtemp(tmpl);
    assert(p);
    g_dir = p;
    size_t n = 0;
    const auto* imgs = embedded::images(n);
    runtime::ImageRegistry::instance().add_all(imgs, n);
    test_blob_image_refs();
    test_pod_overhead_of_blob_image();
    test_kubelet_runs_module_from_blob();
    std::cout << "All blob image tests passed" << std::endl;
    return 0;
}
